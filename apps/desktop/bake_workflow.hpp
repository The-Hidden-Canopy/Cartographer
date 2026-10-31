#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/production/bake_graph.hpp>
#include <carto/project/project.hpp>
#if defined(CARTO_DESKTOP_HAS_D3D12_BAKE)
#include <carto/d3d12/bake_graph.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <stop_token>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace carto::desktop {

enum class CpuTextureBakeKind {
    ambient_occlusion,
    bent_normal,
    tangent_normal,
    world_position,
    material_surface_id,
    curvature,
    thickness,
    height_displacement,
};

[[nodiscard]] inline const char* cpu_texture_bake_name(
    CpuTextureBakeKind kind) noexcept {
    switch (kind) {
    case CpuTextureBakeKind::ambient_occlusion: return "ambient-occlusion";
    case CpuTextureBakeKind::bent_normal: return "bent-normal";
    case CpuTextureBakeKind::tangent_normal: return "tangent-normal";
    case CpuTextureBakeKind::world_position: return "world-position";
    case CpuTextureBakeKind::material_surface_id: return "material-surface-id";
    case CpuTextureBakeKind::curvature: return "curvature";
    case CpuTextureBakeKind::thickness: return "thickness";
    case CpuTextureBakeKind::height_displacement: return "height-displacement";
    }
    return "unknown";
}

[[nodiscard]] inline assets::Sha256Digest digest_project_snapshot(
    const project::ProjectDocument& document) {
    const std::string serialized = document.serialize();
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size()));
}

struct CpuTextureTileBakeResult {
    std::string map_kind;
    std::filesystem::path output_directory;
    assets::Sha256Digest recipe_digest;
    std::size_t tile_count = 0U;
    std::uint64_t covered_texels = 0U;
    std::optional<std::uint64_t> rays_cast;
    double compile_milliseconds = 0.0;
    std::optional<std::size_t> compiled_tiles;
    std::optional<std::size_t> reused_tiles;
};

namespace bake_workflow_detail {

template <typename Recipe>
decltype(auto) bake_recipe_binding(Recipe& recipe) {
    if constexpr (std::is_same_v<std::remove_cv_t<Recipe>, production::BakeRecipe>) {
        return (recipe);
    } else {
        return (recipe.binding);
    }
}


inline core::Result<bool> path_entry_exists_no_follow(
    const std::filesystem::path& path,
    std::string_view label) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && status.type() == std::filesystem::file_type::not_found)) {
        return core::Result<bool>::success(false);
    }
    if (error) {
        return core::Result<bool>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect " + std::string(label) + " path"));
    }
    return core::Result<bool>::success(true);
}

inline core::Result<void> require_output_parent(
    const std::filesystem::path& output_directory,
    std::string_view map_kind) {
    if (output_directory.empty() || output_directory.filename().empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            std::string(map_kind) + " bake output must name a new directory"));
    }
    const auto parent = output_directory.parent_path();
    if (parent.empty()) return core::Result<void>::success();
    std::error_code error;
    if (!std::filesystem::exists(parent, error) || error ||
        !std::filesystem::is_directory(parent, error) || error) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            std::string(map_kind) + " bake output parent directory is unavailable"));
    }
    return core::Result<void>::success();
}

inline core::Result<void> require_not_cancelled(
    std::stop_token stop_token,
    std::string_view map_kind) {
    if (!stop_token.stop_requested()) return core::Result<void>::success();
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_state,
        std::string(map_kind) +
            " bake cancelled before publication; inspect any pending staging directory"));
}

inline core::Result<void> write_staging_manifest(
    const std::filesystem::path& path,
    std::string_view text,
    std::string_view map_kind) {
    std::ofstream output(path, std::ios::binary | std::ios::out);
    if (!output) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to create the " + std::string(map_kind) +
                " receipt manifest in the private staging directory"));
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    if (!output) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "failed while writing the " + std::string(map_kind) +
                " receipt manifest"));
    }
    output.close();
    if (!output) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "failed while closing the " + std::string(map_kind) +
                " receipt manifest"));
    }
    return core::Result<void>::success();
}

inline core::Result<void> require_regular_file_no_follow(
    const std::filesystem::path& path,
    std::string_view label) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || status.type() != std::filesystem::file_type::regular ||
        std::filesystem::is_symlink(status)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            std::string(label) + " must be a regular non-symlink file"));
    }
    return core::Result<void>::success();
}

inline core::Result<std::string> read_bounded_manifest(
    const std::filesystem::path& path,
    std::string_view map_kind) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > production::ProductionReceiptLedger::max_serialized_bytes) {
        return core::Result<std::string>::failure(core::Diagnostic(
            error ? core::ErrorCode::io_error : core::ErrorCode::validation_failed,
            std::string(map_kind) +
                " receipt manifest is unavailable or exceeds its serialized size limit"));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<std::string>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to reopen the " + std::string(map_kind) + " receipt manifest"));
    }
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (size != 0U) input.read(contents.data(), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size) || input.bad()) {
        return core::Result<std::string>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "failed while reading back the " + std::string(map_kind) +
                " receipt manifest"));
    }
    return core::Result<std::string>::success(std::move(contents));
}

inline core::Result<std::string> read_bounded_regular_text(
    const std::filesystem::path& path,
    std::size_t maximum_bytes,
    std::string_view label) {
    if (auto result = require_regular_file_no_follow(path, label); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum_bytes) {
        return core::Result<std::string>::failure(core::Diagnostic(
            error ? core::ErrorCode::io_error : core::ErrorCode::validation_failed,
            std::string(label) + " is unavailable or exceeds its size limit"));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<std::string>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "unable to open " + std::string(label)));
    }
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (size != 0U) input.read(contents.data(), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size) || input.bad()) {
        return core::Result<std::string>::failure(core::Diagnostic(
            core::ErrorCode::io_error, "failed while reading " + std::string(label)));
    }
    return core::Result<std::string>::success(std::move(contents));
}

template <typename Recipe>
inline core::Result<void> validate_tile_source(
    const project::ProjectDocument& document,
    std::uint64_t mesh_asset,
    const Recipe& recipe,
    std::string_view map_kind) {
    const auto mesh = document.meshes().find(mesh_asset);
    if (mesh == document.meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            std::string(map_kind) + " tile source mesh is no longer present in the project"));
    }
    return production::validate_bake_sources(
        bake_recipe_binding(recipe), mesh->second, mesh->second);
}

struct NormalTileCacheTarget {
    production::BakeTileCoordinate tile;
    production::BakeTileDependencyKey dependencies;
    std::filesystem::path path;
    std::uintmax_t maximum_bytes = 0U;
};

inline core::Result<std::optional<production::BakeTileArtifactCacheEntry>>
read_normal_tile_cache_entry(
    const std::filesystem::path& path,
    std::uintmax_t maximum_bytes) {
    const auto exists = path_entry_exists_no_follow(path, "normal-map cache entry");
    if (!exists) {
        return core::Result<std::optional<production::BakeTileArtifactCacheEntry>>::failure(
            exists.error());
    }
    if (!exists.value()) {
        return core::Result<std::optional<production::BakeTileArtifactCacheEntry>>::success(
            std::nullopt);
    }
    if (auto result = require_regular_file_no_follow(
            path, "normal-map cache entry"); !result) {
        return core::Result<std::optional<production::BakeTileArtifactCacheEntry>>::failure(
            result.error());
    }
    std::error_code size_error;
    const auto file_bytes = std::filesystem::file_size(path, size_error);
    if (size_error || file_bytes > maximum_bytes || file_bytes >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        file_bytes > static_cast<std::uintmax_t>(
            std::numeric_limits<std::streamsize>::max())) {
        return core::Result<std::optional<production::BakeTileArtifactCacheEntry>>::failure(
            core::Diagnostic(size_error ? core::ErrorCode::io_error
                                        : core::ErrorCode::validation_failed,
                "normal-map cache entry is unavailable or exceeds its size limit"));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<std::optional<production::BakeTileArtifactCacheEntry>>::failure(
            core::Diagnostic(core::ErrorCode::io_error,
                "unable to open normal-map cache entry"));
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_bytes));
    if (file_bytes != 0U) {
        input.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(file_bytes));
    }
    if (input.gcount() != static_cast<std::streamsize>(file_bytes) || input.bad() ||
        input.peek() != std::char_traits<char>::eof()) {
        return core::Result<std::optional<production::BakeTileArtifactCacheEntry>>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "normal-map cache entry changed while being read"));
    }
    auto entry = production::BakeTileArtifactCacheEntry::deserialize(bytes);
    if (!entry) {
        return core::Result<std::optional<production::BakeTileArtifactCacheEntry>>::failure(
            entry.error().with_context("reading normal-map cache entry"));
    }
    return core::Result<std::optional<production::BakeTileArtifactCacheEntry>>::success(
        std::optional<production::BakeTileArtifactCacheEntry>(
            std::move(entry.value())));
}

inline core::Result<void> write_new_normal_tile_cache_entry(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() > static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max())) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "normal-map cache entry exceeds the stream size limit"));
    }
    const auto exists = path_entry_exists_no_follow(path, "normal-map cache entry");
    if (!exists) return core::Result<void>::failure(exists.error());
    if (exists.value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "normal-map cache entry refuses to overwrite an existing path"));
    }
    auto temporary = path;
    temporary += ".cartographer-tmp";
    const auto temporary_exists = path_entry_exists_no_follow(
        temporary, "normal-map cache temporary entry");
    if (!temporary_exists) return core::Result<void>::failure(temporary_exists.error());
    if (temporary_exists.value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "normal-map cache temporary path already exists"));
    }
    std::ofstream output(temporary, std::ios::binary | std::ios::out);
    if (!output) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to create normal-map cache temporary entry"));
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    output.flush();
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "failed while writing normal-map cache temporary entry"));
    }
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "failed while closing normal-map cache temporary entry"));
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "atomic normal-map cache entry publication failed"));
    }
    return core::Result<void>::success();
}

inline bool path_is_within_lexically(
    const std::filesystem::path& path,
    const std::filesystem::path& parent) {
    auto path_part = path.begin();
    auto parent_part = parent.begin();
    for (; parent_part != parent.end(); ++parent_part, ++path_part) {
        if (path_part == path.end()) return false;
        auto path_component = path_part->native();
        auto parent_component = parent_part->native();
#if defined(_WIN32)
        for (auto& character : path_component) {
            if (character >= static_cast<std::filesystem::path::value_type>('A') &&
                character <= static_cast<std::filesystem::path::value_type>('Z')) {
                character = static_cast<std::filesystem::path::value_type>(
                    character + static_cast<std::filesystem::path::value_type>('a' - 'A'));
            }
        }
        for (auto& character : parent_component) {
            if (character >= static_cast<std::filesystem::path::value_type>('A') &&
                character <= static_cast<std::filesystem::path::value_type>('Z')) {
                character = static_cast<std::filesystem::path::value_type>(
                    character + static_cast<std::filesystem::path::value_type>('a' - 'A'));
            }
        }
#endif
        if (path_component != parent_component) return false;
    }
    return true;
}

