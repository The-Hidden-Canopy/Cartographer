#include <carto/production/lookdev_draw_package.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string>
#include <utility>

namespace carto::production {
namespace {

constexpr std::array<std::uint8_t, 12U> kMagic{
    'C', 'A', 'R', 'T', 'O', '_', 'L', 'O', 'O', 'K', '1', 0U};
constexpr std::uint64_t kMaxMeshResidentBytes =
    static_cast<std::uint64_t>(kMaxRenderMeshArtifactVertices) *
        sizeof(PbrUploadVertex) +
    static_cast<std::uint64_t>(kMaxRenderMeshArtifactIndices) *
        sizeof(std::uint32_t);

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
    if (value.empty() || value.size() > 128U || value.front() == '/' ||
        value.back() == '/' || value.find("..") != std::string_view::npos) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
            byte == '.' || byte == '/';
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

CompiledMaterialArtifact material_from_draw(const LookdevSubmeshDraw& draw) {
    CompiledMaterialArtifact material;
    material.source_identity = draw.material_source_identity;
    material.material_id = draw.material_id;
    material.source_revision = draw.material_source_revision;
    material.source_digest = draw.material_source_digest;
    material.feature_mask = draw.feature_mask;
    material.gpu_constants = draw.gpu_constants;
    material.artifact_digest = draw.material_artifact_digest;
    return material;
}

std::vector<std::uint8_t> canonical_payload(const LookdevDrawPackage& package) {
    std::vector<std::uint8_t> output;
    output.reserve(512U + package.draws.size() * 320U);
    output.insert(output.end(), kMagic.begin(), kMagic.end());
    append_u32(output, package.package_version);
    append_text(output, package.algorithm);
    append_u64(output, package.mesh_source_revision.value());
    append_u64(output, package.mesh_revision.value());
    output.insert(
        output.end(), package.mesh_source_digest.bytes.begin(),
        package.mesh_source_digest.bytes.end());
    output.insert(
        output.end(), package.mesh_artifact_digest.bytes.begin(),
        package.mesh_artifact_digest.bytes.end());
    append_u64(output, package.mesh_resident_bytes);
    append_u32(output, static_cast<std::uint32_t>(package.draws.size()));
    for (const LookdevSubmeshDraw& draw : package.draws) {
        append_u32(output, draw.material_slot.has_value() ? 1U : 0U);
        append_u32(output, draw.material_slot.value_or(0U));
        append_u32(output, draw.first_index);
        append_u32(output, draw.index_count);
        append_text(output, draw.material_source_identity);
        append_u64(output, draw.material_id);
        append_u64(output, draw.material_source_revision.value());
        output.insert(
            output.end(), draw.material_source_digest.bytes.begin(),
            draw.material_source_digest.bytes.end());
        append_u32(output, draw.feature_mask);
        append_float_array(output, draw.gpu_constants.base_color_factor);
        append_float_array(output, draw.gpu_constants.emissive_radiance_and_f0);
        append_float_array(output, draw.gpu_constants.surface_factors);
        append_float_array(output, draw.gpu_constants.alpha_factors);
        append_float_array(output, draw.gpu_constants.clearcoat_factors);
        append_u32_array(output, draw.gpu_constants.texture_asset_ids);
        output.insert(
            output.end(), draw.material_artifact_digest.bytes.begin(),
            draw.material_artifact_digest.bytes.end());
    }
    return output;
}

core::Result<void> validate_structure(const LookdevDrawPackage& package) {
    if (package.package_version != kLookdevDrawPackageVersion ||
        package.algorithm != kLookdevDrawPackageAlgorithm ||
        package.mesh_source_revision.exhausted() || package.mesh_revision.exhausted() ||
        package.mesh_source_digest.is_zero() || package.mesh_artifact_digest.is_zero() ||
        package.mesh_resident_bytes == 0U ||
        package.mesh_resident_bytes > kMaxMeshResidentBytes || package.draws.empty() ||
        package.draws.size() > kMaxRenderMeshArtifactSubmeshes) {
        return core::Result<void>::failure(validation(
            "lookdev draw package identity, mesh evidence, or draw count is invalid"));
    }

    const bool has_slots = package.draws.front().material_slot.has_value();
    std::optional<std::uint32_t> previous_slot;
    std::uint64_t next_index = 0U;
    for (const LookdevSubmeshDraw& draw : package.draws) {
        if (draw.material_slot.has_value() != has_slots ||
            draw.first_index != next_index || draw.index_count == 0U ||
            draw.index_count % 3U != 0U || !safe_identity(draw.material_source_identity)) {
            return core::Result<void>::failure(validation(
                "lookdev draw ranges, slots, or material identity are non-canonical"));
        }
        if (next_index > std::numeric_limits<std::uint32_t>::max() - draw.index_count) {
            return core::Result<void>::failure(validation(
                "lookdev draw index ranges overflow their bounded address space"));
        }
        next_index += draw.index_count;
        if (has_slots) {
            if (previous_slot.has_value() &&
                draw.material_slot.value() <= previous_slot.value()) {
                return core::Result<void>::failure(validation(
                    "lookdev material slots must be unique and sorted"));
            }
            previous_slot = draw.material_slot;
        }
        if (auto result = material_from_draw(draw).validate(); !result) {
            return core::Result<void>::failure(
                result.error().with_context("lookdev submesh material"));
        }
    }
    if (!has_slots && package.draws.size() != 1U) {
        return core::Result<void>::failure(validation(
            "an un-slotted lookdev mesh must contain exactly one draw"));
    }
    if (canonical_payload(package).size() >
        kMaxLookdevDrawPackageBytes - package.package_digest.bytes.size()) {
        return core::Result<void>::failure(validation(
            "lookdev draw package exceeds its serialized byte bound"));
    }
    return core::Result<void>::success();
}

bool same_slot(
    const std::optional<std::uint32_t>& left,
    const std::optional<std::uint32_t>& right) {
    return left.has_value() == right.has_value() &&
        (!left.has_value() || left.value() == right.value());
}

} // namespace

core::Result<void> LookdevDrawPackage::validate() const {
    if (auto result = validate_structure(*this); !result) return result;
    if (package_digest.is_zero() ||
        package_digest != assets::sha256(canonical_payload(*this))) {
        return core::Result<void>::failure(validation(
            "lookdev draw package digest does not match its canonical payload"));
    }
    return core::Result<void>::success();
}

core::Result<void> LookdevDrawPackage::refresh_digest() {
    const auto previous = package_digest;
    package_digest = {};
    if (auto result = validate_structure(*this); !result) {
        package_digest = previous;
        return result;
    }
    package_digest = assets::sha256(canonical_payload(*this));
    return core::Result<void>::success();
}

std::uint64_t LookdevDrawPackage::resident_bytes() const noexcept {
    return mesh_resident_bytes +
        static_cast<std::uint64_t>(draws.size()) *
            sizeof(render::GpuMaterialConstants);
}

core::Result<LookdevDrawPackage> compile_lookdev_draw_package(
    const RenderMeshArtifact& mesh,
    std::span<const LookdevMaterialBinding> bindings) {
    if (auto result = mesh.validate(); !result) {
        return core::Result<LookdevDrawPackage>::failure(result.error());
    }
    const auto upload = pack_pbr_mesh_upload(mesh);
    if (!upload) return core::Result<LookdevDrawPackage>::failure(upload.error());
    if (bindings.size() != mesh.submeshes.size() || bindings.empty()) {
        return core::Result<LookdevDrawPackage>::failure(invalid(
            "lookdev compilation requires exactly one material binding per submesh"));
    }

    std::vector<const LookdevMaterialBinding*> ordered;
    ordered.reserve(bindings.size());
    for (const LookdevMaterialBinding& binding : bindings) {
        if (auto result = binding.material.validate(); !result) {
            return core::Result<LookdevDrawPackage>::failure(
                result.error().with_context("lookdev material binding"));
        }
        ordered.push_back(&binding);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        if (left->material_slot.has_value() != right->material_slot.has_value()) {
            return !left->material_slot.has_value();
        }
        return left->material_slot.value_or(0U) < right->material_slot.value_or(0U);
    });
    for (std::size_t index = 1U; index < ordered.size(); ++index) {
        if (same_slot(ordered[index - 1U]->material_slot, ordered[index]->material_slot)) {
            return core::Result<LookdevDrawPackage>::failure(invalid(
                "lookdev compilation rejects duplicate material-slot bindings"));
        }
    }

