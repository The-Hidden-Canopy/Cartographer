#include <carto/verification/failure_injection.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/lookdev_draw_package.hpp>
#include <carto/production/material_artifact.hpp>
#include <carto/production/prepared_publication.hpp>
#include <carto/production/prepared_scene.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/project/transaction.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

std::vector<std::uint8_t> bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-verification-injection-tests-" + std::to_string(stamp) +
             "-" + std::to_string(sequence.fetch_add(1U)));
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

carto::geometry::EditableMesh workshop_mesh() {
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
        "workshop mesh topology is valid");
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
        "workshop mesh owns UV0 corners");
    require(static_cast<bool>(attributes.add_layer(attributes::AttributeLayer{
                {"condition.material_region", attributes::AttributeDomain::face,
                 attributes::AttributeType::uint32},
                {std::uint32_t{3U}, std::uint32_t{9U}}})),
        "workshop mesh owns stable material regions");
    attributes::UvSetTable uv_sets;
    require(static_cast<bool>(uv_sets.add(attributes::UvSetDescriptor{
                1U, "UV0", "uv0.corners", true, true, false, 1U})),
        "workshop mesh declares UV0");
    require(static_cast<bool>(mesh.set_attribute_payload(
                std::move(attributes), std::move(uv_sets))),
        "workshop mesh accepts its material-region payload");
    return mesh;
}

carto::render::MaterialAsset material(
    carto::render::MaterialId id,
    std::string name) {
    using namespace carto;
    render::MaterialAsset value;
    value.id = id;
    value.name = std::move(name);
    value.revision = core::Revision{1U};
    value.definition.roughness = 0.8;
    return value;
}

carto::project::MaterialCatalog workshop_catalog() {
    using namespace carto;
    project::MaterialCatalog catalog;
    require(static_cast<bool>(catalog.insert_texture(project::TextureSourceRecord{
                .id = 501U,
                .name = "Workshop shared image",
                .relative_locator = "textures/workshop-shared.png",
                .content_digest = digest("workshop-shared-pixels"),
                .format = assets::TextureSourceFormat::png,
                .width = 512U,
                .height = 512U,
                .provenance = "verification-injection-fixture",
                .revision = core::Revision{1U},
            })),
        "workshop source texture enters catalog");
    require(static_cast<bool>(catalog.insert_sampler(project::SamplerIntent{
                .id = 31U,
                .name = "Workshop linear repeat",
                .revision = core::Revision{1U},
            })),
        "workshop sampler enters catalog");
    require(static_cast<bool>(catalog.insert_material(material(71U, "Painted shell"))),
        "paint material enters catalog");
    require(static_cast<bool>(catalog.insert_material(material(72U, "Rubber grip"))),
        "rubber material enters catalog");
    require(static_cast<bool>(catalog.insert_assignment({
                .key = {1U, 3U}, .material = 71U, .variant = std::nullopt})),
        "paint region receives its source assignment");
    require(static_cast<bool>(catalog.insert_assignment({
                .key = {1U, 9U}, .material = 72U, .variant = std::nullopt})),
        "rubber region receives its source assignment");
    require(static_cast<bool>(catalog.validate()), "workshop catalog validates");
    return catalog;
}

struct Fixture {
    carto::project::ProjectDocument document;
    std::uint64_t mesh_asset = 0U;
    carto::production::RenderMeshArtifact render_mesh;
    carto::production::LookdevDrawPackage lookdev;
};

