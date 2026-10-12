#include <carto/project/material_catalog.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::project {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_MATERIAL_CATALOG";
constexpr std::size_t kMaxNameBytes = 256U;
constexpr std::size_t kMaxLocatorBytes = 1'024U;
constexpr std::size_t kMaxProvenanceBytes = 512U;
constexpr std::uint32_t kMaxTextureDimension = 16'384U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool safe_relative_locator(std::string_view value) {
    if (!safe_text(value, kMaxLocatorBytes) || value.find("://") != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path) {
        if (part == "..") return false;
    }
    return true;
}

bool valid_texture_format(assets::TextureSourceFormat format) {
    switch (format) {
    case assets::TextureSourceFormat::png:
    case assets::TextureSourceFormat::jpeg:
    case assets::TextureSourceFormat::tga:
    case assets::TextureSourceFormat::dds:
    case assets::TextureSourceFormat::exr:
        return true;
    }
    return false;
}

bool valid_address_mode(SamplerAddressMode mode) {
    switch (mode) {
    case SamplerAddressMode::repeat:
    case SamplerAddressMode::mirrored_repeat:
    case SamplerAddressMode::clamp_to_edge:
        return true;
    }
    return false;
}

bool valid_filter(SamplerFilter filter) {
    switch (filter) {
    case SamplerFilter::nearest:
    case SamplerFilter::linear:
        return true;
    }
    return false;
}

template <typename Key, typename Value>
core::Result<void> insert_unique(
    std::map<Key, Value>& destination,
    Key key,
    Value value,
    std::string_view label) {
    if (destination.size() >= MaterialCatalog::kMaxRecordsPerKind) {
        return core::Result<void>::failure(validation(
            std::string(label) + " collection has reached its bounded capacity"));
    }
    const auto [iterator, inserted] = destination.emplace(key, std::move(value));
    static_cast<void>(iterator);
    if (!inserted) {
        return core::Result<void>::failure(invalid(
            std::string(label) + " identity is duplicated"));
    }
    return core::Result<void>::success();
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool read_presence(std::istream& input, bool& present) {
    unsigned encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > 1U) return false;
    present = encoded != 0U;
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

template <typename Enum>
bool read_enum(std::istream& input, unsigned maximum, Enum& value) {
    unsigned encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > maximum) return false;
    value = static_cast<Enum>(encoded);
    return true;
}

void write_texture_ref(std::ostream& output, const render::TextureRef& value) {
    output << value.asset << ' ' << value.uv_set << ' ' << value.sampler << ' '
           << value.transform.offset.x << ' ' << value.transform.offset.y << ' '
           << value.transform.scale.x << ' ' << value.transform.scale.y << ' '
           << value.transform.rotation << ' '
           << value.transform.pivot.x << ' ' << value.transform.pivot.y;
    for (const render::TextureChannel channel : value.channels.rgba) {
        output << ' ' << static_cast<unsigned>(channel);
    }
    output << ' ' << static_cast<unsigned>(value.interpretation)
           << ' ' << static_cast<unsigned>(value.normal_convention);
}

bool read_texture_ref(std::istream& input, render::TextureRef& value) {
    unsigned channels[4]{};
    unsigned interpretation = 0U;
    unsigned normal_convention = 0U;
    if (!read_unsigned(input, value.asset) || !read_unsigned(input, value.uv_set) ||
        !read_unsigned(input, value.sampler) ||
        !(input >> value.transform.offset.x >> value.transform.offset.y >>
          value.transform.scale.x >> value.transform.scale.y >>
          value.transform.rotation >> value.transform.pivot.x >>
          value.transform.pivot.y)) {
        return false;
    }
    for (unsigned& channel : channels) {
        if (!read_unsigned(input, channel) ||
            channel > static_cast<unsigned>(render::TextureChannel::constant_one)) {
            return false;
        }
    }
    if (!read_unsigned(input, interpretation) ||
        interpretation > static_cast<unsigned>(render::TextureInterpretation::srgb) ||
        !read_unsigned(input, normal_convention) ||
        normal_convention > static_cast<unsigned>(render::NormalConvention::opengl_y_plus)) {
        return false;
    }
    for (std::size_t index = 0U; index < 4U; ++index) {
        value.channels.rgba[index] = static_cast<render::TextureChannel>(channels[index]);
    }
    value.interpretation = static_cast<render::TextureInterpretation>(interpretation);
    value.normal_convention = static_cast<render::NormalConvention>(normal_convention);
    return true;
}

