#include <carto/assets/blob_store.hpp>
#include <carto/production/prepared_publication.hpp>
#include <carto/production/prepared_scene.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/project/material_catalog.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace carto;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "fixture product opens");
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    require(size >= 0, "fixture product size is readable");
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    require(static_cast<bool>(input), "fixture product reads completely");
    return bytes;
}

std::string read_text(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::span<const std::uint8_t> as_bytes(const std::vector<std::uint8_t>& bytes) {
    return {bytes.data(), bytes.size()};
}

const production::PreparedProductReference& product(
    const production::PreparedSceneEnvelope& envelope,
    std::string_view identity) {
    for (const auto& candidate : envelope.products) {
        if (candidate.identity == identity) return candidate;
    }
    require(false, "compatibility fixture declares the requested product");
    return envelope.products.front();
}

void fixture_is_producer_reopenable(const std::filesystem::path& root) {
    const auto selected = production::read_selected_prepared_scene(root);
    require(static_cast<bool>(selected), "Cartographer reads the compatibility publication");
    require(selected.value().has_value(), "compatibility publication has a selected revision");

    const auto& publication = selected.value().value();
    require(publication.prepared_scene_digest.hex() ==
                "7500eee215cdf83927c0b8bc89ff07003170b5f90021308d0ca7a0fabfd6b53e",
            "compatibility fixture keeps its declared prepared-scene identity");
    require(publication.envelope.source_namespace == "cartographer:compatibility-fixture",
            "compatibility fixture keeps its source namespace");
    require(publication.envelope.source_revision.value() == 7U,
            "compatibility fixture keeps its source revision");
    require(publication.envelope.profile_identity ==
                "cartographer.portable-prepared-scene.v1",
            "compatibility fixture keeps its profile identity");
    require(publication.envelope.coordinates.meters_per_unit == 0.01 &&
                publication.envelope.coordinates.handedness ==
                    production::PreparedHandedness::right_handed &&
                publication.envelope.coordinates.up == production::PreparedAxis::positive_z &&
                publication.envelope.coordinates.forward == production::PreparedAxis::positive_y &&
                publication.envelope.nodes.size() == 1U &&
                publication.envelope.nodes[0].local_transform.translation.x == 1.0 &&
                publication.envelope.nodes[0].local_transform.translation.y == 2.0 &&
                publication.envelope.nodes[0].local_transform.translation.z == 3.0,
            "producer fixture exercises a non-identity axis/unit frame and source node translation");

    const auto& render_reference = product(publication.envelope, "mesh.10.render");
    require(render_reference.kind == production::PreparedProductKind::render_mesh &&
                render_reference.format_version == 2U,
            "compatibility fixture retains its declared v2 render-mesh format");
    const auto render_bytes = read_bytes(publication.revision_path / render_reference.relative_path);
    require(assets::sha256(as_bytes(render_bytes)) == render_reference.content_digest,
            "Cartographer verifies the render product digest from the fixture manifest");
    const auto render_mesh = production::RenderMeshArtifact::deserialize(as_bytes(render_bytes));
    require(static_cast<bool>(render_mesh), "Cartographer reopens the fixture render mesh");
    require(static_cast<bool>(render_mesh.value().validate()),
            "Cartographer validates the fixture render mesh after reopening");
    require(render_mesh.value().vertices.size() == 4U &&
                render_mesh.value().indices.size() == 6U &&
                render_mesh.value().submeshes.size() == 2U,
            "fixture render mesh retains its producer-authored topology");

    const auto& catalog_reference = product(publication.envelope, "source.material-catalog");
    require(catalog_reference.kind == production::PreparedProductKind::material_catalog &&
                catalog_reference.format_version == 1U,
            "compatibility fixture retains its original v1 material-catalog declaration");
    const auto catalog_path = publication.revision_path / catalog_reference.relative_path;
    const auto catalog_text = read_text(catalog_path);
    require(assets::sha256(std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(catalog_text.data()), catalog_text.size()}) ==
                catalog_reference.content_digest,
            "Cartographer verifies the material product digest from the fixture manifest");
    const auto catalog = project::MaterialCatalog::deserialize(catalog_text);
    require(static_cast<bool>(catalog), "Cartographer reopens the fixture material catalog");
    require(static_cast<bool>(catalog.value().validate()),
            "Cartographer validates the fixture material catalog after reopening");
    require(catalog.value().textures().empty() && catalog.value().samplers().empty() &&
                catalog.value().variants().size() == 1U && catalog.value().materials().size() == 2U &&
                catalog.value().assignments().size() == 2U,
            "compatibility catalog stays within the untextured V1 consumer subset with one bounded scalar variant");
    const auto& paint = catalog.value().materials().at(300U);
    const auto& rubber = catalog.value().materials().at(301U);
    const auto& satin = catalog.value().variants().at(77U);
    const auto resolved_satin = catalog.value().resolve_material(300U, 77U);
    require(satin.base_material == 300U && satin.permitted_mesh_assets == std::vector<std::uint64_t>{10U} &&
                satin.overrides.roughness == 0.65 && resolved_satin &&
                std::abs(resolved_satin.value().roughness - 0.65) < 1e-12,
            "Cartographer variant preserves its base, mesh scope, and resolved roughness override");
    require(paint.name == "Fixture paint" && rubber.name == "Fixture rubber" &&
                paint.definition.base_color_factor.x == 1.0 && paint.definition.base_color_factor.y == 1.0 &&
                paint.definition.base_color_factor.z == 1.0 && paint.definition.base_color_factor.w == 1.0 &&
                rubber.definition.base_color_factor.x == 1.0 && rubber.definition.base_color_factor.y == 1.0 &&
                rubber.definition.base_color_factor.z == 1.0 && rubber.definition.base_color_factor.w == 1.0 &&
                paint.definition.metallic == 0.0 && rubber.definition.metallic == 0.0 &&
                std::abs(paint.definition.roughness - 0.35) < 1e-12 &&
                std::abs(rubber.definition.roughness - 0.8) < 1e-12 &&
                paint.definition.dielectric_f0 == 0.04 && rubber.definition.dielectric_f0 == 0.04 &&
                paint.definition.alpha_mode == render::AlphaMode::opaque &&
                rubber.definition.alpha_mode == render::AlphaMode::opaque,
            "Cartographer fixture retains exact source factors consumed by downstream runtime templates");
    require(catalog.value().assignments().at(project::MaterialRegionKey{10U, 7U}).material == 300U &&
                catalog.value().assignments().at(project::MaterialRegionKey{10U, 8U}).material == 301U &&
                catalog.value().assignments().at(project::MaterialRegionKey{10U, 7U}).variant == 77U &&
                !catalog.value().assignments().at(project::MaterialRegionKey{10U, 8U}).variant.has_value(),
            "Cartographer fixture retains region-to-material identity and the scoped variant assignment");

    const auto manifest = read_text(publication.revision_path / "manifest.carto-prepared");
    const auto reopened_envelope = production::PreparedSceneEnvelope::deserialize(manifest);
    require(static_cast<bool>(reopened_envelope),
            "Cartographer reopens the exact manifest used by the destination adapter");
    require(reopened_envelope.value().canonical_digest().value() ==
                publication.prepared_scene_digest,
            "Cartographer's canonical manifest identity matches the selected revision");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: cartographer_prepared_publication_compatibility_tests <fixture-root>\n";
        return EXIT_FAILURE;
    }
    fixture_is_producer_reopenable(argv[1]);
    std::cout << "cartographer prepared publication compatibility: PASS\n";
    return EXIT_SUCCESS;
}
