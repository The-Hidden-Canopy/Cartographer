#include <carto/web_geometry/page_payload.hpp>

#include <bit>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace carto::web_geometry {

namespace {

constexpr std::size_t kHeaderBytes = 12U;
constexpr std::size_t kTriangleBytes = 4U + 8U + 3U * 6U * sizeof(double);
constexpr std::size_t kMaxPageBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kMaxClusters = 1'000'000U;
constexpr std::uint32_t kMaxTriangles = 10'000'000U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

class Reader final {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool read_u32(std::uint32_t& value) noexcept {
        if (remaining() < sizeof(value)) return false;
        value = 0U;
        for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
            value |= static_cast<std::uint32_t>(bytes_[offset_ + shift / 8U]) << shift;
        }
        offset_ += sizeof(value);
        return true;
    }

    [[nodiscard]] bool read_u64(std::uint64_t& value) noexcept {
        if (remaining() < sizeof(value)) return false;
        value = 0U;
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
            value |= static_cast<std::uint64_t>(bytes_[offset_ + shift / 8U]) << shift;
        }
        offset_ += sizeof(value);
        return true;
    }

    [[nodiscard]] bool read_f64(double& value) noexcept {
        std::uint64_t bits = 0U;
        if (!read_u64(bits)) return false;
        value = std::bit_cast<double>(bits);
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }

    [[nodiscard]] bool read_magic() noexcept {
        if (remaining() < kRuntimePagePayloadMagic.size() ||
            bytes_[offset_] != kRuntimePagePayloadMagic[0] ||
            bytes_[offset_ + 1U] != kRuntimePagePayloadMagic[1] ||
            bytes_[offset_ + 2U] != kRuntimePagePayloadMagic[2] ||
            bytes_[offset_ + 3U] != kRuntimePagePayloadMagic[3]) {
            return false;
        }
        offset_ += kRuntimePagePayloadMagic.size();
        return true;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0U;
};

} // namespace

core::Result<RuntimePagePayload> decode_runtime_page_payload(
    std::span<const std::uint8_t> payload) {
    if (payload.size() < kHeaderBytes || payload.size() > kMaxPageBytes) {
        return core::Result<RuntimePagePayload>::failure(validation(
            "web geometry runtime page payload is outside the bounded size"));
    }
    Reader reader(payload);
    if (!reader.read_magic()) {
        return core::Result<RuntimePagePayload>::failure(
            invalid("web geometry runtime page payload magic is unsupported"));
    }
    std::uint32_t version = 0U;
    std::uint32_t cluster_count = 0U;
    if (!reader.read_u32(version) || version != kRuntimePagePayloadVersion ||
        !reader.read_u32(cluster_count) || cluster_count == 0U ||
        cluster_count > kMaxClusters) {
        return core::Result<RuntimePagePayload>::failure(
            invalid("web geometry runtime page payload header is invalid"));
    }

    RuntimePagePayload result;
    result.clusters.reserve(cluster_count);
    std::set<ClusterId> cluster_ids;
    std::size_t triangle_total = 0U;
    for (std::uint32_t cluster_index = 0U; cluster_index < cluster_count; ++cluster_index) {
        RuntimePagePayloadCluster cluster;
        std::uint32_t triangle_count = 0U;
        if (!reader.read_u32(cluster.cluster) || !reader.read_u32(triangle_count) ||
            !cluster_ids.insert(cluster.cluster).second || triangle_count == 0U ||
            triangle_count > kMaxTriangles ||
            triangle_total > kMaxTriangles - triangle_count ||
            static_cast<std::size_t>(triangle_count) > reader.remaining() / kTriangleBytes) {
            return core::Result<RuntimePagePayload>::failure(validation(
                "web geometry runtime page cluster record is invalid"));
        }
        triangle_total += triangle_count;
        cluster.triangles.resize(triangle_count);
        for (auto& triangle : cluster.triangles) {
            std::uint64_t face = 0U;
            if (!reader.read_u32(triangle.source_triangle) || !reader.read_u64(face) ||
                face == 0U) {
                return core::Result<RuntimePagePayload>::failure(invalid(
                    "web geometry runtime page triangle identity is truncated"));
            }
            triangle.source_face = geometry::FaceId{face};
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                auto& position = triangle.positions[corner];
                auto& normal = triangle.normals[corner];
                if (!reader.read_f64(position.x) || !reader.read_f64(position.y) ||
                    !reader.read_f64(position.z) || !reader.read_f64(normal.x) ||
                    !reader.read_f64(normal.y) || !reader.read_f64(normal.z) ||
                    !position.finite() || !normal.finite()) {
                    return core::Result<RuntimePagePayload>::failure(invalid(
                        "web geometry runtime page triangle geometry is invalid"));
                }
            }
        }
        result.clusters.push_back(std::move(cluster));
    }
    if (reader.remaining() != 0U) {
        return core::Result<RuntimePagePayload>::failure(validation(
            "web geometry runtime page payload contains trailing data"));
    }
    return core::Result<RuntimePagePayload>::success(std::move(result));
}

} // namespace carto::web_geometry
