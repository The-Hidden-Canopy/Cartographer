#include <carto/production/lightmap_uv.hpp>

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
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace carto::production {
namespace {

constexpr std::string_view kRecipeMagic = "CARTO_UNIQUE_LIGHTMAP_UV_RECIPE 2";
constexpr std::string_view kOutputMagic = "CARTO_UNIQUE_LIGHTMAP_UV_OUTPUT 2";
constexpr std::string_view kTileOutputMagic = "CARTO_UNIQUE_LIGHTMAP_UV_TILE_OUTPUT 1";
constexpr std::string_view kArtifactMagic = "CARTO_UNIQUE_LIGHTMAP_UV_ARTIFACT 2";
constexpr double kPlanarityRelativeTolerance = 1e-8;
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

bool safe_text(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\n' && byte != '\r';
        });
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

class CanonicalBytes final {
public:
    void text(std::string_view value) {
        u64(static_cast<std::uint64_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void u32(std::uint32_t value) {
        for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    }

    void u64(std::uint64_t value) {
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    }

    void f64(double value) {
        u64(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
    }

    void digest(const assets::Sha256Digest& value) {
        bytes_.insert(bytes_.end(), value.bytes.begin(), value.bytes.end());
    }

    [[nodiscard]] assets::Sha256Digest hash() const { return assets::sha256(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

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

std::string mesh_identity(std::string_view project_namespace, std::uint64_t mesh_asset) {
    return std::string(project_namespace) + "/mesh/" + std::to_string(mesh_asset);
}

struct FaceChart {
    geometry::FaceId face;
    std::vector<geometry::VertexId> vertices;
    std::vector<core::Vec2d> local_uvs;
    double minimum_u = 0.0;
    double minimum_v = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct Placement {
    std::uint32_t tile_index = 0U;
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
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
    std::uint32_t maximum_tiles) {
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
        const std::uint64_t padded_width = chart_width + 2ULL * padding;
        const std::uint64_t padded_height = chart_height + 2ULL * padding;
        if (padded_width > resolution || padded_height > resolution) {
            return std::nullopt;
        }

        bool placed = false;
        for (std::size_t tile_index = 0U; tile_index < states.size(); ++tile_index) {
            PackState candidate = states[tile_index];
            if (candidate.cursor_x + padded_width > resolution) {
                candidate.cursor_y += candidate.shelf_height;
                candidate.cursor_x = 0U;
                candidate.shelf_height = 0U;
            }
            if (candidate.cursor_y + padded_height > resolution) continue;
            placements[chart_index] = Placement{
                static_cast<std::uint32_t>(tile_index),
                static_cast<std::uint32_t>(candidate.cursor_x + padding),
                static_cast<std::uint32_t>(candidate.cursor_y + padding)};
            candidate.cursor_x += padded_width;
            candidate.shelf_height = std::max(candidate.shelf_height, padded_height);
            states[tile_index] = candidate;
            placed = true;
            break;
        }
        if (placed) continue;
        if (states.size() >= maximum_tiles) return std::nullopt;

        PackState first_chart;
        placements[chart_index] = Placement{
            static_cast<std::uint32_t>(states.size()), padding, padding};
        first_chart.cursor_x = padded_width;
        first_chart.shelf_height = padded_height;
        states.push_back(first_chart);
    }
    return placements;
}

std::uint32_t udim_number(std::uint32_t tile_index) {
    return 1001U + tile_index;
}

} // namespace

core::Result<void> UniqueLightmapUvRecipe::validate() const {
    if (!safe_text(project_namespace, 1024U) || mesh_asset == 0U ||
        source.identity != mesh_identity(project_namespace, mesh_asset)) {
        return core::Result<void>::failure(invalid(
            "lightmap UV recipe requires a stable project namespace and mesh identity"));
    }
    if (auto result = source.validate(); !result) return result;
    if (output_uv_set == 0U || !safe_text(output_name, 128U) ||
        !safe_text(output_layer_id, 128U)) {
        return core::Result<void>::failure(invalid(
            "lightmap UV recipe requires a new UV-set identity, name, and layer id"));
    }
    if (resolution < 16U || resolution > kMaxBakeTextureExtent ||
        padding_texels == 0U || padding_texels > resolution / 8U ||
        static_cast<std::uint64_t>(padding_texels) * 2ULL >= resolution ||
        maximum_udim_tiles == 0U || maximum_udim_tiles > kMaxUniqueLightmapUdimTiles) {
        return core::Result<void>::failure(invalid(
            "lightmap UV resolution, texel padding, or UDIM tile budget is outside the supported bounds"));
    }
    return core::Result<void>::success();
}

std::string UniqueLightmapUvRecipe::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << kRecipeMagic << '\n'
           << "version=" << kUniqueLightmapUvVersion << '\n'
           << "project.namespace=" << encode_hex(project_namespace) << '\n'
           << "mesh.asset=" << mesh_asset << '\n'
           << "source.identity=" << encode_hex(source.identity) << '\n'
           << "source.revision=" << source.revision.value() << '\n'
           << "source.digest=" << source.content_digest.hex() << '\n'
           << "output.uv_set=" << output_uv_set << '\n'
           << "output.name=" << encode_hex(output_name) << '\n'
           << "output.layer=" << encode_hex(output_layer_id) << '\n'
           << "resolution=" << resolution << '\n'
           << "padding.texels=" << padding_texels << '\n'
           << "maximum.udim.tiles=" << maximum_udim_tiles << '\n'
           << "chart.strategy=planar-face-v1\n"
           << "packing.strategy=udim-shelf-largest-first-v2\n"
           << "density.policy=uniform-across-used-tiles-v1\n";
    return output.str();
}

core::Result<assets::Sha256Digest> UniqueLightmapUvRecipe::settings_digest() const {
    const std::string serialized = serialize();
    if (serialized.empty()) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "cannot digest an invalid unique-lightmap UV recipe"));
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(serialized.data());
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>(data, serialized.size())));
}

core::Result<void> UniqueLightmapUvArtifact::refresh_digests() {
    if (auto result = recipe.validate(); !result) return result;
    if (algorithm != "carto.lightmap.face-planar-udim-pack.v2" ||
        evidence_class != "carto.cpu-reference" ||
        !std::isfinite(texels_per_unit) || texels_per_unit <= 0.0 ||
        corners.empty() || corners.size() > kMaxUniqueLightmapUvCorners) {
        return core::Result<void>::failure(validation(
            "unique-lightmap UV artifact identity or output size is invalid"));
    }
    const auto settings = recipe.settings_digest();
    if (!settings) return core::Result<void>::failure(settings.error());
    settings_digest = settings.value();

    CanonicalBytes output;
    output.text(kOutputMagic);
    output.u64(static_cast<std::uint64_t>(corners.size()));
    geometry::CornerId previous{};
    std::map<std::uint32_t, std::vector<LightmapUvCorner>> corners_by_tile;
    std::map<geometry::FaceId, std::uint32_t> tile_by_face;
    for (const LightmapUvCorner& corner : corners) {
        if (!corner.corner || (previous && !(previous < corner.corner)) ||
            !corner.face || corner.udim_tile < 1001U ||
            corner.udim_tile >= 1001U + recipe.maximum_udim_tiles ||
            !corner.uv.finite()) {
            return core::Result<void>::failure(validation(
                "unique-lightmap UV artifact has duplicate identities or an invalid UDIM address"));
        }
        const std::uint32_t tile_index = corner.udim_tile - 1001U;
        const double tile_u = static_cast<double>(tile_index % 10U);
        const double tile_v = static_cast<double>(tile_index / 10U);
        if (corner.uv.x < tile_u || corner.uv.x >= tile_u + 1.0 ||
            corner.uv.y < tile_v || corner.uv.y >= tile_v + 1.0) {
            return core::Result<void>::failure(validation(
                "unique-lightmap UV coordinate does not belong to its declared UDIM tile"));
        }
        const auto [face_tile, inserted] = tile_by_face.emplace(corner.face, corner.udim_tile);
        if (!inserted && face_tile->second != corner.udim_tile) {
            return core::Result<void>::failure(validation(
                "unique-lightmap face corners span multiple UDIM tiles"));
        }
        previous = corner.corner;
        output.u64(corner.corner.value);
        output.u64(corner.face.value);
        output.u32(corner.udim_tile);
        output.f64(corner.uv.x);
        output.f64(corner.uv.y);
        corners_by_tile[corner.udim_tile].push_back(corner);
    }

    tiles.clear();
    tiles.reserve(corners_by_tile.size());
    for (const auto& [tile_number, tile_corners] : corners_by_tile) {
        CanonicalBytes tile_output;
        tile_output.text(kTileOutputMagic);
        tile_output.u32(tile_number);
        tile_output.u64(static_cast<std::uint64_t>(tile_corners.size()));
        std::set<geometry::FaceId> tile_faces;
        for (const LightmapUvCorner& corner : tile_corners) {
            tile_output.u64(corner.corner.value);
            tile_output.u64(corner.face.value);
            tile_output.f64(corner.uv.x);
            tile_output.f64(corner.uv.y);
            tile_faces.insert(corner.face);
        }
        tiles.push_back(LightmapUvTileReceipt{
            tile_number,
            static_cast<std::uint64_t>(tile_faces.size()),
            static_cast<std::uint64_t>(tile_corners.size()),
            tile_output.hash()});
    }
    if (tiles.empty() || tiles.size() > recipe.maximum_udim_tiles ||
        tiles.front().udim_tile != 1001U) {
        return core::Result<void>::failure(validation(
            "unique-lightmap UDIM output must use a contiguous tile set beginning at 1001"));
    }
    for (std::size_t index = 0U; index < tiles.size(); ++index) {
        if (tiles[index].udim_tile != 1001U + index) {
            return core::Result<void>::failure(validation(
                "unique-lightmap UDIM output contains a gap in its scheduled tile set"));
        }
    }
    byte_size = static_cast<std::uint64_t>(corners.size()) * 36ULL +
        static_cast<std::uint64_t>(tiles.size()) * 52ULL;
    output.u64(static_cast<std::uint64_t>(tiles.size()));
    for (const LightmapUvTileReceipt& tile : tiles) {
        output.u32(tile.udim_tile);
        output.u64(tile.face_count);
        output.u64(tile.corner_count);
        output.digest(tile.output_digest);
    }
    output_digest = output.hash();

    CanonicalBytes artifact;
    artifact.text(kArtifactMagic);
    artifact.text(recipe.serialize());
    artifact.text(algorithm);
    artifact.text(evidence_class);
    artifact.digest(settings_digest);
    artifact.digest(output_digest);
    artifact.u64(byte_size);
    artifact.f64(texels_per_unit);
    artifact.u64(static_cast<std::uint64_t>(corners.size()));
    for (const LightmapUvCorner& corner : corners) {
        artifact.u64(corner.corner.value);
        artifact.u64(corner.face.value);
        artifact.u32(corner.udim_tile);
        artifact.f64(corner.uv.x);
        artifact.f64(corner.uv.y);
    }
    artifact.u64(static_cast<std::uint64_t>(tiles.size()));
    for (const LightmapUvTileReceipt& tile : tiles) {
        artifact.u32(tile.udim_tile);
        artifact.u64(tile.face_count);
        artifact.u64(tile.corner_count);
        artifact.digest(tile.output_digest);
    }
    artifact_digest = artifact.hash();
    return core::Result<void>::success();
}

core::Result<void> UniqueLightmapUvArtifact::validate(
    const geometry::EditableMesh& current_source) const {
    if (auto result = recipe.validate(); !result) return result;
    const auto current_binding = bind_bake_source(recipe.source.identity, current_source);
    if (!current_binding) return core::Result<void>::failure(current_binding.error());
    if (current_binding.value() != recipe.source) {
        return core::Result<void>::failure(stale(
            "unique-lightmap UV artifact source revision or content digest is stale"));
    }
    const auto topology = current_source.topology();
    if (!topology) return core::Result<void>::failure(topology.error());
    if (topology.value().corners.size() != corners.size()) {
        return core::Result<void>::failure(validation(
            "unique-lightmap UV artifact corner count differs from its source mesh"));
    }
    std::map<geometry::CornerId, geometry::FaceId> expected_faces_by_corner;
    for (const geometry::CornerRecord& corner : topology.value().corners) {
        expected_faces_by_corner.emplace(corner.id, corner.face);
    }
    for (const LightmapUvCorner& corner : corners) {
        const auto expected_face = expected_faces_by_corner.find(corner.corner);
        if (expected_face == expected_faces_by_corner.end() ||
            expected_face->second != corner.face) {
            return core::Result<void>::failure(validation(
                "unique-lightmap UV artifact contains an unknown corner or mismatched face identity"));
        }
        expected_faces_by_corner.erase(expected_face);
    }
    if (!expected_faces_by_corner.empty()) {
        return core::Result<void>::failure(validation(
            "unique-lightmap UV artifact omits source corner identities"));
    }
    UniqueLightmapUvArtifact rebuilt = *this;
    if (auto result = rebuilt.refresh_digests(); !result) return result;
    if (rebuilt.settings_digest != settings_digest ||
        rebuilt.output_digest != output_digest || rebuilt.artifact_digest != artifact_digest ||
        rebuilt.tiles != tiles) {
        return core::Result<void>::failure(validation(
            "unique-lightmap UV artifact digest does not match its recipe and coordinates"));
    }
    return core::Result<void>::success();
}

core::Result<UniqueLightmapUvRecipe> make_unique_lightmap_uv_recipe(
    const project::ProjectDocument& document,
    std::uint64_t mesh_asset,
    attributes::UvSetId output_uv_set,
    std::string output_name,
    std::string output_layer_id,
    std::uint32_t resolution,
    std::uint32_t padding_texels,
    std::uint32_t maximum_udim_tiles) {
    const auto mesh = document.meshes().find(mesh_asset);
    if (mesh == document.meshes().end()) {
        return core::Result<UniqueLightmapUvRecipe>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "lightmap UV target mesh asset does not exist"));
    }
    UniqueLightmapUvRecipe recipe;
    recipe.project_namespace = document.source_namespace();
    recipe.mesh_asset = mesh_asset;
    recipe.source.identity = mesh_identity(recipe.project_namespace, mesh_asset);
    const auto source = bind_bake_source(recipe.source.identity, mesh->second);
    if (!source) return core::Result<UniqueLightmapUvRecipe>::failure(source.error());
    recipe.source = source.value();
    recipe.output_uv_set = output_uv_set;
    recipe.output_name = std::move(output_name);
    recipe.output_layer_id = std::move(output_layer_id);
    recipe.resolution = resolution;
    recipe.padding_texels = padding_texels;
    recipe.maximum_udim_tiles = maximum_udim_tiles;
    if (auto result = recipe.validate(); !result) {
        return core::Result<UniqueLightmapUvRecipe>::failure(result.error());
    }
    return core::Result<UniqueLightmapUvRecipe>::success(std::move(recipe));
}

