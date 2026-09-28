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
    project::ProjectDocument& document,
    const SelectionState& selection)
    : document_(document), selection_(selection) {}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_extrude_selected_face_command(double distance) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<ExtrudeProjectSelectedFaceCommand>(
            document_,
            selection_,
            distance));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_set_selected_vertex_position_command(core::Vec3d position) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<SetProjectSelectedVertexPositionCommand>(
            document_,
            selection_,
            position));
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
        {"mesh.extrude-face", "Extrude selected face"},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_extrude_selected_face_command(arguments.distance);
        });
        !result) {
        return result;
    }
    return register_tool(
        {"mesh.set-vertex-position", "Set selected vertex position"},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_set_selected_vertex_position_command(arguments.position);
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
