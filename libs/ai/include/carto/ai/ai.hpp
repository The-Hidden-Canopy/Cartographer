#pragma once

#include <carto/core/result.hpp>
#include <carto/editor/authoring_context.hpp>
#include <carto/editor/preview_transaction.hpp>
#include <carto/editor/tools.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::ai {

enum class CapabilityEffect : std::uint32_t {
    geometry = 1U,
    topology = 2U,
};

[[nodiscard]] constexpr std::uint32_t effect_mask(CapabilityEffect effect) noexcept {
    return static_cast<std::uint32_t>(effect);
}

enum class ParameterType : std::uint8_t {
    length,
    scalar,
    count,
    boolean,
    vector3,
    edge_reference,
};

enum class ParameterUnit : std::uint8_t {
    none,
    world_length,
    unit_interval,
    world_position,
};

struct ParameterDefinition {
    std::string name;
    ParameterType type = ParameterType::scalar;
    ParameterUnit unit = ParameterUnit::none;
    std::optional<double> minimum;
    std::optional<double> maximum;
    bool minimum_inclusive = true;
    bool maximum_inclusive = true;
    bool required = true;
    bool inference_allowed = false;
    std::string display_hint;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class CapabilityTarget : std::uint8_t {
    selected_face,
    selected_faces,
    selected_vertex,
    selected_vertices,
    selected_edge,
};

enum class CapabilityData : std::uint8_t {
    selected_component,
    target_mesh,
    incident_topology,
};

enum class CapabilityWrite : std::uint8_t {
    vertex_positions,
    mesh_topology,
};

enum class CapabilityInvalidation : std::uint8_t {
    render_mesh,
    topology_cache,
    surface_normals,
    tangent_basis,
    collision_candidates,
    bakes,
    navigation,
};

enum class CapabilityValidator : std::uint8_t {
    target_exists,
    finite_parameters,
    mesh_valid,
    face_planar,
    face_convex,
    manifold_topology,
    operation_preconditions,
};

enum class CapabilityRisk : std::uint8_t {
    reversible_geometry,
    reversible_topology,
    destructive_topology,
    unknown,
};

struct CapabilityCostHints {
    std::uint32_t objects_read = 0U;
    std::uint32_t topology_elements_read = 0U;
    std::uint32_t topology_elements_written = 0U;
};

// This descriptor is a bounded projection of a native tool registration. It
// contains no executable callback, document reference, or mutation authority.
struct OperationDescriptor {
    std::string id;
    std::string display_name;
    // Stable descriptive identity of the native geometry/editor kernel path
    // behind this tool. This is provenance metadata, not an executable
    // capability or a provider-supplied dispatch target.
    std::string kernel_operation;
    std::vector<std::string> parameters;
    std::vector<ParameterDefinition> parameter_schema;
    std::string precondition;
    bool preview_supported = false;
    bool auto_approvable = false;
    std::optional<editor::PreviewKind> preview_kind;
    bool contract_complete = false;
    bool provider_requestable = false;
    std::uint32_t effects = 0U;
    std::vector<CapabilityTarget> targets;
    std::vector<CapabilityData> reads;
    std::vector<CapabilityWrite> writes;
    std::vector<CapabilityInvalidation> invalidates;
    std::vector<CapabilityValidator> validators;
    CapabilityRisk risk = CapabilityRisk::unknown;
    CapabilityCostHints cost;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Context {
    core::Revision project_revision;
    editor::AuthoringContext authoring;
    std::vector<OperationDescriptor> operations;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Proposal {
    std::string request_id;
    core::Revision base_revision;
    editor::AuthoringContext context;
    editor::PreviewKind preview_kind = editor::PreviewKind::object_transform;
    editor::PreviewParameters parameters;
    std::string tool_id;
    std::string intent;
    std::string rationale;

    [[nodiscard]] core::Result<void> validate(const Context& context) const;
    [[nodiscard]] core::Result<void> validate_auto_approval(
        const Context& context) const;
};

// Converts the editor's registered tools into a bounded, provider-neutral
// ontology. Unknown tools remain visible but are not previewable until a
// native preview contract exists for them.
[[nodiscard]] std::vector<OperationDescriptor> build_operation_ontology(
    std::span<const editor::ToolDescriptor> tools);

// The workbench planner receives only native preview capabilities admitted by
// the task's effect envelope. Unsupported tools remain available for audit in
// the full registry, but are not presented as usable planning choices.
[[nodiscard]] std::vector<OperationDescriptor> project_capabilities(
    std::span<const OperationDescriptor> capabilities,
    std::uint32_t permitted_effects);

// Stable JSON projections are used by future local/MCP adapters. They are
// intentionally projections, not a general JSON authority or a mutation API.
[[nodiscard]] std::string context_json(const Context& context);
[[nodiscard]] std::string proposal_json(const Proposal& proposal);

// A small native planner is useful even when no model runtime is installed:
// it provides a deterministic first workflow and proves the proposal/preview
// boundary. A model-backed adapter can later implement the same contract.
class NativePlanner final {
public:
    [[nodiscard]] core::Result<Proposal> propose(
        std::string_view intent,
        const Context& context);

private:
    std::uint64_t next_request_id_ = 1U;
};

} // namespace carto::ai