inline core::Result<production::BakeTileScheduleResult>
schedule_normal_tiles_with_cache(
    const std::filesystem::path& project_path,
    const std::filesystem::path& output_directory,
    const std::filesystem::path& cache_directory,
    std::uint64_t mesh_asset,
    const production::BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    std::stop_token stop_token) {
    if (cache_directory.empty()) {
        return production::schedule_normal_map_tiles_cpu(
            recipe, production::BakeNormalSpace::tangent,
            source, target, {}, stop_token);
    }
    if (cache_directory.filename().empty()) {
        return core::Result<production::BakeTileScheduleResult>::failure(
            core::Diagnostic(core::ErrorCode::invalid_argument,
                "normal-map cache path must name a directory"));
    }
    std::error_code path_error;
    const auto absolute_cache = std::filesystem::absolute(
        cache_directory, path_error).lexically_normal();
    if (path_error) {
        return core::Result<production::BakeTileScheduleResult>::failure(
            core::Diagnostic(core::ErrorCode::io_error,
                "unable to resolve normal-map cache directory"));
    }
    const auto absolute_output = std::filesystem::absolute(
        output_directory, path_error).lexically_normal();
    if (path_error || path_is_within_lexically(absolute_cache, absolute_output) ||
        path_is_within_lexically(absolute_output, absolute_cache)) {
        return core::Result<production::BakeTileScheduleResult>::failure(
            core::Diagnostic(core::ErrorCode::invalid_argument,
                "normal-map cache and output directories must not overlap"));
    }
    const auto cache_parent = cache_directory.parent_path();
    if (!cache_parent.empty()) {
        std::error_code parent_error;
        if (!std::filesystem::exists(cache_parent, parent_error) || parent_error ||
            !std::filesystem::is_directory(cache_parent, parent_error) || parent_error) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                core::Diagnostic(core::ErrorCode::io_error,
                    "normal-map cache parent directory is unavailable"));
        }
    }
    const auto cache_exists = path_entry_exists_no_follow(
        cache_directory, "normal-map cache directory");
    if (!cache_exists) {
        return core::Result<production::BakeTileScheduleResult>::failure(
            cache_exists.error());
    }
    if (cache_exists.value()) {
        std::error_code cache_error;
        const auto status = std::filesystem::symlink_status(cache_directory, cache_error);
        if (cache_error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_directory(status)) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                core::Diagnostic(core::ErrorCode::validation_failed,
                    "normal-map cache path must be a real directory, not a symlink"));
        }
    }

    const auto tiles = production::enumerate_bake_tiles(target, recipe.target_uv_set);
    if (!tiles) {
        return core::Result<production::BakeTileScheduleResult>::failure(tiles.error());
    }
    if (tiles.value().empty() ||
        tiles.value().size() > production::kMaxBakeTileScheduleTiles) {
        return core::Result<production::BakeTileScheduleResult>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "normal-map cache lookup has an empty or over-budget tile set"));
    }
    const std::uint64_t tile_pixels =
        static_cast<std::uint64_t>(recipe.width) * recipe.height;
    if (tile_pixels == 0U || tile_pixels >
            production::kMaxBakeTileScheduleRgbaBytes / 4U) {
        return core::Result<production::BakeTileScheduleResult>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "normal-map cache lookup exceeds the aggregate RGBA8 memory budget"));
    }
    const std::uint64_t tile_bytes = tile_pixels * 4U;
    if (tiles.value().size() >
            production::kMaxBakeTileScheduleRgbaBytes / tile_bytes) {
        return core::Result<production::BakeTileScheduleResult>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "normal-map cache lookup exceeds the aggregate RGBA8 memory budget"));
    }
    const std::uint64_t scheduled_bytes =
        static_cast<std::uint64_t>(tiles.value().size()) * tile_bytes;
    const std::uint64_t cache_parse_reserve = tile_bytes * 2U + 64U * 1024U;
    const std::uint64_t reuse_budget = cache_parse_reserve <=
            production::kMaxBakeTileScheduleRgbaBytes &&
            scheduled_bytes <= production::kMaxBakeTileScheduleRgbaBytes -
                cache_parse_reserve
        ? production::kMaxBakeTileScheduleRgbaBytes - scheduled_bytes - cache_parse_reserve
        : 0U;
    const std::uint64_t maximum_entry_bytes = std::min<std::uint64_t>(
        tile_bytes + 64U * 1024U,
        static_cast<std::uint64_t>(production::kMaxBakeTextureTexels) * 4U +
            64U * 1024U);
    std::uint64_t reusable_bytes = 0U;
    std::vector<NormalTileCacheTarget> cache_targets;
    std::vector<production::BakeTileArtifactCacheEntry> reusable_tiles;
    cache_targets.reserve(tiles.value().size());
    reusable_tiles.reserve(tiles.value().size());
    for (const auto tile : tiles.value()) {
        if (stop_token.stop_requested()) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                core::Diagnostic(core::ErrorCode::invalid_state,
                    "normal-map cache lookup cancelled before output publication"));
        }
        production::BakeRecipe tile_recipe = recipe;
        tile_recipe.tile_u = tile.tile_u;
        tile_recipe.tile_v = tile.tile_v;
        tile_recipe.identity = recipe.identity + "/tile/" +
            std::to_string(tile.tile_u) + "/" + std::to_string(tile.tile_v);
        const auto dependencies = production::make_bake_tile_dependency_key(
            tile_recipe, source, target, production::BakeNormalSpace::tangent);
        if (!dependencies) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                dependencies.error().with_context("building normal-map cache key"));
        }
        auto cache_path = cache_directory /
            (dependencies.value().dependency_digest.hex() +
                ".carto-bake-tile-cache");
        cache_targets.push_back(NormalTileCacheTarget{
            tile, dependencies.value(), cache_path, maximum_entry_bytes});
        if (!cache_exists.value()) continue;
        auto cached = read_normal_tile_cache_entry(cache_path, maximum_entry_bytes);
        if (!cached) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                cached.error().with_context("loading normal-map tile cache"));
        }
        if (!cached.value()) continue;
        if (!cached.value()->dependencies.same_dependencies(dependencies.value())) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                core::Diagnostic(core::ErrorCode::validation_failed,
                    "normal-map cache key does not match its content-addressed path"));
        }
        if (tile_bytes > reuse_budget - reusable_bytes) continue;
        reusable_bytes += tile_bytes;
        reusable_tiles.push_back(std::move(*cached.value()));
    }

    auto schedule = production::schedule_normal_map_tiles_cpu(
        recipe, production::BakeNormalSpace::tangent,
        source, target, reusable_tiles, stop_token);
    reusable_tiles.clear();
    if (!schedule || schedule.value().compiled_tiles == 0U) return schedule;
    if (stop_token.stop_requested()) {
        return core::Result<production::BakeTileScheduleResult>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state,
                "normal-map cache publication cancelled before output publication"));
    }

    const auto current_document = project::ProjectDocument::load(project_path);
    if (!current_document) {
        return core::Result<production::BakeTileScheduleResult>::failure(
            current_document.error().with_context(
                "reloading project before normal-map cache publication"));
    }
    const auto current_mesh = current_document.value().meshes().find(
        mesh_asset);
    const std::string expected_mesh_identity =
        current_document.value().source_namespace() + "/mesh/" +
        std::to_string(mesh_asset);
    if (current_mesh == current_document.value().meshes().end() ||
        recipe.source.identity != expected_mesh_identity ||
        recipe.target.identity != expected_mesh_identity) {
        return core::Result<production::BakeTileScheduleResult>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "normal-map cache source identity changed during compilation"));
    }
    for (const auto& entry : schedule.value().entries) {
        if (auto result = production::validate_bake_sources(
                entry.artifact.recipe, current_mesh->second, current_mesh->second); !result) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                result.error().with_context(
                    "checking normal-map source before cache publication"));
        }
    }

    if (!cache_exists.value()) {
        std::error_code create_error;
        if (!std::filesystem::create_directory(cache_directory, create_error) ||
            create_error) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                core::Diagnostic(core::ErrorCode::io_error,
                    "unable to create normal-map cache directory"));
        }
    }
    std::error_code cache_status_error;
    const auto cache_status = std::filesystem::symlink_status(
        cache_directory, cache_status_error);
    if (cache_status_error || std::filesystem::is_symlink(cache_status) ||
        !std::filesystem::is_directory(cache_status)) {
        return core::Result<production::BakeTileScheduleResult>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "normal-map cache path changed and is not a real directory"));
    }
    for (const auto& entry : schedule.value().entries) {
        if (stop_token.stop_requested()) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                core::Diagnostic(core::ErrorCode::invalid_state,
                    "normal-map cache publication cancelled before output publication"));
        }
        if (entry.reused_from_cache) continue;
        const auto cache_target = std::find_if(
            cache_targets.begin(), cache_targets.end(),
            [&](const NormalTileCacheTarget& candidate) {
                return candidate.tile == entry.tile;
            });
        if (cache_target == cache_targets.end()) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                core::Diagnostic(core::ErrorCode::validation_failed,
                    "compiled normal tile has no cache dependency record"));
        }
        const auto cache_entry = production::make_bake_tile_cache_entry(
            entry.artifact, current_mesh->second, current_mesh->second);
        if (!cache_entry || !cache_entry.value().dependencies.same_dependencies(
                cache_target->dependencies)) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                cache_entry ? core::Diagnostic(core::ErrorCode::stale_data,
                    "normal-map source changed while creating a cache entry")
                    : cache_entry.error());
        }
        const auto serialized = cache_entry.value().serialize();
        if (!serialized) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                serialized.error());
        }
        const auto validate_cached = [&](
            const production::BakeTileArtifactCacheEntry& cached)
                -> core::Result<void> {
            if (!cached.dependencies.same_dependencies(cache_target->dependencies)) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "normal-map cache entry dependency key mismatch"));
            }
            const auto rebound = production::reuse_bake_tile_artifact(
                cached, entry.artifact.recipe,
                current_mesh->second, current_mesh->second);
            if (!rebound) return core::Result<void>::failure(rebound.error());
            if (rebound.value().artifact_digest != entry.artifact.artifact_digest) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "normal-map cache entry differs from deterministic output"));
            }
            return core::Result<void>::success();
        };
        const auto existing = read_normal_tile_cache_entry(
            cache_target->path, cache_target->maximum_bytes);
        if (!existing) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                existing.error());
        }
        if (existing.value()) {
            if (auto result = validate_cached(*existing.value()); !result) {
                return core::Result<production::BakeTileScheduleResult>::failure(
                    result.error());
            }
            continue;
        }
        if (auto result = write_new_normal_tile_cache_entry(
                cache_target->path, serialized.value()); !result) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                result.error());
        }
        const auto reopened = read_normal_tile_cache_entry(
            cache_target->path, cache_target->maximum_bytes);
        if (!reopened || !reopened.value()) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                reopened ? core::Diagnostic(core::ErrorCode::io_error,
                    "normal-map cache entry disappeared after writing")
                    : reopened.error());
        }
        if (auto result = validate_cached(*reopened.value()); !result) {
            return core::Result<production::BakeTileScheduleResult>::failure(
                result.error());
        }
    }
    return schedule;
}

} // namespace bake_workflow_detail

