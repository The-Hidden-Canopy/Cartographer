#include <carto/assets/blob_store.hpp>
#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/material_artifact.hpp>
#include <carto/production/prepared_scene.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
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

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-prepared-scene-tests-" + std::to_string(stamp));
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
    std::string name,
    bool textured) {
    using namespace carto;
    render::MaterialAsset value;
    value.id = id;
    value.name = std::move(name);
    value.revision = core::Revision{1U};
    value.definition.roughness = id == 71U ? 0.4 : 0.8;
    if (textured) {
        value.definition.base_color.asset = 501U;
    }
    return value;
}

carto::project::MaterialCatalog workshop_catalog(std::uint64_t mesh_asset) {
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
                .provenance = "prepared-scene-fixture",
                .revision = core::Revision{1U},
            })),
        "workshop source texture enters catalog");
    require(static_cast<bool>(catalog.insert_texture(project::TextureSourceRecord{
                .id = 999U,
                .name = "Out of scope image",
                .relative_locator = "textures/not-selected.png",
                .content_digest = digest("out-of-scope-pixels"),
                .format = assets::TextureSourceFormat::png,
                .width = 32U,
                .height = 32U,
                .provenance = "prepared-scene-negative-fixture",
                .revision = core::Revision{1U},
            })),
        "unrelated source texture enters the full authoring catalog");
    require(static_cast<bool>(catalog.insert_sampler(project::SamplerIntent{
                .id = 31U,
                .name = "Workshop linear repeat",
                .revision = core::Revision{1U},
            })),
        "workshop sampler enters catalog");
    require(static_cast<bool>(catalog.insert_material(
                material(71U, "Painted shell", true))),
        "paint material enters catalog");
    require(static_cast<bool>(catalog.insert_material(
                material(72U, "Rubber grip", false))),
        "rubber material enters catalog");
    require(static_cast<bool>(catalog.insert_material(
                material(73U, "Unrelated material", false))),
        "unrelated material enters the full authoring catalog");
    require(static_cast<bool>(catalog.insert_assignment({
            .key = {mesh_asset, 3U},
            .material = 71U,
            .variant = std::nullopt,
            })),
        "paint region receives its source assignment");
    require(static_cast<bool>(catalog.insert_assignment({
            .key = {mesh_asset, 9U},
            .material = 72U,
            .variant = std::nullopt,
            })),
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

Fixture make_fixture() {
    using namespace carto;
    TempDirectory temp;
    Fixture fixture;
    journal::Journal journal(temp.path() / "prepared-scene-journal.log");
    auto transaction = project::ProjectTransaction::begin(
        fixture.document, journal, "prepared-scene-test");
    require(static_cast<bool>(transaction), "prepared-scene transaction begins");
    const auto mesh = transaction.value().add_mesh(workshop_mesh());
    require(static_cast<bool>(mesh), "workshop mesh enters project");
    fixture.mesh_asset = mesh.value();
    require(static_cast<bool>(transaction.value().set_material_catalog(
                workshop_catalog(mesh.value()))),
        "workshop catalog enters project");
    const auto root = transaction.value().create_object("Workshop root");
    require(static_cast<bool>(root) &&
            transaction.value().attach_mesh(root.value(), mesh.value()),
        "first workshop placement references the reusable asset");
    const auto second = transaction.value().create_object(
        "Workshop repeat", core::Transform{{2.0, 0.0, 0.0}});
    require(static_cast<bool>(second) &&
            transaction.value().attach_mesh(second.value(), mesh.value()),
        "second workshop placement references the same reusable asset");
    require(static_cast<bool>(transaction.value().commit("prepared.scene.fixture")),
        "workshop source commits through one journal event");

    production::RenderMeshCompileOptions options;
    options.uv_set = 1U;
    options.generate_tangents = true;
    options.material_slot_layer = "condition.material_region";
    const auto compiled_mesh = production::compile_render_mesh_artifact(
        "public.workshop", fixture.document, fixture.mesh_asset, options);
    require(static_cast<bool>(compiled_mesh), "exact workshop render mesh compiles");
    fixture.render_mesh = compiled_mesh.value();

    const auto& catalog = fixture.document.material_catalog();
    const auto paint = production::compile_material_artifact(
        "public.workshop.material.71", catalog.materials().at(71U));
    const auto rubber = production::compile_material_artifact(
        "public.workshop.material.72", catalog.materials().at(72U));
    require(paint && rubber, "workshop material products compile");
    const std::vector<production::LookdevMaterialBinding> bindings{
        {3U, paint.value()},
        {9U, rubber.value()},
    };
    const auto lookdev = production::compile_lookdev_draw_package(
        fixture.render_mesh, bindings);
    require(static_cast<bool>(lookdev), "workshop lookdev package compiles");
    fixture.lookdev = lookdev.value();
    return fixture;
}

carto::production::PreparedSceneEnvelope compose(const Fixture& fixture) {
    using namespace carto;
    const production::PreparedAssetInput input{
        fixture.mesh_asset, &fixture.render_mesh, &fixture.lookdev};
    const auto prepared = production::compose_prepared_scene(
        "public.workshop", fixture.document, "portable.static-workshop.v1",
        digest("portable.static-workshop.v1"), std::span{&input, 1U});
    require(static_cast<bool>(prepared), "neutral prepared scene composes");
    return prepared.value();
}

void exact_products_compose_and_round_trip() {
    using namespace carto;
    const Fixture fixture = make_fixture();
    const auto prepared = compose(fixture);
    const auto material_scope = production::resolve_preparation_scope(fixture.document, {
        production::PreparationScopeKind::scene,
        fixture.document.revision(), {}, {}});
    require(static_cast<bool>(material_scope), "prepared material scope resolves");
    const auto prepared_materials = production::compose_prepared_material_catalog(
        fixture.document, material_scope.value());
    require(prepared_materials && prepared_materials.value().materials().size() == 2U &&
            prepared_materials.value().textures().size() == 1U &&
            !prepared_materials.value().materials().contains(73U) &&
            !prepared_materials.value().textures().contains(999U),
        "prepared material catalog excludes unrelated source records");
    require(prepared.assets.size() == 1U && prepared.nodes.size() == 2U &&
            prepared.products.size() == 4U && prepared.material_bindings.size() == 2U,
        "one reusable asset serves two nodes with exact products and bindings");
    require(std::any_of(prepared.products.begin(), prepared.products.end(),
                [](const production::PreparedProductReference& product) {
                    return product.kind ==
                        production::PreparedProductKind::material_catalog;
                }) &&
            std::any_of(prepared.products.begin(), prepared.products.end(),
                [](const production::PreparedProductReference& product) {
                    return product.kind ==
                        production::PreparedProductKind::texture_source;
                }),
        "scoped material truth and source texture bytes are declared as included products");
    const auto texture_dependency = std::find_if(
        prepared.dependencies.begin(), prepared.dependencies.end(),
        [](const production::PreparedDependencyReference& dependency) {
            return dependency.identity == "source.texture.501";
        });
    require(texture_dependency != prepared.dependencies.end() &&
            texture_dependency->disposition ==
                production::PreparedDependencyDisposition::included_product,
        "texture closure is relocatable rather than a workstation source reference");
    require((prepared.assets.front().channel_mask & production::prepared_uv0) != 0U &&
            (prepared.assets.front().channel_mask & production::prepared_uv1) == 0U &&
            (prepared.assets.front().channel_mask &
                production::prepared_material_regions) != 0U,
        "channel declaration reflects actual product contents");
    require(std::none_of(prepared.capabilities.begin(), prepared.capabilities.end(),
                [](const production::PreparedCapabilityResult& capability) {
                    return capability.evidence ==
                            production::PreparedCapabilityEvidence::native_realization ||
                        capability.evidence ==
                            production::PreparedCapabilityEvidence::runtime_validated;
                }),
        "neutral composition does not fabricate native or runtime evidence");

    const std::string encoded = prepared.serialize();
    const auto reopened = production::PreparedSceneEnvelope::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "prepared scene survives deterministic neutral round trip");
    const auto first_digest = prepared.canonical_digest();
    const auto second_digest = reopened.value().canonical_digest();
    require(first_digest && second_digest && first_digest.value() == second_digest.value(),
        "prepared-scene identity survives reopen");
}

