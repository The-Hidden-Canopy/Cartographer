#include <carto/production/uv_island_pack.hpp>

#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace carto::production {
namespace {

constexpr std::string_view kRecipeMagic = "CARTO_UV_ISLAND_PACK_RECIPE 2";
constexpr std::string_view kOutputMagic = "CARTO_UV_ISLAND_PACK_OUTPUT 1";
constexpr std::string_view kTileMagic = "CARTO_UV_ISLAND_PACK_TILE 1";
constexpr std::string_view kArtifactMagic = "CARTO_UV_ISLAND_PACK_ARTIFACT 2";
constexpr double kGeometryEpsilon = 1e-12;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\n' && byte != '\r';
        });
}

std::string mesh_identity(std::string_view project_namespace, std::uint64_t mesh_asset) {
    return std::string(project_namespace) + "/mesh/" + std::to_string(mesh_asset);
}

std::string encode_hex(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded;
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_f64(std::vector<std::uint8_t>& bytes, double value) {
    append_u64(bytes, std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
}

void append_text(std::vector<std::uint8_t>& bytes, std::string_view value) {
    append_u64(bytes, static_cast<std::uint64_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void append_digest(std::vector<std::uint8_t>& bytes, const assets::Sha256Digest& digest) {
    bytes.insert(bytes.end(), digest.bytes.begin(), digest.bytes.end());
}

assets::Sha256Digest hash_text(std::string_view value) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(value.data());
    return assets::sha256(std::span<const std::uint8_t>(data, value.size()));
}

std::string base64url(const assets::Sha256Digest& digest) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string encoded;
    encoded.reserve(43U);
    std::uint32_t accumulator = 0U;
    unsigned bits = 0U;
    for (const std::uint8_t byte : digest.bytes) {
        accumulator = (accumulator << 8U) | byte;
        bits += 8U;
        while (bits >= 6U) {
            bits -= 6U;
            encoded.push_back(alphabet[(accumulator >> bits) & 0x3fU]);
        }
    }
    if (bits != 0U) {
        encoded.push_back(alphabet[(accumulator << (6U - bits)) & 0x3fU]);
    }
    return encoded;
}

bool same_uv(core::Vec2d left, core::Vec2d right) {
    return left.x == right.x && left.y == right.y;
}

struct EdgeKey {
    geometry::VertexId first;
    geometry::VertexId second;

    [[nodiscard]] bool operator<(const EdgeKey& other) const noexcept {
        return std::tie(first, second) < std::tie(other.first, other.second);
    }
};

struct EdgeUse {
    geometry::FaceId face;
    core::Vec2d first_uv;
    core::Vec2d second_uv;
};

struct FaceChart {
    geometry::FaceId stable_face;
    std::vector<UvIslandPackCorner> corners;
    double minimum_u = 0.0;
    double minimum_v = 0.0;
    double width = 0.0;
    double height = 0.0;
    double density_scale = 1.0;
};

struct Placement {
    std::uint32_t tile_index = 0U;
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    bool rotated_clockwise = false;
};

struct PackState {
    std::uint64_t cursor_x = 0U;
    std::uint64_t cursor_y = 0U;
    std::uint64_t shelf_height = 0U;
};

std::optional<std::vector<Placement>> pack_shelves(
    const std::vector<FaceChart>& charts,
    const std::vector<std::size_t>& order,
    double scale,
    std::uint32_t resolution,
    std::uint32_t padding,
    std::uint32_t maximum_tiles,
    bool allow_quarter_turn_rotation) {
    std::vector<Placement> placements(charts.size());
    std::vector<PackState> states;
    for (const std::size_t chart_index : order) {
        const FaceChart& chart = charts[chart_index];
        const double scaled_width = std::ceil(chart.width * scale);
        const double scaled_height = std::ceil(chart.height * scale);
        if (!std::isfinite(scaled_width) || !std::isfinite(scaled_height) ||
            scaled_width > static_cast<double>(resolution) ||
            scaled_height > static_cast<double>(resolution)) {
            return std::nullopt;
        }
        const std::uint64_t chart_width = std::max<std::uint64_t>(
            1U, static_cast<std::uint64_t>(scaled_width));
        const std::uint64_t chart_height = std::max<std::uint64_t>(
            1U, static_cast<std::uint64_t>(scaled_height));
        bool placed = false;
        for (std::size_t tile_index = 0U; tile_index < states.size(); ++tile_index) {
            struct Candidate {
                PackState state;
                Placement placement;
                std::uint64_t area_score = 0U;
                std::uint64_t right_edge = 0U;
                std::uint64_t top_edge = 0U;
            };
            std::optional<Candidate> best;
            const auto consider = [&](bool rotated) {
                const std::uint64_t oriented_width = rotated ? chart_height : chart_width;
                const std::uint64_t oriented_height = rotated ? chart_width : chart_height;
                const std::uint64_t oriented_padded_width = oriented_width + 2ULL * padding;
                const std::uint64_t oriented_padded_height = oriented_height + 2ULL * padding;
                if (oriented_padded_width > resolution ||
                    oriented_padded_height > resolution) return;
                PackState candidate = states[tile_index];
                if (candidate.cursor_x + oriented_padded_width > resolution) {
                    candidate.cursor_y += candidate.shelf_height;
                    candidate.cursor_x = 0U;
                    candidate.shelf_height = 0U;
                }
                if (candidate.cursor_y + oriented_padded_height > resolution) return;
                const std::uint64_t right_edge = candidate.cursor_x + oriented_padded_width;
                const std::uint64_t top_edge = candidate.cursor_y + std::max(
                    candidate.shelf_height, oriented_padded_height);
                Candidate option{
                    candidate,
                    Placement{
                        static_cast<std::uint32_t>(tile_index),
                        static_cast<std::uint32_t>(candidate.cursor_x + padding),
                        static_cast<std::uint32_t>(candidate.cursor_y + padding),
                        rotated},
                    right_edge * top_edge,
                    right_edge,
                    top_edge};
                option.state.cursor_x += oriented_padded_width;
                option.state.shelf_height = std::max(
                    option.state.shelf_height, oriented_padded_height);
                const auto score = [](const Candidate& value) {
                    return std::tuple{
                        value.area_score, value.right_edge, value.top_edge,
                        value.placement.rotated_clockwise};
                };
                if (!best || score(option) < score(*best)) best = std::move(option);
            };
            consider(false);
            if (allow_quarter_turn_rotation && chart_width != chart_height) {
                consider(true);
            }
            if (best) {
                placements[chart_index] = best->placement;
                states[tile_index] = best->state;
                placed = true;
                break;
            }
        }
        if (placed) continue;
        if (states.size() >= maximum_tiles) return std::nullopt;

        struct FirstChartCandidate {
            PackState state;
            Placement placement;
            std::uint64_t area_score = 0U;
            std::uint64_t right_edge = 0U;
            std::uint64_t top_edge = 0U;
        };
        std::optional<FirstChartCandidate> first_chart;
        const auto consider_first = [&](bool rotated) {
            const std::uint64_t oriented_width = rotated ? chart_height : chart_width;
            const std::uint64_t oriented_height = rotated ? chart_width : chart_height;
            const std::uint64_t oriented_padded_width = oriented_width + 2ULL * padding;
            const std::uint64_t oriented_padded_height = oriented_height + 2ULL * padding;
            if (oriented_padded_width > resolution || oriented_padded_height > resolution) return;
            FirstChartCandidate option{
                PackState{oriented_padded_width, 0U, oriented_padded_height},
                Placement{
                    static_cast<std::uint32_t>(states.size()), padding, padding, rotated},
                oriented_padded_width * oriented_padded_height,
                oriented_padded_width,
                oriented_padded_height};
            const auto score = [](const FirstChartCandidate& value) {
                return std::tuple{
                    value.area_score, value.right_edge, value.top_edge,
                    value.placement.rotated_clockwise};
            };
            if (!first_chart || score(option) < score(*first_chart)) {
                first_chart = std::move(option);
            }
        };
        consider_first(false);
        if (allow_quarter_turn_rotation && chart_width != chart_height) {
            consider_first(true);
        }
        if (!first_chart) return std::nullopt;
        placements[chart_index] = first_chart->placement;
        states.push_back(first_chart->state);
    }
    return placements;
}

core::Result<UvIslandPackArtifact> compile_canonical(
    const UvIslandPackRecipe& recipe,
    const geometry::EditableMesh& source);

} // namespace

core::Result<void> UvIslandPackRecipe::validate() const {
    if (!safe_text(project_namespace, 1024U) || mesh_asset == 0U ||
        source.identity != mesh_identity(project_namespace, mesh_asset)) {
        return core::Result<void>::failure(invalid(
            "UV island recipe requires a stable project namespace and mesh identity"));
    }
    if (auto result = source.validate(); !result) return result;
    if (source_uv_set == 0U || output_uv_set == 0U || source_uv_set == output_uv_set ||
        !safe_text(output_name, 128U) || !safe_text(output_layer_id, 128U)) {
        return core::Result<void>::failure(invalid(
            "UV island recipe requires distinct source and output UV identities"));
    }
    if (resolution < 16U || resolution > kMaxBakeTextureExtent ||
        padding_texels == 0U || padding_texels > resolution / 8U ||
        static_cast<std::uint64_t>(padding_texels) * 2ULL >= resolution ||
        maximum_udim_tiles == 0U || maximum_udim_tiles > kMaxUvIslandPackTiles) {
        return core::Result<void>::failure(invalid(
            "UV island resolution, padding, or UDIM budget is outside the supported bounds"));
    }
    if (density_policy != UvIslandDensityPolicy::preserve_source_uv &&
        density_policy != UvIslandDensityPolicy::uniform_world_area) {
        return core::Result<void>::failure(invalid(
            "UV island density policy is not supported"));
    }
    if (!std::isfinite(target_texels_per_world_unit) ||
        target_texels_per_world_unit < 0.0 ||
        (density_policy == UvIslandDensityPolicy::preserve_source_uv &&
            target_texels_per_world_unit != 0.0)) {
        return core::Result<void>::failure(invalid(
            "target world texel density requires a finite non-negative uniform-world-area policy"));
    }
    return core::Result<void>::success();
}

std::string UvIslandPackRecipe::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << kRecipeMagic << '\n'
           << "version=" << kUvIslandPackVersion << '\n'
           << "project.namespace=" << encode_hex(project_namespace) << '\n'
           << "mesh.asset=" << mesh_asset << '\n'
           << "source.identity=" << encode_hex(source.identity) << '\n'
           << "source.revision=" << source.revision.value() << '\n'
           << "source.digest=" << source.content_digest.hex() << '\n'
           << "source.uv_set=" << source_uv_set << '\n'
           << "output.uv_set=" << output_uv_set << '\n'
           << "output.name=" << encode_hex(output_name) << '\n'
           << "output.layer=" << encode_hex(output_layer_id) << '\n'
           << "resolution=" << resolution << '\n'
           << "padding.texels=" << padding_texels << '\n'
           << "maximum.udim.tiles=" << maximum_udim_tiles << '\n'
           << "density.policy="
           << (density_policy == UvIslandDensityPolicy::preserve_source_uv
                   ? "preserve_source_uv" : "uniform_world_area") << '\n'
           << "target.texels_per_world_unit.bits=" << std::hex
           << std::bit_cast<std::uint64_t>(
                target_texels_per_world_unit == 0.0
                    ? 0.0 : target_texels_per_world_unit)
           << std::dec << '\n'
           << "island.connectivity=exact-shared-edge-endpoints-v1\n"
           << "packing.strategy=largest-first-shelf-v1\n"
           << "rotation="
           << (allow_quarter_turn_rotation ? "quarter_turn_90" : "disabled")
           << '\n';
    return output.str();
}