void write_standard_material(std::ostream& output, const render::StandardMaterial& value) {
    output << value.base_color_factor.x << ' ' << value.base_color_factor.y << ' '
           << value.base_color_factor.z << ' ' << value.base_color_factor.w << ' '
           << value.metallic << ' ' << value.roughness << ' ' << value.normal_scale << ' '
           << value.occlusion_strength << ' ' << value.emissive_factor.x << ' '
           << value.emissive_factor.y << ' ' << value.emissive_factor.z << ' '
           << value.emissive_strength << ' ' << value.dielectric_f0 << ' '
           << value.clearcoat << ' ' << value.clearcoat_roughness << ' '
           << static_cast<unsigned>(value.alpha_mode) << ' ' << value.alpha_cutoff << ' ';
    write_texture_ref(output, value.base_color);
    output << ' ';
    write_texture_ref(output, value.metallic_roughness);
    output << ' ';
    write_texture_ref(output, value.normal);
    output << ' ';
    write_texture_ref(output, value.occlusion);
    output << ' ';
    write_texture_ref(output, value.emissive);
}

bool read_standard_material(std::istream& input, render::StandardMaterial& value) {
    unsigned alpha_mode = 0U;
    if (!(input >> value.base_color_factor.x >> value.base_color_factor.y >>
          value.base_color_factor.z >> value.base_color_factor.w >> value.metallic >>
          value.roughness >> value.normal_scale >> value.occlusion_strength >>
          value.emissive_factor.x >> value.emissive_factor.y >> value.emissive_factor.z >>
          value.emissive_strength >> value.dielectric_f0 >> value.clearcoat >>
          value.clearcoat_roughness) ||
        !read_unsigned(input, alpha_mode) ||
        alpha_mode > static_cast<unsigned>(render::AlphaMode::blended) ||
        !(input >> value.alpha_cutoff) ||
        !read_texture_ref(input, value.base_color) ||
        !read_texture_ref(input, value.metallic_roughness) ||
        !read_texture_ref(input, value.normal) ||
        !read_texture_ref(input, value.occlusion) ||
        !read_texture_ref(input, value.emissive)) {
        return false;
    }
    value.alpha_mode = static_cast<render::AlphaMode>(alpha_mode);
    return true;
}

template <typename T>
void write_optional_scalar(std::ostream& output, const std::optional<T>& value) {
    output << (value.has_value() ? 1U : 0U);
    if (value.has_value()) output << ' ' << value.value();
    output << ' ';
}

template <typename T>
bool read_optional_scalar(std::istream& input, std::optional<T>& value) {
    bool present = false;
    if (!read_presence(input, present)) return false;
    if (!present) {
        value.reset();
        return true;
    }
    T decoded{};
    if (!(input >> decoded)) return false;
    value = decoded;
    return true;
}

void write_optional_vec4(std::ostream& output, const std::optional<core::Vec4d>& value) {
    output << (value.has_value() ? 1U : 0U);
    if (value.has_value()) {
        output << ' ' << value->x << ' ' << value->y << ' ' << value->z << ' ' << value->w;
    }
    output << ' ';
}

bool read_optional_vec4(std::istream& input, std::optional<core::Vec4d>& value) {
    bool present = false;
    if (!read_presence(input, present)) return false;
    if (!present) {
        value.reset();
        return true;
    }
    core::Vec4d decoded;
    if (!(input >> decoded.x >> decoded.y >> decoded.z >> decoded.w)) return false;
    value = decoded;
    return true;
}