void stale_and_incomplete_composition_fails_closed() {
    using namespace carto;
    const Fixture fixture = make_fixture();
    production::RenderMeshArtifact stale = fixture.render_mesh;
    stale.source_revision = core::Revision{fixture.document.revision().value() - 1U};
    const production::PreparedAssetInput stale_input{
        fixture.mesh_asset, &stale, &fixture.lookdev};
    require(!production::compose_prepared_scene(
                "public.workshop", fixture.document, "portable.static-workshop.v1",
                digest("profile"), std::span{&stale_input, 1U}),
        "stale render products cannot enter a new prepared envelope");
    require(!production::compose_prepared_scene(
                "public.workshop", fixture.document, "portable.static-workshop.v1",
                digest("profile"), {}),
        "a scene node cannot publish without its referenced asset product");

    production::LookdevDrawPackage wrong = fixture.lookdev;
    const auto replacement = wrong.draws.back();
    wrong.draws.front().material_source_identity = replacement.material_source_identity;
    wrong.draws.front().material_id = replacement.material_id;
    wrong.draws.front().material_source_revision = replacement.material_source_revision;
    wrong.draws.front().material_source_digest = replacement.material_source_digest;
    wrong.draws.front().feature_mask = replacement.feature_mask;
    wrong.draws.front().gpu_constants = replacement.gpu_constants;
    wrong.draws.front().material_artifact_digest = replacement.material_artifact_digest;
    require(static_cast<bool>(wrong.refresh_digest()),
        "adversarial lookdev package remains internally self-consistent");
    const production::PreparedAssetInput wrong_input{
        fixture.mesh_asset, &fixture.render_mesh, &wrong};
    require(!production::compose_prepared_scene(
                "public.workshop", fixture.document, "portable.static-workshop.v1",
                digest("profile"), std::span{&wrong_input, 1U}),
        "self-consistent lookdev drift still fails against source assignments");
}

