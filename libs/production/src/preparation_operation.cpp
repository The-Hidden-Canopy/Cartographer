#include <carto/production/preparation_operation.hpp>

#include <carto/assets/texture_source.hpp>
#include <carto/production/lookdev_draw_package.hpp>
#include <carto/production/material_artifact.hpp>
#include <carto/production/prepared_scene.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

#ifdef _WIN32
#    define NOMINMAX
#    include <windows.h>
#endif

namespace carto::production {
namespace {

constexpr std::string_view kProfileMagic = "CARTOGRAPHER_PREPARATION_PROFILE";
constexpr std::uintmax_t kMaxTextureSourceBytes = 256U * 1024U * 1024U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::vector<std::uint8_t> text_bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

bool safe_identity(std::string_view value) {
    return !value.empty() && value.size() <= 192U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') ||
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
                byte == '-' || byte == ':' || byte == '/';
        });
}

bool safe_layer_name(const std::optional<std::string>& value) {
    if (!value.has_value()) return true;
    return !value->empty() && value->size() <= 256U &&
        std::all_of(value->begin(), value->end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
                byte != '\r' && byte != '\n';
        });
}

const char* stage_name(PreparationStage stage) noexcept {
    switch (stage) {
    case PreparationStage::resolve: return "resolve";
    case PreparationStage::plan: return "plan";
    case PreparationStage::validate: return "validate";
    case PreparationStage::execute: return "execute";
    case PreparationStage::verify: return "verify";
    case PreparationStage::publish: return "publish";
    }
    return "unknown";
}

void record_failure(
    PreparationOperationReport& report,
    PreparationStage stage,
    const core::Diagnostic& diagnostic,
    std::string_view source_namespace,
    std::optional<std::uint64_t> source_primary = std::nullopt,
    std::optional<std::uint64_t> source_secondary = std::nullopt,
    std::string capability = {}) {
    report.terminal_stage = stage;
    report.diagnostics.push_back(PreparationDiagnostic{
        std::string{"carto.prepare."} + stage_name(stage) + "." +
            core::error_code_name(diagnostic.code),
        PreparationDiagnosticSeverity::error,
        stage,
        std::string{source_namespace},
        source_primary,
        source_secondary,
        std::move(capability),
        diagnostic.message,
        diagnostic.context,
    });
}

bool record_cancellation_if_requested(
    PreparationOperationReport& report,
    PreparationStage stage,
    std::string_view source_namespace,
    const std::stop_token& stop_token) {
    if (!stop_token.stop_requested()) return false;
    report.terminal_stage = stage;
    report.diagnostics.push_back(PreparationDiagnostic{
        std::string{"carto.prepare."} + stage_name(stage) + ".cancelled",
        PreparationDiagnosticSeverity::error,
        stage,
        std::string{source_namespace},
        std::nullopt,
        std::nullopt,
        "preparation.operation",
        "preparation was cancelled before publication",
        {},
    });
    return true;
}

core::Result<void> reject_link_like(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
        return core::Result<void>::failure(io_error(
            "unable to inspect texture source path"));
    }
    if (std::filesystem::is_symlink(status)) {
        return core::Result<void>::failure(validation(
            "texture source paths cannot traverse symbolic links"));
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.wstring().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return core::Result<void>::failure(io_error(
            "unable to inspect texture source path attributes"));
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return core::Result<void>::failure(validation(
            "texture source paths cannot traverse reparse points"));
    }
#endif
    return core::Result<void>::success();
}