core::Result<UniqueLightmapUvArtifact> compile_unique_lightmap_uv(
    const UniqueLightmapUvRecipe& recipe,
    const geometry::EditableMesh& source) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<UniqueLightmapUvArtifact>::failure(result.error());
    }
    const auto current_binding = bind_bake_source(recipe.source.identity, source);
    if (!current_binding) {
        return core::Result<UniqueLightmapUvArtifact>::failure(current_binding.error());
    }
    if (current_binding.value() != recipe.source) {
        return core::Result<UniqueLightmapUvArtifact>::failure(stale(
            "unique-lightmap UV recipe is bound to another mesh revision or content digest"));
    }
    if (source.uv_sets().find(recipe.output_uv_set) != nullptr ||
        source.attributes().find(recipe.output_layer_id) != nullptr) {
        return core::Result<UniqueLightmapUvArtifact>::failure(validation(
            "unique-lightmap UV output set or layer already exists on the source mesh"));
    }
    const auto topology = source.topology();
    if (!topology) {
        return core::Result<UniqueLightmapUvArtifact>::failure(topology.error());
    }
    if (topology.value().corners.empty() ||
        topology.value().corners.size() > kMaxUniqueLightmapUvCorners) {
        return core::Result<UniqueLightmapUvArtifact>::failure(validation(
            "unique-lightmap UV source corner count is outside the supported bound"));
    }

    std::map<std::pair<geometry::FaceId, geometry::VertexId>, geometry::CornerId> corner_ids;
    for (const geometry::CornerRecord& corner : topology.value().corners) {
        if (!corner_ids.emplace(std::make_pair(corner.face, corner.vertex), corner.id).second) {
            return core::Result<UniqueLightmapUvArtifact>::failure(validation(
                "unique-lightmap UV source has ambiguous face-corner identity"));
        }
    }

    std::vector<FaceChart> charts;
    charts.reserve(source.face_count());
    std::uint64_t total_corners = 0U;
    for (const geometry::Face& face : source.faces_sorted()) {
        if (face.vertices.size() < 3U) {
            return core::Result<UniqueLightmapUvArtifact>::failure(validation(
                "unique-lightmap UV source contains a face with fewer than three vertices"));
        }
        if (face.vertices.size() > kMaxUniqueLightmapUvCorners - total_corners) {
            return core::Result<UniqueLightmapUvArtifact>::failure(validation(
                "unique-lightmap UV source exceeds its bounded corner count"));
        }
        total_corners += static_cast<std::uint64_t>(face.vertices.size());
        std::vector<core::Vec3d> positions;
        positions.reserve(face.vertices.size());
        for (const geometry::VertexId id : face.vertices) {
            const geometry::Vertex* vertex = source.find_vertex(id);
            if (vertex == nullptr || !vertex->position.finite()) {
                return core::Result<UniqueLightmapUvArtifact>::failure(validation(
                    "unique-lightmap UV face references a missing or non-finite vertex"));
            }
            positions.push_back(vertex->position);
        }

        core::Vec3d face_normal{};
        double scale = 0.0;
        std::size_t longest_edge = 0U;
        double longest_edge_squared = 0.0;
        for (std::size_t index = 0U; index < positions.size(); ++index) {
            const core::Vec3d current = positions[index];
            const core::Vec3d next = positions[(index + 1U) % positions.size()];
            face_normal.x += (current.y - next.y) * (current.z + next.z);
            face_normal.y += (current.z - next.z) * (current.x + next.x);
            face_normal.z += (current.x - next.x) * (current.y + next.y);
            const double edge_length_squared = (next - current).length_squared();
            if (edge_length_squared > longest_edge_squared) {
                longest_edge_squared = edge_length_squared;
                longest_edge = index;
            }
            scale = std::max(scale, std::sqrt(edge_length_squared));
        }
        face_normal = face_normal.normalized();
        if (!face_normal.finite() || face_normal.length_squared() <= 0.0 ||
            !std::isfinite(scale) || scale <= std::numeric_limits<double>::min() ||
            longest_edge_squared <= kGeometryEpsilon * scale * scale) {
            return core::Result<UniqueLightmapUvArtifact>::failure(validation(
                "unique-lightmap UV source contains a degenerate planar face"));
        }
        const core::Vec3d anchor = positions.front();
        for (const core::Vec3d position : positions) {
            if (std::abs(core::dot(position - anchor, face_normal)) >
                kPlanarityRelativeTolerance * scale) {
                return core::Result<UniqueLightmapUvArtifact>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "unique-lightmap UV generation currently requires planar faces"));
            }
        }

        const core::Vec3d tangent = (positions[(longest_edge + 1U) % positions.size()] -
            positions[longest_edge]).normalized();
        const core::Vec3d bitangent = core::cross(face_normal, tangent).normalized();
        if (!tangent.finite() || tangent.length_squared() <= 0.0 ||
            !bitangent.finite() || bitangent.length_squared() <= 0.0) {
            return core::Result<UniqueLightmapUvArtifact>::failure(validation(
                "unique-lightmap UV face could not produce a stable planar basis"));
        }
        FaceChart chart;
        chart.face = face.id;
        chart.vertices = face.vertices;
        chart.local_uvs.reserve(positions.size());
        chart.minimum_u = std::numeric_limits<double>::infinity();
        chart.minimum_v = std::numeric_limits<double>::infinity();
        double maximum_u = -std::numeric_limits<double>::infinity();
        double maximum_v = -std::numeric_limits<double>::infinity();
        for (const core::Vec3d position : positions) {
            const core::Vec3d relative = position - anchor;
            const core::Vec2d uv{core::dot(relative, tangent), core::dot(relative, bitangent)};
            chart.local_uvs.push_back(uv);
            chart.minimum_u = std::min(chart.minimum_u, uv.x);
            chart.minimum_v = std::min(chart.minimum_v, uv.y);
            maximum_u = std::max(maximum_u, uv.x);
            maximum_v = std::max(maximum_v, uv.y);
        }
        chart.width = maximum_u - chart.minimum_u;
        chart.height = maximum_v - chart.minimum_v;
        if (!std::isfinite(chart.width) || !std::isfinite(chart.height) ||
            chart.width <= kGeometryEpsilon * scale ||
            chart.height <= kGeometryEpsilon * scale) {
            return core::Result<UniqueLightmapUvArtifact>::failure(validation(
                "unique-lightmap UV face projects to a degenerate chart"));
        }
        double signed_area = 0.0;
        for (std::size_t index = 0U; index < chart.local_uvs.size(); ++index) {
            const core::Vec2d a = chart.local_uvs[index];
            const core::Vec2d b = chart.local_uvs[(index + 1U) % chart.local_uvs.size()];
            signed_area += a.x * b.y - b.x * a.y;
        }
        if (!std::isfinite(signed_area) || std::abs(signed_area) <=
            kGeometryEpsilon * scale * scale) {
            return core::Result<UniqueLightmapUvArtifact>::failure(validation(
                "unique-lightmap UV face has zero projected area"));
        }
        const double winding = signed_area > 0.0 ? 1.0 : -1.0;
        const double turn_epsilon = kGeometryEpsilon * scale * scale;
        for (std::size_t index = 0U; index < chart.local_uvs.size(); ++index) {
            const core::Vec2d a = chart.local_uvs[index];
            const core::Vec2d b = chart.local_uvs[(index + 1U) % chart.local_uvs.size()];
            const core::Vec2d c = chart.local_uvs[(index + 2U) % chart.local_uvs.size()];
            const double turn = (b.x - a.x) * (c.y - b.y) -
                (b.y - a.y) * (c.x - b.x);
            if (!std::isfinite(turn) || winding * turn <= turn_epsilon) {
                return core::Result<UniqueLightmapUvArtifact>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "unique-lightmap UV generation currently requires strictly convex faces"));
            }
        }
        charts.push_back(std::move(chart));
    }
    if (charts.empty()) {
        return core::Result<UniqueLightmapUvArtifact>::failure(validation(
            "unique-lightmap UV source contains no charts"));
    }
    const std::uint64_t chart_tile_candidates =
        static_cast<std::uint64_t>(charts.size()) * recipe.maximum_udim_tiles;
    if (chart_tile_candidates > kMaxUniqueLightmapPackCandidates) {
        return core::Result<UniqueLightmapUvArtifact>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "unique-lightmap UDIM packing workload exceeds its bounded chart-tile candidate budget"));
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
        return a.face < b.face;
    });

    double maximum_extent = 0.0;
    for (const FaceChart& chart : charts) {
        maximum_extent = std::max(maximum_extent, std::max(chart.width, chart.height));
    }
    double high = static_cast<double>(recipe.resolution - 2U * recipe.padding_texels) /
        maximum_extent;
    if (!std::isfinite(high)) high = std::numeric_limits<double>::max() / 4.0;
    double low = 0.0;
    auto placements = pack_shelves(charts, order, low,
        recipe.resolution, recipe.padding_texels, recipe.maximum_udim_tiles);
    if (!placements) {
        return core::Result<UniqueLightmapUvArtifact>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "lightmap charts and required padding cannot fit in the requested UDIM tile budget"));
    }
    auto full_density = pack_shelves(charts, order, high,
        recipe.resolution, recipe.padding_texels, recipe.maximum_udim_tiles);
    if (full_density) {
        low = high;
        placements = std::move(full_density);
    } else {
        for (std::uint32_t iteration = 0U; iteration < 48U; ++iteration) {
            const double middle = low + (high - low) * 0.5;
            auto candidate = pack_shelves(charts, order, middle,
                recipe.resolution, recipe.padding_texels, recipe.maximum_udim_tiles);
            if (candidate) {
                low = middle;
                placements = std::move(candidate);
            } else {
                high = middle;
            }
        }
    }
    if (!placements || !std::isfinite(low) || low <= 0.0) {
        return core::Result<UniqueLightmapUvArtifact>::failure(validation(
            "lightmap chart packing did not produce a positive texel density"));
    }

    std::map<geometry::CornerId, LightmapUvCorner> uv_by_corner;
    for (std::size_t chart_index = 0U; chart_index < charts.size(); ++chart_index) {
        const FaceChart& chart = charts[chart_index];
        const Placement placement = (*placements)[chart_index];
        const std::uint32_t tile_u = placement.tile_index % 10U;
        const std::uint32_t tile_v = placement.tile_index / 10U;
        const std::uint32_t tile_number = udim_number(placement.tile_index);
        for (std::size_t corner_index = 0U; corner_index < chart.vertices.size(); ++corner_index) {
            const auto corner = corner_ids.find(std::make_pair(
                chart.face, chart.vertices[corner_index]));
            if (corner == corner_ids.end()) {
                return core::Result<UniqueLightmapUvArtifact>::failure(validation(
                    "unique-lightmap UV face is missing a stable topology corner"));
            }
            const core::Vec2d local = chart.local_uvs[corner_index];
            const core::Vec2d uv{
                static_cast<double>(tile_u) +
                    (static_cast<double>(placement.x) + (local.x - chart.minimum_u) * low) /
                        recipe.resolution,
                static_cast<double>(tile_v) +
                    (static_cast<double>(placement.y) + (local.y - chart.minimum_v) * low) /
                        recipe.resolution};
            if (!uv_by_corner.emplace(corner->second, LightmapUvCorner{
                    corner->second, chart.face, tile_number, uv}).second) {
                return core::Result<UniqueLightmapUvArtifact>::failure(validation(
                    "unique-lightmap UV generator produced a duplicate corner identity"));
            }
        }
    }

    UniqueLightmapUvArtifact artifact;
    artifact.recipe = recipe;
    artifact.algorithm = "carto.lightmap.face-planar-udim-pack.v2";
    artifact.texels_per_unit = low;
    artifact.corners.reserve(uv_by_corner.size());
    for (const auto& [corner, data] : uv_by_corner) {
        static_cast<void>(corner);
        artifact.corners.push_back(data);
    }
    if (auto result = artifact.refresh_digests(); !result) {
        return core::Result<UniqueLightmapUvArtifact>::failure(result.error());
    }
    if (auto result = artifact.validate(source); !result) {
        return core::Result<UniqueLightmapUvArtifact>::failure(result.error());
    }
    return core::Result<UniqueLightmapUvArtifact>::success(std::move(artifact));
}