void prepared_composition_obeys_resolved_scope() {
    using namespace carto;
    const Fixture fixture = make_fixture();
    const auto scope = production::resolve_preparation_scope(fixture.document, {
        production::PreparationScopeKind::asset,
        fixture.document.revision(),
        {fixture.mesh_asset},
        {},
    });
    require(static_cast<bool>(scope), "asset preparation scope resolves");
    const production::PreparedAssetInput input{
        fixture.mesh_asset, &fixture.render_mesh, &fixture.lookdev};
    const auto prepared = production::compose_prepared_scene(
        "public.workshop", fixture.document, scope.value(),
        "portable.asset.v1", digest("portable.asset.v1"), std::span{&input, 1U});
    require(prepared && prepared.value().assets.size() == 1U &&
            prepared.value().nodes.empty() &&
            prepared.value().material_bindings.size() == 2U,
        "asset scope emits only the selected reusable definition and its closure");
    require(!production::compose_prepared_scene(
                "public.workshop", fixture.document, scope.value(),
                "portable.asset.v1", digest("portable.asset.v1"), {}),
        "prepared products must exactly match the resolved stable-ID scope");
}

void malformed_envelopes_fail_closed() {
    using namespace carto;
    const Fixture fixture = make_fixture();
    auto prepared = compose(fixture);

    auto missing_binding = prepared;
    missing_binding.material_bindings.pop_back();
    require(!missing_binding.validate(),
        "every declared material region requires an exact source binding");

    auto path_escape = prepared;
    path_escape.products.front().relative_path = "../private/product.bin";
    require(!path_escape.validate(), "product paths cannot escape the package root");

    auto path_collision = prepared;
    path_collision.products.back().relative_path =
        path_collision.products.front().relative_path;
    require(!path_collision.validate(),
        "two products cannot occupy one portable package path");

    auto cycle = prepared;
    cycle.nodes.front().parent = cycle.nodes.back().id;
    cycle.nodes.back().parent = cycle.nodes.front().id;
    require(!cycle.validate(), "prepared hierarchy cycles are rejected");

    auto unresolved = prepared;
    unresolved.dependencies.push_back(production::PreparedDependencyReference{
        "external.required", production::PreparedDependencyDisposition::unresolved,
        true, {}, std::nullopt, {}});
    std::sort(unresolved.dependencies.begin(), unresolved.dependencies.end(),
        [](const auto& left, const auto& right) { return left.identity < right.identity; });
    require(!unresolved.validate(), "required unresolved dependencies block publication");

    const std::string encoded = prepared.serialize();
    require(!production::PreparedSceneEnvelope::deserialize(encoded + "trailing"),
        "prepared-scene parser rejects trailing unbound data");
    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_PREPARED_SCENE 1"}.size(),
        "CARTOGRAPHER_PREPARED_SCENE 99");
    require(!production::PreparedSceneEnvelope::deserialize(future),
        "unknown future prepared-scene versions fail precisely");
    require(!production::PreparedSceneEnvelope::deserialize(
                "CARTOGRAPHER_PREPARED_SCENE 1\n"
                "SOURCE \"x\" 0 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "PROFILE \"x\" 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "COORDINATES 1 0 1 2\nPRODUCTS 65537\n"),
        "hostile collection counts are rejected before allocation");
}

} // namespace

int main() {
    exact_products_compose_and_round_trip();
    stale_and_incomplete_composition_fails_closed();
    prepared_composition_obeys_resolved_scope();
    malformed_envelopes_fail_closed();
    std::cout << "prepared scene tests passed\n";
    return EXIT_SUCCESS;
}