// Recompiles every derived product against the current document revision so a
// concurrent edit can be followed by a fresh, honest composition.
void refresh_products(Fixture& fixture) {
    using namespace carto;
    production::RenderMeshCompileOptions options;
    options.uv_set = 1U;
    options.generate_tangents = true;
    options.material_slot_layer = "condition.material_region";
    const auto compiled_mesh = production::compile_render_mesh_artifact(
        "public.workshop", fixture.document, fixture.mesh_asset, options);
    require(static_cast<bool>(compiled_mesh), "refresh compiles the render mesh");
    fixture.render_mesh = compiled_mesh.value();
    const auto& catalog = fixture.document.material_catalog();
    const auto paint = production::compile_material_artifact(
        "public.workshop.material.71", catalog.materials().at(71U));
    const auto rubber = production::compile_material_artifact(
        "public.workshop.material.72", catalog.materials().at(72U));
    require(paint && rubber, "refresh compiles the materials");
    const std::vector<production::LookdevMaterialBinding> bindings{
        {3U, paint.value()},
        {9U, rubber.value()},
    };
    const auto lookdev = production::compile_lookdev_draw_package(
        fixture.render_mesh, bindings);
    require(static_cast<bool>(lookdev), "refresh compiles the lookdev package");
    fixture.lookdev = lookdev.value();
}

Fixture make_fixture(carto::journal::Journal& journal) {
    using namespace carto;
    Fixture fixture;
    auto transaction = project::ProjectTransaction::begin(
        fixture.document, journal, "verification-injection-test");
    require(static_cast<bool>(transaction), "injection fixture transaction begins");
    const auto mesh = transaction.value().add_mesh(workshop_mesh());
    require(static_cast<bool>(mesh), "workshop mesh enters project");
    fixture.mesh_asset = mesh.value();
    require(static_cast<bool>(transaction.value().set_material_catalog(
                workshop_catalog())),
        "workshop catalog enters project");
    const auto root = transaction.value().create_object("Workshop root");
    require(static_cast<bool>(root) &&
            transaction.value().attach_mesh(root.value(), mesh.value()),
        "workshop placement references the reusable asset");
    require(static_cast<bool>(transaction.value().commit("injection.fixture")),
        "injection fixture commits through one journal event");
    refresh_products(fixture);
    return fixture;
}

carto::production::PreparedSceneEnvelope compose(const Fixture& fixture) {
    using namespace carto;
    const production::PreparedAssetInput input{
        fixture.mesh_asset, &fixture.render_mesh, &fixture.lookdev};
    const auto prepared = production::compose_prepared_scene(
        fixture.document.source_namespace(), fixture.document,
        "portable.static-workshop.v1",
        digest("portable.static-workshop.v1"), std::span{&input, 1U});
    require(static_cast<bool>(prepared), "neutral prepared scene composes");
    return prepared.value();
}

std::vector<carto::production::PreparedProductPayload> make_payloads(
    const Fixture& fixture,
    const carto::production::PreparedSceneEnvelope& envelope) {
    using namespace carto;
    const auto scope = production::resolve_preparation_scope(fixture.document, {
        production::PreparationScopeKind::scene,
        fixture.document.revision(), {}, {}});
    require(static_cast<bool>(scope), "payload material scope resolves");
    const auto materials = production::compose_prepared_material_catalog(
        fixture.document, scope.value());
    require(static_cast<bool>(materials), "payload material catalog composes");
    const auto render_bytes = fixture.render_mesh.serialize();
    require(static_cast<bool>(render_bytes), "render mesh serializes");
    const auto lookdev_bytes = production::serialize_lookdev_draw_package(
        fixture.lookdev);
    require(static_cast<bool>(lookdev_bytes), "lookdev package serializes");

    std::vector<production::PreparedProductPayload> payloads;
    for (const auto& product : envelope.products) {
        std::vector<std::uint8_t> content;
        switch (product.kind) {
        case production::PreparedProductKind::render_mesh:
            content = render_bytes.value();
            break;
        case production::PreparedProductKind::lookdev_draw_package:
            content = lookdev_bytes.value();
            break;
        case production::PreparedProductKind::material_catalog: {
            const auto text = materials.value().serialize();
            content.assign(text.begin(), text.end());
            break;
        }
        case production::PreparedProductKind::texture_source:
            content = bytes("workshop-shared-pixels");
            break;
        case production::PreparedProductKind::world_model: {
            const auto text = fixture.document.world_model().serialize();
            content.assign(text.begin(), text.end());
            break;
        }
        default:
            require(false, "payload builder met an unknown product kind");
        }
        payloads.push_back({product.identity, std::move(content)});
    }
    return payloads;
}

