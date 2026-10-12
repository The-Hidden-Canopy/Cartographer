#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/local_remesh.hpp>
#include <carto/production/sculpt_stack.hpp>
#include <carto/project/material_catalog.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

#define REQUIRE(condition) \
    do { \
        if (!(condition)) throw std::runtime_error(#condition); \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-local-remesh-tests-" + std::to_string(stamp) + "-" +
                std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

carto::geometry::EditableMesh make_adjacent_quads(
    const std::vector<double>& heights,
    bool with_uv_seam = false) {
    using namespace carto;
    geometry::EditableMesh mesh;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    std::uint64_t next_vertex = 1U;
    std::uint64_t next_face = 1U;
    for (std::size_t component = 0U; component < heights.size(); ++component) {
        const double left = static_cast<double>(component) * 4.0;
        const double right = left + 2.0;
        const double seam = left + 1.0;
        const double height = heights[component];
        const std::uint64_t first = next_vertex;
        vertices.push_back({geometry::VertexId{first}, {left, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first + 1U}, {seam, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first + 2U}, {right, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first + 3U}, {left, height, 0.0}});
        vertices.push_back({geometry::VertexId{first + 4U}, {seam, height, 0.0}});
        vertices.push_back({geometry::VertexId{first + 5U}, {right, height, 0.0}});
        faces.push_back({geometry::FaceId{next_face}, {
            geometry::VertexId{first}, geometry::VertexId{first + 1U},
            geometry::VertexId{first + 4U}, geometry::VertexId{first + 3U}}});
        faces.push_back({geometry::FaceId{next_face + 1U}, {
            geometry::VertexId{first + 1U}, geometry::VertexId{first + 2U},
            geometry::VertexId{first + 5U}, geometry::VertexId{first + 4U}}});
        next_vertex += 6U;
        next_face += 2U;
    }
    REQUIRE(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    if (with_uv_seam) {
        const auto topology = mesh.topology();
        REQUIRE(topology);
        std::vector<geometry::CornerRecord> corners = topology.value().corners;
        std::sort(corners.begin(), corners.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
        std::vector<attributes::AttributeValue> uv_values;
        uv_values.reserve(corners.size());
        for (const auto& corner : corners) {
            const double face_bias = static_cast<double>(corner.face.value) * 100.0;
            uv_values.emplace_back(core::Vec2d{
                face_bias + static_cast<double>(corner.vertex.value),
                face_bias - static_cast<double>(corner.vertex.value)});
        }
        attributes::AttributeSet payload({
            {attributes::AttributeDomain::vertex, mesh.vertex_count()},
            {attributes::AttributeDomain::edge, topology.value().edges.size()},
            {attributes::AttributeDomain::face, mesh.face_count()},
            {attributes::AttributeDomain::corner, corners.size()},
        });
        REQUIRE(payload.add_layer(attributes::AttributeLayer{
            {"uv0", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
            std::move(uv_values)}));
        attributes::UvSetTable uv_sets;
        REQUIRE(uv_sets.add(attributes::UvSetDescriptor{
            7U, "Render UV", "uv0", true, true, false, 1U}));
        REQUIRE(mesh.set_attribute_payload(std::move(payload), std::move(uv_sets)));
    }
    REQUIRE(mesh.validate());
    return mesh;
}

carto::project::ProjectDocument make_project(
    carto::journal::Journal& journal,
    carto::geometry::EditableMesh mesh,
    std::uint64_t& mesh_asset) {
    using namespace carto;
    auto document = project::ProjectDocument::create("Local Remesh Test");
    REQUIRE(document);
    const std::string snapshot = document.value().serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(snapshot.data());
    REQUIRE(journal.append(journal::JournalAppend{
        core::Revision{}, document.value().revision(), "project.create",
        std::span<const std::uint8_t>(bytes, snapshot.size()), std::nullopt}));
    auto transaction = project::ProjectTransaction::begin(
        document.value(), journal, "local-remesh-tests");
    REQUIRE(transaction);
    const auto added = transaction.value().add_mesh(std::move(mesh));
    REQUIRE(added);
    mesh_asset = added.value();
    REQUIRE(transaction.value().commit("local-remesh.fixture"));
    return std::move(document).value();
}

carto::production::LocalRemeshSettings settings_for(
    carto::core::Vec3d center,
    double maximum_edge_length,
    std::uint32_t max_splits,
    double region_radius = 100.0) {
    carto::production::LocalRemeshSettings settings;
    settings.center = center;
    settings.region_radius = region_radius;
    settings.maximum_edge_length = maximum_edge_length;
    settings.max_splits = max_splits;
    return settings;
}

void patch_is_deterministic_preserves_uv_seam_and_persists_receipts() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "project.journal");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_adjacent_quads({1.5, 1.5, 1.2}, true), mesh_asset);
    const auto initial_topology = document.meshes().at(mesh_asset).topology();
    REQUIRE(initial_topology);
    std::vector<std::pair<geometry::EdgeId, double>> eligible;
    for (const auto& edge : initial_topology.value().edges) {
        if (!edge.second_half_edge.has_value()) continue;
        const auto* first = document.meshes().at(mesh_asset).find_vertex(edge.first);
        const auto* second = document.meshes().at(mesh_asset).find_vertex(edge.second);
        REQUIRE(first != nullptr && second != nullptr);
        const double length = std::hypot(
            second->position.x - first->position.x,
            second->position.y - first->position.y,
            second->position.z - first->position.z);
        if (length > 1.1) eligible.emplace_back(edge.id, length);
    }
    std::sort(eligible.begin(), eligible.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) return left.second > right.second;
        return left.first < right.first;
    });
    REQUIRE(eligible.size() == 3U);

    auto transaction = project::ProjectTransaction::begin(
        document, journal, "local-remesh-tests");
    REQUIRE(transaction);
    const auto receipt = production::stage_local_remesh_patch(
        transaction.value(), mesh_asset, settings_for({5.0, 0.6, 0.0}, 1.1, 3U));
    REQUIRE(receipt);
    REQUIRE(receipt.value().validate());
    REQUIRE(receipt.value().splits.size() == 3U);
    for (std::size_t index = 0U; index < eligible.size(); ++index) {
        REQUIRE(receipt.value().splits[index].source_edge == eligible[index].first);
    }
    REQUIRE(receipt.value().splits[0].source_edge_length == 1.5);
    REQUIRE(receipt.value().splits[1].source_edge_length == 1.5);
    REQUIRE(receipt.value().splits[2].source_edge_length == 1.2);
    auto stale_receipt = receipt.value();
    stale_receipt.splits.front().source_edge_length += 0.01;
    REQUIRE(!stale_receipt.validate());

    const auto& remeshed = transaction.value().staged_document().meshes().at(mesh_asset);
    REQUIRE(remeshed.vertex_count() == 21U);
    REQUIRE(remeshed.face_count() == 6U);
    REQUIRE(remeshed.uv_sets().find(7U) != nullptr);
    const auto output_topology = remeshed.topology();
    REQUIRE(output_topology);
    std::vector<geometry::CornerRecord> output_corners = output_topology.value().corners;
    std::sort(output_corners.begin(), output_corners.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    const auto* uv_layer = remeshed.attributes().find("uv0");
    REQUIRE(uv_layer != nullptr);
    std::map<geometry::FaceId, core::Vec2d> seam_uvs;
    for (std::size_t index = 0U; index < output_corners.size(); ++index) {
        const auto& corner = output_corners[index];
        if (corner.vertex != receipt.value().splits.front().topology.created_vertices.front()) {
            continue;
        }
        seam_uvs.emplace(corner.face, std::get<core::Vec2d>(uv_layer->values[index]));
    }
    REQUIRE(seam_uvs.size() == 2U);
    const core::Vec2d first_seam_uv = seam_uvs.begin()->second;
    const core::Vec2d second_seam_uv = std::next(seam_uvs.begin())->second;
    REQUIRE(first_seam_uv.x != second_seam_uv.x || first_seam_uv.y != second_seam_uv.y);

    const std::string operation = receipt.value().transaction_operation();
    REQUIRE(!operation.empty());
    const auto committed = transaction.value().commit(operation);
    REQUIRE(committed);
    REQUIRE(committed.value().operation == operation);
    REQUIRE(document.topology_receipts().size() == 3U);
    for (std::size_t index = 0U; index < document.topology_receipts().size(); ++index) {
        REQUIRE(document.topology_receipts()[index].receipt.serialize() ==
            receipt.value().splits[index].topology.serialize());
    }

    const auto saved = document.save_atomic(directory.path() / "remeshed.cartographer");
    REQUIRE(saved);
    const auto reopened = project::ProjectDocument::load(saved.value().path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().topology_receipts().size() == 3U);
    REQUIRE(reopened.value().serialize() == document.serialize());
}

void material_regions_survive_remesh_and_project_reopen() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "project.journal");

    geometry::EditableMesh mesh = make_adjacent_quads({1.5}, false);
    const auto topology = mesh.topology();
    REQUIRE(topology);
    attributes::AttributeSet payload({
        {attributes::AttributeDomain::vertex, mesh.vertex_count()},
        {attributes::AttributeDomain::edge, topology.value().edges.size()},
        {attributes::AttributeDomain::face, mesh.face_count()},
        {attributes::AttributeDomain::corner, topology.value().corners.size()},
    });
    REQUIRE(payload.add_layer(attributes::AttributeLayer{
        {"condition.material_region", attributes::AttributeDomain::face,
            attributes::AttributeType::uint32},
        {3U, 9U}}));
    REQUIRE(mesh.set_attribute_payload(std::move(payload), attributes::UvSetTable{}));

    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, std::move(mesh), mesh_asset);
    project::MaterialCatalog catalog;
    for (const auto [material_id, name] : {
             std::pair<render::MaterialId, const char*>{71U, "Painted shell"},
             std::pair<render::MaterialId, const char*>{72U, "Rubber grip"}}) {
        render::MaterialAsset material;
        material.id = material_id;
        material.name = name;
        material.revision = core::Revision{1U};
        REQUIRE(catalog.insert_material(std::move(material)));
    }
    REQUIRE(catalog.insert_assignment({{mesh_asset, 3U}, 71U, std::nullopt}));
    REQUIRE(catalog.insert_assignment({{mesh_asset, 9U}, 72U, std::nullopt}));
    auto catalog_transaction = project::ProjectTransaction::begin(
        document, journal, "local-remesh-tests");
    REQUIRE(catalog_transaction);
    REQUIRE(catalog_transaction.value().set_material_catalog(std::move(catalog)));
    REQUIRE(catalog_transaction.value().commit("local-remesh.material-fixture"));

    auto transaction = project::ProjectTransaction::begin(
        document, journal, "local-remesh-tests");
    REQUIRE(transaction);
    const auto receipt = production::stage_local_remesh_patch(
        transaction.value(), mesh_asset, settings_for({1.0, 0.75, 0.0}, 1.1, 1U, 0.1));
    REQUIRE(receipt);
    REQUIRE(receipt.value().splits.size() == 1U);

    const auto& remeshed = transaction.value().staged_document().meshes().at(mesh_asset);
    const auto* regions = remeshed.attributes().find("condition.material_region");
    REQUIRE(regions != nullptr);
    REQUIRE(regions->values.size() == 2U);
    REQUIRE(std::get<std::uint32_t>(regions->values[0]) == 3U);
    REQUIRE(std::get<std::uint32_t>(regions->values[1]) == 9U);
    REQUIRE(transaction.value().staged_document().material_catalog().assignments().size() == 2U);
    REQUIRE(transaction.value().commit(receipt.value().transaction_operation()));

    const auto saved = document.save_atomic(directory.path() / "material-remeshed.cartographer");
    REQUIRE(saved);
    const auto reopened = project::ProjectDocument::load(saved.value().path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().validate());
    REQUIRE(reopened.value().material_catalog().assignments().size() == 2U);
    const auto* reopened_regions = reopened.value().meshes().at(mesh_asset)
        .attributes().find("condition.material_region");
    REQUIRE(reopened_regions != nullptr);
    REQUIRE(reopened_regions->values.size() == 2U);
    REQUIRE(std::get<std::uint32_t>(reopened_regions->values[0]) == 3U);
    REQUIRE(std::get<std::uint32_t>(reopened_regions->values[1]) == 9U);
}

