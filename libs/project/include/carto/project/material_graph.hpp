#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::project {

enum class MaterialGraphValueType : std::uint8_t { scalar, color, normal };
enum class MaterialGraphNodeKind : std::uint8_t {
    constant_scalar, constant_color, parameter_scalar, parameter_color,
    texture_sample, surface_input, add, multiply, mix, unsupported
};
enum class MaterialSurfaceInput : std::uint8_t {
    unknown, curvature, thickness, contact, weathering, edge_process,
    surface_domain, material_stratigraphy, authored_mask
};

// Provider-neutral authored material data. Texture IDs are project identities;
// shader source, runtime handles, filesystem paths, and device authority are excluded.
struct MaterialGraphNode {
    std::uint64_t id = 0U;
    MaterialGraphNodeKind kind = MaterialGraphNodeKind::constant_scalar;
    MaterialGraphValueType value_type = MaterialGraphValueType::scalar;
    std::array<double, 4U> constants{};
    std::uint64_t texture_asset = 0U;
    std::uint64_t uv_set = 1U;
    MaterialSurfaceInput surface_input = MaterialSurfaceInput::unknown;
    std::string parameter_name;
    std::string unsupported_identity;
    // Input order is semantic: arithmetic=(a,b), mix=(a,b,weight),
    // texture sample=(u,v).
    std::vector<std::uint64_t> inputs;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MaterialGraphOutputs {
    std::uint64_t base_color = 0U;
    std::uint64_t alpha = 0U;
    std::uint64_t roughness = 0U;
    std::uint64_t metallic = 0U;
    std::uint64_t normal = 0U;
    std::uint64_t occlusion = 0U;
    std::uint64_t emissive = 0U;
    std::uint64_t transmission = 0U;
    std::uint64_t subsurface = 0U;
    std::uint64_t sheen = 0U;
    std::uint64_t anisotropy = 0U;
    std::uint64_t clearcoat = 0U;
    std::uint64_t clearcoat_roughness = 0U;
    std::uint64_t opacity = 0U;
    std::uint64_t displacement = 0U;
};

struct MaterialGraphDiagnostic {
    std::string code;
    std::string message;
    std::uint64_t node_id = 0U;
};

struct MaterialGraphCompileReport {
    assets::Sha256Digest source_digest;
    bool supported = false;
    std::vector<std::uint64_t> topological_order;
    std::vector<MaterialGraphDiagnostic> diagnostics;

    [[nodiscard]] core::Result<void> validate() const;
};

class MaterialGraph final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr std::size_t kMaxNodes = 4'096U;
    static constexpr std::size_t kMaxInputsPerNode = 4U;
    static constexpr std::size_t kMaxParameterBytes = 128U;

    std::string identity;
    core::Revision revision;
    std::vector<MaterialGraphNode> nodes;
    MaterialGraphOutputs outputs;

    [[nodiscard]] const MaterialGraphNode* find(std::uint64_t id) const noexcept;
    [[nodiscard]] core::Result<std::vector<std::uint64_t>> topological_order() const;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] core::Result<std::string> serialize() const;
    [[nodiscard]] static core::Result<MaterialGraph> deserialize(std::string_view text);
};

// Performs structural validation and deterministic ordering only. A supported
// report does not claim shader compilation, rendering, or runtime admission.
[[nodiscard]] core::Result<MaterialGraphCompileReport> compile_material_graph(
    const MaterialGraph& graph);
[[nodiscard]] core::Result<void> validate_material_graph_source(
    const MaterialGraph& graph,
    std::string_view current_identity,
    core::Revision current_revision,
    const assets::Sha256Digest& current_digest);

} // namespace carto::project
