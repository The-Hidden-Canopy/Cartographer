#include <carto/assets/blob_store.hpp>
#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/material_catalog.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-material-catalog-tests-" + std::to_string(stamp));
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

carto::geometry::EditableMesh make_two_region_mesh(
    std::uint32_t first_region = 3U,
    std::uint32_t second_region = 9U) {
    using namespace carto;
    geometry::EditableMesh mesh;
    require(static_cast<bool>(mesh.insert_bulk(
                {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
                 {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
                 {geometry::VertexId{3U}, {1.0, 1.0, 0.0}},
                 {geometry::VertexId{4U}, {0.0, 1.0, 0.0}}},
                {{geometry::FaceId{1U},
                  {geometry::VertexId{1U}, geometry::VertexId{2U},
                   geometry::VertexId{3U}}},
                 {geometry::FaceId{2U},
                  {geometry::VertexId{1U}, geometry::VertexId{3U},
                   geometry::VertexId{4U}}}})),
        "two-region mesh topology is valid");

    attributes::AttributeSet attributes({
        {attributes::AttributeDomain::corner, 6U},
        {attributes::AttributeDomain::face, 2U},
    });
    require(static_cast<bool>(attributes.add_layer(attributes::AttributeLayer{
                {"uv0.corners", attributes::AttributeDomain::corner,
                 attributes::AttributeType::vec2},
                {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0},
                 core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 0.0},
                 core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 1.0}}})),
        "two-region mesh owns explicit UV0 corners");
    require(static_cast<bool>(attributes.add_layer(attributes::AttributeLayer{
                {"condition.material_region", attributes::AttributeDomain::face,
                 attributes::AttributeType::uint32},
                {first_region, second_region}})),
        "two-region mesh owns stable authored material-region identities");
    attributes::UvSetTable uv_sets;
    require(static_cast<bool>(uv_sets.add(attributes::UvSetDescriptor{
                1U, "UV0", "uv0.corners", true, true, false, 1U})),
        "two-region mesh declares the UV set used by material bindings");
    require(static_cast<bool>(mesh.set_attribute_payload(
                std::move(attributes), std::move(uv_sets))),
        "two-region mesh accepts its source attributes");
    require(static_cast<bool>(mesh.validate()), "two-region mesh validates");
    return mesh;
}

carto::render::MaterialAsset material_fixture(
    carto::render::MaterialId id,
    std::string name,
    bool bind_shared_texture) {
    using namespace carto;
    render::MaterialAsset material;
    material.id = id;
    material.name = std::move(name);
    material.revision = core::Revision{1U};
    material.definition.roughness = id == 71U ? 0.42 : 0.76;
    if (bind_shared_texture) {
        material.definition.base_color.asset = 501U;
        material.definition.base_color.uv_set = 1U;
        material.definition.base_color.sampler = 31U;
        material.definition.base_color.interpretation =
            render::TextureInterpretation::srgb;
        material.definition.metallic_roughness.asset = 501U;
        material.definition.metallic_roughness.uv_set = 1U;
        material.definition.metallic_roughness.sampler = 31U;
        material.definition.metallic_roughness.interpretation =
            render::TextureInterpretation::linear;
        material.definition.metallic_roughness.channels.rgba = {
            render::TextureChannel::blue,
            render::TextureChannel::green,
            render::TextureChannel::constant_zero,
            render::TextureChannel::constant_one,
        };
    }
    return material;
}