    const auto mesh_bytes = mesh.serialize();
    if (!mesh_bytes) return core::Result<LookdevDrawPackage>::failure(mesh_bytes.error());
    LookdevDrawPackage package;
    package.mesh_source_revision = mesh.source_revision;
    package.mesh_revision = mesh.mesh_revision;
    package.mesh_source_digest = mesh.source_digest;
    package.mesh_artifact_digest = assets::sha256(mesh_bytes.value());
    package.mesh_resident_bytes = upload.value().resident_bytes();
    package.draws.reserve(mesh.submeshes.size());

    for (const RenderMeshSubmesh& submesh : mesh.submeshes) {
        const auto binding = std::find_if(
            ordered.begin(), ordered.end(), [&submesh](const auto* candidate) {
                return same_slot(candidate->material_slot, submesh.material_slot);
            });
        if (binding == ordered.end()) {
            return core::Result<LookdevDrawPackage>::failure(invalid(
                "lookdev compilation is missing a render-mesh material slot"));
        }
        const CompiledMaterialArtifact& material = (*binding)->material;
        package.draws.push_back(LookdevSubmeshDraw{
            submesh.material_slot,
            submesh.first_index,
            submesh.index_count,
            material.source_identity,
            material.material_id,
            material.source_revision,
            material.source_digest,
            material.feature_mask,
            material.gpu_constants,
            material.artifact_digest,
        });
    }
    if (auto result = package.refresh_digest(); !result) {
        return core::Result<LookdevDrawPackage>::failure(result.error());
    }
    return core::Result<LookdevDrawPackage>::success(std::move(package));
}