// Runs one saved-UV CPU tile workflow without touching project state. All
// artifacts and the receipt ledger are read back before output publication.
struct CpuTileBakeMetrics {
    std::uint64_t covered_texels = 0U;
    std::optional<std::uint64_t> rays_cast;
    double compile_milliseconds = 0.0;
    std::optional<std::size_t> compiled_tiles;
    std::optional<std::size_t> reused_tiles;
};

template <typename Schedule>
[[nodiscard]] inline CpuTileBakeMetrics completed_tile_schedule_metrics(
    const Schedule& schedule,
    std::uint64_t covered_texels,
    std::optional<std::uint64_t> rays_cast,
    double compile_milliseconds) {
    return CpuTileBakeMetrics{
        covered_texels, rays_cast, compile_milliseconds,
        schedule.entries.size(), 0U};
}

template <typename Recipe, typename ConfigureRecipe, typename ScheduleTiles,
    typename CookKeyFor, typename PublishArtifact, typename ReadArtifact,
    typename ReadMetrics>
inline core::Result<CpuTextureTileBakeResult> bake_uv_tile_family(
    const std::filesystem::path& project_path,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    std::uint32_t width,
    std::uint32_t height,
    const std::filesystem::path& output_directory,
    std::string_view map_kind,
    std::string_view recipe_prefix,
    ConfigureRecipe&& configure_recipe,
    ScheduleTiles&& schedule_tiles,
    CookKeyFor&& cook_key_for,
    PublishArtifact&& publish_artifact,
    ReadArtifact&& read_artifact,
    ReadMetrics&& read_metrics,
    std::stop_token stop_token = {}) {
    using Output = core::Result<CpuTextureTileBakeResult>;
    using namespace bake_workflow_detail;
    if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
        return Output::failure(result.error());
    }
    if (mesh_asset == 0U || uv_set == 0U || width == 0U || height == 0U) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_argument,
            std::string(map_kind) +
                " bake request has invalid mesh, UV, or dimensions"));
    }
    if (auto result = require_output_parent(output_directory, map_kind); !result) {
        return Output::failure(result.error());
    }
    const auto output_entry = path_entry_exists_no_follow(
        output_directory, std::string(map_kind) + " bake output directory");
    if (!output_entry) return Output::failure(output_entry.error());
    if (output_entry.value()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
            std::string(map_kind) + " bake output refuses to overwrite an existing path"));
    }

    const auto document = project::ProjectDocument::load(project_path);
    if (!document) return Output::failure(document.error());
    const auto mesh = document.value().meshes().find(mesh_asset);
    if (mesh == document.value().meshes().end()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::not_found,
            std::string(map_kind) + " bake mesh asset does not exist in the saved project"));
    }
    const auto current_target_digest = production::bake_mesh_target_digest(
        mesh->second, uv_set);
    if (!current_target_digest ||
        mesh->second.revision() != expected_mesh_revision ||
        current_target_digest.value() != expected_target_digest) {
        return Output::failure(current_target_digest ? core::Diagnostic(
            core::ErrorCode::stale_data,
            "saved UV inspection no longer matches the " + std::string(map_kind) +
                " mesh and UV source")
            : current_target_digest.error());
    }
    const std::string source_namespace = document.value().source_namespace();
    const std::string mesh_identity = source_namespace +
        "/mesh/" + std::to_string(mesh_asset);
    const auto source = production::bind_bake_source(mesh_identity, mesh->second);
    const auto target = production::bind_bake_target(mesh_identity, mesh->second, uv_set);
    if (!source || !target) {
        return Output::failure(!source ? source.error() : target.error());
    }
    Recipe recipe;
    auto& binding = bake_recipe_binding(recipe);
    binding.identity = std::string(recipe_prefix) + "/mesh/" +
        std::to_string(mesh_asset) + "/uv/" + std::to_string(uv_set);
    binding.source = source.value();
    binding.target = target.value();
    binding.target_uv_set = uv_set;
    binding.width = width;
    binding.height = height;
    configure_recipe(recipe);
    if (auto result = recipe.validate(); !result) return Output::failure(result.error());

    const auto schedule = schedule_tiles(
        recipe, mesh->second, mesh->second, stop_token);
    if (!schedule) return Output::failure(schedule.error());
    if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
        return Output::failure(result.error());
    }
    if (schedule.value().entries.empty() ||
        schedule.value().entries.size() > production::kMaxBakeTileScheduleTiles) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            std::string(map_kind) +
                " tile scheduler returned an empty or over-budget tile set"));
    }
    const CpuTileBakeMetrics metrics = read_metrics(schedule.value());
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) return Output::failure(recipe_digest.error());

    auto staging_directory = output_directory;
    staging_directory += ".pending-" + recipe_digest.value().hex().substr(0U, 16U);
    const auto staging_entry = path_entry_exists_no_follow(
        staging_directory, std::string(map_kind) + " bake staging directory");
    if (!staging_entry) return Output::failure(staging_entry.error());
    if (staging_entry.value()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
            std::string(map_kind) +
                " bake staging directory already exists and requires inspection"));
    }
    if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
        return Output::failure(result.error());
    }
    std::error_code filesystem_error;
    if (!std::filesystem::create_directory(staging_directory, filesystem_error) ||
        filesystem_error) {
        return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
            "unable to create the new " + std::string(map_kind) +
                " bake staging directory"));
    }

    try {
        assets::BlobStore blobs(staging_directory / "blobs");
        production::ProductionArtifactStore artifact_store(blobs);
        const production::AdmissionPolicy policy;
        const auto current_document = project::ProjectDocument::load(project_path);
        if (!current_document) {
            return Output::failure(current_document.error().with_context(
                "reloading saved project before " + std::string(map_kind) +
                    " artifact publication"));
        }
        if (current_document.value().source_namespace() != source_namespace) {
            return Output::failure(core::Diagnostic(core::ErrorCode::stale_data,
                std::string(map_kind) +
                    " source namespace changed while tiles were compiling"));
        }
        if (auto result = validate_tile_source(
                current_document.value(), mesh_asset, recipe, map_kind); !result) {
            return Output::failure(result.error().with_context(
                "checking " + std::string(map_kind) + " source after tile compilation"));
        }
        const auto current_mesh = current_document.value().meshes().find(mesh_asset);
        for (const auto& entry : schedule.value().entries) {
            if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
                return Output::failure(result.error());
            }
            const auto payload = entry.artifact.serialize();
            const auto cook_key = cook_key_for(
                entry.artifact.recipe, "carto.cpu-reference.v1");
            if (!payload || !cook_key) {
                return Output::failure(!payload ? payload.error() : cook_key.error());
            }
            production::ProductionArtifactReceipt receipt;
            receipt.key = cook_key.value();
            receipt.status = production::ReceiptStatus::admitted;
            receipt.output_digest = assets::sha256(payload.value());
            receipt.output_bytes = payload.value().size();
            receipt.cost.cook_milliseconds = entry.compile_milliseconds;
            receipt.cost.disk_bytes = receipt.output_bytes;
            if (auto result = receipt.refresh_digest(); !result) {
                return Output::failure(result.error());
            }
            if (auto result = publish_artifact(
                    artifact_store, receipt, entry.artifact,
                    current_mesh->second, current_mesh->second,
                    policy, "carto.cpu-reference.v1"); !result) {
                return Output::failure(result.error());
            }
            const auto reopened = read_artifact(
                artifact_store, entry.artifact.recipe, current_mesh->second,
                current_mesh->second, policy, "carto.cpu-reference.v1");
            if (!reopened || reopened.value().artifact_digest !=
                    entry.artifact.artifact_digest) {
                return Output::failure(reopened ? core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    std::string(map_kind) +
                        " tile differs after artifact read-back") : reopened.error());
            }
        }
        const std::string manifest_text = artifact_store.ledger().serialize();
        const auto manifest = production::ProductionReceiptLedger::deserialize(
            manifest_text);
        if (!manifest || manifest.value().size() != schedule.value().entries.size()) {
            return Output::failure(manifest ? core::Diagnostic(
                core::ErrorCode::validation_failed,
                std::string(map_kind) +
                    " receipt ledger has an unexpected record count") : manifest.error());
        }
        const auto manifest_path = staging_directory /
            "receipts.carto-production-receipts";
        const auto manifest_entry = path_entry_exists_no_follow(
            manifest_path, std::string(map_kind) + " receipt manifest");
        if (!manifest_entry) return Output::failure(manifest_entry.error());
        if (manifest_entry.value()) {
            return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
                std::string(map_kind) +
                    " receipt manifest path already exists in the staging directory"));
        }
        if (auto result = write_staging_manifest(
                manifest_path, manifest_text, map_kind); !result) {
            return Output::failure(result.error());
        }
        if (auto result = require_regular_file_no_follow(
                manifest_path, std::string(map_kind) + " receipt manifest"); !result) {
            return Output::failure(result.error());
        }
        const auto readback_text = read_bounded_manifest(manifest_path, map_kind);
        const auto manifest_readback = readback_text
            ? production::ProductionReceiptLedger::deserialize(readback_text.value())
            : core::Result<production::ProductionReceiptLedger>::failure(
                readback_text.error());
        if (!manifest_readback || manifest_readback.value().serialize() != manifest_text ||
            manifest_readback.value().size() != schedule.value().entries.size()) {
            return Output::failure(manifest_readback ? core::Diagnostic(
                core::ErrorCode::validation_failed,
                std::string(map_kind) +
                    " receipt manifest differs after read-back") : manifest_readback.error());
        }

        const auto publication_document = project::ProjectDocument::load(project_path);
        if (!publication_document) {
            return Output::failure(publication_document.error().with_context(
                "final project check before " + std::string(map_kind) +
                    " tile publication"));
        }
        if (publication_document.value().source_namespace() != source_namespace) {
            return Output::failure(core::Diagnostic(core::ErrorCode::stale_data,
                std::string(map_kind) +
                    " source namespace changed before tile publication"));
        }
        if (auto result = validate_tile_source(
                publication_document.value(), mesh_asset, recipe, map_kind); !result) {
            return Output::failure(result.error().with_context(
                "final source check before " + std::string(map_kind) +
                    " tile publication"));
        }
        const auto final_output_entry = path_entry_exists_no_follow(
            output_directory, std::string(map_kind) + " bake output directory");
        if (!final_output_entry) return Output::failure(final_output_entry.error());
        if (final_output_entry.value()) {
            return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
                std::string(map_kind) + " bake output path appeared before publication"));
        }
        if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
            return Output::failure(result.error());
        }
        filesystem_error.clear();
        std::filesystem::rename(staging_directory, output_directory, filesystem_error);
        if (filesystem_error) {
            return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
                "atomic " + std::string(map_kind) +
                    " output publication failed; inspect the staging directory"));
        }
    } catch (const std::exception& error) {
        return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
            std::string(map_kind) + " tile publication failed: " + error.what()));
    }

    return Output::success(CpuTextureTileBakeResult{
        std::string(map_kind), output_directory, recipe_digest.value(),
        schedule.value().entries.size(), metrics.covered_texels,
        metrics.rays_cast, metrics.compile_milliseconds,
        metrics.compiled_tiles, metrics.reused_tiles});
}

