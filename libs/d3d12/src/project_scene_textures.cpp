#include <carto/assets/blob_store.hpp>
#include <carto/assets/texture_image_decoder.hpp>
#include <carto/assets/texture_mip.hpp>
#include <carto/d3d12/scene_renderer.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace carto::d3d12 {
namespace {

constexpr std::uint64_t kMaxSceneTextureBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxSceneTextures = 1024U;
using AlphaCoverageThreshold = std::uint8_t;

struct RequestedSceneTexture {
    assets::TextureSemantic semantic = assets::TextureSemantic::data;
    std::optional<AlphaCoverageThreshold> alpha_coverage_threshold;
};

core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}

core::Result<void> collect_material_texture(
    std::map<assets::TextureAssetId, RequestedSceneTexture>& requested,
    const render::TextureRef& reference,
    assets::TextureSemantic semantic,
    std::optional<AlphaCoverageThreshold> alpha_coverage_threshold = std::nullopt) {
    if (!reference) return core::Result<void>::success();
    const auto [found, inserted] = requested.emplace(reference.asset,
        RequestedSceneTexture{semantic, alpha_coverage_threshold});
    if (!inserted && found->second.semantic != semantic) {
        return core::Result<void>::failure({core::ErrorCode::unsupported,
            "one saved texture asset is used by multiple native material semantics"});
    }
    if (!inserted && alpha_coverage_threshold.has_value()) {
        if (found->second.alpha_coverage_threshold.has_value() &&
            found->second.alpha_coverage_threshold != alpha_coverage_threshold) {
            return core::Result<void>::failure({core::ErrorCode::unsupported,
                "one saved base-color texture is used by masked materials with incompatible alpha cutoffs"});
        }
        found->second.alpha_coverage_threshold = alpha_coverage_threshold;
    }
    return core::Result<void>::success();
}

std::optional<AlphaCoverageThreshold> mask_alpha_coverage_threshold(
    const render::StandardMaterial& material) {
    if (material.alpha_mode != render::AlphaMode::masked ||
        !material.base_color || material.alpha_cutoff <= 0.0) {
        return std::nullopt;
    }
    // No source alpha value can survive this material's cutoff, even after
    // filtering, so it imposes no coverage-preservation rule on shared mips.
    if (material.base_color_factor.w < material.alpha_cutoff)
        return std::nullopt;
    const double source_threshold = material.alpha_cutoff /
        material.base_color_factor.w;
    if (source_threshold > 1.0) return std::nullopt;
    const auto byte_threshold = static_cast<std::uint8_t>(
        std::clamp(std::ceil(source_threshold * 255.0), 1.0, 255.0));
    return byte_threshold;
}

core::Result<std::uint64_t> mip_payload_bytes(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t max_levels) {
    std::uint64_t total = 0U;
    std::uint32_t levels = 0U;
    while (true) {
        const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
        if (pixels > (std::numeric_limits<std::uint64_t>::max() - total) / 4U) {
            return core::Result<std::uint64_t>::failure(invalid(
                "saved texture mip payload overflows its bounded size"));
        }
        total += pixels * 4U;
        if (width == 1U && height == 1U) break;
        if (++levels >= max_levels) {
            return core::Result<std::uint64_t>::failure(invalid(
                "saved texture exceeds the native mip-level bound"));
        }
        width = std::max(1U, width / 2U);
        height = std::max(1U, height / 2U);
    }
    return core::Result<std::uint64_t>::success(total);
}

core::Result<std::vector<std::uint8_t>> read_verified_texture_blob(
    const std::filesystem::path& project_root,
    const project::TextureSourceRecord& source,
    const assets::TextureSourceLimits& limits) {
    using Output = core::Result<std::vector<std::uint8_t>>;
    assets::BlobStore store(project_root / ".carto-assets" / "source-blobs",
        assets::BlobStoreLimits{limits.max_source_bytes, 256U});
    const auto expected_path = store.path_for(source.content_digest);
    std::error_code error;
    const auto stored_path = std::filesystem::canonical(expected_path, error);
    if (error) return Output::failure({core::ErrorCode::not_found,
        "saved material texture bytes are missing from the project asset store"});
    const auto expected_relative = expected_path.lexically_relative(project_root);
    const auto stored_relative = stored_path.lexically_relative(project_root);
    if (stored_path.lexically_normal() != expected_path.lexically_normal() ||
        expected_relative.empty() || expected_relative.is_absolute() ||
        stored_relative.generic_string() != expected_relative.generic_string() ||
        source.relative_locator != expected_relative.generic_string() ||
        std::any_of(stored_relative.begin(), stored_relative.end(),
            [](const auto& component) { return component == ".."; })) {
        return Output::failure(invalid(
            "saved material texture locator is not its project-local content-addressed path"));
    }
    const auto size = std::filesystem::file_size(stored_path, error);
    if (error || size == 0U || size > limits.max_source_bytes) {
        return Output::failure(invalid(
            "saved material texture source is empty, unavailable, or exceeds its byte limit"));
    }
    return store.read(source.content_digest);
}

} // namespace

