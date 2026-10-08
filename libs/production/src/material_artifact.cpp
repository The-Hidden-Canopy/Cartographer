#include <carto/production/material_artifact.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string>
#include <utility>

namespace carto::production {
namespace {

constexpr std::array<std::uint8_t, 12U> kMagic{
    'C', 'A', 'R', 'T', 'O', '_', 'M', 'A', 'T', 'V', '2', 0U};
constexpr std::string_view kSourceDigestDomain = "carto.material-source.v2";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_identity(std::string_view value) {
    if (value.empty() || value.size() > 128U ||
        value.front() == '/' || value.back() == '/' ||
        value.find("..") != std::string_view::npos) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
            byte == '.' || byte == '/';
    });
}

bool safe_name(std::string_view value) {
    return !value.empty() && value.size() <= 256U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU;
        });
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_f32(std::vector<std::uint8_t>& output, float value) {
    if (value == 0.0F) value = 0.0F;
    append_u32(output, std::bit_cast<std::uint32_t>(value));
}

void append_f64(std::vector<std::uint8_t>& output, double value) {
    if (value == 0.0) value = 0.0;
    append_u64(output, std::bit_cast<std::uint64_t>(value));
}

void append_text(std::vector<std::uint8_t>& output, std::string_view value) {
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

bool read_u32(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    std::uint32_t& value) {
    if (cursor > bytes.size() || bytes.size() - cursor < sizeof(value)) return false;
    value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(bytes[cursor++]) << shift;
    }
    return true;
}

bool read_u64(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    std::uint64_t& value) {
    if (cursor > bytes.size() || bytes.size() - cursor < sizeof(value)) return false;
    value = 0U;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(bytes[cursor++]) << shift;
    }
    return true;
}

bool read_f32(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    float& value) {
    std::uint32_t bits = 0U;
    if (!read_u32(bytes, cursor, bits)) return false;
    value = std::bit_cast<float>(bits);
    return true;
}

bool read_text(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    std::string& value,
    std::uint32_t maximum) {
    std::uint32_t size = 0U;
    if (!read_u32(bytes, cursor, size) || size == 0U || size > maximum ||
        cursor > bytes.size() || bytes.size() - cursor < size) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(bytes.data() + cursor), size);
    cursor += size;
    return true;
}

void append_texture_ref(
    std::vector<std::uint8_t>& output,
    const render::TextureRef& binding) {
    append_u64(output, binding.asset);
    append_u64(output, binding.uv_set);
    append_u64(output, binding.sampler);
    append_f64(output, binding.transform.offset.x);
    append_f64(output, binding.transform.offset.y);
    append_f64(output, binding.transform.scale.x);
    append_f64(output, binding.transform.scale.y);
    append_f64(output, binding.transform.rotation);
    append_f64(output, binding.transform.pivot.x);
    append_f64(output, binding.transform.pivot.y);
    for (const render::TextureChannel channel : binding.channels.rgba) {
        append_u32(output, static_cast<std::uint32_t>(channel));
    }
    append_u32(output, static_cast<std::uint32_t>(binding.interpretation));
    append_u32(output, static_cast<std::uint32_t>(binding.normal_convention));
}

std::vector<std::uint8_t> canonical_material_source(
    const render::MaterialAsset& material) {
    std::vector<std::uint8_t> output;
    output.reserve(768U);
    output.insert(output.end(), kSourceDigestDomain.begin(), kSourceDigestDomain.end());
    append_u64(output, material.id);
    append_text(output, material.name);
    const render::StandardMaterial& value = material.definition;
    for (const double component : {
             value.base_color_factor.x,
             value.base_color_factor.y,
             value.base_color_factor.z,
             value.base_color_factor.w,
             value.metallic,
             value.roughness,
             value.normal_scale,
             value.occlusion_strength,
             value.emissive_factor.x,
             value.emissive_factor.y,
             value.emissive_factor.z,
             value.emissive_strength,
             value.dielectric_f0,
             value.clearcoat,
             value.clearcoat_roughness,
             value.alpha_cutoff}) {
        append_f64(output, component);
    }
    append_u32(output, static_cast<std::uint32_t>(value.alpha_mode));
    for (const render::TextureRef* binding : {
             &value.base_color,
             &value.metallic_roughness,
             &value.normal,
             &value.occlusion,
             &value.emissive}) {
        append_texture_ref(output, *binding);
    }
    return output;
}

template <std::size_t N>
void append_float_array(
    std::vector<std::uint8_t>& output,
    const std::array<float, N>& values) {
    for (const float value : values) append_f32(output, value);
}

template <std::size_t N>
void append_u32_array(
    std::vector<std::uint8_t>& output,
    const std::array<std::uint32_t, N>& values) {
    for (const std::uint32_t value : values) append_u32(output, value);
}

