#include <carto/assets/blob_store.hpp>
#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/material_artifact.hpp>
#include <carto/production/bake_graph.hpp>
#include <carto/production/prepared_publication.hpp>
#include <carto/production/prepared_scene.hpp>
#include <carto/project/transaction.hpp>
#include <carto/world/model.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
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

carto::world::WorldModel workshop_world_model() {
    using namespace carto;
    world::WorldModel model;
    require(static_cast<bool>(model.insert_area(world::SemanticArea{
                .id = world::AreaId{101U},
                .semantic_type = "camp.inner",
                .geometry = world::AuthoringBinding{
                    world::AuthoringBindingKind::mesh_asset, 1U},
                .parent = std::nullopt,
                .tags = {"motel", "roof"},
            })),
        "semantic area enters world fixture");
    require(static_cast<bool>(model.insert_area(world::SemanticArea{
                .id = world::AreaId{102U},
                .semantic_type = "camp.outer",
                .geometry = world::AuthoringBinding{
                    world::AuthoringBindingKind::none, 0U},
                .parent = world::AreaId{101U},
                .tags = {},
            })),
        "child semantic area enters world fixture");
    require(static_cast<bool>(model.insert_path(world::SemanticPath{
                .id = world::PathId{201U},
                .semantic_type = "route.supply",
                .control_points = {{0.0, 0.0, 0.0}, {4.0, 0.0, 0.0}},
                .start_area = world::AreaId{101U},
                .end_area = world::AreaId{102U},
                .width = 2.0,
                .closed = false,
            })),
        "semantic path enters world fixture");
    std::map<std::uint64_t, world::StructureNode> nodes;
    nodes.emplace(1U, world::StructureNode{
        1U, "room.a", world::AreaId{101U}});
    nodes.emplace(2U, world::StructureNode{
        2U, "room.b", world::AreaId{102U}});
    std::map<std::uint64_t, world::StructureEdge> edges;
    edges.emplace(1U, world::StructureEdge{
        1U, 1U, 2U, "adjacent", world::PathId{201U}, false});
    require(static_cast<bool>(model.insert_structure_graph(world::StructureGraph{
                world::StructureGraphId{301U}, "structure.block",
                std::move(nodes), std::move(edges)})),
        "structure graph enters world fixture");
    require(static_cast<bool>(model.insert_instance_field(world::InstanceField{
                .id = world::InstanceFieldId{401U},
                .semantic_type = "field.debris",
                .area = world::AreaId{101U},
                .bounds = core::Bounds3d{{0.0, 0.0, 0.0}, {8.0, 2.0, 8.0}},
                .recipe_identity = "recipe.debris.v1",
                .recipe_digest = digest("recipe-bytes"),
                .seed = 7U,
                .maximum_instances = 64U,
                .suppressed_instances = {2U, 5U},
                .geometry_evidence = std::nullopt,
                .physical_material_intent = {},
                .construction_profile = {},
                .surface_domain_identity = {},
            })),
        "instance field enters world fixture");
    require(static_cast<bool>(model.validate()), "world fixture validates");
    return model;
}