core::Result<assets::Sha256Digest> UvIslandPackRecipe::settings_digest() const {
    const std::string serialized = serialize();
    if (serialized.empty()) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "cannot digest an invalid UV island pack recipe"));
    }
    return core::Result<assets::Sha256Digest>::success(hash_text(serialized));
}

core::Result<void> UvIslandPackArtifact::refresh_digests() {
    if (auto result = recipe.validate(); !result) return result;
    if (algorithm != "carto.uv.island-shelf-pack.v2" ||
        evidence_class != "carto.cpu-reference" ||
        !std::isfinite(pixels_per_uv_unit) || pixels_per_uv_unit <= 0.0 ||
        !std::isfinite(texels_per_world_unit) ||
        (recipe.density_policy == UvIslandDensityPolicy::uniform_world_area &&
            texels_per_world_unit != pixels_per_uv_unit) ||
        (recipe.density_policy == UvIslandDensityPolicy::preserve_source_uv &&
            texels_per_world_unit != 0.0) ||
        corners.empty() || corners.size() > kMaxUvIslandPackCorners) {
        return core::Result<void>::failure(validation(
            "UV island pack artifact identity or output size is invalid"));
    }
    const auto settings = recipe.settings_digest();
    if (!settings) return core::Result<void>::failure(settings.error());
    settings_digest = settings.value();

    std::vector<std::uint8_t> output;
    append_text(output, kOutputMagic);
    append_u64(output, static_cast<std::uint64_t>(corners.size()));
    geometry::CornerId previous{};
    std::map<std::uint32_t, std::vector<UvIslandPackCorner>> corners_by_tile;
    std::map<geometry::FaceId, std::uint32_t> tile_by_face;
    for (const UvIslandPackCorner& corner : corners) {
        if (!corner.corner || (previous && !(previous < corner.corner)) ||
            !corner.face || corner.udim_tile < 1001U ||
            corner.udim_tile >= 1001U + recipe.maximum_udim_tiles ||
            !corner.uv.finite()) {
            return core::Result<void>::failure(validation(
                "UV island pack output contains a duplicate or invalid corner"));
        }
        const std::uint32_t tile_index = corner.udim_tile - 1001U;
        const double tile_u = static_cast<double>(tile_index % 10U);
        const double tile_v = static_cast<double>(tile_index / 10U);
        if (corner.uv.x < tile_u || corner.uv.x >= tile_u + 1.0 ||
            corner.uv.y < tile_v || corner.uv.y >= tile_v + 1.0) {
            return core::Result<void>::failure(validation(
                "UV island pack coordinate does not belong to its declared UDIM tile"));
        }
        const auto [face_tile, inserted] = tile_by_face.emplace(corner.face, corner.udim_tile);
        if (!inserted && face_tile->second != corner.udim_tile) {
            return core::Result<void>::failure(validation(
                "UV island pack face corners span multiple UDIM tiles"));
        }
        previous = corner.corner;
        append_u64(output, corner.corner.value);
        append_u64(output, corner.face.value);
        append_u32(output, corner.udim_tile);
        append_f64(output, corner.uv.x);
        append_f64(output, corner.uv.y);
        corners_by_tile[corner.udim_tile].push_back(corner);
    }

    tiles.clear();
    tiles.reserve(corners_by_tile.size());
    for (const auto& [tile_number, tile_corners] : corners_by_tile) {
        std::vector<std::uint8_t> tile_output;
        append_text(tile_output, kTileMagic);
        append_u32(tile_output, tile_number);
        append_u64(tile_output, static_cast<std::uint64_t>(tile_corners.size()));
        std::set<geometry::FaceId> tile_faces;
        for (const UvIslandPackCorner& corner : tile_corners) {
            append_u64(tile_output, corner.corner.value);
            append_u64(tile_output, corner.face.value);
            append_f64(tile_output, corner.uv.x);
            append_f64(tile_output, corner.uv.y);
            tile_faces.insert(corner.face);
        }
        tiles.push_back(UvIslandPackTileReceipt{
            tile_number,
            static_cast<std::uint64_t>(tile_faces.size()),
            static_cast<std::uint64_t>(tile_corners.size()),
            assets::sha256(tile_output)});
    }
    if (tiles.empty() || tiles.size() > recipe.maximum_udim_tiles ||
        tiles.front().udim_tile != 1001U) {
        return core::Result<void>::failure(validation(
            "UV island pack must use a contiguous tile set beginning at 1001"));
    }
    for (std::size_t index = 0U; index < tiles.size(); ++index) {
        if (tiles[index].udim_tile != 1001U + index) {
            return core::Result<void>::failure(validation(
                "UV island pack contains a gap in its scheduled UDIM tiles"));
        }
    }

    byte_size = static_cast<std::uint64_t>(corners.size()) * 36ULL +
        static_cast<std::uint64_t>(tiles.size()) * 52ULL;
    append_u64(output, static_cast<std::uint64_t>(tiles.size()));
    for (const UvIslandPackTileReceipt& tile : tiles) {
        append_u32(output, tile.udim_tile);
        append_u64(output, tile.face_count);
        append_u64(output, tile.corner_count);
        append_digest(output, tile.output_digest);
    }
    output_digest = assets::sha256(output);

    std::vector<std::uint8_t> artifact;
    append_text(artifact, kArtifactMagic);
    append_text(artifact, recipe.serialize());
    append_text(artifact, algorithm);
    append_text(artifact, evidence_class);
    append_digest(artifact, settings_digest);
    append_digest(artifact, output_digest);
    append_u64(artifact, byte_size);
    append_f64(artifact, pixels_per_uv_unit);
    append_f64(artifact, texels_per_world_unit);
    append_u64(artifact, static_cast<std::uint64_t>(corners.size()));
    for (const UvIslandPackCorner& corner : corners) {
        append_u64(artifact, corner.corner.value);
        append_u64(artifact, corner.face.value);
        append_u32(artifact, corner.udim_tile);
        append_f64(artifact, corner.uv.x);
        append_f64(artifact, corner.uv.y);
    }
    append_u64(artifact, static_cast<std::uint64_t>(tiles.size()));
    for (const UvIslandPackTileReceipt& tile : tiles) {
        append_u32(artifact, tile.udim_tile);
        append_u64(artifact, tile.face_count);
        append_u64(artifact, tile.corner_count);
        append_digest(artifact, tile.output_digest);
    }
    artifact_digest = assets::sha256(artifact);
    return core::Result<void>::success();
}