template <std::size_t N>
bool read_float_array(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    std::array<float, N>& values) {
    for (float& value : values) {
        if (!read_f32(bytes, cursor, value)) return false;
    }
    return true;
}

template <std::size_t N>
bool read_u32_array(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    std::array<std::uint32_t, N>& values) {
    for (std::uint32_t& value : values) {
        if (!read_u32(bytes, cursor, value)) return false;
    }
    return true;
}

std::vector<std::uint8_t> canonical_artifact_payload(
    const CompiledMaterialArtifact& artifact) {
    std::vector<std::uint8_t> output;
    output.reserve(512U);
    output.insert(output.end(), kMagic.begin(), kMagic.end());
    append_u32(output, artifact.artifact_version);
    append_text(output, artifact.algorithm);
    append_text(output, artifact.source_identity);
    append_u64(output, artifact.material_id);
    append_u64(output, artifact.source_revision.value());
    output.insert(
        output.end(), artifact.source_digest.bytes.begin(), artifact.source_digest.bytes.end());
    append_u32(output, artifact.feature_mask);
    append_float_array(output, artifact.gpu_constants.base_color_factor);
    append_float_array(output, artifact.gpu_constants.emissive_radiance_and_f0);
    append_float_array(output, artifact.gpu_constants.surface_factors);
    append_float_array(output, artifact.gpu_constants.alpha_factors);
    append_float_array(output, artifact.gpu_constants.clearcoat_factors);
    append_u32_array(output, artifact.gpu_constants.texture_asset_ids);
    return output;
}

} // namespace

core::Result<assets::Sha256Digest> material_source_digest(
    const render::MaterialAsset& material) {
    if (auto result = material.validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    if (!safe_name(material.name)) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "material source name exceeds canonical artifact bounds"));
    }
    return core::Result<assets::Sha256Digest>::success(
        assets::sha256(canonical_material_source(material)));
}

core::Result<void> CompiledMaterialArtifact::validate() const {
    if (artifact_version != kMaterialArtifactVersion ||
        algorithm != kMaterialArtifactAlgorithm || !safe_identity(source_identity) ||
        material_id == 0U || source_digest.is_zero() ||
        (feature_mask & ~kKnownMaterialFeatureMask) != 0U ||
        (feature_mask & (kMaterialFeatureStandardPbr |
                         kMaterialFeatureDielectricF0)) !=
            (kMaterialFeatureStandardPbr | kMaterialFeatureDielectricF0)) {
        return core::Result<void>::failure(validation(
            "compiled material artifact identity or feature mask is invalid"));
    }
    if (auto result = render::validate(gpu_constants); !result) return result;
    const bool clearcoat_active = gpu_constants.clearcoat_factors[0U] > 0.0F;
    if (clearcoat_active !=
        ((feature_mask & kMaterialFeatureClearcoat) != 0U)) {
        return core::Result<void>::failure(validation(
            "compiled material feature mask disagrees with its GPU constants"));
    }
    if (artifact_digest.is_zero() ||
        artifact_digest != assets::sha256(canonical_artifact_payload(*this))) {
        return core::Result<void>::failure(validation(
            "compiled material artifact digest does not match its canonical payload"));
    }
    return core::Result<void>::success();
}

core::Result<void> CompiledMaterialArtifact::refresh_digest() {
    const auto previous = artifact_digest;
    artifact_digest = {};
    if (artifact_version != kMaterialArtifactVersion ||
        algorithm != kMaterialArtifactAlgorithm || !safe_identity(source_identity) ||
        material_id == 0U || source_digest.is_zero() ||
        (feature_mask & ~kKnownMaterialFeatureMask) != 0U) {
        artifact_digest = previous;
        return core::Result<void>::failure(validation(
            "compiled material artifact cannot digest invalid identity"));
    }
    if (auto result = render::validate(gpu_constants); !result) {
        artifact_digest = previous;
        return result;
    }
    const bool clearcoat_active = gpu_constants.clearcoat_factors[0U] > 0.0F;
    if ((feature_mask & (kMaterialFeatureStandardPbr |
                         kMaterialFeatureDielectricF0)) !=
            (kMaterialFeatureStandardPbr | kMaterialFeatureDielectricF0) ||
        clearcoat_active != ((feature_mask & kMaterialFeatureClearcoat) != 0U)) {
        artifact_digest = previous;
        return core::Result<void>::failure(validation(
            "compiled material feature mask cannot be digested"));
    }
    artifact_digest = assets::sha256(canonical_artifact_payload(*this));
    return core::Result<void>::success();
}