void write_optional_vec3(std::ostream& output, const std::optional<core::Vec3d>& value) {
    output << (value.has_value() ? 1U : 0U);
    if (value.has_value()) output << ' ' << value->x << ' ' << value->y << ' ' << value->z;
    output << ' ';
}

bool read_optional_vec3(std::istream& input, std::optional<core::Vec3d>& value) {
    bool present = false;
    if (!read_presence(input, present)) return false;
    if (!present) {
        value.reset();
        return true;
    }
    core::Vec3d decoded;
    if (!(input >> decoded.x >> decoded.y >> decoded.z)) return false;
    value = decoded;
    return true;
}

void write_optional_alpha(std::ostream& output, const std::optional<render::AlphaMode>& value) {
    output << (value.has_value() ? 1U : 0U);
    if (value.has_value()) output << ' ' << static_cast<unsigned>(value.value());
    output << ' ';
}

bool read_optional_alpha(std::istream& input, std::optional<render::AlphaMode>& value) {
    bool present = false;
    if (!read_presence(input, present)) return false;
    if (!present) {
        value.reset();
        return true;
    }
    render::AlphaMode decoded;
    if (!read_enum(input, static_cast<unsigned>(render::AlphaMode::blended), decoded)) {
        return false;
    }
    value = decoded;
    return true;
}

void write_optional_texture(
    std::ostream& output,
    const std::optional<render::TextureRef>& value) {
    output << (value.has_value() ? 1U : 0U);
    if (value.has_value()) {
        output << ' ';
        write_texture_ref(output, value.value());
    }
    output << ' ';
}

bool read_optional_texture(
    std::istream& input,
    std::optional<render::TextureRef>& value) {
    bool present = false;
    if (!read_presence(input, present)) return false;
    if (!present) {
        value.reset();
        return true;
    }
    render::TextureRef decoded;
    if (!read_texture_ref(input, decoded)) return false;
    value = decoded;
    return true;
}

void write_overrides(std::ostream& output, const MaterialVariantOverride& value) {
    write_optional_vec4(output, value.base_color_factor);
    write_optional_scalar(output, value.metallic);
    write_optional_scalar(output, value.roughness);
    write_optional_scalar(output, value.normal_scale);
    write_optional_scalar(output, value.occlusion_strength);
    write_optional_vec3(output, value.emissive_factor);
    write_optional_scalar(output, value.emissive_strength);
    write_optional_scalar(output, value.dielectric_f0);
    write_optional_scalar(output, value.clearcoat);
    write_optional_scalar(output, value.clearcoat_roughness);
    write_optional_alpha(output, value.alpha_mode);
    write_optional_scalar(output, value.alpha_cutoff);
    write_optional_texture(output, value.base_color);
    write_optional_texture(output, value.metallic_roughness);
    write_optional_texture(output, value.normal);
    write_optional_texture(output, value.occlusion);
    write_optional_texture(output, value.emissive);
}

bool read_overrides(std::istream& input, MaterialVariantOverride& value) {
    return read_optional_vec4(input, value.base_color_factor) &&
        read_optional_scalar(input, value.metallic) &&
        read_optional_scalar(input, value.roughness) &&
        read_optional_scalar(input, value.normal_scale) &&
        read_optional_scalar(input, value.occlusion_strength) &&
        read_optional_vec3(input, value.emissive_factor) &&
        read_optional_scalar(input, value.emissive_strength) &&
        read_optional_scalar(input, value.dielectric_f0) &&
        read_optional_scalar(input, value.clearcoat) &&
        read_optional_scalar(input, value.clearcoat_roughness) &&
        read_optional_alpha(input, value.alpha_mode) &&
        read_optional_scalar(input, value.alpha_cutoff) &&
        read_optional_texture(input, value.base_color) &&
        read_optional_texture(input, value.metallic_roughness) &&
        read_optional_texture(input, value.normal) &&
        read_optional_texture(input, value.occlusion) &&
        read_optional_texture(input, value.emissive);
}