core::Result<void> UvIslandPackArtifact::validate(
    const geometry::EditableMesh& current_source) const {
    if (auto result = recipe.validate(); !result) return result;
    const auto expected = compile_canonical(recipe, current_source);
    if (!expected) return core::Result<void>::failure(expected.error());
    const UvIslandPackArtifact& canonical = expected.value();
    if (algorithm != canonical.algorithm || evidence_class != canonical.evidence_class ||
        settings_digest != canonical.settings_digest || output_digest != canonical.output_digest ||
        artifact_digest != canonical.artifact_digest || byte_size != canonical.byte_size ||
        pixels_per_uv_unit != canonical.pixels_per_uv_unit ||
        texels_per_world_unit != canonical.texels_per_world_unit ||
        corners != canonical.corners || tiles != canonical.tiles) {
        return core::Result<void>::failure(validation(
            "UV island pack artifact differs from deterministic source recompilation"));
    }
    return core::Result<void>::success();
}

core::Result<UvIslandPackRecipe> make_uv_island_pack_recipe(
    const project::ProjectDocument& document,
    std::uint64_t mesh_asset,
    attributes::UvSetId source_uv_set,
    attributes::UvSetId output_uv_set,
    std::string output_name,
    std::string output_layer_id,
    std::uint32_t resolution,
    std::uint32_t padding_texels,
    std::uint32_t maximum_udim_tiles,
    UvIslandDensityPolicy density_policy,
    double target_texels_per_world_unit,
    bool allow_quarter_turn_rotation) {
    const auto mesh = document.meshes().find(mesh_asset);
    if (mesh == document.meshes().end()) {
        return core::Result<UvIslandPackRecipe>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "UV island source mesh does not exist in the project"));
    }
    const std::string identity = mesh_identity(document.source_namespace(), mesh_asset);
    const auto binding = bind_bake_target(identity, mesh->second, source_uv_set);
    if (!binding) return core::Result<UvIslandPackRecipe>::failure(binding.error());
    UvIslandPackRecipe recipe{
        document.source_namespace(), mesh_asset, binding.value(), source_uv_set,
        output_uv_set, std::move(output_name), std::move(output_layer_id),
        resolution, padding_texels, maximum_udim_tiles, density_policy,
        target_texels_per_world_unit, allow_quarter_turn_rotation};
    if (auto result = recipe.validate(); !result) {
        return core::Result<UvIslandPackRecipe>::failure(result.error());
    }
    if (mesh->second.uv_sets().find(output_uv_set) != nullptr ||
        mesh->second.attributes().find(recipe.output_layer_id) != nullptr) {
        return core::Result<UvIslandPackRecipe>::failure(validation(
            "UV island pack output UV set or layer already exists"));
    }
    return core::Result<UvIslandPackRecipe>::success(std::move(recipe));
}