inline core::Result<CpuTextureTileBakeResult> bake_world_position_tiles(
    const std::filesystem::path& project_path,
    core::Revision expected_project_revision,
    assets::Sha256Digest expected_project_digest,
    std::uint64_t target_scene_node,
    std::uint64_t expected_mesh_asset,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    attributes::UvSetId uv_set,
    std::uint32_t width,
    std::uint32_t height,
    const std::filesystem::path& output_directory,
    std::stop_token stop_token = {}) {
    using Output = core::Result<CpuTextureTileBakeResult>;
    using namespace bake_workflow_detail;
    constexpr std::string_view map_kind = "world-position";
    if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
        return Output::failure(result.error());
    }
    if (target_scene_node == 0U || expected_mesh_asset == 0U || uv_set == 0U ||
        width == 0U || height == 0U) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_argument,
            "world-position bake request has invalid node, mesh, UV, or dimensions"));
    }
    if (auto result = require_output_parent(output_directory, map_kind); !result) {
        return Output::failure(result.error());
    }
    const auto output_entry = path_entry_exists_no_follow(
        output_directory, "world-position bake output directory");
    if (!output_entry) return Output::failure(output_entry.error());
    if (output_entry.value()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
            "world-position bake output refuses to overwrite an existing path"));
    }

    const auto document = project::ProjectDocument::load(project_path);
    if (!document) return Output::failure(document.error());
    if (document.value().revision() != expected_project_revision ||
        digest_project_snapshot(document.value()) != expected_project_digest) {
        return Output::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "saved project no longer matches the inspected world-position source"));
    }
    const auto base_recipe = production::bind_world_position_recipe(
        "bake/world-position/node/" + std::to_string(target_scene_node) +
            "/uv/" + std::to_string(uv_set),
        document.value(), target_scene_node, uv_set, width, height);
    if (!base_recipe) return Output::failure(base_recipe.error());
    if (base_recipe.value().target_mesh_asset != expected_mesh_asset ||
        base_recipe.value().target_mesh_revision != expected_mesh_revision ||
        base_recipe.value().target_mesh_digest != expected_target_digest) {
        return Output::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "world-position inspection no longer matches its scene node and mesh"));
    }
    const auto schedule = production::schedule_world_position_tiles_cpu(
        base_recipe.value(), document.value(), stop_token);
    if (!schedule) return Output::failure(schedule.error());
    if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
        return Output::failure(result.error());
    }
    if (schedule.value().entries.empty() ||
        schedule.value().entries.size() > production::kMaxBakeTileScheduleTiles) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "world-position tile scheduler returned an empty or over-budget tile set"));
    }
    if (auto result = production::validate_world_position_source(
            base_recipe.value(), document.value()); !result) {
        return Output::failure(result.error());
    }
    const auto recipe_digest = base_recipe.value().canonical_digest();
    if (!recipe_digest) return Output::failure(recipe_digest.error());
    auto staging_directory = output_directory;
    staging_directory += ".pending-" + recipe_digest.value().hex().substr(0U, 16U);
    const auto staging_entry = path_entry_exists_no_follow(
        staging_directory, "world-position bake staging directory");
    if (!staging_entry) return Output::failure(staging_entry.error());
    if (staging_entry.value()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
            "world-position staging directory already exists and requires inspection"));
    }
    if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
        return Output::failure(result.error());
    }
    std::error_code filesystem_error;
    if (!std::filesystem::create_directory(staging_directory, filesystem_error) ||
        filesystem_error) {
        return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
            "unable to create the new world-position staging directory"));
    }

    try {
        assets::BlobStore blobs(staging_directory / "blobs");
        production::ProductionArtifactStore artifact_store(blobs);
        const production::AdmissionPolicy policy;
        const production::AuthoritativeSource source{
            base_recipe.value().source_namespace,
            base_recipe.value().source_revision,
            base_recipe.value().source_digest};
        const auto current_document = project::ProjectDocument::load(project_path);
        if (!current_document) {
            return Output::failure(current_document.error().with_context(
                "reloading project before world-position artifact publication"));
        }
        if (auto result = production::validate_world_position_source(
                base_recipe.value(), current_document.value()); !result) {
            return Output::failure(result.error().with_context(
                "checking world-position source after tile compilation"));
        }
        for (const auto& entry : schedule.value().entries) {
            if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
                return Output::failure(result.error());
            }
            if (entry.tile.tile_u != entry.artifact.recipe.tile_u ||
                entry.tile.tile_v != entry.artifact.recipe.tile_v) {
                return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                    "world-position schedule contains a mismatched tile artifact"));
            }
            if (auto result = entry.artifact.validate(); !result) {
                return Output::failure(result.error());
            }
            if (auto result = production::validate_world_position_source(
                    entry.artifact.recipe, current_document.value()); !result) {
                return Output::failure(result.error().with_context(
                    "revalidating world-position tile source before publication"));
            }
            const auto payload = entry.artifact.serialize();
            const auto cook_key = production::world_position_cook_key(
                entry.artifact.recipe, "carto.cpu-reference.v1");
            if (!payload || !cook_key) {
                return Output::failure(!payload ? payload.error() : cook_key.error());
            }
            production::ProductionArtifactReceipt receipt;
            receipt.key = cook_key.value();
            receipt.status = production::ReceiptStatus::admitted;
            receipt.output_digest = assets::sha256(payload.value());
            receipt.output_bytes = payload.value().size();
            receipt.cost.cook_milliseconds = entry.compile_milliseconds;
            receipt.cost.disk_bytes = receipt.output_bytes;
            if (auto result = receipt.refresh_digest(); !result) {
                return Output::failure(result.error());
            }
            if (auto result = artifact_store.publish(receipt,
                    std::span<const std::uint8_t>{payload.value()}, source, policy); !result) {
                return Output::failure(result.error());
            }
            const auto reopened = artifact_store.read(cook_key.value(), source, policy);
            const auto reopened_artifact = reopened
                ? production::WorldPositionMapArtifact::deserialize(reopened.value())
                : core::Result<production::WorldPositionMapArtifact>::failure(
                    reopened.error());
            if (!reopened_artifact || reopened_artifact.value().artifact_digest !=
                    entry.artifact.artifact_digest) {
                return Output::failure(reopened_artifact ? core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "world-position tile differs after artifact read-back")
                    : reopened_artifact.error());
            }
        }
        const std::string manifest_text = artifact_store.ledger().serialize();
        const auto manifest = production::ProductionReceiptLedger::deserialize(
            manifest_text);
        if (!manifest || manifest.value().size() != schedule.value().entries.size()) {
            return Output::failure(manifest ? core::Diagnostic(
                core::ErrorCode::validation_failed,
                "world-position receipt ledger has an unexpected record count")
                : manifest.error());
        }
        const auto manifest_path = staging_directory /
            "receipts.carto-production-receipts";
        const auto manifest_entry = path_entry_exists_no_follow(
            manifest_path, "world-position receipt manifest");
        if (!manifest_entry) return Output::failure(manifest_entry.error());
        if (manifest_entry.value()) {
            return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
                "world-position receipt manifest path already exists in staging"));
        }
        if (auto result = write_staging_manifest(
                manifest_path, manifest_text, map_kind); !result) {
            return Output::failure(result.error());
        }
        if (auto result = require_regular_file_no_follow(
                manifest_path, "world-position receipt manifest"); !result) {
            return Output::failure(result.error());
        }
        const auto readback_text = read_bounded_manifest(manifest_path, map_kind);
        const auto manifest_readback = readback_text
            ? production::ProductionReceiptLedger::deserialize(readback_text.value())
            : core::Result<production::ProductionReceiptLedger>::failure(
                readback_text.error());
        if (!manifest_readback ||
            manifest_readback.value().serialize() != manifest_text ||
            manifest_readback.value().size() != schedule.value().entries.size()) {
            return Output::failure(manifest_readback ? core::Diagnostic(
                core::ErrorCode::validation_failed,
                "world-position receipt manifest differs after read-back")
                : manifest_readback.error());
        }
        const auto publication_document = project::ProjectDocument::load(project_path);
        if (!publication_document) {
            return Output::failure(publication_document.error().with_context(
                "final project check before world-position tile publication"));
        }
        if (auto result = production::validate_world_position_source(
                base_recipe.value(), publication_document.value()); !result) {
            return Output::failure(result.error().with_context(
                "final source check before world-position tile publication"));
        }
        const auto final_output_entry = path_entry_exists_no_follow(
            output_directory, "world-position bake output directory");
        if (!final_output_entry) return Output::failure(final_output_entry.error());
        if (final_output_entry.value()) {
            return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
                "world-position output path appeared before publication"));
        }
        if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
            return Output::failure(result.error());
        }
        filesystem_error.clear();
        std::filesystem::rename(staging_directory, output_directory, filesystem_error);
        if (filesystem_error) {
            return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
                "atomic world-position output publication failed; inspect the staging directory"));
        }
    } catch (const std::exception& error) {
        return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
            std::string("world-position tile publication failed: ") + error.what()));
    }

    return Output::success(CpuTextureTileBakeResult{
        std::string(map_kind), output_directory, recipe_digest.value(),
        schedule.value().entries.size(), schedule.value().covered_texels,
        std::nullopt, schedule.value().compile_milliseconds,
        schedule.value().entries.size(), 0U});
}