core::Result<void> validate_lookdev_draw_package_sources(
    const LookdevDrawPackage& package,
    const RenderMeshArtifact& current_mesh,
    std::span<const LookdevMaterialBinding> current_bindings) {
    if (auto result = package.validate(); !result) return result;
    const auto current = compile_lookdev_draw_package(current_mesh, current_bindings);
    if (!current) {
        return core::Result<void>::failure(
            current.error().with_context("current lookdev draw sources"));
    }
    if (current.value().package_digest != package.package_digest) {
        return core::Result<void>::failure(stale(
            "lookdev draw package no longer matches its mesh or material products"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> serialize_lookdev_draw_package(
    const LookdevDrawPackage& package) {
    if (auto result = package.validate(); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    auto bytes = canonical_payload(package);
    if (bytes.size() > kMaxLookdevDrawPackageBytes - package.package_digest.bytes.size()) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "lookdev draw package exceeds its serialized byte bound"));
    }
    bytes.insert(
        bytes.end(), package.package_digest.bytes.begin(), package.package_digest.bytes.end());
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<LookdevDrawPackage> deserialize_lookdev_draw_package(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kMagic.size() + 4U + 32U ||
        bytes.size() > kMaxLookdevDrawPackageBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return core::Result<LookdevDrawPackage>::failure(invalid(
            "lookdev draw package framing is invalid"));
    }
    std::size_t cursor = kMagic.size();
    LookdevDrawPackage package;
    std::uint64_t mesh_source_revision = 0U;
    std::uint64_t mesh_revision = 0U;
    std::uint32_t draw_count = 0U;
    if (!read_u32(bytes, cursor, package.package_version) ||
        !read_text(bytes, cursor, package.algorithm, 64U) ||
        !read_u64(bytes, cursor, mesh_source_revision) ||
        !read_u64(bytes, cursor, mesh_revision) || cursor > bytes.size() ||
        bytes.size() - cursor < package.mesh_source_digest.bytes.size()) {
        return core::Result<LookdevDrawPackage>::failure(invalid(
            "lookdev draw package mesh header is truncated or unbounded"));
    }
    package.mesh_source_revision = core::Revision{mesh_source_revision};
    package.mesh_revision = core::Revision{mesh_revision};
    std::copy_n(
        bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
        package.mesh_source_digest.bytes.size(), package.mesh_source_digest.bytes.begin());
    cursor += package.mesh_source_digest.bytes.size();
    if (cursor > bytes.size() ||
        bytes.size() - cursor < package.mesh_artifact_digest.bytes.size()) {
        return core::Result<LookdevDrawPackage>::failure(invalid(
            "lookdev draw package mesh artifact digest is truncated"));
    }
    std::copy_n(
        bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
        package.mesh_artifact_digest.bytes.size(), package.mesh_artifact_digest.bytes.begin());
    cursor += package.mesh_artifact_digest.bytes.size();
    if (!read_u64(bytes, cursor, package.mesh_resident_bytes) ||
        !read_u32(bytes, cursor, draw_count) || draw_count == 0U ||
        draw_count > kMaxRenderMeshArtifactSubmeshes) {
        return core::Result<LookdevDrawPackage>::failure(invalid(
            "lookdev draw package draw header is invalid"));
    }
    package.draws.reserve(draw_count);
    for (std::uint32_t index = 0U; index < draw_count; ++index) {
        LookdevSubmeshDraw draw;
        std::uint32_t has_slot = 0U;
        std::uint32_t slot = 0U;
        std::uint64_t material_revision = 0U;
        if (!read_u32(bytes, cursor, has_slot) || has_slot > 1U ||
            !read_u32(bytes, cursor, slot) || (!has_slot && slot != 0U) ||
            !read_u32(bytes, cursor, draw.first_index) ||
            !read_u32(bytes, cursor, draw.index_count) ||
            !read_text(bytes, cursor, draw.material_source_identity, 128U) ||
            !read_u64(bytes, cursor, draw.material_id) ||
            !read_u64(bytes, cursor, material_revision) || cursor > bytes.size() ||
            bytes.size() - cursor < draw.material_source_digest.bytes.size()) {
            return core::Result<LookdevDrawPackage>::failure(invalid(
                "lookdev draw package contains a truncated draw identity"));
        }
        if (has_slot) draw.material_slot = slot;
        draw.material_source_revision = core::Revision{material_revision};
        std::copy_n(
            bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
            draw.material_source_digest.bytes.size(), draw.material_source_digest.bytes.begin());
        cursor += draw.material_source_digest.bytes.size();
        if (!read_u32(bytes, cursor, draw.feature_mask) ||
            !read_float_array(bytes, cursor, draw.gpu_constants.base_color_factor) ||
            !read_float_array(bytes, cursor, draw.gpu_constants.emissive_radiance_and_f0) ||
            !read_float_array(bytes, cursor, draw.gpu_constants.surface_factors) ||
            !read_float_array(bytes, cursor, draw.gpu_constants.alpha_factors) ||
            !read_float_array(bytes, cursor, draw.gpu_constants.clearcoat_factors) ||
            !read_u32_array(bytes, cursor, draw.gpu_constants.texture_asset_ids) ||
            cursor > bytes.size() ||
            bytes.size() - cursor < draw.material_artifact_digest.bytes.size()) {
            return core::Result<LookdevDrawPackage>::failure(invalid(
                "lookdev draw package contains truncated material constants"));
        }
        std::copy_n(
            bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
            draw.material_artifact_digest.bytes.size(),
            draw.material_artifact_digest.bytes.begin());
        cursor += draw.material_artifact_digest.bytes.size();
        package.draws.push_back(std::move(draw));
    }
    if (cursor > bytes.size() ||
        bytes.size() - cursor != package.package_digest.bytes.size()) {
        return core::Result<LookdevDrawPackage>::failure(invalid(
            "lookdev draw package has a truncated digest or trailing data"));
    }
    std::copy_n(
        bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
        package.package_digest.bytes.size(), package.package_digest.bytes.begin());
    if (auto result = package.validate(); !result) {
        return core::Result<LookdevDrawPackage>::failure(result.error());
    }
    return core::Result<LookdevDrawPackage>::success(std::move(package));
}

} // namespace carto::production