namespace {

core::Result<UvIslandPackArtifact> compile_canonical(
    const UvIslandPackRecipe& recipe,
    const geometry::EditableMesh& source) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<UvIslandPackArtifact>::failure(result.error());
    }
    if (source.revision() != recipe.source.revision) {
        return core::Result<UvIslandPackArtifact>::failure(stale(
            "UV island pack source mesh revision is stale"));
    }
    const auto current_binding = bind_bake_target(recipe.source.identity, source,
        recipe.source_uv_set);
    if (!current_binding) {
        return core::Result<UvIslandPackArtifact>::failure(current_binding.error());
    }
    if (current_binding.value() != recipe.source) {
        return core::Result<UvIslandPackArtifact>::failure(stale(
            "UV island pack source UV or mesh content digest is stale"));
    }
    if (source.uv_sets().find(recipe.output_uv_set) != nullptr ||
        source.attributes().find(recipe.output_layer_id) != nullptr) {
        return core::Result<UvIslandPackArtifact>::failure(validation(
            "UV island pack output UV set or layer already exists"));
    }
    const attributes::UvSetDescriptor* source_set = source.uv_sets().find(recipe.source_uv_set);
    const attributes::AttributeLayer* source_layer = source_set == nullptr
        ? nullptr : source.attributes().find(source_set->layer_id);
    if (source_set == nullptr || source_layer == nullptr ||
        source_layer->descriptor.domain != attributes::AttributeDomain::corner ||
        source_layer->descriptor.type != attributes::AttributeType::vec2) {
        return core::Result<UvIslandPackArtifact>::failure(validation(
            "UV island pack requires a corner-domain vec2 source UV set"));
    }
    const auto topology = source.topology();
    if (!topology) return core::Result<UvIslandPackArtifact>::failure(topology.error());
    if (topology.value().corners.empty() ||
        topology.value().corners.size() > kMaxUvIslandPackCorners ||
        source_layer->values.size() != topology.value().corners.size()) {
        return core::Result<UvIslandPackArtifact>::failure(validation(
            "UV island source corner count is empty or outside the supported bound"));
    }

    std::map<std::pair<geometry::FaceId, geometry::VertexId>, geometry::CornerId>
        corner_by_face_vertex;
    std::map<geometry::CornerId, core::Vec2d> uv_by_corner;
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        const geometry::CornerRecord& corner = topology.value().corners[index];
        const auto* uv = std::get_if<core::Vec2d>(&source_layer->values[index]);
        if (uv == nullptr || !uv->finite() ||
            !corner_by_face_vertex.emplace(
                std::make_pair(corner.face, corner.vertex), corner.id).second ||
            !uv_by_corner.emplace(corner.id, *uv).second) {
            return core::Result<UvIslandPackArtifact>::failure(validation(
                "UV island source contains malformed or ambiguous corner data"));
        }
    }

    const std::vector<geometry::Face> faces = source.faces_sorted();
    if (faces.empty()) {
        return core::Result<UvIslandPackArtifact>::failure(validation(
            "UV island source mesh contains no faces"));
    }
    std::map<geometry::FaceId, geometry::FaceId> parent;
    std::map<EdgeKey, std::vector<EdgeUse>> uses_by_edge;
    for (const geometry::Face& face : faces) {
        if (face.vertices.size() < 3U ||
            !parent.emplace(face.id, face.id).second) {
            return core::Result<UvIslandPackArtifact>::failure(validation(
                "UV island source has an invalid or duplicate face identity"));
        }
        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            const geometry::VertexId from = face.vertices[index];
            const geometry::VertexId to = face.vertices[(index + 1U) % face.vertices.size()];
            const auto from_uv = uv_by_corner.find(corner_by_face_vertex.at({face.id, from}));
            const auto to_uv = uv_by_corner.find(corner_by_face_vertex.at({face.id, to}));
            if (from_uv == uv_by_corner.end() || to_uv == uv_by_corner.end()) {
                return core::Result<UvIslandPackArtifact>::failure(validation(
                    "UV island source face is missing a stable corner coordinate"));
            }
            const bool forward = from < to;
            const EdgeKey key{forward ? from : to, forward ? to : from};
            uses_by_edge[key].push_back(EdgeUse{
                face.id, forward ? from_uv->second : to_uv->second,
                forward ? to_uv->second : from_uv->second});
        }
    }

    auto find_root = [&parent](auto&& self, geometry::FaceId face) -> geometry::FaceId {
        geometry::FaceId& value = parent.at(face);
        if (value != face) value = self(self, value);
        return value;
    };
    for (const auto& [edge, uses] : uses_by_edge) {
        static_cast<void>(edge);
        if (uses.size() > 2U) {
            return core::Result<UvIslandPackArtifact>::failure(unsupported(
                "UV island packing rejects non-manifold shared-edge fans"));
        }
        if (uses.size() != 2U ||
            !same_uv(uses[0U].first_uv, uses[1U].first_uv) ||
            !same_uv(uses[0U].second_uv, uses[1U].second_uv)) {
            continue;
        }
        geometry::FaceId left = find_root(find_root, uses[0U].face);
        geometry::FaceId right = find_root(find_root, uses[1U].face);
        if (left != right) {
            if (right < left) std::swap(left, right);
            parent[right] = left;
        }
    }

    std::map<geometry::FaceId, std::vector<geometry::FaceId>> face_islands;
    for (const geometry::Face& face : faces) {
        face_islands[find_root(find_root, face.id)].push_back(face.id);
    }
    if (face_islands.empty() || face_islands.size() > kMaxUvIslandPackCorners) {
        return core::Result<UvIslandPackArtifact>::failure(validation(
            "UV island source has no packable islands"));
    }
    const std::uint64_t orientation_count =
        recipe.allow_quarter_turn_rotation ? 2ULL : 1ULL;
    const std::uint64_t candidate_budget_per_chart =
        static_cast<std::uint64_t>(recipe.maximum_udim_tiles) * orientation_count;
    if (face_islands.size() >
        kMaxUvIslandPackCandidates / candidate_budget_per_chart) {
        return core::Result<UvIslandPackArtifact>::failure(unsupported(
            "UV island packing workload exceeds its bounded chart-tile-orientation candidate budget"));
    }

    std::vector<FaceChart> charts;
    charts.reserve(face_islands.size());
    for (const auto& [island, island_faces] : face_islands) {
        FaceChart chart;
        chart.stable_face = island_faces.front();
        chart.minimum_u = std::numeric_limits<double>::infinity();
        chart.minimum_v = std::numeric_limits<double>::infinity();
        double maximum_u = -std::numeric_limits<double>::infinity();
        double maximum_v = -std::numeric_limits<double>::infinity();
        double chart_world_area = 0.0;
        double chart_uv_area = 0.0;
        for (const geometry::FaceId face_id : island_faces) {
            const geometry::Face* face = source.find_face(face_id);
            if (face == nullptr) {
                return core::Result<UvIslandPackArtifact>::failure(validation(
                    "UV island source face disappeared during chart construction"));
            }
            std::vector<core::Vec3d> face_positions;
            std::vector<core::Vec2d> face_uvs;
            const bool measure_world_area =
                recipe.density_policy == UvIslandDensityPolicy::uniform_world_area;
            if (measure_world_area) {
                face_positions.reserve(face->vertices.size());
                face_uvs.reserve(face->vertices.size());
            }
            for (const geometry::VertexId vertex : face->vertices) {
                const geometry::CornerId corner = corner_by_face_vertex.at({face_id, vertex});
                const core::Vec2d uv = uv_by_corner.at(corner);
                chart.corners.push_back(UvIslandPackCorner{corner, face_id, 1001U, uv});
                if (measure_world_area) {
                    const geometry::Vertex* source_vertex = source.find_vertex(vertex);
                    if (source_vertex == nullptr || !source_vertex->position.finite()) {
                        return core::Result<UvIslandPackArtifact>::failure(validation(
                            "UV density source face references invalid geometry"));
                    }
                    face_positions.push_back(source_vertex->position);
                    face_uvs.push_back(uv);
                }
                chart.minimum_u = std::min(chart.minimum_u, uv.x);
                chart.minimum_v = std::min(chart.minimum_v, uv.y);
                maximum_u = std::max(maximum_u, uv.x);
                maximum_v = std::max(maximum_v, uv.y);
            }

            if (measure_world_area) {
                const core::Vec3d origin = face_positions.front();
                const core::Vec2d uv_origin = face_uvs.front();
                core::Vec3d polygon_normal{};
                double polygon_uv_twice_area = 0.0;
                for (std::size_t index = 0U; index < face_positions.size(); ++index) {
                    const std::size_t next = (index + 1U) % face_positions.size();
                    polygon_normal = polygon_normal + core::cross(
                        face_positions[index] - origin,
                        face_positions[next] - origin);
                    polygon_uv_twice_area +=
                        (face_uvs[index].x - uv_origin.x) *
                            (face_uvs[next].y - uv_origin.y) -
                        (face_uvs[index].y - uv_origin.y) *
                            (face_uvs[next].x - uv_origin.x);
                }
                if (!polygon_normal.finite() ||
                    polygon_normal.length() <= 2.0 * kGeometryEpsilon ||
                    !std::isfinite(polygon_uv_twice_area) ||
                    std::abs(polygon_uv_twice_area) <= 2.0 * kGeometryEpsilon) {
                    return core::Result<UvIslandPackArtifact>::failure(validation(
                        "uniform world-area density requires non-degenerate face geometry and UVs"));
                }
                const core::Vec3d polygon_direction = polygon_normal.normalized();
                if (!polygon_direction.finite()) {
                    return core::Result<UvIslandPackArtifact>::failure(validation(
                        "uniform world-area density face direction is not representable"));
                }

                double face_world_area = 0.0;
                double face_uv_area = 0.0;
                for (std::size_t fan = 1U; fan + 1U < face_positions.size(); ++fan) {
                    const core::Vec3d world_cross = core::cross(
                        face_positions[fan] - origin,
                        face_positions[fan + 1U] - origin);
                    const double world_triangle_area = world_cross.length() * 0.5;
                    const double uv_twice_area =
                        (face_uvs[fan].x - uv_origin.x) *
                            (face_uvs[fan + 1U].y - uv_origin.y) -
                        (face_uvs[fan].y - uv_origin.y) *
                            (face_uvs[fan + 1U].x - uv_origin.x);
                    if (!world_cross.finite() || !std::isfinite(world_triangle_area) ||
                        world_triangle_area <= kGeometryEpsilon ||
                        core::dot(world_cross.normalized(), polygon_direction) <= 0.0 ||
                        !std::isfinite(uv_twice_area) ||
                        std::abs(uv_twice_area) <= 2.0 * kGeometryEpsilon ||
                        ((uv_twice_area < 0.0) != (polygon_uv_twice_area < 0.0))) {
                        return core::Result<UvIslandPackArtifact>::failure(unsupported(
                            "uniform world-area density requires consistently oriented face fans"));
                    }
                    face_world_area += world_triangle_area;
                    face_uv_area += std::abs(uv_twice_area) * 0.5;
                }
                if (!std::isfinite(face_world_area) || !std::isfinite(face_uv_area) ||
                    face_world_area <= kGeometryEpsilon ||
                    face_uv_area <= kGeometryEpsilon) {
                    return core::Result<UvIslandPackArtifact>::failure(validation(
                        "uniform world-area density encountered a degenerate face area"));
                }
                chart_world_area += face_world_area;
                chart_uv_area += face_uv_area;
                if (!std::isfinite(chart_world_area) || !std::isfinite(chart_uv_area)) {
                    return core::Result<UvIslandPackArtifact>::failure(validation(
                        "uniform world-area density accumulation is non-finite"));
                }
            }
        }
        chart.width = maximum_u - chart.minimum_u;
        chart.height = maximum_v - chart.minimum_v;
        if (recipe.density_policy == UvIslandDensityPolicy::uniform_world_area) {
            if (chart_world_area <= kGeometryEpsilon || chart_uv_area <= kGeometryEpsilon) {
                return core::Result<UvIslandPackArtifact>::failure(validation(
                    "uniform world-area density chart has no measurable area"));
            }
            chart.density_scale = std::sqrt(chart_world_area / chart_uv_area);
            if (!std::isfinite(chart.density_scale) || chart.density_scale <= 0.0) {
                return core::Result<UvIslandPackArtifact>::failure(validation(
                    "uniform world-area density scale is not representable"));
            }
            chart.width *= chart.density_scale;
            chart.height *= chart.density_scale;
        }
        const double extent = std::max(chart.width, chart.height);
        if (!std::isfinite(extent) || extent <= kGeometryEpsilon ||
            chart.width <= kGeometryEpsilon || chart.height <= kGeometryEpsilon) {
            return core::Result<UvIslandPackArtifact>::failure(validation(
                "UV island has a degenerate or non-finite bounding rectangle"));
        }
        charts.push_back(std::move(chart));
    }

    std::vector<std::size_t> order(charts.size());
    for (std::size_t index = 0U; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&charts](std::size_t left, std::size_t right) {
        const FaceChart& a = charts[left];
        const FaceChart& b = charts[right];
        const double a_long = std::max(a.width, a.height);
        const double b_long = std::max(b.width, b.height);
        if (a_long != b_long) return a_long > b_long;
        const double a_area = a.width * a.height;
        const double b_area = b.width * b.height;
        if (a_area != b_area) return a_area > b_area;
        return a.stable_face < b.stable_face;
    });

    double maximum_extent = 0.0;
    for (const FaceChart& chart : charts) {
        maximum_extent = std::max(maximum_extent, std::max(chart.width, chart.height));
    }
    double high = static_cast<double>(recipe.resolution - 2U * recipe.padding_texels) /
        maximum_extent;
    if (!std::isfinite(high) || high <= 0.0) {
        return core::Result<UvIslandPackArtifact>::failure(validation(
            "UV island source scale cannot be represented by the requested atlas"));
    }
    double low = 0.0;
    std::optional<std::vector<Placement>> placements;
    if (recipe.target_texels_per_world_unit > 0.0) {
        low = recipe.target_texels_per_world_unit;
        placements = pack_shelves(charts, order, low,
            recipe.resolution, recipe.padding_texels, recipe.maximum_udim_tiles,
            recipe.allow_quarter_turn_rotation);
        if (!placements) {
            return core::Result<UvIslandPackArtifact>::failure(unsupported(
                "requested world texel density cannot fit in the declared UDIM budget"));
        }
    } else {
        placements = pack_shelves(charts, order, low,
            recipe.resolution, recipe.padding_texels, recipe.maximum_udim_tiles,
            recipe.allow_quarter_turn_rotation);
        if (!placements) {
            return core::Result<UvIslandPackArtifact>::failure(unsupported(
                "UV islands and required padding cannot fit in the requested UDIM budget"));
        }
        auto full_density = pack_shelves(charts, order, high,
            recipe.resolution, recipe.padding_texels, recipe.maximum_udim_tiles,
            recipe.allow_quarter_turn_rotation);
        if (full_density) {
            low = high;
            placements = std::move(full_density);
        } else {
            for (std::uint32_t iteration = 0U; iteration < 48U; ++iteration) {
                const double middle = low + (high - low) * 0.5;
                auto candidate = pack_shelves(charts, order, middle,
                    recipe.resolution, recipe.padding_texels, recipe.maximum_udim_tiles,
                    recipe.allow_quarter_turn_rotation);
                if (candidate) {
                    low = middle;
                    placements = std::move(candidate);
                } else {
                    high = middle;
                }
            }
        }
    }
    if (!placements || !std::isfinite(low) || low <= 0.0) {
        return core::Result<UvIslandPackArtifact>::failure(validation(
            "UV island packing did not produce a positive uniform scale"));
    }

    std::map<geometry::CornerId, UvIslandPackCorner> packed_by_corner;
    for (std::size_t chart_index = 0U; chart_index < charts.size(); ++chart_index) {
        const FaceChart& chart = charts[chart_index];
        const Placement placement = (*placements)[chart_index];
        const std::uint32_t tile_u = placement.tile_index % 10U;
        const std::uint32_t tile_v = placement.tile_index / 10U;
        const std::uint32_t tile_number = 1001U + placement.tile_index;
        for (const UvIslandPackCorner& original : chart.corners) {
            const double local_u =
                (original.uv.x - chart.minimum_u) * chart.density_scale;
            const double local_v =
                (original.uv.y - chart.minimum_v) * chart.density_scale;
            const double oriented_u = placement.rotated_clockwise
                ? chart.height - local_v : local_u;
            const double oriented_v = placement.rotated_clockwise
                ? local_u : local_v;
            const core::Vec2d uv{
                static_cast<double>(tile_u) +
                    (static_cast<double>(placement.x) +
                        oriented_u * low) / recipe.resolution,
                static_cast<double>(tile_v) +
                    (static_cast<double>(placement.y) +
                        oriented_v * low) / recipe.resolution};
            if (!uv.finite() || !packed_by_corner.emplace(original.corner,
                    UvIslandPackCorner{original.corner, original.face, tile_number, uv}).second) {
                return core::Result<UvIslandPackArtifact>::failure(validation(
                    "UV island packing produced a duplicate or non-finite corner"));
            }
        }
    }

    UvIslandPackArtifact artifact;
    artifact.recipe = recipe;
    artifact.pixels_per_uv_unit = low;
    artifact.texels_per_world_unit =
        recipe.density_policy == UvIslandDensityPolicy::uniform_world_area
        ? low : 0.0;
    artifact.corners.reserve(packed_by_corner.size());
    for (const auto& [corner, packed] : packed_by_corner) {
        static_cast<void>(corner);
        artifact.corners.push_back(packed);
    }
    if (auto result = artifact.refresh_digests(); !result) {
        return core::Result<UvIslandPackArtifact>::failure(result.error());
    }
    return core::Result<UvIslandPackArtifact>::success(std::move(artifact));
}

} // namespace