inline core::Result<CpuTextureTileBakeResult> bake_material_surface_id_tiles(
    const std::filesystem::path& project_path,
    core::Revision expected_project_revision,
    assets::Sha256Digest expected_project_digest,
    const std::filesystem::path& recipe_path,
    std::uint64_t expected_mesh_asset,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    attributes::UvSetId uv_set,
    const std::filesystem::path& output_directory,
    std::stop_token stop_token = {}) {
    using Output = core::Result<CpuTextureTileBakeResult>;
    using namespace bake_workflow_detail;
    constexpr std::string_view map_kind = "material-surface-id";
    if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
        return Output::failure(result.error());
    }
    if (expected_mesh_asset == 0U || uv_set == 0U) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_argument,
            "material/surface ID bake request has an invalid mesh or UV set"));
    }
    if (auto result = require_output_parent(output_directory, map_kind); !result) {
        return Output::failure(result.error());
    }
    const auto output_entry = path_entry_exists_no_follow(
        output_directory, "material/surface ID bake output directory");
    if (!output_entry) return Output::failure(output_entry.error());
    if (output_entry.value()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
            "material/surface ID bake output refuses to overwrite an existing path"));
    }
    const auto recipe_text = read_bounded_regular_text(recipe_path,
        production::kMaxMaterialSurfaceIdRecipeBytes,
        "material/surface ID recipe");
    if (!recipe_text) return Output::failure(recipe_text.error());
    const auto recipe = production::MaterialSurfaceIdRecipe::deserialize(
        recipe_text.value());
    if (!recipe) return Output::failure(recipe.error());

    const auto document = project::ProjectDocument::load(project_path);
    if (!document) return Output::failure(document.error());
    if (document.value().revision() != expected_project_revision ||
        digest_project_snapshot(document.value()) != expected_project_digest) {
        return Output::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "saved project no longer matches the inspected material/surface ID source"));
    }
    if (recipe.value().target_mesh_asset != expected_mesh_asset ||
        recipe.value().target_mesh_revision != expected_mesh_revision ||
        recipe.value().target_mesh_digest != expected_target_digest ||
        recipe.value().target_uv_set != uv_set) {
        return Output::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "material/surface ID recipe does not match the inspected mesh and UV set"));
    }
    if (auto result = production::validate_material_surface_id_source(
            recipe.value(), document.value()); !result) {
        return Output::failure(result.error());
    }
    const auto schedule = production::schedule_material_surface_id_tiles_cpu(
        recipe.value(), document.value(), stop_token);
    if (!schedule) return Output::failure(schedule.error());
    if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
        return Output::failure(result.error());
    }
    if (schedule.value().entries.empty() ||
        schedule.value().entries.size() > production::kMaxBakeTileScheduleTiles) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "material/surface ID scheduler returned an empty or over-budget tile set"));
    }
    const auto recipe_digest = recipe.value().canonical_digest();
    if (!recipe_digest) return Output::failure(recipe_digest.error());
    auto staging_directory = output_directory;
    staging_directory += ".pending-" + recipe_digest.value().hex().substr(0U, 16U);
    const auto staging_entry = path_entry_exists_no_follow(
        staging_directory, "material/surface ID bake staging directory");
    if (!staging_entry) return Output::failure(staging_entry.error());
    if (staging_entry.value()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
            "material/surface ID staging directory already exists and requires inspection"));
    }
    if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
        return Output::failure(result.error());
    }
    std::error_code filesystem_error;
    if (!std::filesystem::create_directory(staging_directory, filesystem_error) ||
        filesystem_error) {
        return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
            "unable to create the new material/surface ID staging directory"));
    }

    try {
        assets::BlobStore blobs(staging_directory / "blobs");
        production::ProductionArtifactStore artifact_store(blobs);
        const production::AdmissionPolicy policy;
        const production::AuthoritativeSource source{
            recipe.value().source_namespace,
            recipe.value().source_revision,
            recipe.value().source_digest};
        const auto current_document = project::ProjectDocument::load(project_path);
        if (!current_document) {
            return Output::failure(current_document.error().with_context(
                "reloading project before material/surface ID publication"));
        }
        if (auto result = production::validate_material_surface_id_source(
                recipe.value(), current_document.value()); !result) {
            return Output::failure(result.error().with_context(
                "checking material/surface ID source after tile compilation"));
        }
        for (const auto& entry : schedule.value().entries) {
            if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
                return Output::failure(result.error());
            }
            if (entry.tile.tile_u != entry.artifact.recipe.tile_u ||
                entry.tile.tile_v != entry.artifact.recipe.tile_v) {
                return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                    "material/surface ID schedule contains a mismatched tile artifact"));
            }
            if (auto result = entry.artifact.validate(); !result) {
                return Output::failure(result.error());
            }
            if (auto result = production::validate_material_surface_id_artifact_source(
                    entry.artifact, current_document.value()); !result) {
                return Output::failure(result.error().with_context(
                    "revalidating material/surface ID tile before publication"));
            }
            const auto payload = entry.artifact.serialize();
            const auto cook_key = production::material_surface_id_cook_key(
                entry.artifact.recipe, "carto.cpu-reference.v1");
            if (!payload || !cook_key) {
                return Output::failure(!payload ? payload.error() : cook_key.error());
            }
            production::ProductionArtifactReceipt receipt;
            receipt.key = cook_key.value();
            receipt.status = production::ReceiptStatus::admitted;
            receipt.output_digest = assets::sha256(payload.value());
            receipt.output_bytes = payload.value().size();
            receipt.cost.cook_milliseconds = entry.compile_milliseconds;
            receipt.cost.disk_bytes = receipt.output_bytes;
            if (auto result = receipt.refresh_digest(); !result) {
                return Output::failure(result.error());
            }
            if (auto result = artifact_store.publish(receipt,
                    std::span<const std::uint8_t>{payload.value()}, source, policy); !result) {
                return Output::failure(result.error());
            }
            const auto reopened = artifact_store.read(cook_key.value(), source, policy);
            const auto reopened_artifact = reopened
                ? production::MaterialSurfaceIdMapArtifact::deserialize(reopened.value())
                : core::Result<production::MaterialSurfaceIdMapArtifact>::failure(
                    reopened.error());
            if (!reopened_artifact || reopened_artifact.value().artifact_digest !=
                    entry.artifact.artifact_digest) {
                return Output::failure(reopened_artifact ? core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "material/surface ID tile differs after artifact read-back")
                    : reopened_artifact.error());
            }
        }
        const std::string manifest_text = artifact_store.ledger().serialize();
        const auto manifest = production::ProductionReceiptLedger::deserialize(
            manifest_text);
        if (!manifest || manifest.value().size() != schedule.value().entries.size()) {
            return Output::failure(manifest ? core::Diagnostic(
                core::ErrorCode::validation_failed,
                "material/surface ID receipt ledger has an unexpected record count")
                : manifest.error());
        }
        const auto manifest_path = staging_directory /
            "receipts.carto-production-receipts";
        const auto manifest_entry = path_entry_exists_no_follow(
            manifest_path, "material/surface ID receipt manifest");
        if (!manifest_entry) return Output::failure(manifest_entry.error());
        if (manifest_entry.value()) {
            return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
                "material/surface ID receipt manifest already exists in staging"));
        }
        if (auto result = write_staging_manifest(
                manifest_path, manifest_text, map_kind); !result) {
            return Output::failure(result.error());
        }
        if (auto result = require_regular_file_no_follow(
                manifest_path, "material/surface ID receipt manifest"); !result) {
            return Output::failure(result.error());
        }
        const auto readback_text = read_bounded_manifest(manifest_path, map_kind);
        const auto manifest_readback = readback_text
            ? production::ProductionReceiptLedger::deserialize(readback_text.value())
            : core::Result<production::ProductionReceiptLedger>::failure(
                readback_text.error());
        if (!manifest_readback ||
            manifest_readback.value().serialize() != manifest_text ||
            manifest_readback.value().size() != schedule.value().entries.size()) {
            return Output::failure(manifest_readback ? core::Diagnostic(
                core::ErrorCode::validation_failed,
                "material/surface ID receipt manifest differs after read-back")
                : manifest_readback.error());
        }
        const auto publication_document = project::ProjectDocument::load(project_path);
        if (!publication_document) {
            return Output::failure(publication_document.error().with_context(
                "final project check before material/surface ID publication"));
        }
        if (auto result = production::validate_material_surface_id_source(
                recipe.value(), publication_document.value()); !result) {
            return Output::failure(result.error().with_context(
                "final source check before material/surface ID publication"));
        }
        const auto final_output_entry = path_entry_exists_no_follow(
            output_directory, "material/surface ID bake output directory");
        if (!final_output_entry) return Output::failure(final_output_entry.error());
        if (final_output_entry.value()) {
            return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
                "material/surface ID output path appeared before publication"));
        }
        if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
            return Output::failure(result.error());
        }
        filesystem_error.clear();
        std::filesystem::rename(staging_directory, output_directory, filesystem_error);
        if (filesystem_error) {
            return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
                "atomic material/surface ID output publication failed; inspect staging"));
        }
    } catch (const std::exception& error) {
        return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
            std::string("material/surface ID tile publication failed: ") + error.what()));
    }

    return Output::success(CpuTextureTileBakeResult{
        std::string(map_kind), output_directory, recipe_digest.value(),
        schedule.value().entries.size(), schedule.value().covered_texels,
        std::nullopt, schedule.value().compile_milliseconds,
        schedule.value().entries.size(), 0U});
}

inline core::Result<CpuTextureTileBakeResult> bake_ambient_occlusion_tiles(
    const std::filesystem::path& project_path,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t sample_count,
    double max_distance,
    std::uint64_t seed,
    const std::filesystem::path& output_directory,
    std::stop_token stop_token = {}) {
    return bake_uv_tile_family<production::AmbientOcclusionRecipe>(
        project_path, mesh_asset, uv_set, expected_mesh_revision,
        expected_target_digest, width, height, output_directory,
        "ambient-occlusion", "bake/ao",
        [sample_count, max_distance, seed](auto& recipe) {
            recipe.sample_count = sample_count;
            recipe.max_distance = max_distance;
            recipe.origin_bias = 1.0e-4;
            recipe.seed = seed;
        },
        [](const production::AmbientOcclusionRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            std::stop_token stop_token) {
            return production::schedule_ambient_occlusion_tiles_cpu(
                recipe, source, target, stop_token);
        },
        [](const production::AmbientOcclusionRecipe& recipe, std::string profile) {
            return production::bake_ambient_occlusion_cook_key(
                recipe, std::move(profile));
        },
        [](production::ProductionArtifactStore& store,
            production::ProductionArtifactReceipt receipt,
            const production::AmbientOcclusionMapArtifact& artifact,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::publish_ambient_occlusion_map(store,
                std::move(receipt), artifact, source, target, policy,
                std::move(profile));
        },
        [](const production::ProductionArtifactStore& store,
            const production::AmbientOcclusionRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::read_ambient_occlusion_map(store, recipe,
                source, target, policy, std::move(profile));
        },
        [](const production::AmbientOcclusionTileScheduleResult& schedule) {
            return completed_tile_schedule_metrics(schedule,
                schedule.covered_texels, schedule.rays_cast,
                schedule.compile_milliseconds);
        }, stop_token);
}

inline core::Result<CpuTextureTileBakeResult> bake_bent_normal_tiles(
    const std::filesystem::path& project_path,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t sample_count,
    double max_distance,
    std::uint64_t seed,
    const std::filesystem::path& output_directory,
    std::stop_token stop_token = {}) {
    return bake_uv_tile_family<production::AmbientOcclusionRecipe>(
        project_path, mesh_asset, uv_set, expected_mesh_revision,
        expected_target_digest, width, height, output_directory,
        "bent-normal", "bake/bent-normal",
        [sample_count, max_distance, seed](auto& recipe) {
            recipe.sample_count = sample_count;
            recipe.max_distance = max_distance;
            recipe.origin_bias = 1.0e-4;
            recipe.seed = seed;
        },
        [](const production::AmbientOcclusionRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            std::stop_token stop_token) {
            return production::schedule_bent_normal_tiles_cpu(
                recipe, source, target, stop_token);
        },
        [](const production::AmbientOcclusionRecipe& recipe, std::string profile) {
            return production::bake_bent_normal_cook_key(
                recipe, std::move(profile));
        },
        [](production::ProductionArtifactStore& store,
            production::ProductionArtifactReceipt receipt,
            const production::BentNormalMapArtifact& artifact,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::publish_bent_normal_map(store,
                std::move(receipt), artifact, source, target, policy,
                std::move(profile));
        },
        [](const production::ProductionArtifactStore& store,
            const production::AmbientOcclusionRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::read_bent_normal_map(store, recipe,
                source, target, policy, std::move(profile));
        },
        [](const production::BentNormalTileScheduleResult& schedule) {
            return completed_tile_schedule_metrics(schedule,
                schedule.covered_texels, schedule.rays_cast,
                schedule.compile_milliseconds);
        }, stop_token);
}

