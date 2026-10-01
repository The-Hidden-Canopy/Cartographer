#include <carto/editor/tools.hpp>

#include <utility>

namespace carto::editor {

namespace {

using core::Diagnostic;
using core::ErrorCode;

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

} // namespace

ToolContext::ToolContext(
    ProjectCommandAdmission admission,
    project::ProjectDocument& document,
    const SelectionState& selection)
    : admission_(admission), document_(document), selection_(selection) {}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_extrude_selected_face_command(double distance) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<ExtrudeProjectSelectedFaceCommand>(
            admission_,
            document_,
            selection_,
            distance));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_inset_selected_face_command(double distance) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<InsetProjectSelectedFaceCommand>(
            admission_,
            document_,
            selection_,
            distance));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_set_selected_vertex_position_command(core::Vec3d position) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<SetProjectSelectedVertexPositionCommand>(
            admission_,
            document_,
            selection_,
            position));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_delete_selected_face_command(bool remove_orphaned_vertices) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<DeleteProjectSelectedFaceCommand>(
            admission_,
            document_,
            selection_,
            remove_orphaned_vertices));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_split_selected_edge_command(double factor) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<SplitProjectSelectedEdgeCommand>(
            admission_,
            document_,
            selection_,
            factor));
}

core::Result<void> ToolRegistry::register_tool(
    ToolDescriptor descriptor,
    ToolFactory factory) {
    if (descriptor.id.empty() || descriptor.display_name.empty()) {
        return core::Result<void>::failure(
            invalid("tool registration requires an id and display name"));
    }
    if (!factory) {
        return core::Result<void>::failure(invalid("tool registration requires a factory"));
    }
    if (tools_.contains(descriptor.id)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "tool id is already registered"));
    }
    const std::string id = descriptor.id;
    tools_.emplace(id, Entry{std::move(descriptor), std::move(factory)});
    return core::Result<void>::success();
}

core::Result<void> ToolRegistry::register_builtin_tools() {
    if (auto result = register_tool(
        {"mesh.extrude-face", "Extrude selected face",
         ToolKernelKind::editable_mesh_extrude_face},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_extrude_selected_face_command(arguments.distance);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.inset-face", "Inset selected face",
         ToolKernelKind::editable_mesh_inset_face},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_inset_selected_face_command(arguments.distance);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.remove-face", "Delete selected face",
         ToolKernelKind::editable_mesh_delete_face},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_delete_selected_face_command(
                arguments.remove_orphaned_vertices);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.set-vertex-position", "Set selected vertex position",
         ToolKernelKind::editable_mesh_set_vertex_position},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_set_selected_vertex_position_command(arguments.position);
        });
        !result) {
        return result;
    }
    return register_tool(
        {"mesh.split-edge", "Split selected edge",
         ToolKernelKind::editable_mesh_split_edge},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_split_selected_edge_command(arguments.factor);
        });
}

core::Result<void> ToolRegistry::invoke(
    std::string_view tool_id,
    const ToolContext& context,
    const ToolArguments& arguments,
    CommandBus& history) const {
    const auto iterator = tools_.find(std::string(tool_id));
    if (iterator == tools_.end()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "tool id is not registered"));
    }
    auto command = iterator->second.factory(context, arguments);
    if (!command) {
        return core::Result<void>::failure(command.error());
    }
    if (!command.value()) {
        return core::Result<void>::failure(
            invalid("tool factory returned a null command"));
    }
    return history.execute(std::move(command.value()));
}

std::vector<ToolDescriptor> ToolRegistry::descriptors() const {
    std::vector<ToolDescriptor> result;
    result.reserve(tools_.size());
    for (const auto& [id, entry] : tools_) {
        static_cast<void>(id);
        result.push_back(entry.descriptor);
    }
    return result;
}

} // namespace carto::editor
