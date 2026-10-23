#include <array>
#include <algorithm>
#include <cstdint>
#include <span>

namespace carto::plugin_package {

namespace {

constexpr std::array<std::uint64_t, 80> kRoundConstants{
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

constexpr std::uint64_t rotate_right(std::uint64_t value, unsigned amount) noexcept {
    return (value >> amount) | (value << (64U - amount));
}

constexpr std::uint64_t shr(std::uint64_t value, unsigned amount) noexcept {
    return value >> amount;
}

class Sha512State final {
public:
    Sha512State()
        : state_{0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                 0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL} {}

    void update(std::span<const std::uint8_t> bytes) {
        bit_count_ += static_cast<std::uint64_t>(bytes.size()) * 8ULL;
        for (const auto byte : bytes) {
            block_[block_size_++] = byte;
            if (block_size_ == block_.size()) {
                transform();
                block_size_ = 0U;
            }
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 64> finish() {
        block_[block_size_++] = 0x80U;
        if (block_size_ > 112U) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.end(), 0U);
            transform();
            block_size_ = 0U;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.begin() + 112, 0U);
        for (unsigned index = 0U; index < 8U; ++index) {
            block_[127U - index] = static_cast<std::uint8_t>(bit_count_ >> (index * 8U));
        }
        transform();

        std::array<std::uint8_t, 64> digest{};
        for (unsigned index = 0U; index < state_.size(); ++index) {
            digest[index * 8U] = static_cast<std::uint8_t>(state_[index] >> 56U);
            digest[index * 8U + 1U] = static_cast<std::uint8_t>(state_[index] >> 48U);
            digest[index * 8U + 2U] = static_cast<std::uint8_t>(state_[index] >> 40U);
            digest[index * 8U + 3U] = static_cast<std::uint8_t>(state_[index] >> 32U);
            digest[index * 8U + 4U] = static_cast<std::uint8_t>(state_[index] >> 24U);
            digest[index * 8U + 5U] = static_cast<std::uint8_t>(state_[index] >> 16U);
            digest[index * 8U + 6U] = static_cast<std::uint8_t>(state_[index] >> 8U);
            digest[index * 8U + 7U] = static_cast<std::uint8_t>(state_[index]);
        }
        return digest;
    }

private:
    void transform() {
        std::array<std::uint64_t, 80> schedule{};
        for (unsigned index = 0U; index < 16U; ++index) {
            const unsigned offset = index * 8U;
            schedule[index] =
                (static_cast<std::uint64_t>(block_[offset]) << 56U) |
                (static_cast<std::uint64_t>(block_[offset + 1U]) << 48U) |
                (static_cast<std::uint64_t>(block_[offset + 2U]) << 40U) |
                (static_cast<std::uint64_t>(block_[offset + 3U]) << 32U) |
                (static_cast<std::uint64_t>(block_[offset + 4U]) << 24U) |
                (static_cast<std::uint64_t>(block_[offset + 5U]) << 16U) |
                (static_cast<std::uint64_t>(block_[offset + 6U]) << 8U) |
                static_cast<std::uint64_t>(block_[offset + 7U]);
        }
        for (unsigned index = 16U; index < schedule.size(); ++index) {
            const std::uint64_t s0 = rotate_right(schedule[index - 15U], 1U) ^
                rotate_right(schedule[index - 15U], 8U) ^ shr(schedule[index - 15U], 7U);
            const std::uint64_t s1 = rotate_right(schedule[index - 2U], 19U) ^
                rotate_right(schedule[index - 2U], 61U) ^ shr(schedule[index - 2U], 6U);
            schedule[index] = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
        }

        std::uint64_t a = state_[0];
        std::uint64_t b = state_[1];
        std::uint64_t c = state_[2];
        std::uint64_t d = state_[3];
        std::uint64_t e = state_[4];
        std::uint64_t f = state_[5];
        std::uint64_t g = state_[6];
        std::uint64_t h = state_[7];
        for (unsigned index = 0U; index < schedule.size(); ++index) {
            const std::uint64_t s1 = rotate_right(e, 14U) ^ rotate_right(e, 18U) ^ rotate_right(e, 41U);
            const std::uint64_t ch = (e & f) ^ ((~e) & g);
            const std::uint64_t temp1 = h + s1 + ch + kRoundConstants[index] + schedule[index];
            const std::uint64_t s0 = rotate_right(a, 28U) ^ rotate_right(a, 34U) ^ rotate_right(a, 39U);
            const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint64_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
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

    std::array<std::uint64_t, 8> state_{};
    std::array<std::uint8_t, 128> block_{};
    std::size_t block_size_ = 0U;
    std::uint64_t bit_count_ = 0ULL;
};

} // namespace

std::array<std::uint8_t, 64> sha512(std::span<const std::uint8_t> bytes) {
    Sha512State state;
    state.update(bytes);
    return state.finish();
}

} // namespace carto::plugin_package