core::Result<void> reject_link_chain(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    if (error) {
        return core::Result<void>::failure(io_error(
            "unable to resolve texture source path"));
    }
    auto cursor = absolute.root_path();
    if (!cursor.empty()) {
        if (auto result = reject_link_like(cursor); !result) return result;
    }
    for (const auto& component : absolute.relative_path()) {
        if (component.empty() || component == ".") continue;
        if (component == "..") {
            return core::Result<void>::failure(validation(
                "texture source paths cannot traverse a parent component"));
        }
        cursor /= component;
        if (auto result = reject_link_like(cursor); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> read_texture_source(
    const std::filesystem::path& source_root,
    const project::TextureSourceRecord& texture) {
    std::error_code error;
    if (!std::filesystem::is_directory(source_root, error) || error) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error(
            "texture source root is unavailable"));
    }
    const auto cursor = source_root / std::filesystem::path(texture.relative_locator);
    if (auto result = reject_link_chain(cursor); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    if (!std::filesystem::is_regular_file(cursor, error) || error) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error(
            "texture source is missing or not a regular file"));
    }
    const auto size = std::filesystem::file_size(cursor, error);
    if (error || size == 0U || size > kMaxTextureSourceBytes ||
        size > std::numeric_limits<std::size_t>::max()) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "texture source size is outside the preparation bound"));
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(cursor, std::ios::binary);
    if (!input) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error(
            "unable to open texture source"));
    }
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        (!input.good() && !input.eof())) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error(
            "unable to capture complete texture source bytes"));
    }
    const auto inspected = assets::inspect_texture_source(
        bytes, assets::texture_source_media_type(texture.format));
    if (!inspected) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            inspected.error().with_context("texture source inspection"));
    }
    if (inspected.value().source.digest != texture.content_digest ||
        inspected.value().format != texture.format ||
        (texture.width != 0U && inspected.value().width != texture.width) ||
        (texture.height != 0U && inspected.value().height != texture.height)) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "texture source bytes no longer match the captured catalog record"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<RenderMeshCompileOptions> resolve_mesh_options(
    const geometry::EditableMesh& mesh,
    const project::MaterialCatalog& catalog,
    std::uint64_t mesh_asset,
    const PreparationProfile& profile) {
    RenderMeshCompileOptions options;
    if (profile.use_active_render_uv) {
        if (const auto* active = mesh.uv_sets().active_for_render(); active != nullptr) {
            options.uv_set = active->id;
        }
    }
    if (profile.include_unique_lightmap_uv) {
        std::vector<attributes::UvSetId> candidates;
        for (const auto& [id, descriptor] : mesh.uv_sets().descriptors()) {
            if (descriptor.lightmap_candidate && id != options.uv_set.value_or(0U)) {
                candidates.push_back(id);
            }
        }
        if (candidates.size() > 1U) {
            return core::Result<RenderMeshCompileOptions>::failure(validation(
                "mesh has multiple lightmap UV candidates; the profile requires an explicit choice"));
        }
        if (!candidates.empty()) options.uv1_set = candidates.front();
    }
    options.generate_tangents =
        profile.generate_tangents_when_uv_present && options.uv_set.has_value();
    options.vertex_color_layer = profile.vertex_color_layer;

    bool has_assignments = false;
    for (const auto& [key, assignment] : catalog.assignments()) {
        static_cast<void>(assignment);
        if (key.mesh_asset == mesh_asset) {
            has_assignments = true;
            break;
        }
    }
    if (profile.material_region_layer.has_value()) {
        if (mesh.attributes().find(profile.material_region_layer.value()) != nullptr) {
            options.material_slot_layer = profile.material_region_layer;
        } else if (has_assignments) {
            return core::Result<RenderMeshCompileOptions>::failure(validation(
                "mesh has durable material assignments but lacks the profile material-region layer"));
        }
    } else if (has_assignments) {
        return core::Result<RenderMeshCompileOptions>::failure(validation(
            "profile disables material regions for a mesh with durable assignments"));
    }
    return core::Result<RenderMeshCompileOptions>::success(std::move(options));
}