inline core::Result<CpuTextureTileBakeResult> bake_tangent_normal_tiles(
    const std::filesystem::path& project_path,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    std::uint32_t width,
    std::uint32_t height,
    const std::filesystem::path& output_directory,
    std::stop_token stop_token = {},
    const std::filesystem::path& cache_directory = {}) {
    const auto project_path_copy = project_path;
    const auto output_directory_copy = output_directory;
    const auto cache_directory_copy = cache_directory;
    return bake_uv_tile_family<production::BakeRecipe>(
        project_path, mesh_asset, uv_set, expected_mesh_revision,
        expected_target_digest, width, height, output_directory,
        "tangent-normal", "bake/normal",
        [](auto&) {},
        [project_path_copy, output_directory_copy, cache_directory_copy, mesh_asset](
            const production::BakeRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            std::stop_token stop_token) {
            return bake_workflow_detail::schedule_normal_tiles_with_cache(
                project_path_copy, output_directory_copy, cache_directory_copy,
                mesh_asset, recipe, source, target, stop_token);
        },
        [](const production::BakeRecipe& recipe, std::string profile) {
            return production::bake_production_cook_key(
                recipe, production::BakeNormalSpace::tangent, std::move(profile));
        },
        [](production::ProductionArtifactStore& store,
            production::ProductionArtifactReceipt receipt,
            const production::NormalMapArtifact& artifact,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::publish_normal_map(store, std::move(receipt),
                artifact, source, target, policy, std::move(profile));
        },
        [](const production::ProductionArtifactStore& store,
            const production::BakeRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::read_normal_map(store, recipe,
                production::BakeNormalSpace::tangent, source, target,
                policy, std::move(profile));
        },
        [](const production::BakeTileScheduleResult& schedule) {
            CpuTileBakeMetrics metrics;
            for (const auto& entry : schedule.entries) {
                metrics.covered_texels += entry.artifact.covered_texels;
                metrics.compile_milliseconds += entry.compile_milliseconds;
            }
            metrics.compiled_tiles = schedule.compiled_tiles;
            metrics.reused_tiles = schedule.reused_tiles;
            return metrics;
        }, stop_token);
}

inline core::Result<CpuTextureTileBakeResult> bake_curvature_tiles(
    const std::filesystem::path& project_path,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    std::uint32_t width,
    std::uint32_t height,
    double projection_half_extent,
    bool reject_projection_misses,
    double normalization_scale,
    const std::filesystem::path& output_directory,
    std::stop_token stop_token = {}) {
    return bake_uv_tile_family<production::CurvatureRecipe>(
        project_path, mesh_asset, uv_set, expected_mesh_revision,
        expected_target_digest, width, height, output_directory,
        "curvature", "bake/curvature",
        [projection_half_extent, reject_projection_misses,
            normalization_scale](auto& recipe) {
            recipe.binding.projection_half_extent = projection_half_extent;
            recipe.binding.reject_projection_misses = reject_projection_misses;
            recipe.normalization_scale = normalization_scale;
        },
        [](const production::CurvatureRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            std::stop_token stop_token) {
            return production::schedule_curvature_tiles_cpu(
                recipe, source, target, stop_token);
        },
        [](const production::CurvatureRecipe& recipe, std::string profile) {
            return production::bake_curvature_cook_key(recipe, std::move(profile));
        },
        [](production::ProductionArtifactStore& store,
            production::ProductionArtifactReceipt receipt,
            const production::CurvatureMapArtifact& artifact,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::publish_curvature_map(store, std::move(receipt),
                artifact, source, target, policy, std::move(profile));
        },
        [](const production::ProductionArtifactStore& store,
            const production::CurvatureRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::read_curvature_map(store, recipe, source, target,
                policy, std::move(profile));
        },
        [](const production::CurvatureTileScheduleResult& schedule) {
            return completed_tile_schedule_metrics(schedule,
                schedule.covered_texels, std::nullopt,
                schedule.compile_milliseconds);
        }, stop_token);
}

inline core::Result<CpuTextureTileBakeResult> bake_thickness_tiles(
    const std::filesystem::path& project_path,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    std::uint32_t width,
    std::uint32_t height,
    double projection_half_extent,
    bool reject_projection_misses,
    double maximum_distance,
    double surface_bias,
    bool reject_thickness_misses,
    const std::filesystem::path& output_directory,
    std::stop_token stop_token = {}) {
    return bake_uv_tile_family<production::ThicknessRecipe>(
        project_path, mesh_asset, uv_set, expected_mesh_revision,
        expected_target_digest, width, height, output_directory,
        "thickness", "bake/thickness",
        [projection_half_extent, reject_projection_misses, maximum_distance,
            surface_bias, reject_thickness_misses](auto& recipe) {
            recipe.binding.projection_half_extent = projection_half_extent;
            recipe.binding.reject_projection_misses = reject_projection_misses;
            recipe.maximum_distance = maximum_distance;
            recipe.surface_bias = surface_bias;
            recipe.reject_thickness_misses = reject_thickness_misses;
        },
        [](const production::ThicknessRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            std::stop_token stop_token) {
            return production::schedule_thickness_tiles_cpu(
                recipe, source, target, stop_token);
        },
        [](const production::ThicknessRecipe& recipe, std::string profile) {
            return production::bake_thickness_cook_key(recipe, std::move(profile));
        },
        [](production::ProductionArtifactStore& store,
            production::ProductionArtifactReceipt receipt,
            const production::ThicknessMapArtifact& artifact,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::publish_thickness_map(store, std::move(receipt),
                artifact, source, target, policy, std::move(profile));
        },
        [](const production::ProductionArtifactStore& store,
            const production::ThicknessRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::read_thickness_map(store, recipe, source, target,
                policy, std::move(profile));
        },
        [](const production::ThicknessTileScheduleResult& schedule) {
            return completed_tile_schedule_metrics(schedule,
                schedule.covered_texels, std::nullopt,
                schedule.compile_milliseconds);
        }, stop_token);
}

inline core::Result<CpuTextureTileBakeResult> bake_height_displacement_tiles(
    const std::filesystem::path& project_path,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    std::uint32_t width,
    std::uint32_t height,
    double projection_half_extent,
    bool reject_projection_misses,
    double normalization_distance,
    bool reject_clamped_samples,
    const std::filesystem::path& output_directory,
    std::stop_token stop_token = {}) {
    return bake_uv_tile_family<production::HeightDisplacementRecipe>(
        project_path, mesh_asset, uv_set, expected_mesh_revision,
        expected_target_digest, width, height, output_directory,
        "height-displacement", "bake/height-displacement",
        [projection_half_extent, reject_projection_misses,
            normalization_distance, reject_clamped_samples](auto& recipe) {
            recipe.binding.projection_half_extent = projection_half_extent;
            recipe.binding.reject_projection_misses = reject_projection_misses;
            recipe.normalization_distance = normalization_distance;
            recipe.reject_clamped_samples = reject_clamped_samples;
        },
        [](const production::HeightDisplacementRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            std::stop_token stop_token) {
            return production::schedule_height_displacement_tiles_cpu(
                recipe, source, target, stop_token);
        },
        [](const production::HeightDisplacementRecipe& recipe, std::string profile) {
            return production::bake_height_displacement_cook_key(
                recipe, std::move(profile));
        },
        [](production::ProductionArtifactStore& store,
            production::ProductionArtifactReceipt receipt,
            const production::HeightDisplacementMapArtifact& artifact,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::publish_height_displacement_map(store,
                std::move(receipt), artifact, source, target, policy,
                std::move(profile));
        },
        [](const production::ProductionArtifactStore& store,
            const production::HeightDisplacementRecipe& recipe,
            const geometry::EditableMesh& source,
            const geometry::EditableMesh& target,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::read_height_displacement_map(store, recipe,
                source, target, policy, std::move(profile));
        },
        [](const production::HeightDisplacementTileScheduleResult& schedule) {
            return completed_tile_schedule_metrics(schedule,
                schedule.covered_texels, std::nullopt,
                schedule.compile_milliseconds);
        }, stop_token);
}

#if defined(CARTO_DESKTOP_HAS_D3D12_BAKE)
namespace d3d12_bake_workflow_detail {

inline core::Result<void> preflight_output_directory(
    const std::filesystem::path& output_directory,
    std::string_view map_kind) {
    if (auto result = bake_workflow_detail::require_output_parent(
            output_directory, map_kind); !result) {
        return result;
    }
    const auto output_entry = bake_workflow_detail::path_entry_exists_no_follow(
        output_directory, std::string(map_kind) + " D3D12 bake output");
    if (!output_entry) return core::Result<void>::failure(output_entry.error());
    if (output_entry.value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            std::string(map_kind) + " bake output refuses to overwrite an existing path"));
    }
    return core::Result<void>::success();
}

inline core::Result<project::ProjectDocument> load_inspected_mesh(
    const std::filesystem::path& project_path,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    std::string_view map_kind) {
    using Output = core::Result<project::ProjectDocument>;
    if (mesh_asset == 0U || uv_set == 0U) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_argument,
            std::string(map_kind) + " D3D12 bake has an invalid mesh or UV set"));
    }
    auto document = project::ProjectDocument::load(project_path);
    if (!document) return Output::failure(document.error());
    const auto mesh = document.value().meshes().find(mesh_asset);
    if (mesh == document.value().meshes().end()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::not_found,
            std::string(map_kind) + " D3D12 bake mesh is absent from the saved project"));
    }
    const auto digest = production::bake_mesh_target_digest(mesh->second, uv_set);
    if (!digest || mesh->second.revision() != expected_mesh_revision ||
        digest.value() != expected_target_digest) {
        return Output::failure(digest ? core::Diagnostic(core::ErrorCode::stale_data,
            "saved UV inspection no longer matches the D3D12 bake mesh and UV source")
            : digest.error());
    }
    return Output::success(std::move(document.value()));
}

template <typename Schedule, typename Recipe, typename ArtifactFor,
    typename ReportFor, typename ProfileFor, typename KeyForRecipe,
    typename PublishArtifact, typename ReadArtifact, typename ValidateArtifact,
    typename ReadMetrics>
