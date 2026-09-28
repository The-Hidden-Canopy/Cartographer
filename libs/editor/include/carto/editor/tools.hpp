#pragma once

#include <carto/core/result.hpp>
#include <carto/editor/command_bus.hpp>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::editor {

struct ToolDescriptor {
    std::string id;
    std::string display_name;
};

struct ToolContext {
    ToolContext(const SelectionState& selection, geometry::EditableMesh& mesh);

    [[nodiscard]] const SelectionState& selection() const noexcept { return selection_; }
    [[nodiscard]] core::Result<std::unique_ptr<EditorCommand>>
    make_extrude_selected_face_command(double distance) const;

private:
    const SelectionState& selection_;
    geometry::EditableMesh& mesh_;
};

struct ToolArguments {
    double distance = 0.0;
};

using ToolFactory = std::function<core::Result<std::unique_ptr<EditorCommand>>(
    const ToolContext&,
    const ToolArguments&)>;

class ToolRegistry {
public:
    [[nodiscard]] core::Result<void> register_tool(
        ToolDescriptor descriptor,
        ToolFactory factory);
    [[nodiscard]] core::Result<void> register_builtin_tools();

    [[nodiscard]] core::Result<void> invoke(
        std::string_view tool_id,
        const ToolContext& context,
        const ToolArguments& arguments,
        CommandBus& history) const;

    [[nodiscard]] std::vector<ToolDescriptor> descriptors() const;

private:
    struct Entry {
        ToolDescriptor descriptor;
        ToolFactory factory;
    };

    std::map<std::string, Entry> tools_;
};

} // namespace carto::editor