core::Result<LookdevDrawPackage> compile_scoped_lookdev(
    const project::MaterialCatalog& catalog,
    std::uint64_t mesh_asset,
    const RenderMeshArtifact& mesh) {
    std::vector<LookdevMaterialBinding> bindings;
    bindings.reserve(mesh.submeshes.size());
    for (const auto& submesh : mesh.submeshes) {
        if (!submesh.material_slot.has_value()) {
            return core::Result<LookdevDrawPackage>::failure(validation(
                "materialized lookdev mesh contains an unassigned submesh"));
        }
        const project::MaterialRegionKey key{
            mesh_asset, submesh.material_slot.value()};
        const auto assignment = catalog.assignments().find(key);
        if (assignment == catalog.assignments().end()) {
            return core::Result<LookdevDrawPackage>::failure(validation(
                "material region has no durable source assignment"));
        }
        const auto source_material = catalog.materials().find(
            assignment->second.material);
        if (source_material == catalog.materials().end()) {
            return core::Result<LookdevDrawPackage>::failure(validation(
                "material assignment source does not resolve"));
        }
        const auto resolved = catalog.resolve_material(
            assignment->second.material, assignment->second.variant);
        if (!resolved) {
            return core::Result<LookdevDrawPackage>::failure(resolved.error());
        }
        render::MaterialAsset material = source_material->second;
        material.definition = resolved.value();
        std::string source_identity =
            "material." + std::to_string(material.id) + ".base";
        if (assignment->second.variant.has_value()) {
            const auto variant = catalog.variants().find(
                assignment->second.variant.value());
            if (variant == catalog.variants().end()) {
                return core::Result<LookdevDrawPackage>::failure(validation(
                    "material assignment variant does not resolve"));
            }
            material.name += " / " + variant->second.name;
            source_identity = "material." + std::to_string(material.id) +
                ".variant." + std::to_string(variant->second.id);
        }
        const auto compiled = compile_material_artifact(
            std::move(source_identity), material);
        if (!compiled) {
            return core::Result<LookdevDrawPackage>::failure(
                compiled.error().with_context("material region " +
                    std::to_string(submesh.material_slot.value())));
        }
        bindings.push_back(LookdevMaterialBinding{
            submesh.material_slot, compiled.value()});
    }
    return compile_lookdev_draw_package(mesh, bindings);
}

std::string operation_identity_text(
    const ResolvedPreparationScope& scope,
    const assets::Sha256Digest& profile_digest,
    std::string_view source_namespace,
    core::Revision source_revision,
    const assets::Sha256Digest& source_digest) {
    std::ostringstream output;
    output << "CARTOGRAPHER_PREPARATION_OPERATION 1\nSOURCE "
           << std::quoted(source_namespace) << ' ' << source_revision.value() << ' '
           << source_digest.hex() << "\nPROFILE "
           << profile_digest.hex() << "\nSCOPE "
           << static_cast<unsigned>(scope.kind) << ' ' << scope.asset_ids.size();
    for (const auto id : scope.asset_ids) output << ' ' << id;
    output << ' ' << scope.node_ids.size();
    for (const auto id : scope.node_ids) output << ' ' << id;
    output << "\nEND\n";
    return output.str();
}

} // namespace

core::Result<void> PreparationProfile::validate() const {
    if (!safe_identity(identity) || !safe_layer_name(material_region_layer) ||
        !safe_layer_name(vertex_color_layer) ||
        (!use_active_render_uv && (include_unique_lightmap_uv ||
            generate_tangents_when_uv_present)) ||
        (require_lookdev_for_material_regions && !include_lookdev)) {
        return core::Result<void>::failure(invalid(
            "preparation profile identity, channel policy, or lookdev policy is invalid"));
    }
    return core::Result<void>::success();
}