core::Result<std::vector<SceneRenderTexture>> load_project_scene_textures(
    const std::filesystem::path& project_path,
    const project::ProjectDocument& document) {
    using Output = core::Result<std::vector<SceneRenderTexture>>;
    if (project_path.empty()) {
        return Output::failure({core::ErrorCode::invalid_argument,
            "project texture loading requires the saved project path"});
    }
    if (auto valid = document.validate(); !valid) return Output::failure(valid.error());

    std::map<assets::TextureAssetId, RequestedSceneTexture> requested;
    const auto collect = [&requested](const render::StandardMaterial& material)
        -> core::Result<void> {
        const std::array<std::pair<const render::TextureRef*, assets::TextureSemantic>, 5U>
            bindings{{
                {&material.base_color, assets::TextureSemantic::base_color},
                {&material.metallic_roughness, assets::TextureSemantic::metallic_roughness},
                {&material.normal, assets::TextureSemantic::normal},
                {&material.occlusion, assets::TextureSemantic::occlusion},
                {&material.emissive, assets::TextureSemantic::emissive},
            }};
        for (const auto& [reference, semantic] : bindings) {
            std::optional<AlphaCoverageThreshold> alpha_threshold;
            if (reference == &material.base_color)
                alpha_threshold = mask_alpha_coverage_threshold(material);
            if (auto result = collect_material_texture(
                    requested, *reference, semantic, alpha_threshold); !result) {
                return result;
            }
        }
        return core::Result<void>::success();
    };

    const auto& catalog = document.material_catalog();
    std::set<std::uint64_t> visible_mesh_assets;
    std::size_t visible_objects = 0U;
    for (const auto& object : document.scene().objects_sorted()) {
        if (!object.visible || !object.mesh_asset.has_value()) continue;
        if (++visible_objects > 4096U) {
            return Output::failure(invalid(
                "saved scene exceeds 4096 visible mesh objects"));
        }
        visible_mesh_assets.insert(*object.mesh_asset);
    }
    for (const auto& [key, assignment] : catalog.assignments()) {
        if (!visible_mesh_assets.contains(key.mesh_asset)) continue;
        const auto material = catalog.resolve_material(assignment.material, assignment.variant);
        if (!material) return Output::failure(material.error());
        if (auto result = collect(material.value()); !result) {
            return Output::failure(result.error());
        }
    }
    if (requested.size() > kMaxSceneTextures) {
        return Output::failure(invalid("saved scene exceeds 1024 referenced textures"));
    }

    std::error_code error;
    const auto requested_root = project_path.parent_path().empty()
        ? std::filesystem::current_path(error)
        : project_path.parent_path();
    if (error) return Output::failure({core::ErrorCode::io_error,
        "unable to resolve the saved project directory"});
    const auto project_root = std::filesystem::weakly_canonical(requested_root, error);
    if (error || project_root.empty()) {
        return Output::failure({core::ErrorCode::io_error,
            "unable to resolve the saved project directory"});
    }

    assets::TextureSourceLimits limits;
    std::vector<SceneRenderTexture> loaded;
    loaded.reserve(requested.size());
    std::uint64_t aggregate_mip_bytes = 0U;
    for (const auto& [identity, requested_texture] : requested) {
        const auto source = catalog.textures().find(identity);
        if (source == catalog.textures().end()) {
            return Output::failure({core::ErrorCode::not_found,
                "saved material texture has no current project source record"});
        }
        auto bytes = read_verified_texture_blob(project_root, source->second, limits);
        if (!bytes) return Output::failure(bytes.error().with_context(
            "saved material texture " + std::to_string(identity)));
        const auto inspected = assets::inspect_texture_source(bytes.value(), {}, limits);
        if (!inspected) return Output::failure(inspected.error().with_context(
            "saved material texture container"));
        if (inspected.value().source.digest != source->second.content_digest ||
            inspected.value().format != source->second.format ||
            inspected.value().width != source->second.width ||
            inspected.value().height != source->second.height) {
            return Output::failure({core::ErrorCode::stale_data,
                "saved material texture bytes differ from the current source record"});
        }
        const auto mip_bytes = mip_payload_bytes(
            source->second.width, source->second.height, limits.max_mip_levels);
        if (!mip_bytes) return Output::failure(mip_bytes.error());
        if (mip_bytes.value() > kMaxSceneTextureBytes - aggregate_mip_bytes) {
            return Output::failure(invalid(
                "saved scene texture mip payload exceeds the native 512 MiB bound"));
        }
        auto decoded = assets::decode_texture_image_rgba8(std::move(bytes.value()),
            source->second.content_digest, source->second.width, source->second.height, limits);
        if (!decoded) return Output::failure(decoded.error().with_context(
            "saved material texture pixel decode"));
        const bool preserve_alpha_coverage =
            requested_texture.alpha_coverage_threshold.has_value();
        const auto alpha_cutoff = preserve_alpha_coverage
            ? *requested_texture.alpha_coverage_threshold
            : static_cast<std::uint8_t>(128U);
        const assets::Rgba8MipOptions options{
            requested_texture.semantic,
            assets::expected_color_space(requested_texture.semantic),
            preserve_alpha_coverage,
            alpha_cutoff};
        auto mips = assets::generate_rgba8_mips(decoded.value(), options, limits);
        if (!mips) return Output::failure(mips.error().with_context(
            "saved material texture mip generation"));
        if (mips.value().resident_bytes() != mip_bytes.value()) {
            return Output::failure({core::ErrorCode::validation_failed,
                "generated material texture mip payload differs from its bounded extent"});
        }
        aggregate_mip_bytes += mip_bytes.value();
        loaded.push_back({identity, source->second.content_digest, std::move(mips.value())});
    }
    return Output::success(std::move(loaded));
}

} // namespace carto::d3d12