core::Result<void> apply_unique_lightmap_uv(
    project::ProjectTransaction& transaction,
    std::uint64_t mesh_asset,
    const UniqueLightmapUvArtifact& artifact) {
    const project::ProjectDocument& staged = transaction.staged_document();
    if (staged.source_namespace() != artifact.recipe.project_namespace ||
        mesh_asset != artifact.recipe.mesh_asset) {
        return core::Result<void>::failure(stale(
            "unique-lightmap UV artifact belongs to another project or mesh asset"));
    }
    const auto mesh = staged.meshes().find(mesh_asset);
    if (mesh == staged.meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "lightmap UV transaction target mesh does not exist"));
    }
    if (auto result = artifact.validate(mesh->second); !result) return result;
    if (mesh->second.uv_sets().find(artifact.recipe.output_uv_set) != nullptr ||
        mesh->second.attributes().find(artifact.recipe.output_layer_id) != nullptr) {
        return core::Result<void>::failure(validation(
            "lightmap UV target set or corner layer already exists"));
    }
    const auto topology = mesh->second.topology();
    if (!topology) return core::Result<void>::failure(topology.error());
    std::map<geometry::CornerId, core::Vec2d> coordinates;
    for (const LightmapUvCorner& corner : artifact.corners) {
        coordinates.emplace(corner.corner, corner.uv);
    }
    std::vector<attributes::AttributeValue> values;
    values.reserve(topology.value().corners.size());
    for (const geometry::CornerRecord& corner : topology.value().corners) {
        const auto coordinate = coordinates.find(corner.id);
        if (coordinate == coordinates.end()) {
            return core::Result<void>::failure(validation(
                "lightmap UV artifact omits a staged mesh corner"));
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
            true,
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

std::string lightmap_uv_transaction_operation(const UniqueLightmapUvArtifact& artifact) {
    if (artifact.settings_digest.is_zero() || artifact.output_digest.is_zero()) return {};
    return "uvl1." + base64url(artifact.settings_digest) + "." +
        base64url(artifact.output_digest);
}

} // namespace carto::production
