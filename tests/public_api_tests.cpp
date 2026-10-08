#include <carto/application/application.hpp>
#include <carto/editor/command_bus.hpp>
#include <carto/editor/selection.hpp>
#include <carto/editor/tools.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/mesh_attributes/provenance.hpp>
#include <carto/project/project.hpp>

#include <concepts>
#include <string_view>

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
static_assert(requires(const carto::geometry::TopologyEditReceipt& receipt) {
    receipt.validate();
    receipt.serialize();
});
static_assert(requires(std::string_view text) {
    carto::geometry::TopologyEditReceipt::deserialize(text);
});
static_assert(requires(const carto::project::ProjectDocument& document) {
    document.topology_receipts();
});
static_assert(requires(const carto::geometry::TopologySnapshot& snapshot,
                       carto::geometry::EdgeId edge,
    carto::geometry::VertexId vertex) {
    snapshot.edge_loop(edge);
    snapshot.edge_ring(edge);
    snapshot.boundary_loop(edge);
    snapshot.vertex_fan(vertex);
    snapshot.face_region(carto::geometry::FaceId{});
    snapshot.linked_component(vertex);
    snapshot.shortest_path(vertex, vertex);
});
static_assert(requires(const carto::geometry::EditableMesh& source,
                       const carto::geometry::EditableMesh& destination,
                       const carto::geometry::TopologyEditReceipt& receipt) {
    carto::mesh_attributes::provenance_from_receipt(source, destination, receipt);
});

} // namespace

int main() {
    auto source = carto::geometry::make_plane(2.0, 2.0);
    if (!source) return 1;
    carto::geometry::EditableMesh destination = source.value();
    const auto face = source.value().faces_sorted().front().id;
    const auto receipt = destination.extrude_face_with_receipt(face, 0.25);
    if (!receipt) return 1;
    const auto decoded = carto::geometry::TopologyEditReceipt::deserialize(
        receipt.value().serialize());
    if (!decoded || decoded.value().serialize() != receipt.value().serialize()) return 1;
    const auto provenance = carto::mesh_attributes::provenance_from_receipt(
        source.value(), destination, receipt.value());
    return provenance ? 0 : 1;
}