void apply_overrides(
    render::StandardMaterial& target,
    const MaterialVariantOverride& source) {
    if (source.base_color_factor) target.base_color_factor = *source.base_color_factor;
    if (source.metallic) target.metallic = *source.metallic;
    if (source.roughness) target.roughness = *source.roughness;
    if (source.normal_scale) target.normal_scale = *source.normal_scale;
    if (source.occlusion_strength) target.occlusion_strength = *source.occlusion_strength;
    if (source.emissive_factor) target.emissive_factor = *source.emissive_factor;
    if (source.emissive_strength) target.emissive_strength = *source.emissive_strength;
    if (source.dielectric_f0) target.dielectric_f0 = *source.dielectric_f0;
    if (source.clearcoat) target.clearcoat = *source.clearcoat;
    if (source.clearcoat_roughness) target.clearcoat_roughness = *source.clearcoat_roughness;
    if (source.alpha_mode) target.alpha_mode = *source.alpha_mode;
    if (source.alpha_cutoff) target.alpha_cutoff = *source.alpha_cutoff;
    if (source.base_color) target.base_color = *source.base_color;
    if (source.metallic_roughness) target.metallic_roughness = *source.metallic_roughness;
    if (source.normal) target.normal = *source.normal;
    if (source.occlusion) target.occlusion = *source.occlusion;
    if (source.emissive) target.emissive = *source.emissive;
}