Fixture make_fixture(const carto::world::WorldModel& world_model = {}) {
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
    if (!world_model.empty()) {
        require(static_cast<bool>(transaction.value().set_world_model(world_model)),
            "workshop world model enters project");
    }
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

void world_position_product_composes_and_publishes_for_exact_instance() {
    using namespace carto;
    const Fixture fixture = make_fixture();
    const auto nodes = fixture.document.scene().objects_sorted();
    require(nodes.size() == 2U, "fixture has two instances of one reusable asset");
    const auto recipe = production::bind_world_position_recipe(
        "bake/workshop/root-world-position", fixture.document, nodes.front().id.value,
        1U, 8U, 8U);
    require(static_cast<bool>(recipe), "world-position recipe binds the root instance");
    const auto baked = production::compile_world_position_cpu(recipe.value(), fixture.document);
    require(baked.succeeded(), "world-position map compiles from the exact project scene");
    const auto repeated_recipe = production::bind_world_position_recipe(
        "bake/workshop/repeated-world-position", fixture.document, nodes.back().id.value,
        1U, 8U, 8U);
    require(static_cast<bool>(repeated_recipe), "world-position recipe binds the repeated instance");
    const auto repeated_baked = production::compile_world_position_cpu(
        repeated_recipe.value(), fixture.document);
    require(repeated_baked.succeeded() &&
            repeated_baked.artifact->artifact_digest != baked.artifact->artifact_digest,
        "repeated mesh instances produce distinct world-space products");
    const auto source_mesh = fixture.document.meshes().find(fixture.mesh_asset);
    require(source_mesh != fixture.document.meshes().end(),
        "material/surface ID source mesh resolves");
    const auto compiled_source_mesh = source_mesh->second.compile();
    require(static_cast<bool>(compiled_source_mesh),
        "material/surface ID source mesh compiles");
    surface::SurfaceDomain paint_domain{
        .id = surface::SurfaceDomainId{301U},
        .source_faces = {geometry::FaceId{1U}},
        .source_revision = compiled_source_mesh.value().source_revision,
        .semantic_class = "painted-shell",
        .material_class = "paint",
        .source_content_digest = {},
        .digest = {},
    };
    surface::SurfaceDomain rubber_domain{
        .id = surface::SurfaceDomainId{302U},
        .source_faces = {geometry::FaceId{2U}},
        .source_revision = compiled_source_mesh.value().source_revision,
        .semantic_class = "grip",
        .material_class = "rubber",
        .source_content_digest = {},
        .digest = {},
    };
    require(static_cast<bool>(paint_domain.refresh_digest(compiled_source_mesh.value())) &&
            static_cast<bool>(rubber_domain.refresh_digest(compiled_source_mesh.value())),
        "caller-authored surface memberships bind to the compiled mesh");
    const auto id_recipe = production::bind_material_surface_id_recipe(
        "bake/workshop/material-surface-ids", fixture.document, fixture.mesh_asset,
        1U, "condition.material_region", 16U, 16U,
        {rubber_domain, paint_domain});
    require(static_cast<bool>(id_recipe),
        "material/surface ID recipe binds exact regions, catalog, UVs, and domains");
    const auto id_baked = production::compile_material_surface_id_cpu(
        id_recipe.value(), fixture.document);
    require(id_baked.succeeded() && id_baked.covered_texels > 0U,
        "CPU material/surface ID product bakes from the exact source asset");
    const production::PreparedAssetInput asset_input{
        fixture.mesh_asset, &fixture.render_mesh, &fixture.lookdev, &*id_baked.artifact};
    const production::PreparedWorldPositionInput position_inputs[]{
        {&*baked.artifact}, {&*repeated_baked.artifact}};
    const auto prepared = production::compose_prepared_scene(
        "public.workshop", fixture.document, "portable.static-workshop.v1",
        digest("portable.static-workshop.v1"), std::span{&asset_input, 1U},
        position_inputs);
    require(static_cast<bool>(prepared), "world-position product enters exact prepared composition");
    const std::string identity = "node." + std::to_string(nodes.front().id.value) +
        ".world-position.uv-1.tile-0-0";
    const auto product = std::find_if(prepared.value().products.begin(),
        prepared.value().products.end(), [&identity](const auto& value) {
            return value.identity == identity;
        });
    require(product != prepared.value().products.end() &&
            product->kind == production::PreparedProductKind::world_position_data &&
            product->format_version == production::kWorldPositionArtifactVersion,
        "prepared manifest declares the typed world-position product for its scene node");
    const std::string repeated_identity = "node." + std::to_string(nodes.back().id.value) +
        ".world-position.uv-1.tile-0-0";
    require(std::any_of(prepared.value().products.begin(), prepared.value().products.end(),
        [&repeated_identity](const auto& value) {
            return value.identity == repeated_identity &&
                value.kind == production::PreparedProductKind::world_position_data;
        }), "prepared manifest keeps the repeated instance's product separate");
    const auto dependency = std::find_if(prepared.value().dependencies.begin(),
        prepared.value().dependencies.end(), [&identity](const auto& value) {
            return value.identity == identity;
        });
    require(dependency != prepared.value().dependencies.end() &&
            dependency->content_digest == product->content_digest &&
            dependency->disposition == production::PreparedDependencyDisposition::included_product,
        "prepared dependency closure includes the exact position bytes");
    require(std::any_of(prepared.value().capabilities.begin(), prepared.value().capabilities.end(),
        [](const auto& capability) {
            return capability.feature == "scene.world-position" &&
                capability.evidence == production::PreparedCapabilityEvidence::derived_product;
        }), "prepared capability reports a derived product without claiming runtime validation");
    const std::string id_identity = "mesh." + std::to_string(fixture.mesh_asset) +
        ".material-surface-ids.uv-1.tile-0-0";
    const auto id_product = std::find_if(prepared.value().products.begin(),
        prepared.value().products.end(), [&id_identity](const auto& value) {
            return value.identity == id_identity;
        });
    require(id_product != prepared.value().products.end() &&
            id_product->kind == production::PreparedProductKind::material_surface_id_data &&
            id_product->format_version == production::kMaterialSurfaceIdArtifactVersion,
        "prepared manifest declares the typed per-asset material/surface ID product");
    require(std::any_of(prepared.value().dependencies.begin(), prepared.value().dependencies.end(),
        [&id_identity](const auto& dependency) {
            return dependency.identity == id_identity && dependency.content_digest.has_value();
        }), "prepared dependency closure includes the exact ID product digest");
    require(std::any_of(prepared.value().capabilities.begin(), prepared.value().capabilities.end(),
        [](const auto& capability) {
            return capability.feature == "scene.material-surface-ids" &&
                capability.evidence == production::PreparedCapabilityEvidence::derived_product;
        }), "prepared capability records derived IDs without claiming runtime validation");
    auto forged_id_artifact = *id_baked.artifact;
    std::size_t forged_offset = 0U;
    for (; forged_offset < forged_id_artifact.output_bytes.size();
         forged_offset += production::kMaterialSurfaceIdTexelBytes) {
        std::uint64_t face = 0U;
        for (unsigned byte = 0U; byte < 8U; ++byte) {
            face |= static_cast<std::uint64_t>(
                forged_id_artifact.output_bytes[forged_offset + byte]) << (byte * 8U);
        }
        if (face != 0U) break;
    }
    require(forged_offset < forged_id_artifact.output_bytes.size(),
        "material/surface ID product has a texel to forge");
    forged_id_artifact.output_bytes[forged_offset + 8U] ^= 0x01U;
    require(static_cast<bool>(forged_id_artifact.refresh_digests()) &&
            static_cast<bool>(forged_id_artifact.validate()),
        "forged material assignment keeps internally valid artifact digests");
    const production::PreparedAssetInput forged_asset_input{
        fixture.mesh_asset, &fixture.render_mesh, &fixture.lookdev, &forged_id_artifact};
    require(!production::compose_prepared_scene(
        "public.workshop", fixture.document, "portable.static-workshop.v1",
        digest("portable.static-workshop.v1"), std::span{&forged_asset_input, 1U}),
        "prepared publication rejects a rehashed ID map that disagrees with source assignment");

    const auto stale = [&]() {
        auto value = *baked.artifact;
        value.recipe.source_revision = core::Revision{
            fixture.document.revision().value() - 1U};
        require(static_cast<bool>(value.refresh_digests()),
            "adversarial stale artifact remains internally self-consistent");
        return value;
    }();
    const production::PreparedWorldPositionInput stale_input{&stale};
    require(!production::compose_prepared_scene(
        "public.workshop", fixture.document, "portable.static-workshop.v1",
        digest("portable.static-workshop.v1"), std::span{&asset_input, 1U},
        std::span{&stale_input, 1U}),
        "a self-consistent artifact from a stale project revision is rejected");
    auto stale_id_artifact = *id_baked.artifact;
    stale_id_artifact.recipe.source_revision = core::Revision{
        fixture.document.revision().value() - 1U};
    require(static_cast<bool>(stale_id_artifact.refresh_digests()),
        "stale ID artifact remains internally self-consistent for the rejection check");
    const production::PreparedAssetInput stale_id_asset_input{
        fixture.mesh_asset, &fixture.render_mesh, &fixture.lookdev, &stale_id_artifact};
    require(!production::compose_prepared_scene(
        "public.workshop", fixture.document, "portable.static-workshop.v1",
        digest("portable.static-workshop.v1"),
        std::span{&stale_id_asset_input, 1U}),
        "prepared composition rejects a self-consistent stale material/surface ID artifact");
    const production::PreparedWorldPositionInput duplicate_inputs[]{
        {&*baked.artifact}, {&*baked.artifact}};
    require(!production::compose_prepared_scene(
        "public.workshop", fixture.document, "portable.static-workshop.v1",
        digest("portable.static-workshop.v1"), std::span{&asset_input, 1U},
        duplicate_inputs), "duplicate instance UV products are rejected");
    const auto asset_scope = production::resolve_preparation_scope(fixture.document, {
        production::PreparationScopeKind::asset, fixture.document.revision(),
        {fixture.mesh_asset}, {}});
    require(static_cast<bool>(asset_scope), "asset-only scope resolves independently");
    require(!production::compose_prepared_scene(
        "public.workshop", fixture.document, asset_scope.value(),
        "portable.asset.v1", digest("portable.asset.v1"),
        std::span{&asset_input, 1U}, position_inputs),
        "instance world-space data cannot escape into a scope without scene nodes");

    const auto render_bytes = fixture.render_mesh.serialize();
    const auto lookdev_bytes = production::serialize_lookdev_draw_package(fixture.lookdev);
    const auto material_scope = production::resolve_preparation_scope(fixture.document, {
        production::PreparationScopeKind::scene, fixture.document.revision(), {}, {}});
    require(render_bytes && lookdev_bytes && material_scope,
        "prepared publication source products serialize");
    const auto material_catalog = production::compose_prepared_material_catalog(
        fixture.document, material_scope.value());
    const auto position_bytes = baked.artifact->serialize();
    const auto repeated_position_bytes = repeated_baked.artifact->serialize();
    const auto id_bytes = id_baked.artifact->serialize();
    require(material_catalog && position_bytes && repeated_position_bytes && id_bytes,
        "world-position and material/surface ID publication payloads serialize");
    const std::string material_text = material_catalog.value().serialize();
    const std::string texture_text = "workshop-shared-pixels";
    const auto as_bytes = [](std::string_view text) {
        return std::vector<std::uint8_t>(text.begin(), text.end());
    };
    std::vector<production::PreparedProductPayload> payloads;
    for (const auto& reference : prepared.value().products) {
        switch (reference.kind) {
        case production::PreparedProductKind::render_mesh:
            payloads.push_back({reference.identity, render_bytes.value()});
            break;
        case production::PreparedProductKind::lookdev_draw_package:
            payloads.push_back({reference.identity, lookdev_bytes.value()});
            break;
        case production::PreparedProductKind::material_catalog:
            payloads.push_back({reference.identity, as_bytes(material_text)});
            break;
        case production::PreparedProductKind::texture_source:
            payloads.push_back({reference.identity, as_bytes(texture_text)});
            break;
        case production::PreparedProductKind::world_position_data:
            payloads.push_back({reference.identity,
                reference.identity == identity ? position_bytes.value()
                    : repeated_position_bytes.value()});
            break;
        case production::PreparedProductKind::material_surface_id_data:
            payloads.push_back({reference.identity, id_bytes.value()});
            break;
        default:
            require(false, "fixture has no unhandled prepared product kind");
        }
    }
    TempDirectory output;
    const auto published = production::publish_prepared_scene(
        output.path() / "published", prepared.value(), payloads, std::nullopt);
    if (!published) {
        std::cerr << "prepared publication diagnostic: " << published.error().message << '\n';
    }
    require(static_cast<bool>(published), "full prepared closure publishes atomically");
    auto corrupted_payloads = payloads;
    const auto position_payload = std::find_if(corrupted_payloads.begin(),
        corrupted_payloads.end(), [&identity](const auto& value) {
            return value.identity == identity;
        });
    require(position_payload != corrupted_payloads.end() && !position_payload->bytes.empty(),
        "published fixture includes the bound position payload");
    position_payload->bytes.back() ^= 0x01U;
    require(!production::publish_prepared_scene(
        output.path() / "published", prepared.value(), corrupted_payloads,
        published.value().prepared_scene_digest),
        "corrupt position bytes cannot replace the last good prepared revision");
    const auto still_selected = production::read_selected_prepared_scene(
        output.path() / "published");
    if (!still_selected) {
        std::cerr << "selected revision read after rejected publication: "
                  << still_selected.error().message << '\n';
        for (const auto& context : still_selected.error().context) {
            std::cerr << "  context: " << context << '\n';
        }
    } else if (!still_selected.value().has_value()) {
        std::cerr << "selected revision disappeared after rejected publication\n";
    } else if (still_selected.value()->prepared_scene_digest !=
               published.value().prepared_scene_digest) {
        std::cerr << "selected digest changed after rejected publication: expected "
                  << published.value().prepared_scene_digest.hex() << ", got "
                  << still_selected.value()->prepared_scene_digest.hex() << '\n';
    }
    require(still_selected && still_selected.value().has_value() &&
            still_selected.value()->prepared_scene_digest ==
                published.value().prepared_scene_digest,
        "failed position publication preserves the selected prepared revision");
    const auto reopened = production::read_selected_prepared_scene(output.path() / "published");
    require(reopened && reopened.value().has_value(), "prepared publication reopens");
    for (const auto& [product_identity, expected_artifact] : std::array{
            std::pair{identity, &*baked.artifact},
            std::pair{repeated_identity, &*repeated_baked.artifact}}) {
        const auto reopened_product = std::find_if(
            reopened.value()->envelope.products.begin(), reopened.value()->envelope.products.end(),
            [&product_identity](const auto& value) { return value.identity == product_identity; });
        require(reopened_product != reopened.value()->envelope.products.end(),
            "published manifest retains each instance world-position reference");
        std::ifstream stream(reopened.value()->revision_path / reopened_product->relative_path,
            std::ios::binary);
        require(static_cast<bool>(stream),
            "published world-position bytes are addressable by their manifest reference");
        const std::vector<std::uint8_t> published_bytes{
            std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        const auto reopened_artifact = production::WorldPositionMapArtifact::deserialize(
            published_bytes);
        require(reopened_artifact &&
                reopened_artifact.value().artifact_digest == expected_artifact->artifact_digest,
            "published instance world-position product independently verifies after reopen");
    }
    const auto reopened_id_product = std::find_if(
        reopened.value()->envelope.products.begin(), reopened.value()->envelope.products.end(),
        [&id_identity](const auto& value) { return value.identity == id_identity; });
    require(reopened_id_product != reopened.value()->envelope.products.end(),
        "published manifest retains the material/surface ID product reference");
    std::ifstream id_stream(
        reopened.value()->revision_path / reopened_id_product->relative_path, std::ios::binary);
    require(static_cast<bool>(id_stream),
        "published material/surface ID bytes are addressable by the manifest reference");
    const std::vector<std::uint8_t> published_id_bytes{
        std::istreambuf_iterator<char>(id_stream), std::istreambuf_iterator<char>()};
    const auto reopened_id_artifact = production::MaterialSurfaceIdMapArtifact::deserialize(
        published_id_bytes);
    require(reopened_id_artifact && reopened_id_artifact.value().artifact_digest ==
            id_baked.artifact->artifact_digest,
        "published material/surface ID product independently verifies after reopen");
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

void world_primitives_compose_with_stable_correspondence() {
    using namespace carto;
    const Fixture fixture = make_fixture(workshop_world_model());
    const auto prepared = compose(fixture);
    const auto world_product = std::find_if(prepared.products.begin(),
        prepared.products.end(),
        [](const production::PreparedProductReference& product) {
            return product.kind == production::PreparedProductKind::world_model;
        });
    require(world_product != prepared.products.end(),
        "the durable world model is declared as one content-addressed product");

    const auto count_prepared = [&prepared](production::PreparedEntityKind kind) {
        return static_cast<std::size_t>(std::count_if(prepared.correspondences.begin(),
            prepared.correspondences.end(),
            [kind](const production::PreparedSourceCorrespondence& correspondence) {
                return correspondence.prepared_kind == kind;
            }));
    };
    require(count_prepared(production::PreparedEntityKind::semantic_area) == 2U &&
            count_prepared(production::PreparedEntityKind::semantic_path) == 1U &&
            count_prepared(production::PreparedEntityKind::structure_graph) == 1U &&
            count_prepared(production::PreparedEntityKind::instance_field) == 1U,
        "every durable world primitive receives an explicit stable correspondence");
    const auto area_correspondence = std::find_if(prepared.correspondences.begin(),
        prepared.correspondences.end(),
        [](const production::PreparedSourceCorrespondence& correspondence) {
            return correspondence.prepared_kind ==
                production::PreparedEntityKind::semantic_area &&
                correspondence.prepared_primary == 101U;
        });
    require(area_correspondence != prepared.correspondences.end() &&
            area_correspondence->source_kind ==
                production::PreparedSourceEntityKind::semantic_area &&
            area_correspondence->source_primary == 101U &&
            area_correspondence->source_secondary == 0U &&
            area_correspondence->prepared_secondary == 0U,
        "world correspondences preserve stable source identity exactly");

    const std::string encoded = prepared.serialize();
    const auto reopened = production::PreparedSceneEnvelope::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "world-primitive correspondences survive a deterministic round trip");
    const auto first_digest = prepared.canonical_digest();
    const auto second_digest = reopened.value().canonical_digest();
    require(first_digest && second_digest &&
            first_digest.value() == second_digest.value(),
        "world correspondence identity survives reopen");

    auto missing_world = prepared;
    missing_world.products.erase(std::remove_if(missing_world.products.begin(),
        missing_world.products.end(),
        [](const production::PreparedProductReference& product) {
            return product.kind == production::PreparedProductKind::world_model;
        }), missing_world.products.end());
    missing_world.dependencies.erase(std::remove_if(
        missing_world.dependencies.begin(), missing_world.dependencies.end(),
        [](const production::PreparedDependencyReference& dependency) {
            return dependency.identity == "world.model";
        }), missing_world.dependencies.end());
    require(!missing_world.validate(),
        "world correspondences fail closed without the world-model product");

    auto drifted_identity = prepared;
    auto drifted_entry = std::find_if(drifted_identity.correspondences.begin(),
        drifted_identity.correspondences.end(),
        [](const production::PreparedSourceCorrespondence& correspondence) {
            return correspondence.prepared_kind ==
                production::PreparedEntityKind::semantic_area &&
                correspondence.prepared_primary == 101U;
        });
    require(drifted_entry != drifted_identity.correspondences.end(),
        "drift fixture contains the selected world correspondence");
    drifted_entry->prepared_primary = 102U;
    require(!drifted_identity.validate(),
        "world correspondences must preserve exact source identity");
}

void extension_bindings_are_digest_bound_and_fail_closed() {
    using namespace carto;
    const Fixture fixture = make_fixture();
    auto prepared = compose(fixture);
    prepared.extension_bindings.push_back(production::PreparedExtensionBinding{
        "ext.motel-decay", "carto.semantic-sidecar.v1", digest("decay-payload")});
    prepared.extension_bindings.push_back(production::PreparedExtensionBinding{
        "ext.weather", "carto.environment.v2", digest("weather-payload")});
    require(static_cast<bool>(prepared.validate()),
        "canonically sorted extension bindings validate");

    const auto bound_digest = prepared.canonical_digest();
    require(bound_digest && !bound_digest.value().is_zero(),
        "extension bindings participate in the canonical digest");
    auto rebuilt = compose(fixture);
    rebuilt.extension_bindings.push_back(production::PreparedExtensionBinding{
        "ext.motel-decay", "carto.semantic-sidecar.v1", digest("decay-payload")});
    rebuilt.extension_bindings.push_back(production::PreparedExtensionBinding{
        "ext.weather", "carto.environment.v2", digest("weather-payload")});
    const auto rebuilt_digest = rebuilt.canonical_digest();
    require(rebuilt_digest && rebuilt_digest.value() == bound_digest.value(),
        "identical extension bindings reproduce the identical digest");

    auto unsorted = prepared;
    std::swap(unsorted.extension_bindings.front(), unsorted.extension_bindings.back());
    require(!unsorted.validate(),
        "extension bindings must be canonically sorted and unique");
    auto duplicated = prepared;
    duplicated.extension_bindings.back().extension_identity =
        duplicated.extension_bindings.front().extension_identity;
    require(!duplicated.validate(), "extension binding identities must be unique");
    auto zero_digest = prepared;
    zero_digest.extension_bindings.front().payload_digest = assets::Sha256Digest{};
    require(!zero_digest.validate(),
        "extension bindings refuse a zero payload digest");
    auto unsafe_identity = prepared;
    unsafe_identity.extension_bindings.front().extension_identity =
        "not a safe identity";
    require(!unsafe_identity.validate(),
        "extension binding identities are bounded safe tokens");

    auto drifted = prepared;
    drifted.extension_bindings.front().payload_digest = digest("other-payload");
    const auto drifted_digest = drifted.canonical_digest();
    require(drifted_digest && drifted_digest.value() != bound_digest.value(),
        "a drifted extension payload changes the canonical digest");

    const std::string encoded = prepared.serialize();
    require(encoded.find("EXTENSION_BINDINGS 2\n") != std::string::npos,
        "extension bindings are serialized into the canonical text");
    const auto reopened = production::PreparedSceneEnvelope::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded &&
            reopened.value().canonical_digest().value() == bound_digest.value(),
        "extension bindings survive reopen with a stable digest");

    const auto unbound = compose(fixture);
    const std::string unbound_encoded = unbound.serialize();
    const auto section_at = unbound_encoded.find("EXTENSION_BINDINGS 0\n");
    require(section_at != std::string::npos,
        "unbound envelopes serialize an empty extension-binding section");
    std::string legacy = unbound_encoded;
    legacy.erase(section_at, std::string{"EXTENSION_BINDINGS 0\n"}.size());
    const auto reopened_legacy =
        production::PreparedSceneEnvelope::deserialize(legacy);
    require(reopened_legacy &&
            reopened_legacy.value().extension_bindings.empty() &&
            reopened_legacy.value().serialize() == unbound_encoded,
        "envelopes serialized before extension bindings reopen with an empty set");

    const char* kHostilePrefix =
        "CARTOGRAPHER_PREPARED_SCENE 1\n"
        "SOURCE \"x\" 1 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "PROFILE \"x\" 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "COORDINATES 1 0 1 2\nPRODUCTS 0\nASSETS 0\nNODES 0\n"
        "MATERIAL_BINDINGS 0\nDEPENDENCIES 0\nCAPABILITIES 0\nCORRESPONDENCES 0\n";
    require(!production::PreparedSceneEnvelope::deserialize(
                std::string{kHostilePrefix} + "EXTENSION_BINDINGS 4097\n"),
        "oversized extension-binding counts are rejected before allocation");
    require(!production::PreparedSceneEnvelope::deserialize(
                std::string{kHostilePrefix} +
                "EXTENSION_BINDINGS 1\nEXTENSION_BINDING \"ext.a\" \"schema.v1\" not-a-digest\n"),
        "malformed extension-binding digests fail closed");
    require(!production::PreparedSceneEnvelope::deserialize(
                std::string{kHostilePrefix} +
                "CORRESPONDENCES 1\nCORRESPONDENCE 0 1 0 7 1 0\nEND\n"),
        "unknown correspondence kinds fail closed");
}

} // namespace

int main() {
    exact_products_compose_and_round_trip();
    stale_and_incomplete_composition_fails_closed();
    prepared_composition_obeys_resolved_scope();
        world_position_product_composes_and_publishes_for_exact_instance();
    malformed_envelopes_fail_closed();
    world_primitives_compose_with_stable_correspondence();
    extension_bindings_are_digest_bound_and_fail_closed();
    std::cout << "prepared scene tests passed\n";
    return EXIT_SUCCESS;
}