void boundary_policy_rejects_boundary_edges_until_explicitly_enabled() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "project.journal");
    geometry::EditableMesh triangle;
    REQUIRE(triangle.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {3.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 3.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}}}));
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, std::move(triangle), mesh_asset);
    const std::string source_before = document.serialize();
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "local-remesh-tests");
    REQUIRE(transaction);

    auto settings = settings_for({1.5, 1.5, 0.0}, 4.0, 1U, 0.1);
    const auto rejected = production::stage_local_remesh_patch(
        transaction.value(), mesh_asset, settings);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::not_found);
    REQUIRE(transaction.value().active());
    REQUIRE(transaction.value().staged_document().serialize() == source_before);

    settings.preserve_boundary = false;
    const auto admitted = production::stage_local_remesh_patch(
        transaction.value(), mesh_asset, settings);
    REQUIRE(admitted);
    REQUIRE(admitted.value().splits.size() == 1U);
    REQUIRE(admitted.value().splits.front().source_edge_length > 4.0);
    REQUIRE(transaction.value().commit(admitted.value().transaction_operation()));
    REQUIRE(document.topology_receipts().size() == 1U);
}

void boundary_skip_invalid_settings_and_inactive_transaction_fail_closed() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "project.journal");
    geometry::EditableMesh triangle;
    REQUIRE(triangle.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {3.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 3.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}}}));
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, std::move(triangle), mesh_asset);
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "local-remesh-tests");
    REQUIRE(transaction);
    const core::Revision document_revision = document.revision();
    const std::string document_before = document.serialize();
    const auto no_internal = production::stage_local_remesh_patch(
        transaction.value(), mesh_asset, settings_for({1.0, 1.0, 0.0}, 1.0, 1U));
    REQUIRE(!no_internal);
    REQUIRE(no_internal.error().code == core::ErrorCode::not_found);
    REQUIRE(transaction.value().active());
    REQUIRE(document.revision() == document_revision);

    auto invalid_settings = settings_for({0.0, 0.0, 0.0}, 1.0, 1U, 0.0);
    const auto invalid = production::stage_local_remesh_patch(
        transaction.value(), mesh_asset, invalid_settings);
    REQUIRE(!invalid);
    REQUIRE(invalid.error().code == core::ErrorCode::invalid_argument);
    REQUIRE(transaction.value().active());
    REQUIRE(document.serialize() == document_before);
    REQUIRE(transaction.value().create_object("Unrelated staged edit"));
    const auto nonempty = production::stage_local_remesh_patch(
        transaction.value(), mesh_asset, settings_for({1.0, 1.0, 0.0}, 1.0, 1U));
    REQUIRE(!nonempty);
    REQUIRE(nonempty.error().code == core::ErrorCode::invalid_state);
    REQUIRE(transaction.value().active());
    REQUIRE(transaction.value().staged_document().scene().objects_sorted().size() == 1U);
    REQUIRE(transaction.value().rollback());
    const auto inactive = production::stage_local_remesh_patch(
        transaction.value(), mesh_asset, settings_for({1.0, 1.0, 0.0}, 1.0, 1U));
    REQUIRE(!inactive);
    REQUIRE(inactive.error().code == core::ErrorCode::invalid_state);
    REQUIRE(document.revision() == document_revision);
}