std::string PreparationProfile::serialize() const {
    std::ostringstream output;
    output << kProfileMagic << ' ' << kPreparationProfileVersion << '\n'
           << "IDENTITY " << std::quoted(identity) << '\n'
           << "CHANNELS " << (use_active_render_uv ? 1U : 0U) << ' '
           << (include_unique_lightmap_uv ? 1U : 0U) << ' '
           << (generate_tangents_when_uv_present ? 1U : 0U) << ' '
           << std::quoted(material_region_layer.value_or("")) << ' '
           << std::quoted(vertex_color_layer.value_or("")) << '\n'
           << "LOOKDEV " << (include_lookdev ? 1U : 0U) << ' '
           << (require_lookdev_for_material_regions ? 1U : 0U) << '\n'
           << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PreparationProfile::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<PreparationProfile> PreparationProfile::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxPreparationProfileBytes) {
        return core::Result<PreparationProfile>::failure(invalid(
            "preparation profile text is empty or exceeds its safety limit"));
    }
    std::istringstream input(std::string{text});
    std::string token;
    std::uint64_t version = 0U;
    if (!(input >> token >> version) || token != kProfileMagic ||
        version != kPreparationProfileVersion) {
        return core::Result<PreparationProfile>::failure(
            core::Diagnostic(core::ErrorCode::version_mismatch,
                "unsupported preparation profile schema version"));
    }

    PreparationProfile profile;
    if (!(input >> token) || token != "IDENTITY" ||
        !(input >> std::quoted(profile.identity))) {
        return core::Result<PreparationProfile>::failure(invalid(
            "preparation profile is missing its identity"));
    }

    std::uint32_t use_active_render_uv = 0U;
    std::uint32_t include_unique_lightmap_uv = 0U;
    std::uint32_t generate_tangents = 0U;
    std::string material_region_layer;
    std::string vertex_color_layer;
    if (!(input >> token) || token != "CHANNELS" ||
        !(input >> use_active_render_uv >> include_unique_lightmap_uv >>
            generate_tangents >> std::quoted(material_region_layer) >>
            std::quoted(vertex_color_layer)) ||
        use_active_render_uv > 1U || include_unique_lightmap_uv > 1U ||
        generate_tangents > 1U) {
        return core::Result<PreparationProfile>::failure(invalid(
            "preparation profile channel policy is invalid"));
    }
    profile.use_active_render_uv = use_active_render_uv != 0U;
    profile.include_unique_lightmap_uv = include_unique_lightmap_uv != 0U;
    profile.generate_tangents_when_uv_present = generate_tangents != 0U;
    if (material_region_layer.empty()) {
        profile.material_region_layer.reset();
    } else {
        profile.material_region_layer = std::move(material_region_layer);
    }
    if (vertex_color_layer.empty()) {
        profile.vertex_color_layer.reset();
    } else {
        profile.vertex_color_layer = std::move(vertex_color_layer);
    }

    std::uint32_t include_lookdev = 0U;
    std::uint32_t require_lookdev = 0U;
    if (!(input >> token) || token != "LOOKDEV" ||
        !(input >> include_lookdev >> require_lookdev) ||
        include_lookdev > 1U || require_lookdev > 1U) {
        return core::Result<PreparationProfile>::failure(invalid(
            "preparation profile lookdev policy is invalid"));
    }
    profile.include_lookdev = include_lookdev != 0U;
    profile.require_lookdev_for_material_regions = require_lookdev != 0U;

    if (!(input >> token) || token != "END") {
        return core::Result<PreparationProfile>::failure(invalid(
            "preparation profile is missing its terminal record"));
    }
    if (input >> token) {
        return core::Result<PreparationProfile>::failure(invalid(
            "preparation profile contains unexpected trailing data"));
    }
    if (auto result = profile.validate(); !result) {
        return core::Result<PreparationProfile>::failure(result.error());
    }
    return core::Result<PreparationProfile>::success(std::move(profile));
}

PreparationProfile portable_preparation_profile() {
    PreparationProfile profile;
    profile.identity = "cartographer.portable-prepared-scene.v1";
    return profile;
}

