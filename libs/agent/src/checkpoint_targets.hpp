#pragma once

#include <carto/agent/workbench.hpp>
#include <carto/ai/ai.hpp>

namespace carto::agent::detail {
// Use the execution grammar, never substring matching, for embedded identities.
inline core::Result<std::vector<geometry::EdgeId>> checkpoint_edges(const AgentPlanStep& step) {
    std::vector<geometry::EdgeId> edges;
    if (const auto* edge = std::get_if<geometry::EdgeId>(&step.target.component)) edges.push_back(*edge);
    if (const auto* vertex = std::get_if<geometry::VertexId>(&step.target.component)) {
        editor::ToolRegistry tools;
        if (auto registered = tools.register_builtin_tools(); !registered)
            return core::Result<std::vector<geometry::EdgeId>>::failure(registered.error());
        editor::AuthoringContext authoring;
        authoring.selection_mode = editor::SelectionMode::vertex;
        authoring.objects = {step.target.object};
        authoring.component_object = step.target.object;
        authoring.component_mesh_revision = core::Revision{};
        authoring.vertices = {*vertex};
        ai::Context context{core::Revision{}, authoring, ai::build_operation_ontology(tools.descriptors())};
        ai::NativePlanner planner;
        const auto proposal = planner.propose(step.intent, context);
        if (!proposal) return core::Result<std::vector<geometry::EdgeId>>::failure(proposal.error());
        if (proposal.value().preview_kind == editor::PreviewKind::slide_vertex &&
            !proposal.value().parameters.support_edge) {
            return core::Result<std::vector<geometry::EdgeId>>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "checkpoint slide requires an explicit support edge to preserve endpoint identity"));
        }
        if (proposal.value().parameters.support_edge) edges.push_back(*proposal.value().parameters.support_edge);
    }
    return core::Result<std::vector<geometry::EdgeId>>::success(std::move(edges));
}
} // namespace carto::agent::detail