template <typename Callback>
core::Result<void> for_each_texture_binding(
    const render::StandardMaterial& material,
    Callback&& callback) {
    for (const render::TextureRef* binding : {
             &material.base_color,
             &material.metallic_roughness,
             &material.normal,
             &material.occlusion,
             &material.emissive}) {
        if (!binding->asset) continue;
        if (auto result = callback(*binding); !result) return result;
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> TextureSourceRecord::validate() const {
    const bool dimensions_known = width != 0U || height != 0U;
    if (id == 0U || !safe_text(name, kMaxNameBytes) ||
        !safe_relative_locator(relative_locator) || content_digest.is_zero() ||
        !valid_texture_format(format) || !safe_text(provenance, kMaxProvenanceBytes) ||
        revision.exhausted() ||
        (dimensions_known && (width == 0U || height == 0U ||
            width > kMaxTextureDimension || height > kMaxTextureDimension))) {
        return core::Result<void>::failure(invalid(
            "texture source identity, locator, digest, format, dimensions, or provenance is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> SamplerIntent::validate() const {
    if (id == 0U || !safe_text(name, kMaxNameBytes) ||
        !valid_address_mode(address_u) || !valid_address_mode(address_v) ||
        !valid_address_mode(address_w) || !valid_filter(minification) ||
        !valid_filter(magnification) || !valid_filter(mip) || revision.exhausted()) {
        return core::Result<void>::failure(invalid(
            "sampler identity, name, addressing, or filtering is invalid"));
    }
    return core::Result<void>::success();
}

bool MaterialVariantOverride::empty() const noexcept {
    return !base_color_factor && !metallic && !roughness && !normal_scale &&
        !occlusion_strength && !emissive_factor && !emissive_strength &&
        !dielectric_f0 && !clearcoat && !clearcoat_roughness && !alpha_mode &&
        !alpha_cutoff && !base_color && !metallic_roughness && !normal &&
        !occlusion && !emissive;
}

core::Result<void> MaterialVariantOverride::validate() const {
    if (empty()) {
        return core::Result<void>::failure(invalid(
            "material variant must contain at least one explicit override"));
    }
    render::StandardMaterial candidate;
    apply_overrides(candidate, *this);
    return candidate.validate();
}

core::Result<void> MaterialVariant::validate() const {
    if (id == 0U || base_material == 0U || !safe_text(name, kMaxNameBytes) ||
        permitted_mesh_assets.size() > MaterialCatalog::kMaxVariantMeshScope ||
        revision.exhausted()) {
        return core::Result<void>::failure(invalid(
            "material variant identity, name, base, or scope is invalid"));
    }
    if (!std::is_sorted(permitted_mesh_assets.begin(), permitted_mesh_assets.end()) ||
        std::adjacent_find(permitted_mesh_assets.begin(), permitted_mesh_assets.end()) !=
            permitted_mesh_assets.end() ||
        std::any_of(permitted_mesh_assets.begin(), permitted_mesh_assets.end(),
            [](std::uint64_t value) { return value == 0U; })) {
        return core::Result<void>::failure(validation(
            "material variant mesh scope must be non-zero, sorted, and unique"));
    }
    return overrides.validate();
}

core::Result<void> MaterialRegionAssignment::validate() const {
    if (key.mesh_asset == 0U || key.region_id == 0U || material == 0U ||
        (variant.has_value() && variant.value() == 0U)) {
        return core::Result<void>::failure(invalid(
            "material-region assignment identities must be non-zero"));
    }
    return core::Result<void>::success();
}

core::Result<void> MaterialCatalog::insert_texture(TextureSourceRecord texture) {
    if (auto result = texture.validate(); !result) return result;
    const auto identity = texture.id;
    return insert_unique(textures_, identity, std::move(texture), "texture source");
}

core::Result<void> MaterialCatalog::insert_sampler(SamplerIntent sampler) {
    if (auto result = sampler.validate(); !result) return result;
    const auto identity = sampler.id;
    return insert_unique(samplers_, identity, std::move(sampler), "sampler intent");
}

core::Result<void> MaterialCatalog::insert_material(render::MaterialAsset material) {
    if (auto result = material.validate(); !result) return result;
    if (!safe_text(material.name, kMaxNameBytes) || material.revision.exhausted()) {
        return core::Result<void>::failure(invalid(
            "material display name or revision is invalid"));
    }
    const auto identity = material.id;
    return insert_unique(materials_, identity, std::move(material), "material source");
}

core::Result<void> MaterialCatalog::insert_variant(MaterialVariant variant) {
    if (auto result = variant.validate(); !result) return result;
    const auto identity = variant.id;
    return insert_unique(variants_, identity, std::move(variant), "material variant");
}

core::Result<void> MaterialCatalog::insert_assignment(MaterialRegionAssignment assignment) {
    if (auto result = assignment.validate(); !result) return result;
    const auto identity = assignment.key;
    return insert_unique(assignments_, identity, std::move(assignment),
        "material-region assignment");
}

bool MaterialCatalog::empty() const noexcept {
    return textures_.empty() && samplers_.empty() && materials_.empty() &&
        variants_.empty() && assignments_.empty();
}

core::Result<render::StandardMaterial> MaterialCatalog::resolve_material(
    render::MaterialId material,
    std::optional<MaterialVariantId> variant) const {
    const auto material_it = materials_.find(material);
    if (material_it == materials_.end()) {
        return core::Result<render::StandardMaterial>::failure(
            validation("material reference does not resolve in the source catalog"));
    }
    render::StandardMaterial resolved = material_it->second.definition;
    if (variant.has_value()) {
        const auto variant_it = variants_.find(variant.value());
        if (variant_it == variants_.end() || variant_it->second.base_material != material) {
            return core::Result<render::StandardMaterial>::failure(
                validation("material variant does not resolve to the requested base material"));
        }
        apply_overrides(resolved, variant_it->second.overrides);
    }
    if (auto result = resolved.validate(); !result) {
        return core::Result<render::StandardMaterial>::failure(
            result.error().with_context("resolved material variant"));
    }
    return core::Result<render::StandardMaterial>::success(std::move(resolved));
}

core::Result<void> MaterialCatalog::validate() const {
    if (textures_.size() > kMaxRecordsPerKind || samplers_.size() > kMaxRecordsPerKind ||
        materials_.size() > kMaxRecordsPerKind || variants_.size() > kMaxRecordsPerKind ||
        assignments_.size() > kMaxRecordsPerKind) {
        return core::Result<void>::failure(validation(
            "material catalog exceeds a bounded collection size"));
    }
    for (const auto& [identity, texture] : textures_) {
        if (identity != texture.id) {
            return core::Result<void>::failure(validation(
                "texture source map identity does not match its record"));
        }
        if (auto result = texture.validate(); !result) return result;
    }
    for (const auto& [identity, sampler] : samplers_) {
        if (identity != sampler.id) {
            return core::Result<void>::failure(validation(
                "sampler map identity does not match its record"));
        }
        if (auto result = sampler.validate(); !result) return result;
    }
    const auto validate_bindings = [this](const render::StandardMaterial& material) {
        return for_each_texture_binding(material, [this](const render::TextureRef& binding) {
            if (!textures_.contains(binding.asset)) {
                return core::Result<void>::failure(validation(
                    "material texture binding references a missing texture source"));
            }
            if (binding.sampler != 0U && !samplers_.contains(binding.sampler)) {
                return core::Result<void>::failure(validation(
                    "material texture binding references a missing sampler intent"));
            }
            return core::Result<void>::success();
        });
    };
    for (const auto& [identity, material] : materials_) {
        if (identity != material.id) {
            return core::Result<void>::failure(validation(
                "material map identity does not match its record"));
        }
        if (auto result = material.validate(); !result) return result;
        if (!safe_text(material.name, kMaxNameBytes) || material.revision.exhausted()) {
            return core::Result<void>::failure(validation(
                "material source name or revision is invalid"));
        }
        if (auto result = validate_bindings(material.definition); !result) return result;
    }
    for (const auto& [identity, variant] : variants_) {
        if (identity != variant.id) {
            return core::Result<void>::failure(validation(
                "material variant map identity does not match its record"));
        }
        if (auto result = variant.validate(); !result) return result;
        const auto resolved = resolve_material(variant.base_material, variant.id);
        if (!resolved) return core::Result<void>::failure(resolved.error());
        if (auto result = validate_bindings(resolved.value()); !result) return result;
    }
    for (const auto& [identity, assignment] : assignments_) {
        if (identity != assignment.key) {
            return core::Result<void>::failure(validation(
                "material-region assignment map identity does not match its record"));
        }
        if (auto result = assignment.validate(); !result) return result;
        if (!materials_.contains(assignment.material)) {
            return core::Result<void>::failure(validation(
                "material-region assignment references a missing material"));
        }
        if (assignment.variant.has_value()) {
            const auto variant = variants_.find(assignment.variant.value());
            if (variant == variants_.end() ||
                variant->second.base_material != assignment.material) {
                return core::Result<void>::failure(validation(
                    "material-region assignment variant does not match its base material"));
            }
            if (!variant->second.permitted_mesh_assets.empty() &&
                !std::binary_search(
                    variant->second.permitted_mesh_assets.begin(),
                    variant->second.permitted_mesh_assets.end(),
                    assignment.key.mesh_asset)) {
                return core::Result<void>::failure(validation(
                    "material-region assignment uses a variant outside its permitted mesh scope"));
            }
        }
    }
    return core::Result<void>::success();
}

std::string MaterialCatalog::serialize() const {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "TEXTURES " << textures_.size() << '\n';
    for (const auto& [identity, texture] : textures_) {
        static_cast<void>(identity);
        output << "TEXTURE " << texture.id << ' ' << texture.revision.value() << ' '
               << static_cast<unsigned>(texture.format) << ' ' << texture.width << ' '
               << texture.height << ' ' << texture.content_digest.hex() << ' '
               << std::quoted(texture.name) << ' ' << std::quoted(texture.relative_locator)
               << ' ' << std::quoted(texture.provenance) << '\n';
    }
    output << "SAMPLERS " << samplers_.size() << '\n';
    for (const auto& [identity, sampler] : samplers_) {
        static_cast<void>(identity);
        output << "SAMPLER " << sampler.id << ' ' << sampler.revision.value() << ' '
               << static_cast<unsigned>(sampler.address_u) << ' '
               << static_cast<unsigned>(sampler.address_v) << ' '
               << static_cast<unsigned>(sampler.address_w) << ' '
               << static_cast<unsigned>(sampler.minification) << ' '
               << static_cast<unsigned>(sampler.magnification) << ' '
               << static_cast<unsigned>(sampler.mip) << ' '
               << std::quoted(sampler.name) << '\n';
    }
    output << "MATERIALS " << materials_.size() << '\n';
    for (const auto& [identity, material] : materials_) {
        static_cast<void>(identity);
        output << "MATERIAL " << material.id << ' ' << material.revision.value() << ' '
               << std::quoted(material.name) << ' ';
        write_standard_material(output, material.definition);
        output << '\n';
    }
    output << "VARIANTS " << variants_.size() << '\n';
    for (const auto& [identity, variant] : variants_) {
        static_cast<void>(identity);
        output << "VARIANT " << variant.id << ' ' << variant.revision.value() << ' '
               << variant.base_material << ' ' << std::quoted(variant.name) << ' '
               << variant.permitted_mesh_assets.size();
        for (const std::uint64_t mesh : variant.permitted_mesh_assets) {
            output << ' ' << mesh;
        }
        output << '\n' << "OVERRIDES ";
        write_overrides(output, variant.overrides);
        output << '\n';
    }
    output << "ASSIGNMENTS " << assignments_.size() << '\n';
    for (const auto& [identity, assignment] : assignments_) {
        static_cast<void>(identity);
        output << "ASSIGNMENT " << assignment.key.mesh_asset << ' '
               << assignment.key.region_id << ' ' << assignment.material << ' '
               << assignment.variant.value_or(0U) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> MaterialCatalog::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string encoded = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()}));
}

core::Result<MaterialCatalog> MaterialCatalog::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<MaterialCatalog>::failure(parse_error(
            "material catalog exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kMagic) || !read_unsigned(input, version)) {
        return core::Result<MaterialCatalog>::failure(parse_error(
            "material catalog header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<MaterialCatalog>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "material catalog schema version is unsupported"));
    }

    MaterialCatalog catalog;
    std::size_t count = 0U;
    if (!require_token(input, "TEXTURES") || !read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<MaterialCatalog>::failure(parse_error(
            "material catalog texture count is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        TextureSourceRecord texture;
        unsigned format = 0U;
        std::uint64_t revision = 0U;
        std::string digest;
        if (!require_token(input, "TEXTURE") || !read_unsigned(input, texture.id) ||
            !read_unsigned(input, revision) || !read_unsigned(input, format) ||
            format > static_cast<unsigned>(assets::TextureSourceFormat::exr) ||
            !read_unsigned(input, texture.width) || !read_unsigned(input, texture.height) ||
            !(input >> digest >> std::quoted(texture.name) >>
              std::quoted(texture.relative_locator) >> std::quoted(texture.provenance))) {
            return core::Result<MaterialCatalog>::failure(parse_error(
                "material catalog texture record is invalid"));
        }
        const auto parsed_digest = assets::Sha256Digest::from_hex(digest);
        if (!parsed_digest) {
            return core::Result<MaterialCatalog>::failure(
                parsed_digest.error().with_context("material catalog texture digest"));
        }
        texture.content_digest = parsed_digest.value();
        texture.format = static_cast<assets::TextureSourceFormat>(format);
        texture.revision = core::Revision{revision};
        if (auto result = catalog.insert_texture(std::move(texture)); !result) {
            return core::Result<MaterialCatalog>::failure(result.error());
        }
    }

    if (!require_token(input, "SAMPLERS") || !read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<MaterialCatalog>::failure(parse_error(
            "material catalog sampler count is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        SamplerIntent sampler;
        std::uint64_t revision = 0U;
        if (!require_token(input, "SAMPLER") || !read_unsigned(input, sampler.id) ||
            !read_unsigned(input, revision) ||
            !read_enum(input, static_cast<unsigned>(SamplerAddressMode::clamp_to_edge),
                sampler.address_u) ||
            !read_enum(input, static_cast<unsigned>(SamplerAddressMode::clamp_to_edge),
                sampler.address_v) ||
            !read_enum(input, static_cast<unsigned>(SamplerAddressMode::clamp_to_edge),
                sampler.address_w) ||
            !read_enum(input, static_cast<unsigned>(SamplerFilter::linear),
                sampler.minification) ||
            !read_enum(input, static_cast<unsigned>(SamplerFilter::linear),
                sampler.magnification) ||
            !read_enum(input, static_cast<unsigned>(SamplerFilter::linear), sampler.mip) ||
            !(input >> std::quoted(sampler.name))) {
            return core::Result<MaterialCatalog>::failure(parse_error(
                "material catalog sampler record is invalid"));
        }
        sampler.revision = core::Revision{revision};
        if (auto result = catalog.insert_sampler(std::move(sampler)); !result) {
            return core::Result<MaterialCatalog>::failure(result.error());
        }
    }

    if (!require_token(input, "MATERIALS") || !read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<MaterialCatalog>::failure(parse_error(
            "material catalog material count is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        render::MaterialAsset material;
        std::uint64_t revision = 0U;
        if (!require_token(input, "MATERIAL") || !read_unsigned(input, material.id) ||
            !read_unsigned(input, revision) || !(input >> std::quoted(material.name)) ||
            !read_standard_material(input, material.definition)) {
            return core::Result<MaterialCatalog>::failure(parse_error(
                "material catalog material record is invalid"));
        }
        material.revision = core::Revision{revision};
        if (auto result = catalog.insert_material(std::move(material)); !result) {
            return core::Result<MaterialCatalog>::failure(result.error());
        }
    }

    if (!require_token(input, "VARIANTS") || !read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<MaterialCatalog>::failure(parse_error(
            "material catalog variant count is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        MaterialVariant variant;
        std::uint64_t revision = 0U;
        std::size_t mesh_count = 0U;
        if (!require_token(input, "VARIANT") || !read_unsigned(input, variant.id) ||
            !read_unsigned(input, revision) || !read_unsigned(input, variant.base_material) ||
            !(input >> std::quoted(variant.name)) ||
            !read_count(input, kMaxVariantMeshScope, mesh_count)) {
            return core::Result<MaterialCatalog>::failure(parse_error(
                "material catalog variant record is invalid"));
        }
        variant.revision = core::Revision{revision};
        variant.permitted_mesh_assets.reserve(mesh_count);
        for (std::size_t mesh_index = 0U; mesh_index < mesh_count; ++mesh_index) {
            std::uint64_t mesh = 0U;
            if (!read_unsigned(input, mesh)) {
                return core::Result<MaterialCatalog>::failure(parse_error(
                    "material variant mesh scope is invalid"));
            }
            variant.permitted_mesh_assets.push_back(mesh);
        }
        if (!require_token(input, "OVERRIDES") || !read_overrides(input, variant.overrides)) {
            return core::Result<MaterialCatalog>::failure(parse_error(
                "material variant override payload is invalid"));
        }
        if (auto result = catalog.insert_variant(std::move(variant)); !result) {
            return core::Result<MaterialCatalog>::failure(result.error());
        }
    }

    if (!require_token(input, "ASSIGNMENTS") || !read_count(input, kMaxRecordsPerKind, count)) {
        return core::Result<MaterialCatalog>::failure(parse_error(
            "material catalog assignment count is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        MaterialRegionAssignment assignment;
        MaterialVariantId variant = 0U;
        if (!require_token(input, "ASSIGNMENT") ||
            !read_unsigned(input, assignment.key.mesh_asset) ||
            !read_unsigned(input, assignment.key.region_id) ||
            !read_unsigned(input, assignment.material) || !read_unsigned(input, variant)) {
            return core::Result<MaterialCatalog>::failure(parse_error(
                "material catalog assignment record is invalid"));
        }
        if (variant != 0U) assignment.variant = variant;
        if (auto result = catalog.insert_assignment(std::move(assignment)); !result) {
            return core::Result<MaterialCatalog>::failure(result.error());
        }
    }
    if (!require_token(input, "END")) {
        return core::Result<MaterialCatalog>::failure(parse_error(
            "material catalog is missing its terminal record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<MaterialCatalog>::failure(parse_error(
            "material catalog contains trailing data"));
    }
    if (auto result = catalog.validate(); !result) {
        return core::Result<MaterialCatalog>::failure(result.error());
    }
    return core::Result<MaterialCatalog>::success(std::move(catalog));
}

} // namespace carto::project