carto::project::MaterialCatalog catalog_fixture(std::uint64_t mesh_asset) {
    using namespace carto;
    project::MaterialCatalog catalog;
    require(static_cast<bool>(catalog.insert_texture(project::TextureSourceRecord{
                .id = 501U,
                .name = "Shared workshop surface",
                .relative_locator = "textures/workshop-shared.png",
                .content_digest = digest("workshop-shared-image"),
                .format = assets::TextureSourceFormat::png,
                .width = 1024U,
                .height = 1024U,
                .provenance = "workshop-fixture/source-v1",
                .revision = core::Revision{1U},
            })),
        "texture source enters catalog");
    require(static_cast<bool>(catalog.insert_sampler(project::SamplerIntent{
                .id = 31U,
                .name = "Workshop repeat linear",
                .address_u = project::SamplerAddressMode::repeat,
                .address_v = project::SamplerAddressMode::repeat,
                .address_w = project::SamplerAddressMode::clamp_to_edge,
                .minification = project::SamplerFilter::linear,
                .magnification = project::SamplerFilter::linear,
                .mip = project::SamplerFilter::linear,
                .revision = core::Revision{1U},
            })),
        "sampler intent enters catalog");
    require(static_cast<bool>(catalog.insert_material(
                material_fixture(71U, "Painted shell", true))),
        "painted material enters catalog");
    require(static_cast<bool>(catalog.insert_material(
                material_fixture(72U, "Rubber grip", false))),
        "rubber material enters catalog");

    project::MaterialVariant variant;
    variant.id = 901U;
    variant.name = "Worn paint";
    variant.base_material = 71U;
    variant.overrides.roughness = 0.84;
    variant.permitted_mesh_assets = {mesh_asset};
    variant.revision = core::Revision{1U};
    require(static_cast<bool>(catalog.insert_variant(std::move(variant))),
        "explicit material variant enters catalog");
    require(static_cast<bool>(catalog.insert_assignment({
                .key = {mesh_asset, 3U},
                .material = 71U,
                .variant = 901U,
            })),
        "painted region assignment enters catalog");
    require(static_cast<bool>(catalog.insert_assignment({
                .key = {mesh_asset, 9U},
                .material = 72U,
                .variant = std::nullopt,
            })),
        "rubber region assignment enters catalog");
    require(static_cast<bool>(catalog.validate()), "complete source catalog validates");
    return catalog;
}

void catalog_round_trips_and_resolves_variants_deterministically() {
    using namespace carto;
    const auto catalog = catalog_fixture(1U);
    const std::string encoded = catalog.serialize();
    const auto reopened = project::MaterialCatalog::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "material catalog survives deterministic save and reopen");
    const auto first_digest = catalog.canonical_digest();
    const auto reopened_digest = reopened.value().canonical_digest();
    require(first_digest && reopened_digest && first_digest.value() == reopened_digest.value(),
        "material catalog canonical digest survives reopen");
    const auto resolved = reopened.value().resolve_material(71U, 901U);
    require(resolved && resolved.value().roughness == 0.84 &&
            resolved.value().base_color.asset == 501U,
        "variant changes only its explicit field and preserves base texture intent");
}

void hostile_catalog_inputs_fail_closed() {
    using namespace carto;
    auto valid = catalog_fixture(1U);

    auto duplicate = project::TextureSourceRecord{
        .id = 501U,
        .name = "Duplicate",
        .relative_locator = "textures/duplicate.png",
        .content_digest = digest("duplicate"),
        .format = assets::TextureSourceFormat::png,
        .width = 1U,
        .height = 1U,
        .provenance = "fixture",
    };
    require(!valid.insert_texture(std::move(duplicate)),
        "duplicate texture source identity is rejected");

    auto traversal = project::TextureSourceRecord{
        .id = 900U,
        .name = "Traversal",
        .relative_locator = "../private/secret.png",
        .content_digest = digest("secret"),
        .format = assets::TextureSourceFormat::png,
        .width = 1U,
        .height = 1U,
        .provenance = "fixture",
    };
    require(!traversal.validate(),
        "texture source traversal cannot escape the project root");
    traversal.relative_locator = "C:\\private\\secret.png";
    require(!traversal.validate(), "absolute workstation texture paths are rejected");

    traversal.relative_locator = "textures/exhausted.png";
    traversal.revision = core::Revision{core::Revision::max_value()};
    require(!traversal.validate(), "exhausted texture revisions are rejected");
    project::SamplerIntent exhausted_sampler;
    exhausted_sampler.id = 901U;
    exhausted_sampler.name = "Exhausted sampler";
    exhausted_sampler.revision = core::Revision{core::Revision::max_value()};
    require(!exhausted_sampler.validate(), "exhausted sampler revisions are rejected");
    auto exhausted_material = material_fixture(902U, "Exhausted material", false);
    exhausted_material.revision = core::Revision{core::Revision::max_value()};
    project::MaterialCatalog exhausted_material_catalog;
    require(!exhausted_material_catalog.insert_material(std::move(exhausted_material)),
        "exhausted material revisions are rejected");

    project::MaterialCatalog missing_texture;
    require(static_cast<bool>(missing_texture.insert_material(
                material_fixture(71U, "Missing texture", true))),
        "locally valid material can be staged before aggregate reference validation");
    require(!missing_texture.validate(),
        "material binding to a missing texture and sampler fails aggregate validation");

    project::MaterialCatalog missing_material;
    require(static_cast<bool>(missing_material.insert_assignment({
                .key = {1U, 3U}, .material = 999U, .variant = std::nullopt})),
        "locally valid assignment can be staged before aggregate validation");
    require(!missing_material.validate(),
        "assignment to a missing material fails aggregate validation");

    project::MaterialVariant unsorted;
    unsorted.id = 1001U;
    unsorted.name = "Bad scope";
    unsorted.base_material = 71U;
    unsorted.overrides.roughness = 0.5;
    unsorted.permitted_mesh_assets = {9U, 2U};
    require(!unsorted.validate(), "unsorted material-variant scope is rejected");
    unsorted.permitted_mesh_assets = {};
    unsorted.revision = core::Revision{core::Revision::max_value()};
    require(!unsorted.validate(), "exhausted material-variant revisions are rejected");

    const std::string serialized = catalog_fixture(1U).serialize();
    require(!project::MaterialCatalog::deserialize(serialized + "trailing"),
        "material catalog rejects trailing unbound data");
    require(!project::MaterialCatalog::deserialize(
                "CARTOGRAPHER_MATERIAL_CATALOG 1\nTEXTURES 65537\n"),
        "hostile collection counts are rejected before allocation");
}

