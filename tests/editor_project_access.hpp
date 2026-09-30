#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/editor/selection.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/project/project.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include <carto/editor/tools.hpp>
#undef private
#if defined(__clang__)
#    pragma clang diagnostic pop
#endif

#include <utility>

namespace carto::project::testing {

// Test-only construction access for editor commands. Production callers must
// receive ProjectCommandAdmission from ApplicationSession or ToolContext.
class EditorProjectAccess final {
public:
    explicit EditorProjectAccess(ProjectDocument& document) noexcept
        : document_(document) {}

    [[nodiscard]] editor::ToolContext tool_context(
        const editor::SelectionState& selection) {
        editor::ProjectCommandAdmission admission;
        return editor::ToolContext(admission, document_, selection);
    }

    [[nodiscard]] std::unique_ptr<editor::EditorCommand> create_mesh_object_command(
        std::string object_name,
        geometry::EditableMesh mesh) {
        editor::ProjectCommandAdmission admission;
        return std::make_unique<editor::CreateMeshObjectCommand>(
            admission, document_, std::move(object_name), std::move(mesh));
    }

    [[nodiscard]] std::unique_ptr<editor::EditorCommand>
    set_project_object_transform_command(
        scene::ObjectId object,
        core::Transform before,
        core::Transform after) {
        editor::ProjectCommandAdmission admission;
        return std::make_unique<editor::SetProjectObjectTransformCommand>(
            admission, document_, object, before, after);
    }

private:
    ProjectDocument& document_;
};

[[nodiscard]] inline EditorProjectAccess editor_access(
    ProjectDocument& document) noexcept {
    return EditorProjectAccess(document);
}

} // namespace carto::project::testing