void exhausted_budget_and_discrete_attribute_failures_roll_back() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "project.journal");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_adjacent_quads({1.5, 1.5}, false), mesh_asset);
    const core::Revision document_revision = document.revision();
    const std::string source_document = document.serialize();
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "local-remesh-tests");
    REQUIRE(transaction);
    const auto exhausted = production::stage_local_remesh_patch(
        transaction.value(), mesh_asset, settings_for({4.0, 0.75, 0.0}, 1.1, 1U));
    REQUIRE(!exhausted);
    REQUIRE(exhausted.error().code == core::ErrorCode::unsupported);
    REQUIRE(!transaction.value().active());
    REQUIRE(document.revision() == document_revision);
    REQUIRE(document.serialize() == source_document);
    REQUIRE(document.topology_receipts().empty());

    auto discrete_mesh = make_adjacent_quads({1.5}, false);
    auto topology = discrete_mesh.topology();
    REQUIRE(topology);
    attributes::AttributeSet payload({
        {attributes::AttributeDomain::vertex, discrete_mesh.vertex_count()},
        {attributes::AttributeDomain::edge, topology.value().edges.size()},
        {attributes::AttributeDomain::face, discrete_mesh.face_count()},
        {attributes::AttributeDomain::corner, topology.value().corners.size()},
    });
    std::vector<attributes::AttributeValue> labels(
        discrete_mesh.vertex_count(), std::uint32_t{7U});
    REQUIRE(payload.add_layer(attributes::AttributeLayer{
        {"vertex.label", attributes::AttributeDomain::vertex,
            attributes::AttributeType::uint32},
        std::move(labels)}));
    REQUIRE(discrete_mesh.set_attribute_payload(std::move(payload), attributes::UvSetTable{}));

    TempDirectory discrete_directory;
    journal::Journal discrete_journal(discrete_directory.path() / "project.journal");
    std::uint64_t discrete_asset = 0U;
    project::ProjectDocument discrete_document = make_project(
        discrete_journal, std::move(discrete_mesh), discrete_asset);
    auto discrete_transaction = project::ProjectTransaction::begin(
        discrete_document, discrete_journal, "local-remesh-tests");
    REQUIRE(discrete_transaction);
    const std::string discrete_before = discrete_document.serialize();
    const auto unsupported_layer = production::stage_local_remesh_patch(
        discrete_transaction.value(), discrete_asset,
        settings_for({1.0, 0.75, 0.0}, 1.1, 1U));
    REQUIRE(!unsupported_layer);
    REQUIRE(unsupported_layer.error().code == core::ErrorCode::unsupported);
    REQUIRE(!discrete_transaction.value().active());
    REQUIRE(discrete_document.serialize() == discrete_before);
    REQUIRE(discrete_document.topology_receipts().empty());
}