core::Result<CompiledMaterialArtifact> compile_material_artifact(
    std::string source_identity,
    const render::MaterialAsset& material) {
    if (!safe_identity(source_identity)) {
        return core::Result<CompiledMaterialArtifact>::failure(invalid(
            "compiled material requires a bounded stable source identity"));
    }
    const auto source_digest = material_source_digest(material);
    if (!source_digest) {
        return core::Result<CompiledMaterialArtifact>::failure(source_digest.error());
    }
    const auto gpu_constants = render::pack_material_for_gpu(material.definition);
    if (!gpu_constants) {
        return core::Result<CompiledMaterialArtifact>::failure(gpu_constants.error());
    }
    CompiledMaterialArtifact artifact;
    artifact.source_identity = std::move(source_identity);
    artifact.material_id = material.id;
    artifact.source_revision = material.revision;
    artifact.source_digest = source_digest.value();
    artifact.feature_mask =
        kMaterialFeatureStandardPbr | kMaterialFeatureDielectricF0;
    if (material.definition.clearcoat > 0.0) {
        artifact.feature_mask |= kMaterialFeatureClearcoat;
    }
    artifact.gpu_constants = gpu_constants.value();
    if (auto result = artifact.refresh_digest(); !result) {
        return core::Result<CompiledMaterialArtifact>::failure(result.error());
    }
    return core::Result<CompiledMaterialArtifact>::success(std::move(artifact));
}

core::Result<void> validate_material_artifact_source(
    const CompiledMaterialArtifact& artifact,
    std::string_view current_source_identity,
    const render::MaterialAsset& current_material) {
    if (auto result = artifact.validate(); !result) return result;
    const auto digest = material_source_digest(current_material);
    if (!digest) return core::Result<void>::failure(digest.error());
    if (current_source_identity != artifact.source_identity ||
        current_material.id != artifact.material_id ||
        current_material.revision != artifact.source_revision ||
        digest.value() != artifact.source_digest) {
        return core::Result<void>::failure(stale(
            "compiled material artifact no longer matches the authoritative source"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> serialize_material_artifact(
    const CompiledMaterialArtifact& artifact) {
    if (auto result = artifact.validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    auto bytes = canonical_artifact_payload(artifact);
    if (bytes.size() > kMaxMaterialArtifactBytes - artifact.artifact_digest.bytes.size()) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "compiled material artifact exceeds its serialized byte bound"));
    }
    bytes.insert(
        bytes.end(), artifact.artifact_digest.bytes.begin(), artifact.artifact_digest.bytes.end());
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<CompiledMaterialArtifact> deserialize_material_artifact(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kMagic.size() + 4U + 32U ||
        bytes.size() > kMaxMaterialArtifactBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return core::Result<CompiledMaterialArtifact>::failure(invalid(
            "compiled material artifact framing is invalid"));
    }
    std::size_t cursor = kMagic.size();
    CompiledMaterialArtifact artifact;
    std::uint64_t revision = 0U;
    if (!read_u32(bytes, cursor, artifact.artifact_version) ||
        !read_text(bytes, cursor, artifact.algorithm, 64U) ||
        !read_text(bytes, cursor, artifact.source_identity, 128U) ||
        !read_u64(bytes, cursor, artifact.material_id) ||
        !read_u64(bytes, cursor, revision) ||
        cursor > bytes.size() || bytes.size() - cursor < artifact.source_digest.bytes.size()) {
        return core::Result<CompiledMaterialArtifact>::failure(invalid(
            "compiled material artifact header is truncated or unbounded"));
    }
    artifact.source_revision = core::Revision{revision};
    std::copy_n(
        bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
        artifact.source_digest.bytes.size(), artifact.source_digest.bytes.begin());
    cursor += artifact.source_digest.bytes.size();
    if (!read_u32(bytes, cursor, artifact.feature_mask) ||
        !read_float_array(bytes, cursor, artifact.gpu_constants.base_color_factor) ||
        !read_float_array(bytes, cursor, artifact.gpu_constants.emissive_radiance_and_f0) ||
        !read_float_array(bytes, cursor, artifact.gpu_constants.surface_factors) ||
        !read_float_array(bytes, cursor, artifact.gpu_constants.alpha_factors) ||
        !read_float_array(bytes, cursor, artifact.gpu_constants.clearcoat_factors) ||
        !read_u32_array(bytes, cursor, artifact.gpu_constants.texture_asset_ids) ||
        cursor > bytes.size() ||
        bytes.size() - cursor != artifact.artifact_digest.bytes.size()) {
        return core::Result<CompiledMaterialArtifact>::failure(invalid(
            "compiled material artifact payload is truncated or has trailing data"));
    }
    std::copy_n(
        bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
        artifact.artifact_digest.bytes.size(), artifact.artifact_digest.bytes.begin());
    if (auto result = artifact.validate(); !result) {
        return core::Result<CompiledMaterialArtifact>::failure(result.error());
    }
    return core::Result<CompiledMaterialArtifact>::success(std::move(artifact));
}

} // namespace carto::production