PreparationOperationReport prepare_and_publish(
    const project::ProjectDocument& document,
    const PreparationOperationRequest& request) {
    PreparationOperationReport report;
    report.source_revision = document.revision();
    const std::string& source_namespace = document.source_namespace();
    if (record_cancellation_if_requested(report, PreparationStage::resolve,
            source_namespace, request.stop_token)) {
        return report;
    }
    if (auto result = document.validate(); !result) {
        record_failure(report, PreparationStage::resolve, result.error(),
            source_namespace);
        return report;
    }
    if (!safe_identity(source_namespace) || request.output_root.empty() ||
        request.output_root.filename().empty()) {
        record_failure(report, PreparationStage::resolve,
            invalid("preparation source namespace and output root are required"),
            source_namespace);
        return report;
    }
    const auto profile_digest = request.profile.canonical_digest();
    if (!profile_digest) {
        record_failure(report, PreparationStage::resolve, profile_digest.error(),
            source_namespace, std::nullopt, std::nullopt,
            "preparation.profile");
        return report;
    }
    const auto scope = resolve_preparation_scope(document, request.scope);
    if (!scope) {
        record_failure(report, PreparationStage::resolve, scope.error(),
            source_namespace, std::nullopt, std::nullopt,
            "preparation.scope");
        return report;
    }
    const std::string canonical_source = document.serialize();
    const assets::Sha256Digest source_digest = digest_text(canonical_source);
    const AuthoritativeSource source_snapshot{
        source_namespace, document.revision(), source_digest};
    if (auto result = source_snapshot.validate(); !result) {
        record_failure(report, PreparationStage::resolve, result.error(),
            source_namespace, std::nullopt, std::nullopt,
            "preparation.source-snapshot");
        return report;
    }
    report.resolved_scope = scope.value();
    report.operation_id = digest_text(operation_identity_text(
        scope.value(), profile_digest.value(), source_namespace,
        document.revision(), source_digest));
    report.terminal_stage = PreparationStage::plan;
    if (record_cancellation_if_requested(report, PreparationStage::plan,
            source_namespace, request.stop_token)) {
        return report;
    }

    const auto prepared_materials = compose_prepared_material_catalog(
        document, scope.value());
    if (!prepared_materials) {
        record_failure(report, PreparationStage::plan, prepared_materials.error(),
            source_namespace, std::nullopt, std::nullopt,
            "scene.material-authoring");
        return report;
    }

    std::map<std::uint64_t, RenderMeshCompileOptions> mesh_options;
    report.terminal_stage = PreparationStage::validate;
    for (const auto mesh_asset : scope.value().asset_ids) {
        if (record_cancellation_if_requested(report, PreparationStage::validate,
                source_namespace, request.stop_token)) {
            return report;
        }
        const auto source_mesh = document.meshes().find(mesh_asset);
        if (source_mesh == document.meshes().end()) {
            record_failure(report, PreparationStage::validate,
                validation("resolved mesh asset disappeared from the immutable snapshot"),
                source_namespace, mesh_asset, std::nullopt,
                "scene.geometry");
            return report;
        }
        const auto options = resolve_mesh_options(
            source_mesh->second, document.material_catalog(), mesh_asset,
            request.profile);
        if (!options) {
            record_failure(report, PreparationStage::validate, options.error(),
                source_namespace, mesh_asset, std::nullopt,
                "scene.geometry");
            return report;
        }
        mesh_options.emplace(mesh_asset, options.value());
    }

    std::map<std::uint64_t, RenderMeshArtifact> render_meshes;
    std::map<std::uint64_t, LookdevDrawPackage> lookdev_packages;
    report.terminal_stage = PreparationStage::execute;
    for (const auto& [mesh_asset, options] : mesh_options) {
        if (record_cancellation_if_requested(report, PreparationStage::execute,
                source_namespace, request.stop_token)) {
            return report;
        }
        const auto source_mesh = document.meshes().find(mesh_asset);
        if (source_mesh == document.meshes().end()) {
            record_failure(report, PreparationStage::execute,
                validation("resolved mesh asset disappeared from the immutable snapshot"),
                source_namespace, mesh_asset, std::nullopt,
                "scene.geometry");
            return report;
        }
        const auto compiled = compile_render_mesh_artifact(
            source_snapshot, source_mesh->second, options);
        if (!compiled) {
            record_failure(report, PreparationStage::execute, compiled.error(),
                source_namespace, mesh_asset, std::nullopt,
                "scene.geometry");
            return report;
        }
        render_meshes.emplace(mesh_asset, compiled.value());

        const bool has_material_regions = std::any_of(
            compiled.value().submeshes.begin(), compiled.value().submeshes.end(),
            [](const RenderMeshSubmesh& submesh) {
                return submesh.material_slot.has_value();
            });
        if (!has_material_regions || !request.profile.include_lookdev) continue;
        const auto lookdev = compile_scoped_lookdev(
            document.material_catalog(), mesh_asset, compiled.value());
        if (!lookdev) {
            if (request.profile.require_lookdev_for_material_regions) {
                record_failure(report, PreparationStage::execute, lookdev.error(),
                    source_namespace, mesh_asset, std::nullopt,
                    "scene.material-lookdev");
                return report;
            }
            report.diagnostics.push_back(PreparationDiagnostic{
                "carto.prepare.execute.optional_lookdev_unsupported",
                PreparationDiagnosticSeverity::warning,
                PreparationStage::execute,
                source_namespace,
                mesh_asset,
                std::nullopt,
                "scene.material-lookdev",
                lookdev.error().message,
                lookdev.error().context,
            });
            continue;
        }
        lookdev_packages.emplace(mesh_asset, lookdev.value());
    }

    std::vector<PreparedAssetInput> inputs;
    inputs.reserve(render_meshes.size());
    for (const auto& [mesh_asset, render_mesh] : render_meshes) {
        const auto lookdev = lookdev_packages.find(mesh_asset);
        inputs.push_back(PreparedAssetInput{
            mesh_asset,
            &render_mesh,
            lookdev == lookdev_packages.end() ? nullptr : &lookdev->second,
        });
    }
    if (record_cancellation_if_requested(report, PreparationStage::verify,
            source_namespace, request.stop_token)) {
        return report;
    }
    const auto envelope = compose_prepared_scene(
        source_namespace, document, scope.value(), request.profile.identity,
        profile_digest.value(), inputs);
    if (!envelope) {
        record_failure(report, PreparationStage::verify, envelope.error(),
            source_namespace, std::nullopt, std::nullopt,
            "prepared-scene.envelope");
        return report;
    }
    report.envelope = envelope.value();

    std::map<std::string, std::vector<std::uint8_t>> payload_map;
    for (const auto& [mesh_asset, render_mesh] : render_meshes) {
        const auto serialized = render_mesh.serialize();
        if (!serialized) {
            record_failure(report, PreparationStage::verify, serialized.error(),
                source_namespace, mesh_asset, std::nullopt,
                "scene.geometry");
            return report;
        }
        payload_map.emplace(
            "mesh." + std::to_string(mesh_asset) + ".render", serialized.value());
    }
    for (const auto& [mesh_asset, lookdev] : lookdev_packages) {
        const auto serialized = serialize_lookdev_draw_package(lookdev);
        if (!serialized) {
            record_failure(report, PreparationStage::verify, serialized.error(),
                source_namespace, mesh_asset, std::nullopt,
                "scene.material-lookdev");
            return report;
        }
        payload_map.emplace(
            "mesh." + std::to_string(mesh_asset) + ".lookdev", serialized.value());
    }
    if (!prepared_materials.value().empty()) {
        payload_map.emplace(
            "source.material-catalog",
            text_bytes(prepared_materials.value().serialize()));
        for (const auto& [texture_id, texture] : prepared_materials.value().textures()) {
            if (record_cancellation_if_requested(report, PreparationStage::verify,
                    source_namespace, request.stop_token)) {
                return report;
            }
            const auto captured = read_texture_source(request.source_root, texture);
            if (!captured) {
                record_failure(report, PreparationStage::verify, captured.error(),
                    source_namespace, texture_id, std::nullopt,
                    "scene.texture-source");
                return report;
            }
            payload_map.emplace(
                "source.texture." + std::to_string(texture_id), captured.value());
        }
    }
    if (scope.value().kind == PreparationScopeKind::scene &&
        !document.world_model().empty()) {
        payload_map.emplace(
            "world.model", text_bytes(document.world_model().serialize()));
    }

    std::vector<PreparedProductPayload> payloads;
    payloads.reserve(envelope.value().products.size());
    for (const auto& product : envelope.value().products) {
        const auto payload = payload_map.find(product.identity);
        if (payload == payload_map.end()) {
            record_failure(report, PreparationStage::verify,
                validation("prepared product has no executable payload owner"),
                source_namespace, std::nullopt, std::nullopt,
                product.identity);
            return report;
        }
        payloads.push_back(PreparedProductPayload{product.identity, payload->second});
    }
    if (payload_map.size() != payloads.size()) {
        record_failure(report, PreparationStage::verify,
            validation("execution produced an undeclared prepared payload"),
            source_namespace);
        return report;
    }

    if (record_cancellation_if_requested(report, PreparationStage::publish,
            source_namespace, request.stop_token)) {
        return report;
    }

    const auto publication = publish_prepared_scene(
        request.output_root, envelope.value(), payloads,
        request.expected_selected_digest);
    if (!publication) {
        record_failure(report, PreparationStage::publish, publication.error(),
            source_namespace, std::nullopt, std::nullopt,
            "prepared-scene.publication");
        return report;
    }
    report.publication = publication.value();
    report.terminal_stage = PreparationStage::publish;
    return report;
}

} // namespace carto::production