void attached_sculpt_stack_is_not_silently_invalidated() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "project.journal");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_adjacent_quads({1.5}, false), mesh_asset);
    const auto stack = production::make_sculpt_stack(document, mesh_asset);
    REQUIRE(stack);
    auto stack_transaction = project::ProjectTransaction::begin(
        document, journal, "local-remesh-tests");
    REQUIRE(stack_transaction);
    REQUIRE(production::stage_sculpt_stack(stack_transaction.value(), stack.value()));
    const auto stack_digest = stack.value().digest();
    REQUIRE(stack_digest);
    REQUIRE(stack_transaction.value().commit(
        production::sculpt_stack_transaction_operation(stack_digest.value())));

    const std::string before = document.serialize();
    auto remesh_transaction = project::ProjectTransaction::begin(
        document, journal, "local-remesh-tests");
    REQUIRE(remesh_transaction);
    const auto rejected = production::stage_local_remesh_patch(
        remesh_transaction.value(), mesh_asset, settings_for({1.0, 0.75, 0.0}, 1.1, 1U));
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::unsupported);
    REQUIRE(remesh_transaction.value().active());
    REQUIRE(document.serialize() == before);
    REQUIRE(remesh_transaction.value().rollback());
}

} // namespace

int main() {
    try {
        patch_is_deterministic_preserves_uv_seam_and_persists_receipts();
        std::cout << "PASS deterministic bounded remesh, UV seam preservation, and receipt persistence\n";
        material_regions_survive_remesh_and_project_reopen();
        std::cout << "PASS material-region identity and catalog bindings survive remesh and reopen\n";
        boundary_policy_rejects_boundary_edges_until_explicitly_enabled();
        std::cout << "PASS boundary edges stay fixed unless the authored policy enables them\n";
        boundary_skip_invalid_settings_and_inactive_transaction_fail_closed();
        std::cout << "PASS boundary skip, invalid settings, and inactive transaction rejection\n";
        exhausted_budget_and_discrete_attribute_failures_roll_back();
        std::cout << "PASS split-budget and discrete attribute failures roll back\n";
        attached_sculpt_stack_is_not_silently_invalidated();
        std::cout << "PASS attached SculptStack requires an explicit rebase\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL local remesh tests: " << error.what() << '\n';
        return 1;
    }
}
