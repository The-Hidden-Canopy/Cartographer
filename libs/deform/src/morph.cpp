#include <carto/deform/morph.hpp>

#include <carto/deform/receipt.hpp>

#include <carto/core/diagnostic.hpp>

#include <cmath>
#include <cstring>

namespace carto::deform {

namespace {

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void append_f64(std::vector<std::uint8_t>& bytes, double value) {
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(double));
}

class BlobReader {
public:
    BlobReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool read_u32(std::uint32_t& value) {
        if (remaining() < 4U) {
            return false;
        }
        value = 0U;
        for (unsigned int index = 0U; index < 4U; ++index) {
            value |= static_cast<std::uint32_t>(bytes_[offset_ + index]) << (index * 8U);
        }
        offset_ += 4U;
        return true;
    }

    [[nodiscard]] bool read_u64(std::uint64_t& value) {
        if (remaining() < 8U) {
            return false;
        }
        value = 0U;
        for (unsigned int index = 0U; index < 8U; ++index) {
            value |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << (index * 8U);
        }
        offset_ += 8U;
        return true;
    }

    [[nodiscard]] bool read_f64(double& value) {
        if (remaining() < sizeof(double)) {
            return false;
        }
        std::uint8_t raw[sizeof(double)];
        std::memcpy(raw, bytes_.data() + offset_, sizeof(double));
        std::memcpy(&value, raw, sizeof(double));
        offset_ += sizeof(double);
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0U;
};

} // namespace

core::Result<std::vector<std::uint8_t>> encode_delta_blob(
    const std::vector<MorphDelta>& deltas) {
    if (deltas.size() > character::MorphTarget::kMaxDisplacedPoints) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "delta population exceeds the morph target envelope"));
    }
    const std::uint64_t total_bytes =
        kMorphDeltaBlobHeaderBytes +
        kMorphDeltaBlobRecordBytes *
            static_cast<std::uint64_t>(deltas.size());
    if (total_bytes > character::MorphTarget::kMaxDeltaBlobBytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "delta blob exceeds the byte envelope"));
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(total_bytes));
    append_u32(bytes, kMorphDeltaBlobVersion);
    append_u32(bytes, 0U);
    append_u64(bytes, static_cast<std::uint64_t>(deltas.size()));
    std::uint32_t previous_index = 0U;
    bool have_previous = false;
    for (const MorphDelta& delta : deltas) {
        if (!delta.delta.finite()) {
            return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "delta blob rejects non-finite deltas"));
        }
        if (have_previous && delta.vertex_index <= previous_index) {
            return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "delta blob indexes must be strictly increasing"));
        }
        previous_index = delta.vertex_index;
        have_previous = true;
        append_u32(bytes, delta.vertex_index);
        append_f64(bytes, delta.delta.x);
        append_f64(bytes, delta.delta.y);
        append_f64(bytes, delta.delta.z);
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<std::vector<MorphDelta>> decode_delta_blob(
    std::span<const std::uint8_t> bytes,
    std::uint64_t expected_count) {
    if (expected_count > character::MorphTarget::kMaxDisplacedPoints) {
        return core::Result<std::vector<MorphDelta>>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "declared delta population exceeds the morph target envelope"));
    }
    const std::uint64_t expected_bytes =
        kMorphDeltaBlobHeaderBytes + kMorphDeltaBlobRecordBytes * expected_count;
    if (expected_bytes > character::MorphTarget::kMaxDeltaBlobBytes ||
        expected_bytes != static_cast<std::uint64_t>(bytes.size())) {
        return core::Result<std::vector<MorphDelta>>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "delta blob size does not match the declared envelope"));
    }
    BlobReader reader(bytes);
    std::uint32_t version = 0U;
    std::uint32_t reserved = 0U;
    std::uint64_t count = 0U;
    if (!reader.read_u32(version) || !reader.read_u32(reserved) ||
        !reader.read_u64(count) || version != kMorphDeltaBlobVersion ||
        reserved != 0U || count != expected_count) {
        return core::Result<std::vector<MorphDelta>>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "delta blob header is malformed or versioned unexpectedly"));
    }
    std::vector<MorphDelta> deltas;
    deltas.reserve(static_cast<std::size_t>(count));
    std::uint32_t previous_index = 0U;
    bool have_previous = false;
    for (std::uint64_t index = 0U; index < count; ++index) {
        MorphDelta delta;
        if (!reader.read_u32(delta.vertex_index) || !reader.read_f64(delta.delta.x) ||
            !reader.read_f64(delta.delta.y) || !reader.read_f64(delta.delta.z)) {
            return core::Result<std::vector<MorphDelta>>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "delta blob is truncated inside a record"));
        }
        if (!delta.delta.finite()) {
            return core::Result<std::vector<MorphDelta>>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "delta blob decodes to a non-finite delta"));
        }
        if (have_previous && delta.vertex_index <= previous_index) {
            return core::Result<std::vector<MorphDelta>>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "delta blob indexes are not strictly increasing"));
        }
        previous_index = delta.vertex_index;
        have_previous = true;
        deltas.push_back(delta);
    }
    return core::Result<std::vector<MorphDelta>>::success(std::move(deltas));
}

