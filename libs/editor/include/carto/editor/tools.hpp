#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/editor/command_bus.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::editor {

// The registered native kernel identity is editor metadata, not an AI
// capability. Keeping it beside the factory registration lets descriptive
// consumers detect an ID/factory contract drift instead of guessing from the
// display name or tool ID alone.
enum class ToolKernelKind {
    unclassified,
    editable_mesh_extrude_face,
    editable_mesh_inset_face,
    editable_mesh_delete_face,
    editable_mesh_set_vertex_position,
    editable_mesh_slide_vertex,
    editable_mesh_split_edge,
    editable_mesh_dissolve_edge,
    editable_mesh_tri_to_quad,
    editable_mesh_merge_vertices,
    editable_mesh_poke_face,
};

struct ToolDescriptor {
    std::string id;
    std::string display_name;
    ToolKernelKind kernel = ToolKernelKind::unclassified;
};

struct ToolContext {
    ToolContext(
        ProjectCommandAdmission admission,
        project::ProjectDocument& document,
        const SelectionState& selection);

    [[nodiscard]] const SelectionState& selection() const noexcept { return selection_; }
    [[nodiscard]] core::Result<std::unique_ptr<EditorCommand>>
    make_extrude_selected_face_command(double distance) const;
    [[nodiscard]] core::Result<std::unique_ptr<EditorCommand>>
    make_inset_selected_face_command(double distance) const;
    [[nodiscard]] core::Result<std::unique_ptr<EditorCommand>>
    make_set_selected_vertex_position_command(core::Vec3d position) const;
    [[nodiscard]] core::Result<std::unique_ptr<EditorCommand>>
    make_slide_selected_vertex_command(
        std::optional<geometry::EdgeId> support_edge,
        double factor) const;
    [[nodiscard]] core::Result<std::unique_ptr<EditorCommand>>
    make_delete_selected_face_command(bool remove_orphaned_vertices) const;
    [[nodiscard]] core::Result<std::unique_ptr<EditorCommand>>
    make_split_selected_edge_command(double factor) const;
    [[nodiscard]] core::Result<std::unique_ptr<EditorCommand>>
    make_dissolve_selected_edge_command() const;
    [[nodiscard]] core::Result<std::unique_ptr<EditorCommand>>
    make_tri_to_quad_selected_faces_command() const;
    [[nodiscard]] core::Result<std::unique_ptr<EditorCommand>>
    make_merge_selected_vertices_command() const;
    [[nodiscard]] core::Result<std::unique_ptr<EditorCommand>>
    make_poke_selected_face_command() const;

private:
    friend class ::carto::application::ApplicationSession;

    ProjectCommandAdmission admission_;
    project::ProjectDocument& document_;
    const SelectionState& selection_;
};

struct ToolArguments {
    double distance = 0.0;
    core::Vec3d position{};
    bool remove_orphaned_vertices = true;
    double factor = 0.5;
    std::optional<geometry::EdgeId> support_edge;
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
