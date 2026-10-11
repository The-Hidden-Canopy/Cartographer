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

// The ontology is native and derived from the registered editor tools. It is
// descriptive only: an AI provider can select an operation, but it cannot
// obtain a document, mesh, command bus, or filesystem capability through it.
struct OperationDescriptor {
    std::string id;
    std::string display_name;
    // Stable descriptive identity of the native geometry/editor kernel path
    // behind this tool. This is provenance metadata, not an executable
    // capability or a provider-supplied dispatch target.
    std::string kernel_operation;
    std::vector<std::string> parameters;
    std::string precondition;
    bool preview_supported = false;
    bool auto_approvable = false;
    std::optional<editor::PreviewKind> preview_kind;

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