core::Result<MorphApplicationResult> apply_morph_target(
    const MorphApplicationInput& input) {
    if (input.target == nullptr) {
        return core::Result<MorphApplicationResult>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument, "morph application requires a target"));
    }
    const character::MorphTarget& target = *input.target;
    const core::Result<void> target_valid = target.validate();
    if (!target_valid) {
        return core::Result<MorphApplicationResult>::failure(target_valid.error());
    }
    if (!std::isfinite(input.weight) || input.weight < 0.0 || input.weight > 1.0) {
        return core::Result<MorphApplicationResult>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "morph weight must be finite and within [0, 1]"));
    }
    if (input.mesh_content_digest != target.source_mesh_digest) {
        return core::Result<MorphApplicationResult>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "morph target source mesh digest does not match the attested mesh"));
    }
    if (input.topology_correspondence_digest != target.topology_correspondence_digest) {
        return core::Result<MorphApplicationResult>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "morph target correspondence digest does not match the attested "
            "correspondence"));
    }
    if (static_cast<std::uint64_t>(input.delta_blob.size()) != target.delta_blob_bytes) {
        return core::Result<MorphApplicationResult>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "delta blob byte count does not match the morph target record"));
    }
    if (assets::sha256(input.delta_blob) != target.delta_blob_digest) {
        return core::Result<MorphApplicationResult>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "delta blob digest does not match the morph target record"));
    }
    if (!std::isfinite(target.precision_bound) || target.precision_bound < 0.0) {
        return core::Result<MorphApplicationResult>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "morph target declares a non-finite or negative precision bound"));
    }
    const core::Result<std::vector<MorphDelta>> deltas =
        decode_delta_blob(input.delta_blob, target.displaced_point_count);
    if (!deltas) {
        return core::Result<MorphApplicationResult>::failure(deltas.error());
    }
    MorphApplicationResult result;
    result.positions.assign(input.bind_positions.begin(), input.bind_positions.end());
    result.displaced_points = target.displaced_point_count;
    result.tangents_require_recompute =
        target.tangent_out == character::TangentPolicy::recompute;
    for (const MorphDelta& delta : deltas.value()) {
        if (delta.vertex_index >= result.positions.size()) {
            return core::Result<MorphApplicationResult>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "morph delta indexes a point outside the position stream"));
        }
        if (std::abs(delta.delta.x) > target.precision_bound ||
            std::abs(delta.delta.y) > target.precision_bound ||
            std::abs(delta.delta.z) > target.precision_bound) {
            return core::Result<MorphApplicationResult>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "decoded delta exceeds the declared precision bound"));
        }
        core::Vec3d& position = result.positions[delta.vertex_index];
        position = position + delta.delta * input.weight;
    }
    result.applied_delta_digest = positions_digest(result.positions);
    return core::Result<MorphApplicationResult>::success(std::move(result));
}

assets::Sha256Digest morph_policy_digest(const character::MorphTarget& target) {
    std::vector<std::uint8_t> bytes;
    const auto& mesh = target.source_mesh_digest.bytes;
    bytes.insert(bytes.end(), mesh.begin(), mesh.end());
    const auto& correspondence = target.topology_correspondence_digest.bytes;
    bytes.insert(bytes.end(), correspondence.begin(), correspondence.end());
    const auto& blob = target.delta_blob_digest.bytes;
    bytes.insert(bytes.end(), blob.begin(), blob.end());
    append_u64(bytes, target.delta_blob_bytes);
    append_u64(bytes, target.displaced_point_count);
    append_u64(bytes, target.source_mesh_asset);
    append_f64(bytes, target.precision_bound);
    bytes.push_back(static_cast<std::uint8_t>(target.tangent_in));
    bytes.push_back(static_cast<std::uint8_t>(target.tangent_out));
    return assets::sha256(bytes);
}

} // namespace carto::deform