carto::assets::Sha256Digest selected_digest(const std::filesystem::path& root) {
    const auto selected = carto::production::read_selected_prepared_scene(root);
    require(selected && selected.value().has_value(),
        "selected publication resolves");
    return selected.value()->prepared_scene_digest;
}

void concurrent_edit_between_plan_and_publish_fails_closed() {
    using namespace carto;
    TempDirectory temp;
    const auto root = temp.path() / "published";
    journal::Journal journal(temp.path() / "injection-journal.log");
    Fixture fixture = make_fixture(journal);

    const auto planned = compose(fixture);
    const auto planned_digest = planned.canonical_digest();
    require(static_cast<bool>(planned_digest), "planned digest computes");
    require(static_cast<bool>(verification::prepublish_source_check(fixture.document, planned)),
        "a freshly planned envelope passes the readiness gate");

    const auto planned_payloads = make_payloads(fixture, planned);
    const auto published_first = production::publish_prepared_scene(
        root, planned, planned_payloads, std::nullopt);
    require(published_first &&
            published_first.value().prepared_scene_digest == planned_digest.value(),
        "the planned revision publishes and becomes current");
    require(selected_digest(root) == planned_digest.value(),
        "current resolves to the planned revision");

    const auto edit = verification::inject_concurrent_edit(
        fixture.document, journal, "verification-concurrent-writer");
    require(edit && edit.value().revision_before == planned.source_revision &&
            edit.value().revision_after.value() ==
                planned.source_revision.value() + 1U &&
            fixture.document.revision() == edit.value().revision_after,
        "concurrent edit deterministically advances the durable revision");

    const auto stale_plan = verification::prepublish_source_check(
        fixture.document, planned);
    require(!stale_plan &&
            stale_plan.error().code == core::ErrorCode::stale_data,
        "a plan whose source revision changed after planning fails closed");

    const auto stale_writer = production::publish_prepared_scene(
        root, planned, planned_payloads, std::nullopt);
    require(!stale_writer &&
            stale_writer.error().code == core::ErrorCode::stale_data,
        "a stale concurrent writer loses the compare-and-swap");
    require(selected_digest(root) == planned_digest.value(),
        "the losing concurrent writer leaves current on the last good revision");

    refresh_products(fixture);
    const auto refreshed = compose(fixture);
    const auto refreshed_digest = refreshed.canonical_digest();
    require(refreshed_digest && refreshed_digest.value() != planned_digest.value(),
        "a refreshed composition over the edited source has a new identity");
    require(static_cast<bool>(verification::prepublish_source_check(
                fixture.document, refreshed)),
        "the refreshed plan passes the readiness gate");

    auto corrupted = make_payloads(fixture, refreshed);
    require(!corrupted.empty(), "corruption fixture has payloads");
    require(static_cast<bool>(verification::flip_payload_byte(
                corrupted.front().bytes, 0U)),
        "deterministic pre-publish payload corruption applies");
    const auto corrupt_publish = production::publish_prepared_scene(
        root, refreshed, corrupted, planned_digest.value());
    require(!corrupt_publish &&
            corrupt_publish.error().code == core::ErrorCode::validation_failed,
        "a corrupted payload fails publication before selection moves");
    require(selected_digest(root) == planned_digest.value(),
        "payload corruption leaves current on the last good revision");

    const auto published_second = production::publish_prepared_scene(
        root, refreshed, make_payloads(fixture, refreshed), planned_digest.value());
    require(published_second && published_second.value().previous_selected_digest ==
            planned_digest.value() &&
            published_second.value().prepared_scene_digest == refreshed_digest.value(),
        "the honest refreshed revision atomically advances selection");
    require(selected_digest(root) == refreshed_digest.value(),
        "current resolves to the refreshed verified revision");
}