void project_save_reopen_and_journal_preserve_authoring_truth() {
    using namespace carto;
    TempDirectory temp;
    project::ProjectDocument document;
    journal::Journal journal(temp.path() / "catalog-journal.log");

    auto transaction = project::ProjectTransaction::begin(
        document, journal, "material-authoring-test");
    require(static_cast<bool>(transaction), "material authoring transaction begins");
    const auto mesh = transaction.value().add_mesh(make_two_region_mesh());
    require(static_cast<bool>(mesh), "two-region mesh enters project transaction");
    const auto catalog = catalog_fixture(mesh.value());
    require(static_cast<bool>(transaction.value().set_material_catalog(catalog)),
        "material source catalog enters governed project transaction");
    const auto receipt = transaction.value().commit("material.catalog.persist");
    require(receipt && receipt.value().revision_before == core::Revision{} &&
            receipt.value().revision_after == core::Revision{2U},
        "mesh and catalog publish through one durable journal event");

    const auto events = journal.read_all();
    require(events && events.value().size() == 1U &&
            events.value().front().event_type == "material.catalog.persist",
        "catalog mutation emits exactly one approved audit event");

    const auto path = temp.path() / "workshop.carto";
    require(static_cast<bool>(document.save_atomic(path)),
        "schema-v9 project saves durable material authoring truth");
    const auto cache = temp.path() / "derived-cache";
    std::filesystem::create_directories(cache);
    std::ofstream(cache / "compiled-material.bin", std::ios::binary) << "derived";
    std::error_code ignored;
    std::filesystem::remove_all(cache, ignored);
    const auto reopened = project::ProjectDocument::load(path);
    require(reopened && reopened.value().material_catalog().serialize() == catalog.serialize(),
        "catalog survives close, derived-cache removal, and project reopen");

    auto removal = project::ProjectTransaction::begin(
        document, journal, "material-authoring-test");
    require(removal && !removal.value().remove_mesh(mesh.value()),
        "mesh removal cannot orphan durable material assignments");
    require(static_cast<bool>(removal.value().rollback()),
        "rejected mesh removal rolls back without mutation");

    auto replacement = project::ProjectTransaction::begin(
        document, journal, "material-authoring-test");
    require(replacement &&
            !replacement.value().replace_mesh(mesh.value(), make_two_region_mesh(3U, 3U)),
        "mesh replacement cannot silently remove an assigned stable region");
    require(static_cast<bool>(replacement.value().rollback()),
        "rejected region-breaking replacement rolls back");
    const auto events_after_rejection = journal.read_all();
    require(events_after_rejection && events_after_rejection.value().size() == 1U,
        "failed background mutations append no false domain event");
}

