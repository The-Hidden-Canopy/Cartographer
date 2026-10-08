#include <carto/assets/blob_store.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

namespace carto::assets {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

std::atomic<std::uint64_t> g_blob_temp_counter{0U};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

class Sha256State final {
public:
    Sha256State()
        : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void update(std::span<const std::uint8_t> bytes) {
        bit_count_ += static_cast<std::uint64_t>(bytes.size()) * 8U;
        for (const auto byte : bytes) {
            block_[block_size_++] = byte;
            if (block_size_ == block_.size()) {
                transform();
                block_size_ = 0U;
            }
        }
    }

    [[nodiscard]] Sha256Digest finish() {
        block_[block_size_++] = 0x80U;
        if (block_size_ > 56U) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.end(), 0U);
            transform();
            block_size_ = 0U;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.begin() + 56, 0U);
        for (unsigned index = 0U; index < 8U; ++index) {
            block_[63U - index] = static_cast<std::uint8_t>(bit_count_ >> (index * 8U));
        }
        transform();

        Sha256Digest digest;
        for (unsigned index = 0U; index < state_.size(); ++index) {
            digest.bytes[index * 4U] = static_cast<std::uint8_t>(state_[index] >> 24U);
            digest.bytes[index * 4U + 1U] = static_cast<std::uint8_t>(state_[index] >> 16U);
            digest.bytes[index * 4U + 2U] = static_cast<std::uint8_t>(state_[index] >> 8U);
            digest.bytes[index * 4U + 3U] = static_cast<std::uint8_t>(state_[index]);
        }
        return digest;
    }

private:
    void transform() {
        std::array<std::uint32_t, 64> schedule{};
        for (unsigned index = 0U; index < 16U; ++index) {
            const unsigned offset = index * 4U;
            schedule[index] = (static_cast<std::uint32_t>(block_[offset]) << 24U) |
                (static_cast<std::uint32_t>(block_[offset + 1U]) << 16U) |
                (static_cast<std::uint32_t>(block_[offset + 2U]) << 8U) |
                static_cast<std::uint32_t>(block_[offset + 3U]);
        }
        for (unsigned index = 16U; index < schedule.size(); ++index) {
            const std::uint32_t s0 = rotate_right(schedule[index - 15U], 7U) ^
                rotate_right(schedule[index - 15U], 18U) ^ (schedule[index - 15U] >> 3U);
            const std::uint32_t s1 = rotate_right(schedule[index - 2U], 17U) ^
                rotate_right(schedule[index - 2U], 19U) ^ (schedule[index - 2U] >> 10U);
            schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (unsigned index = 0U; index < schedule.size(); ++index) {
            const std::uint32_t s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temporary1 = h + s1 + choose + kRoundConstants[index] + schedule[index];
            const std::uint32_t s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_size_ = 0U;
    std::uint64_t bit_count_ = 0U;
};

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

} // namespace

bool Sha256Digest::is_zero() const noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t value) { return value == 0U; });
}

std::string Sha256Digest::hex() const {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        stream << std::setw(2) << static_cast<unsigned>(byte);
    }
    return stream.str();
}