void stale_derived_products_fail_closed() {
    using namespace carto;
    TempDirectory temp;
    journal::Journal journal(temp.path() / "stale-journal.log");
    Fixture fixture = make_fixture(journal);
    require(static_cast<bool>(compose(fixture).validate()),
        "fresh fixture composition is valid");

    auto stale_mesh = fixture.render_mesh;
    require(static_cast<bool>(verification::force_stale_render_mesh(stale_mesh)),
        "render mesh source drift applies");
    require(stale_mesh.source_revision.value() + 1U ==
            fixture.document.revision().value(),
        "source drift rewinds the artifact exactly one revision");
    const production::PreparedAssetInput stale_mesh_input{
        fixture.mesh_asset, &stale_mesh, &fixture.lookdev};
    const auto stale_mesh_scene = production::compose_prepared_scene(
        "public.workshop", fixture.document, "portable.static-workshop.v1",
        digest("portable.static-workshop.v1"), std::span{&stale_mesh_input, 1U});
    require(!stale_mesh_scene &&
            stale_mesh_scene.error().code == core::ErrorCode::stale_data,
        "a stale render product cannot enter a new prepared envelope");

    auto stale_lookdev = fixture.lookdev;
    require(static_cast<bool>(
                verification::force_stale_lookdev_package(stale_lookdev)),
        "lookdev source drift applies and refreshes the package digest");
    const production::PreparedAssetInput stale_lookdev_input{
        fixture.mesh_asset, &fixture.render_mesh, &stale_lookdev};
    const auto stale_lookdev_scene = production::compose_prepared_scene(
        "public.workshop", fixture.document, "portable.static-workshop.v1",
        digest("portable.static-workshop.v1"), std::span{&stale_lookdev_input, 1U});
    require(!stale_lookdev_scene &&
            stale_lookdev_scene.error().code == core::ErrorCode::stale_data,
        "a self-consistent drifted lookdev package still fails closed");

    production::RenderMeshArtifact revision_zero;
    revision_zero.source_revision = core::Revision{0U};
    require(!verification::force_stale_render_mesh(revision_zero),
        "revision-zero drift has no prior revision to force");
}

void payload_corruption_helpers_fail_closed_on_bad_offsets() {
    using carto::verification::flip_payload_byte;
    using carto::verification::truncate_payload;
    using carto::verification::zero_payload_range;
    std::vector<std::uint8_t> payload{1U, 2U, 3U, 4U, 5U};

    require(!flip_payload_byte(payload, payload.size()),
        "flipping past the payload end fails closed");
    require(static_cast<bool>(flip_payload_byte(payload, 4U)) && payload[4U] == 250U,
        "flipping one byte is deterministic");

    require(!zero_payload_range(payload, 0U, 0U),
        "zeroing an empty range fails closed");
    require(!zero_payload_range(payload, 2U, 10U),
        "zeroing past the payload end fails closed");
    require(static_cast<bool>(zero_payload_range(payload, 2U, 2U)) &&
            payload[2U] == 0U && payload[3U] == 0U && payload[1U] == 2U,
        "zeroing one range leaves the rest untouched");

    require(!truncate_payload(payload, payload.size()),
        "truncating to the current size fails closed");
    require(static_cast<bool>(truncate_payload(payload, 2U)) && payload.size() == 2U,
        "truncation shortens the payload deterministically");
}

} // namespace

int main() {
    concurrent_edit_between_plan_and_publish_fails_closed();
    stale_derived_products_fail_closed();
    payload_corruption_helpers_fail_closed_on_bad_offsets();
    std::cout << "verification failure injection tests passed\n";
    return EXIT_SUCCESS;
}
