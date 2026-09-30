#include <carto/application/application.hpp>
#include <carto/editor/command_bus.hpp>
#include <carto/editor/selection.hpp>
#include <carto/editor/tools.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/project/project.hpp>

#include <concepts>

namespace {

template <typename Document>
concept has_public_create_object = requires(Document& document) {
    document.create_object("unauthorized");
};

template <typename Document>
concept has_public_assignment = requires(Document& left, const Document& right) {
    left = right;
};

template <typename Admission>
concept has_public_admission_constructor = requires {
    Admission{};
};

template <typename Session>
concept has_unadmitted_dispatch = requires(
    Session& session,
    const carto::application::ApplicationAction& action) {
    session.dispatch(action);
};

template <typename Document>
concept has_unadmitted_tool_context = requires(
    Document& document,
    const carto::editor::SelectionState& selection) {
    carto::editor::ToolContext(document, selection);
};

static_assert(!has_public_create_object<carto::project::ProjectDocument>);
static_assert(!has_public_assignment<carto::project::ProjectDocument>);
static_assert(!has_public_admission_constructor<carto::editor::ProjectCommandAdmission>);
static_assert(!has_unadmitted_dispatch<carto::application::ApplicationSession>);
static_assert(!has_unadmitted_tool_context<carto::project::ProjectDocument>);

} // namespace

int main() {
    return 0;
}