core::Result<Sha256Digest> Sha256Digest::from_hex(std::string_view value) {
    if (value.size() != 64U) {
        return core::Result<Sha256Digest>::failure(invalid("SHA-256 digest must contain 64 hex characters"));
    }
    Sha256Digest digest;
    for (std::size_t index = 0U; index < digest.bytes.size(); ++index) {
        const int high = hex_value(value[index * 2U]);
        const int low = hex_value(value[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return core::Result<Sha256Digest>::failure(invalid("SHA-256 digest contains a non-hex character"));
        }
        digest.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return core::Result<Sha256Digest>::success(digest);
}

Sha256Digest sha256(std::span<const std::uint8_t> bytes) {
    Sha256State state;
    state.update(bytes);
    return state.finish();
}

core::Result<void> BlobStoreLimits::validate() const {
    if (max_blob_bytes == 0U ||
        max_blob_bytes > kBlobStoreAbsoluteMaxBlobBytes ||
        max_media_type_bytes == 0U ||
        max_media_type_bytes > kBlobStoreAbsoluteMaxMediaTypeBytes) {
        return core::Result<void>::failure(invalid(
            "blob-store limits must be non-zero and within process safety bounds"));
    }
    return core::Result<void>::success();
}

BlobStore::BlobStore(std::filesystem::path root, BlobStoreLimits limits)
    : root_(std::move(root)), limits_(limits) {}

std::filesystem::path BlobStore::path_for(const Sha256Digest& digest) const {
    const std::string name = digest.hex();
    return root_ / "sha256" / name.substr(0U, 2U) / name.substr(2U, 2U) / name;
}

core::Result<BlobRef> BlobStore::put(
    std::string_view bytes,
    std::string media_type) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return put(std::span<const std::uint8_t>(data, bytes.size()), std::move(media_type));
}

core::Result<BlobRef> BlobStore::put(
    std::span<const std::uint8_t> bytes,
    std::string media_type) {
    if (auto result = limits_.validate(); !result) {
        return core::Result<BlobRef>::failure(result.error());
    }
    if (bytes.size() > limits_.max_blob_bytes) {
        return core::Result<BlobRef>::failure(invalid("blob exceeds the configured size limit"));
    }
    if (media_type.empty() || media_type.size() > limits_.max_media_type_bytes ||
        media_type.find_first_of("\r\n\t") != std::string::npos) {
        return core::Result<BlobRef>::failure(invalid("blob media type is invalid or too long"));
    }
    const Sha256Digest digest = sha256(bytes);
    const std::filesystem::path destination = path_for(digest);
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        return core::Result<BlobRef>::failure(io_error("unable to create blob directories"));
    }

    const bool destination_exists = std::filesystem::exists(destination, error);
    if (error) {
        return core::Result<BlobRef>::failure(io_error("unable to inspect blob path"));
    }
    if (destination_exists) {
        if (error || !std::filesystem::is_regular_file(destination, error)) {
            return core::Result<BlobRef>::failure(io_error("blob path is not a regular file"));
        }
        const auto existing = read(digest);
        if (!existing) {
            return core::Result<BlobRef>::failure(existing.error());
        }
    } else {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
        const auto counter = g_blob_temp_counter.fetch_add(1U, std::memory_order_relaxed);
        const std::filesystem::path temporary = destination.string() + ".tmp-" +
            std::to_string(static_cast<unsigned long long>(stamp)) + "-" +
            std::to_string(static_cast<unsigned long long>(thread)) + "-" +
            std::to_string(static_cast<unsigned long long>(counter));
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return core::Result<BlobRef>::failure(io_error("unable to create temporary blob"));
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, error);
            return core::Result<BlobRef>::failure(io_error("unable to write temporary blob"));
        }
        stream.close();
        std::filesystem::rename(temporary, destination, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            if (!std::filesystem::exists(destination)) {
                return core::Result<BlobRef>::failure(io_error("unable to publish blob atomically"));
            }
        }
    }
    const auto verified = read(digest);
    if (!verified) {
        return core::Result<BlobRef>::failure(verified.error().with_context(
            "published blob verification"));
    }
    return core::Result<BlobRef>::success(BlobRef{digest, verified.value().size(), std::move(media_type)});
}

core::Result<std::vector<std::uint8_t>> BlobStore::read(const Sha256Digest& digest) const {
    const std::filesystem::path path = path_for(digest);
    std::error_code error;
    const bool regular_file = std::filesystem::is_regular_file(path, error);
    if (error) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            io_error("unable to inspect blob path"));
    }
    if (!regular_file) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            core::Diagnostic(core::ErrorCode::not_found, "blob does not exist"));
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            io_error("unable to inspect blob size"));
    }
    if (size > limits_.max_blob_bytes || size > std::numeric_limits<std::size_t>::max()) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            validation("blob size is invalid or exceeds the configured limit"));
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error("unable to open blob"));
    }
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream && !stream.eof()) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error("unable to read blob"));
    }
    if (sha256(bytes) != digest) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            validation("blob content does not match its digest"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<void> BlobStore::verify(const Sha256Digest& digest) const {
    auto result = read(digest);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    return core::Result<void>::success();
}

} // namespace carto::assets