void project_schema_v8_migration_is_explicit() {
    using namespace carto;
    TempDirectory temp;
    project::ProjectDocument document;
    journal::Journal journal(temp.path() / "schema-journal.log");
    auto transaction = project::ProjectTransaction::begin(document, journal, "schema-test");
    require(static_cast<bool>(transaction), "schema transaction begins");
    const auto mesh = transaction.value().add_mesh(make_two_region_mesh());
    require(mesh && transaction.value().set_material_catalog(catalog_fixture(mesh.value())) &&
            transaction.value().commit("material.schema.fixture"),
        "schema-v9 fixture commits through the journal boundary");

    const std::string current = document.serialize();
    const auto catalog_start = current.find("MATERIAL_CATALOG ");
    const auto project_end = current.rfind("\nEND\n");
    require(catalog_start != std::string::npos && project_end != std::string::npos &&
            project_end > catalog_start,
        "schema-v9 fixture contains its mandatory catalog record");

    std::string missing = current;
    missing.erase(catalog_start, project_end - catalog_start + 1U);
    require(!project::ProjectDocument::deserialize(missing),
        "schema-v9 projects cannot omit their material catalog");

    std::string legacy = missing;
    const std::string current_header = "CARTOGRAPHER_PROJECT " +
        std::to_string(project::ProjectDocument::kSchemaVersion);
    require(legacy.starts_with(current_header), "legacy fixture finds current project header");
    legacy.replace(0U, current_header.size(), "CARTOGRAPHER_PROJECT 7");
    const auto namespace_start = legacy.find("SOURCE_NAMESPACE ");
    const auto namespace_end = legacy.find('\n', namespace_start);
    require(namespace_start != std::string::npos && namespace_end != std::string::npos,
        "legacy fixture finds the current source namespace record");
    legacy.erase(namespace_start, namespace_end - namespace_start + 1U);
    const auto reopened_legacy = project::ProjectDocument::deserialize(legacy);
    if (!reopened_legacy) {
        std::cerr << "schema-v7 reopen diagnostic: "
                  << reopened_legacy.error().message << '\n';
    }
    require(reopened_legacy && reopened_legacy.value().material_catalog().empty(),
        "schema-v7 projects remain readable with an explicit empty source catalog");
    const auto reopened_again = project::ProjectDocument::deserialize(legacy);
    require(reopened_again &&
            reopened_legacy.value().source_namespace().starts_with(
                "cartographer.project.migrated.") &&
            reopened_again.value().source_namespace() ==
                reopened_legacy.value().source_namespace(),
        "legacy migration assigns one deterministic persistent source namespace");
}

void failed_catalog_admission_has_no_mutation_or_event() {
    using namespace carto;
    TempDirectory temp;
    project::ProjectDocument document;
    journal::Journal journal(temp.path() / "failed-admission-journal.log");
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "material-authoring-test");
    require(static_cast<bool>(transaction), "failed-admission transaction begins");
    const auto mesh = transaction.value().add_mesh(make_two_region_mesh());
    require(static_cast<bool>(mesh), "failed-admission fixture stages a mesh");
    require(!transaction.value().set_material_catalog(catalog_fixture(mesh.value() + 99U)),
        "catalog assignment to a missing mesh fails closed");
    require(static_cast<bool>(transaction.value().rollback()),
        "failed catalog admission rolls back its staged mesh");
    const auto events = journal.read_all();
    require(events && events.value().empty(),
        "failed catalog admission emits no audit or domain event");
    require(document.meshes().empty() && document.material_catalog().empty(),
        "failed catalog admission leaves authoritative project state unchanged");
}

} // namespace

int main() {
    catalog_round_trips_and_resolves_variants_deterministically();
    hostile_catalog_inputs_fail_closed();
    project_save_reopen_and_journal_preserve_authoring_truth();
    project_schema_v8_migration_is_explicit();
    failed_catalog_admission_has_no_mutation_or_event();
    std::cout << "material catalog tests passed\n";
    return EXIT_SUCCESS;
}