core::Result<CpuTextureTileBakeResult> publish_tile_schedule(
    const std::filesystem::path& project_path,
    const std::string& expected_source_namespace,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    const std::filesystem::path& output_directory,
    std::string_view map_kind,
    const Recipe& base_recipe,
    const Schedule& schedule,
    ArtifactFor&& artifact_for,
    ReportFor&& report_for,
    ProfileFor&& profile_for,
    KeyForRecipe&& key_for_recipe,
    PublishArtifact&& publish_artifact,
    ReadArtifact&& read_artifact,
    ValidateArtifact&& validate_artifact,
    ReadMetrics&& read_metrics,
    std::stop_token stop_token) {
    using Output = core::Result<CpuTextureTileBakeResult>;
    using namespace bake_workflow_detail;
    if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
        return Output::failure(result.error());
    }
    if (schedule.entries.empty() ||
        schedule.entries.size() > production::kMaxBakeTileScheduleTiles) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            std::string(map_kind) + " D3D12 scheduler returned an invalid tile set"));
    }
    if (auto result = preflight_output_directory(output_directory, map_kind); !result) {
        return Output::failure(result.error());
    }
    const auto original_document = load_inspected_mesh(
        project_path, mesh_asset, uv_set, expected_mesh_revision,
        expected_target_digest, map_kind);
    if (!original_document) return Output::failure(original_document.error());
    if (original_document.value().source_namespace() != expected_source_namespace) {
        return Output::failure(core::Diagnostic(core::ErrorCode::stale_data,
            std::string(map_kind) + " project source namespace changed during D3D12 work"));
    }
    const auto original_mesh = original_document.value().meshes().find(mesh_asset);
    if (auto result = validate_artifact(base_recipe,
            original_mesh->second, original_mesh->second); !result) {
        return Output::failure(result.error().with_context(
            "validating D3D12 bake source before publication"));
    }
    const CpuTileBakeMetrics metrics = read_metrics(schedule);
    const auto recipe_digest = base_recipe.canonical_digest();
    if (!recipe_digest) return Output::failure(recipe_digest.error());

    auto staging_directory = output_directory;
    staging_directory += ".pending-" + recipe_digest.value().hex().substr(0U, 16U);
    const auto staging_entry = path_entry_exists_no_follow(
        staging_directory, std::string(map_kind) + " D3D12 staging directory");
    if (!staging_entry) return Output::failure(staging_entry.error());
    if (staging_entry.value()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
            std::string(map_kind) +
                " D3D12 staging directory already exists and requires inspection"));
    }
    if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
        return Output::failure(result.error());
    }
    std::error_code filesystem_error;
    if (!std::filesystem::create_directory(staging_directory, filesystem_error) ||
        filesystem_error) {
        return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
            "unable to create the new D3D12 bake staging directory"));
    }

    try {
        assets::BlobStore blobs(staging_directory / "blobs");
        production::ProductionArtifactStore artifact_store(blobs);
        const production::AdmissionPolicy policy;
        std::vector<assets::Sha256Digest> receipt_digests;
        receipt_digests.reserve(schedule.entries.size());
        std::optional<std::string> platform_profile;
        for (const auto& entry : schedule.entries) {
            if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
                return Output::failure(result.error());
            }
            const auto& artifact = artifact_for(entry);
            const auto& report = report_for(entry);
            const auto profile = profile_for(report);
            if (!profile) return Output::failure(profile.error());
            if (!report.projection.has_value()) {
                return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                    "D3D12 tile report lacks projection worker provenance"));
            }
            if (!platform_profile.has_value()) {
                platform_profile = profile.value();
            } else if (*platform_profile != profile.value()) {
                return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                    "D3D12 tile schedule mixes worker platform profiles"));
            }
            const auto payload = artifact.serialize();
            const auto cook_key = key_for_recipe(artifact.recipe, profile.value());
            if (!payload || !cook_key) {
                return Output::failure(!payload ? payload.error() : cook_key.error());
            }
            production::ProductionArtifactReceipt receipt;
            receipt.key = cook_key.value();
            receipt.status = production::ReceiptStatus::admitted;
            receipt.output_digest = assets::sha256(payload.value());
            receipt.output_bytes = payload.value().size();
            receipt.quality.measured_error = 0.0;
            receipt.quality.importance = 1.0;
            receipt.quality.confidence = 1.0;
            receipt.cost.cook_milliseconds = entry.compile_milliseconds;
            receipt.cost.gpu_milliseconds = static_cast<double>(
                report.projection->wall_time_nanoseconds) / 1'000'000.0;
            receipt.cost.disk_bytes = receipt.output_bytes;
            if (auto result = receipt.refresh_digest(); !result) {
                return Output::failure(result.error());
            }
            if (auto result = publish_artifact(
                    artifact_store, receipt, report, artifact,
                    original_mesh->second, original_mesh->second,
                    policy, profile.value()); !result) {
                return Output::failure(result.error());
            }
            const auto reopened = read_artifact(
                artifact_store, artifact.recipe, original_mesh->second,
                original_mesh->second, policy, profile.value());
            if (!reopened || reopened.value().artifact_digest != artifact.artifact_digest) {
                return Output::failure(reopened ? core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    std::string(map_kind) +
                        " D3D12 artifact differs after production-store read-back")
                    : reopened.error());
            }
            receipt_digests.push_back(receipt.receipt_digest);
        }

        const std::string manifest_text = artifact_store.ledger().serialize();
        const auto manifest = production::ProductionReceiptLedger::deserialize(manifest_text);
        if (!manifest || manifest.value().size() != receipt_digests.size()) {
            return Output::failure(manifest ? core::Diagnostic(
                core::ErrorCode::validation_failed,
                std::string(map_kind) + " D3D12 receipt ledger count changed")
                : manifest.error());
        }
        for (std::size_t index = 0U; index < receipt_digests.size(); ++index) {
            if (manifest.value().records()[index].receipt_digest != receipt_digests[index]) {
                return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                    "D3D12 receipt ledger order changed during read-back"));
            }
        }
        const auto manifest_path = staging_directory /
            "receipts.carto-production-receipts";
        const auto manifest_entry = path_entry_exists_no_follow(
            manifest_path, std::string(map_kind) + " D3D12 receipt manifest");
        if (!manifest_entry) return Output::failure(manifest_entry.error());
        if (manifest_entry.value()) {
            return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
                "D3D12 receipt manifest path already exists in staging"));
        }
        if (auto result = write_staging_manifest(
                manifest_path, manifest_text, map_kind); !result) {
            return Output::failure(result.error());
        }
        if (auto result = require_regular_file_no_follow(
                manifest_path, std::string(map_kind) + " receipt manifest"); !result) {
            return Output::failure(result.error());
        }
        const auto readback_text = read_bounded_manifest(manifest_path, map_kind);
        const auto readback = readback_text
            ? production::ProductionReceiptLedger::deserialize(readback_text.value())
            : core::Result<production::ProductionReceiptLedger>::failure(
                readback_text.error());
        if (!readback || readback.value().serialize() != manifest_text ||
            readback.value().size() != receipt_digests.size()) {
            return Output::failure(readback ? core::Diagnostic(
                core::ErrorCode::validation_failed,
                "D3D12 receipt manifest differs after file read-back")
                : readback.error());
        }
        for (std::size_t index = 0U; index < receipt_digests.size(); ++index) {
            if (readback.value().records()[index].receipt_digest != receipt_digests[index]) {
                return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                    "D3D12 receipt manifest order differs after file read-back"));
            }
        }

        if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
            return Output::failure(result.error());
        }
        const auto publication_document = load_inspected_mesh(
            project_path, mesh_asset, uv_set, expected_mesh_revision,
            expected_target_digest, map_kind);
        if (!publication_document) {
            return Output::failure(publication_document.error().with_context(
                "final source check before D3D12 tile publication"));
        }
        if (publication_document.value().source_namespace() != expected_source_namespace) {
            return Output::failure(core::Diagnostic(core::ErrorCode::stale_data,
                std::string(map_kind) +
                    " project source namespace changed before D3D12 publication"));
        }
        const auto publication_mesh = publication_document.value().meshes().find(mesh_asset);
        for (const auto& entry : schedule.entries) {
            if (auto result = validate_artifact(
                    artifact_for(entry).recipe, publication_mesh->second,
                    publication_mesh->second); !result) {
                return Output::failure(result.error().with_context(
                    "revalidating D3D12 tile source before final publication"));
            }
        }
        const auto final_output = path_entry_exists_no_follow(
            output_directory, std::string(map_kind) + " D3D12 bake output");
        if (!final_output) return Output::failure(final_output.error());
        if (final_output.value()) {
            return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
                std::string(map_kind) + " bake output path appeared before publication"));
        }
        if (auto result = require_not_cancelled(stop_token, map_kind); !result) {
            return Output::failure(result.error());
        }
        filesystem_error.clear();
        std::filesystem::rename(staging_directory, output_directory, filesystem_error);
        if (filesystem_error) {
            return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
                "atomic D3D12 bake publication failed; inspect the staging directory"));
        }
    } catch (const std::exception& error) {
        return Output::failure(core::Diagnostic(core::ErrorCode::io_error,
            std::string(map_kind) + " D3D12 bake publication failed: " + error.what()));
    }
    return Output::success(CpuTextureTileBakeResult{
        std::string(map_kind), output_directory, recipe_digest.value(),
        schedule.entries.size(), metrics.covered_texels, metrics.rays_cast,
        metrics.compile_milliseconds, metrics.compiled_tiles,
        metrics.reused_tiles});
}

} // namespace d3d12_bake_workflow_detail

inline core::Result<CpuTextureTileBakeResult> bake_tangent_normal_tiles_d3d12(
    const std::filesystem::path& project_path,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    std::uint32_t width,
    std::uint32_t height,
    const std::filesystem::path& output_directory,
    std::stop_token stop_token = {}) {
    using Output = core::Result<CpuTextureTileBakeResult>;
    using namespace d3d12_bake_workflow_detail;
    if (stop_token.stop_requested()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
            "tangent-normal D3D12 bake cancelled before device creation"));
    }
    if (auto result = preflight_output_directory(output_directory, "tangent-normal");
        !result) {
        return Output::failure(result.error());
    }
    const auto document = load_inspected_mesh(project_path, mesh_asset, uv_set,
        expected_mesh_revision, expected_target_digest, "tangent-normal");
    if (!document) return Output::failure(document.error());
    const auto mesh = document.value().meshes().find(mesh_asset);
    const std::string source_namespace = document.value().source_namespace();
    const std::string identity = source_namespace + "/mesh/" +
        std::to_string(mesh_asset);
    const auto source = production::bind_bake_source(identity, mesh->second);
    const auto target = production::bind_bake_target(identity, mesh->second, uv_set);
    if (!source || !target) {
        return Output::failure(!source ? source.error() : target.error());
    }
    production::BakeRecipe recipe;
    recipe.identity = "bake/normal/mesh/" + std::to_string(mesh_asset) +
        "/uv/" + std::to_string(uv_set);
    recipe.source = source.value();
    recipe.target = target.value();
    recipe.target_uv_set = uv_set;
    recipe.width = width;
    recipe.height = height;
    if (auto result = recipe.validate(); !result) return Output::failure(result.error());
    const auto device = d3d12::D3D12Device::create();
    if (!device) return Output::failure(device.error().with_context(
        "creating the desktop D3D12 normal-map worker"));
    const auto schedule = d3d12::schedule_tangent_normal_map_tiles_d3d12(
        *device.value(), recipe, mesh->second, mesh->second, stop_token);
    if (!schedule) return Output::failure(schedule.error());
    return publish_tile_schedule(
        project_path, source_namespace, mesh_asset, uv_set, expected_mesh_revision,
        expected_target_digest, output_directory, "tangent-normal", recipe,
        schedule.value(),
        [](const d3d12::D3D12NormalMapTileScheduleEntry& entry)
            -> const production::NormalMapArtifact& { return *entry.bake.bake.artifact; },
        [](const d3d12::D3D12NormalMapTileScheduleEntry& entry)
            -> const d3d12::D3D12NormalMapBakeReport& { return entry.bake; },
        [](const d3d12::D3D12NormalMapBakeReport& report)
            -> core::Result<std::string> {
            if (!report.projection.has_value()) {
                return core::Result<std::string>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "D3D12 normal-map tile lacks projection provenance"));
            }
            return d3d12::d3d12_bake_platform_profile(*report.projection);
        },
        [](const production::BakeRecipe& tile_recipe, std::string_view profile) {
            return production::bake_production_cook_key(
                tile_recipe, production::BakeNormalSpace::tangent, profile);
        },
        [](production::ProductionArtifactStore& store,
            production::ProductionArtifactReceipt receipt,
            const d3d12::D3D12NormalMapBakeReport& report,
            const production::NormalMapArtifact&,
            const geometry::EditableMesh& source_mesh,
            const geometry::EditableMesh& target_mesh,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return d3d12::publish_tangent_normal_map_d3d12(
                store, std::move(receipt), report, source_mesh, target_mesh,
                policy, std::move(profile));
        },
        [](const production::ProductionArtifactStore& store,
            const production::BakeRecipe& tile_recipe,
            const geometry::EditableMesh& source_mesh,
            const geometry::EditableMesh& target_mesh,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::read_normal_map(store, tile_recipe,
                production::BakeNormalSpace::tangent, source_mesh, target_mesh,
                policy, std::move(profile));
        },
        [](const production::BakeRecipe& tile_recipe,
            const geometry::EditableMesh& source_mesh,
            const geometry::EditableMesh& target_mesh) {
            return production::validate_bake_sources(
                tile_recipe, source_mesh, target_mesh);
        },
        [](const d3d12::D3D12NormalMapTileScheduleResult& result) {
            CpuTileBakeMetrics metrics = completed_tile_schedule_metrics(
                result, 0U, std::nullopt, result.compile_milliseconds);
            for (const auto& entry : result.entries) {
                metrics.covered_texels += entry.bake.bake.artifact->covered_texels;
            }
            return metrics;
        }, stop_token);
}

