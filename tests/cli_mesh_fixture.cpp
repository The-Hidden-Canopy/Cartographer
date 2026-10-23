#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string message) {
    if (!condition) throw std::runtime_error(std::move(message));
}

carto::geometry::EditableMesh make_mesh() {
    using namespace carto;
    geometry::EditableMesh mesh;
    auto inserted = mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {1.0, 1.0, 0.0}},
         {geometry::VertexId{4U}, {0.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}, geometry::VertexId{4U}}}});
    require(static_cast<bool>(inserted), "could not create the mesh CLI fixture");
    attributes::AttributeSet attribute_set({{attributes::AttributeDomain::corner, 4U}});
    require(static_cast<bool>(attribute_set.add_layer(attributes::AttributeLayer{
            {"uv0.corners", attributes::AttributeDomain::corner,
             attributes::AttributeType::vec2},
            {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0},
             core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 1.0}}})),
        "could not create the mesh CLI fixture UV layer");
    attributes::UvSetTable uv_sets;
    require(static_cast<bool>(uv_sets.add(attributes::UvSetDescriptor{
            1U, "UV0", "uv0.corners", true, true, false, 1U})),
        "could not create the mesh CLI fixture UV set");
    require(static_cast<bool>(mesh.set_attribute_payload(
            std::move(attribute_set), std::move(uv_sets))),
        "could not attach the mesh CLI fixture UV payload");
    return mesh;
}

int create_fixture(const std::filesystem::path& path) {
    using namespace carto;
    auto created = project::ProjectDocument::create("Mesh CLI Fixture");
    require(static_cast<bool>(created), "could not create the fixture project");
    project::ProjectDocument document = std::move(created).value();
    journal::Journal journal(path.string() + ".fixture-journal.log");
    const std::string initial_snapshot = document.serialize();
    const auto* initial_bytes = reinterpret_cast<const std::uint8_t*>(initial_snapshot.data());
    require(static_cast<bool>(journal.append(journal::JournalAppend{
                core::Revision{}, document.revision(), "project.create",
                std::span<const std::uint8_t>(initial_bytes, initial_snapshot.size()),
                std::nullopt})),
        "could not anchor the fixture project revision in its journal");
    auto transaction = project::ProjectTransaction::begin(document, journal, "mesh-cli-fixture");
    require(static_cast<bool>(transaction), "could not begin the mesh fixture transaction");
    require(static_cast<bool>(transaction.value().add_mesh(make_mesh())),
        "could not add the fixture mesh");
    require(static_cast<bool>(transaction.value().commit("fixture.mesh.create")),
        "could not commit the fixture mesh");
    require(static_cast<bool>(document.save_atomic(path)), "could not save the fixture project");
    return 0;
}

int mutate_fixture(const std::filesystem::path& path) {
    using namespace carto;
    auto loaded = project::ProjectDocument::load(path);
    require(static_cast<bool>(loaded), "could not reopen the mesh CLI fixture project");
    project::ProjectDocument document = std::move(loaded).value();
    const auto source = document.meshes().find(1U);
    require(source != document.meshes().end(), "mesh CLI fixture mesh is missing");
    geometry::EditableMesh changed = source->second;
    require(static_cast<bool>(changed.set_vertex_position(
            geometry::VertexId{1U}, {0.0, 0.0, 0.25})),
        "could not alter the mesh CLI fixture");
    journal::Journal journal(path.string() + ".fixture-journal.log");
    auto transaction = project::ProjectTransaction::begin(document, journal, "mesh-cli-fixture");
    require(static_cast<bool>(transaction), "could not begin the fixture mutation");
    require(static_cast<bool>(transaction.value().replace_mesh(1U, std::move(changed))),
        "could not stage the fixture mesh mutation");
    require(static_cast<bool>(transaction.value().commit("fixture.mesh.mutate")),
        "could not commit the fixture mesh mutation");
    require(static_cast<bool>(document.save_atomic(path)), "could not save the mutated fixture");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "create") return create_fixture(argv[2]);
        if (argc == 3 && std::string_view(argv[1]) == "mutate") return mutate_fixture(argv[2]);
        throw std::runtime_error("usage: cli_mesh_fixture create|mutate project.carto");
    } catch (const std::exception& error) {
        std::cerr << "FAIL mesh CLI fixture: " << error.what() << '\n';
        return 1;
    }
}