core::Result<UvIslandPackArtifact> compile_uv_island_pack(
    const UvIslandPackRecipe& recipe,
    const geometry::EditableMesh& source) {
    return compile_canonical(recipe, source);
}

core::Result<void> apply_uv_island_pack(
    project::ProjectTransaction& transaction,
    std::uint64_t mesh_asset,
    const UvIslandPackArtifact& artifact) {
    const project::ProjectDocument& staged = transaction.staged_document();
    if (staged.source_namespace() != artifact.recipe.project_namespace ||
        mesh_asset != artifact.recipe.mesh_asset) {
        return core::Result<void>::failure(stale(
            "UV island pack artifact belongs to another project or mesh asset"));
    }
    const auto mesh = staged.meshes().find(mesh_asset);
    if (mesh == staged.meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "UV island transaction target mesh does not exist"));
    }
    if (auto result = artifact.validate(mesh->second); !result) return result;
    if (mesh->second.uv_sets().find(artifact.recipe.output_uv_set) != nullptr ||
        mesh->second.attributes().find(artifact.recipe.output_layer_id) != nullptr) {
        return core::Result<void>::failure(validation(
            "UV island pack target set or corner layer already exists"));
    }
    const auto topology = mesh->second.topology();
    if (!topology) return core::Result<void>::failure(topology.error());
    std::map<geometry::CornerId, core::Vec2d> coordinates;
    for (const UvIslandPackCorner& corner : artifact.corners) {
        coordinates.emplace(corner.corner, corner.uv);
    }
    std::vector<attributes::AttributeValue> values;
    values.reserve(topology.value().corners.size());
    for (const geometry::CornerRecord& corner : topology.value().corners) {
        const auto coordinate = coordinates.find(corner.id);
        if (coordinate == coordinates.end()) {
            return core::Result<void>::failure(validation(
                "UV island pack artifact omits a staged mesh corner"));
        }
        values.emplace_back(coordinate->second);
    }

    attributes::AttributeSet attribute_set({
        {attributes::AttributeDomain::vertex, mesh->second.vertex_count()},
        {attributes::AttributeDomain::edge, topology.value().edges.size()},
        {attributes::AttributeDomain::face, mesh->second.face_count()},
        {attributes::AttributeDomain::corner, topology.value().corners.size()},
    });
    for (const auto& [ignored, layer] : mesh->second.attributes().layers()) {
        static_cast<void>(ignored);
        if (auto result = attribute_set.add_layer(layer); !result) return result;
    }
    if (auto result = attribute_set.add_layer(attributes::AttributeLayer{
            {artifact.recipe.output_layer_id, attributes::AttributeDomain::corner,
                attributes::AttributeType::vec2},
            std::move(values)}); !result) {
        return result;
    }
    attributes::UvSetTable uv_sets = mesh->second.uv_sets();
    if (auto result = uv_sets.add(attributes::UvSetDescriptor{
            artifact.recipe.output_uv_set,
            artifact.recipe.output_name,
            artifact.recipe.output_layer_id,
            false,
            false,
            false,
            2U}); !result) {
        return result;
    }
    geometry::EditableMesh updated = mesh->second;
    if (auto result = updated.set_attribute_payload(std::move(attribute_set), std::move(uv_sets));
        !result) {
        return result;
    }
    if (auto result = updated.validate(); !result) return result;
    return transaction.replace_mesh(mesh_asset, std::move(updated));
}

std::string uv_island_pack_transaction_operation(const UvIslandPackArtifact& artifact) {
    if (artifact.settings_digest.is_zero() || artifact.output_digest.is_zero()) return {};
    return "uvp2." + base64url(artifact.settings_digest) + "." +
        base64url(artifact.output_digest);
}

} // namespace carto::production