inline core::Result<CpuTextureTileBakeResult> bake_ambient_occlusion_tiles_d3d12(
    const std::filesystem::path& project_path,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t sample_count,
    double max_distance,
    std::uint64_t seed,
    const std::filesystem::path& output_directory,
    std::stop_token stop_token = {}) {
    using Output = core::Result<CpuTextureTileBakeResult>;
    using namespace d3d12_bake_workflow_detail;
    if (stop_token.stop_requested()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
            "ambient-occlusion D3D12 bake cancelled before device creation"));
    }
    if (auto result = preflight_output_directory(output_directory, "ambient-occlusion");
        !result) {
        return Output::failure(result.error());
    }
    const auto document = load_inspected_mesh(project_path, mesh_asset, uv_set,
        expected_mesh_revision, expected_target_digest, "ambient-occlusion");
    if (!document) return Output::failure(document.error());
    const auto mesh = document.value().meshes().find(mesh_asset);
    const std::string source_namespace = document.value().source_namespace();
    const std::string identity = source_namespace + "/mesh/" +
        std::to_string(mesh_asset);
    const auto source = production::bind_bake_source(identity, mesh->second);
    const auto target = production::bind_bake_target(identity, mesh->second, uv_set);
    if (!source || !target) {
        return Output::failure(!source ? source.error() : target.error());
    }
    production::AmbientOcclusionRecipe recipe;
    recipe.binding.identity = "bake/ao/mesh/" + std::to_string(mesh_asset) +
        "/uv/" + std::to_string(uv_set);
    recipe.binding.source = source.value();
    recipe.binding.target = target.value();
    recipe.binding.target_uv_set = uv_set;
    recipe.binding.width = width;
    recipe.binding.height = height;
    recipe.sample_count = sample_count;
    recipe.max_distance = max_distance;
    recipe.origin_bias = 1.0e-4;
    recipe.seed = seed;
    if (auto result = recipe.validate(); !result) return Output::failure(result.error());
    const auto device = d3d12::D3D12Device::create();
    if (!device) return Output::failure(device.error().with_context(
        "creating the desktop D3D12 AO worker"));
    const auto schedule = d3d12::schedule_ambient_occlusion_tiles_d3d12(
        *device.value(), recipe, mesh->second, mesh->second, stop_token);
    if (!schedule) return Output::failure(schedule.error());
    return publish_tile_schedule(
        project_path, source_namespace, mesh_asset, uv_set, expected_mesh_revision,
        expected_target_digest, output_directory, "ambient-occlusion", recipe,
        schedule.value(),
        [](const d3d12::D3D12AmbientOcclusionTileScheduleEntry& entry)
            -> const production::AmbientOcclusionMapArtifact& {
            return *entry.bake.bake.artifact;
        },
        [](const d3d12::D3D12AmbientOcclusionTileScheduleEntry& entry)
            -> const d3d12::D3D12AmbientOcclusionBakeReport& { return entry.bake; },
        [](const d3d12::D3D12AmbientOcclusionBakeReport& report)
            -> core::Result<std::string> {
            if (!report.projection.has_value()) {
                return core::Result<std::string>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "D3D12 AO tile lacks projection provenance"));
            }
            return d3d12::d3d12_bake_platform_profile(*report.projection);
        },
        [](const production::AmbientOcclusionRecipe& tile_recipe,
            std::string_view profile) {
            return production::bake_ambient_occlusion_cook_key(tile_recipe, profile);
        },
        [](production::ProductionArtifactStore& store,
            production::ProductionArtifactReceipt receipt,
            const d3d12::D3D12AmbientOcclusionBakeReport& report,
            const production::AmbientOcclusionMapArtifact&,
            const geometry::EditableMesh& source_mesh,
            const geometry::EditableMesh& target_mesh,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return d3d12::publish_ambient_occlusion_map_d3d12(
                store, std::move(receipt), report, source_mesh, target_mesh,
                policy, std::move(profile));
        },
        [](const production::ProductionArtifactStore& store,
            const production::AmbientOcclusionRecipe& tile_recipe,
            const geometry::EditableMesh& source_mesh,
            const geometry::EditableMesh& target_mesh,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::read_ambient_occlusion_map(store, tile_recipe,
                source_mesh, target_mesh, policy, std::move(profile));
        },
        [](const production::AmbientOcclusionRecipe& tile_recipe,
            const geometry::EditableMesh& source_mesh,
            const geometry::EditableMesh& target_mesh) {
            return production::validate_bake_sources(
                tile_recipe.binding, source_mesh, target_mesh);
        },
        [](const d3d12::D3D12AmbientOcclusionTileScheduleResult& result) {
            return completed_tile_schedule_metrics(result,
                result.covered_texels, result.rays_cast,
                result.compile_milliseconds);
        }, stop_token);
}

inline core::Result<CpuTextureTileBakeResult> bake_bent_normal_tiles_d3d12(
    const std::filesystem::path& project_path,
    std::uint64_t mesh_asset,
    attributes::UvSetId uv_set,
    core::Revision expected_mesh_revision,
    assets::Sha256Digest expected_target_digest,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t sample_count,
    double max_distance,
    std::uint64_t seed,
    const std::filesystem::path& output_directory,
    std::stop_token stop_token = {}) {
    using Output = core::Result<CpuTextureTileBakeResult>;
    using namespace d3d12_bake_workflow_detail;
    if (stop_token.stop_requested()) {
        return Output::failure(core::Diagnostic(core::ErrorCode::invalid_state,
            "bent-normal D3D12 bake cancelled before device creation"));
    }
    if (auto result = preflight_output_directory(output_directory, "bent-normal");
        !result) {
        return Output::failure(result.error());
    }
    const auto document = load_inspected_mesh(project_path, mesh_asset, uv_set,
        expected_mesh_revision, expected_target_digest, "bent-normal");
    if (!document) return Output::failure(document.error());
    const auto mesh = document.value().meshes().find(mesh_asset);
    const std::string source_namespace = document.value().source_namespace();
    const std::string identity = source_namespace + "/mesh/" +
        std::to_string(mesh_asset);
    const auto source = production::bind_bake_source(identity, mesh->second);
    const auto target = production::bind_bake_target(identity, mesh->second, uv_set);
    if (!source || !target) {
        return Output::failure(!source ? source.error() : target.error());
    }
    production::AmbientOcclusionRecipe recipe;
    recipe.binding.identity = "bake/bent-normal/mesh/" +
        std::to_string(mesh_asset) + "/uv/" + std::to_string(uv_set);
    recipe.binding.source = source.value();
    recipe.binding.target = target.value();
    recipe.binding.target_uv_set = uv_set;
    recipe.binding.width = width;
    recipe.binding.height = height;
    recipe.sample_count = sample_count;
    recipe.max_distance = max_distance;
    recipe.origin_bias = 1.0e-4;
    recipe.seed = seed;
    if (auto result = recipe.validate(); !result) return Output::failure(result.error());
    const auto device = d3d12::D3D12Device::create();
    if (!device) return Output::failure(device.error().with_context(
        "creating the desktop D3D12 bent-normal worker"));
    const auto schedule = d3d12::schedule_bent_normal_tiles_d3d12(
        *device.value(), recipe, mesh->second, mesh->second, stop_token);
    if (!schedule) return Output::failure(schedule.error());
    return publish_tile_schedule(
        project_path, source_namespace, mesh_asset, uv_set, expected_mesh_revision,
        expected_target_digest, output_directory, "bent-normal", recipe,
        schedule.value(),
        [](const d3d12::D3D12BentNormalTileScheduleEntry& entry)
            -> const production::BentNormalMapArtifact& {
            return *entry.bake.bake.artifact;
        },
        [](const d3d12::D3D12BentNormalTileScheduleEntry& entry)
            -> const d3d12::D3D12BentNormalBakeReport& { return entry.bake; },
        [](const d3d12::D3D12BentNormalBakeReport& report)
            -> core::Result<std::string> {
            if (!report.projection.has_value()) {
                return core::Result<std::string>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "D3D12 bent-normal tile lacks projection provenance"));
            }
            return d3d12::d3d12_bake_platform_profile(*report.projection);
        },
        [](const production::AmbientOcclusionRecipe& tile_recipe,
            std::string_view profile) {
            return production::bake_bent_normal_cook_key(tile_recipe, profile);
        },
        [](production::ProductionArtifactStore& store,
            production::ProductionArtifactReceipt receipt,
            const d3d12::D3D12BentNormalBakeReport& report,
            const production::BentNormalMapArtifact&,
            const geometry::EditableMesh& source_mesh,
            const geometry::EditableMesh& target_mesh,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return d3d12::publish_bent_normal_map_d3d12(
                store, std::move(receipt), report, source_mesh, target_mesh,
                policy, std::move(profile));
        },
        [](const production::ProductionArtifactStore& store,
            const production::AmbientOcclusionRecipe& tile_recipe,
            const geometry::EditableMesh& source_mesh,
            const geometry::EditableMesh& target_mesh,
            const production::AdmissionPolicy& policy,
            std::string profile) {
            return production::read_bent_normal_map(store, tile_recipe,
                source_mesh, target_mesh, policy, std::move(profile));
        },
        [](const production::AmbientOcclusionRecipe& tile_recipe,
            const geometry::EditableMesh& source_mesh,
            const geometry::EditableMesh& target_mesh) {
            return production::validate_bake_sources(
                tile_recipe.binding, source_mesh, target_mesh);
        },
        [](const d3d12::D3D12BentNormalTileScheduleResult& result) {
            return completed_tile_schedule_metrics(result,
                result.covered_texels, result.rays_cast,
                result.compile_milliseconds);
        }, stop_token);
}
#endif

} // namespace carto::desktop
