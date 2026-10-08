#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/preparation_operation.hpp>
#include <carto/project/material_catalog.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include "link_test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-preparation-operation-tests-" + std::to_string(stamp) +
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

std::vector<std::uint8_t> one_pixel_tga(std::uint8_t red) {
    return {
        0U, 0U, 2U,
        0U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U,
        1U, 0U, 1U, 0U,
        24U, 0x20U,
        0U, 0U, red,
    };
}

void write_bytes(
    const std::filesystem::path& path,
    std::span<const std::uint8_t> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "test binary opens");
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(static_cast<bool>(output), "test binary writes completely");
}

carto::geometry::EditableMesh material_triangle() {
    using namespace carto;
    geometry::EditableMesh mesh;
    require(static_cast<bool>(mesh.insert_bulk(
                {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
                 {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
                 {geometry::VertexId{3U}, {0.0, 1.0, 0.0}}},
                {{geometry::FaceId{1U},
                  {geometry::VertexId{1U}, geometry::VertexId{2U},
                   geometry::VertexId{3U}}}})),
        "operation fixture topology is valid");
    attributes::AttributeSet attributes({
        {attributes::AttributeDomain::corner, 3U},
        {attributes::AttributeDomain::face, 1U},
    });
    require(static_cast<bool>(attributes.add_layer(attributes::AttributeLayer{
                {"uv0.corners", attributes::AttributeDomain::corner,
                 attributes::AttributeType::vec2},
                {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0},
                 core::Vec2d{0.0, 1.0}}})),
        "operation fixture UV layer is valid");
    require(static_cast<bool>(attributes.add_layer(attributes::AttributeLayer{
                {"condition.material_region", attributes::AttributeDomain::face,
                 attributes::AttributeType::uint32},
                {std::uint32_t{7U}}})),
        "operation fixture material region is valid");
    attributes::UvSetTable uv_sets;
    require(static_cast<bool>(uv_sets.add(attributes::UvSetDescriptor{
                1U, "UV0", "uv0.corners", true, true, false, 1U})),
        "operation fixture UV set is valid");
    require(static_cast<bool>(mesh.set_attribute_payload(
                std::move(attributes), std::move(uv_sets))),
        "operation fixture accepts its attributes");
    return mesh;
}

carto::geometry::EditableMesh two_region_quad() {
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
        "workshop quad topology is valid");
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
        "workshop quad UV layer is valid");
    require(static_cast<bool>(attributes.add_layer(attributes::AttributeLayer{
                {"condition.material_region", attributes::AttributeDomain::face,
                 attributes::AttributeType::uint32},
                {std::uint32_t{7U}, std::uint32_t{8U}}})),
        "workshop quad material regions are valid");
    attributes::UvSetTable uv_sets;
    require(static_cast<bool>(uv_sets.add(attributes::UvSetDescriptor{
                1U, "UV0", "uv0.corners", true, true, false, 1U})),
        "workshop quad UV set is valid");
    require(static_cast<bool>(mesh.set_attribute_payload(
                std::move(attributes), std::move(uv_sets))),
        "workshop quad accepts its attributes");
    return mesh;
}

carto::project::MaterialCatalog material_catalog(
    std::uint64_t mesh_asset,
    const std::vector<std::uint8_t>& texture_bytes,
    double roughness,
    std::uint64_t revision,
    carto::render::TextureInterpretation interpretation =
        carto::render::TextureInterpretation::asset) {
    using namespace carto;
    project::MaterialCatalog catalog;
    require(static_cast<bool>(catalog.insert_texture(project::TextureSourceRecord{
                .id = 501U,
                .name = "Fixture albedo",
                .relative_locator = "textures/albedo.tga",
                .content_digest = assets::sha256(texture_bytes),
                .format = assets::TextureSourceFormat::tga,
                .width = 1U,
                .height = 1U,
                .provenance = "preparation-operation-fixture",
                .revision = core::Revision{revision},
            })),
        "fixture texture enters source catalog");
    render::MaterialAsset material;
    material.id = 71U;
    material.name = "Fixture paint";
    material.revision = core::Revision{revision};
    material.definition.roughness = roughness;
    material.definition.base_color.asset = 501U;
    material.definition.base_color.uv_set = 1U;
    material.definition.base_color.interpretation = interpretation;
    require(static_cast<bool>(catalog.insert_material(std::move(material))),
        "fixture material enters source catalog");
    require(static_cast<bool>(catalog.insert_assignment({
                .key = {mesh_asset, 7U},
                .material = 71U,
                .variant = std::nullopt,
            })),
        "fixture region receives its material assignment");
    require(static_cast<bool>(catalog.validate()), "fixture material catalog validates");
    return catalog;
}

carto::project::ProjectDocument make_document(
    const std::filesystem::path& journal_path,
    const std::vector<std::uint8_t>& texture_bytes,
    carto::render::TextureInterpretation interpretation =
        carto::render::TextureInterpretation::asset) {
    using namespace carto;
    project::ProjectDocument document;
    journal::Journal journal(journal_path);
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "preparation-operation-test");
    require(static_cast<bool>(transaction), "fixture transaction begins");
    const auto mesh_asset = transaction.value().add_mesh(material_triangle());
    require(mesh_asset && mesh_asset.value() == 1U,
        "fixture mesh enters project with stable identity");
    require(static_cast<bool>(transaction.value().set_material_catalog(
                material_catalog(
                    mesh_asset.value(), texture_bytes, 0.35, 1U, interpretation))),
        "fixture source catalog enters project");
    const auto object = transaction.value().create_object("Fixture object");
    require(object && transaction.value().attach_mesh(object.value(), mesh_asset.value()),
        "fixture scene references its reusable mesh");
    require(static_cast<bool>(transaction.value().commit("prepare.fixture.create")),
        "fixture commits through one journal event");
    return document;
}

carto::project::ProjectDocument make_two_mesh_workshop_document(
    const std::filesystem::path& journal_path,
    const std::vector<std::uint8_t>& texture_bytes) {
    using namespace carto;
    project::ProjectDocument document;
    journal::Journal journal(journal_path);
    auto transaction = project::ProjectTransaction::begin(
        document, journal, "preparation-operation-workshop-test");
    require(static_cast<bool>(transaction), "workshop transaction begins");
    const auto first_mesh = transaction.value().add_mesh(material_triangle());
    const auto second_mesh = transaction.value().add_mesh(two_region_quad());
    require(first_mesh && second_mesh && first_mesh.value() == 1U &&
            second_mesh.value() == 2U,
        "workshop owns two stable mesh assets");

    project::MaterialCatalog catalog;
    require(static_cast<bool>(catalog.insert_texture(project::TextureSourceRecord{
                .id = 501U,
                .name = "Workshop albedo",
                .relative_locator = "textures/albedo.tga",
                .content_digest = assets::sha256(texture_bytes),
                .format = assets::TextureSourceFormat::tga,
                .width = 1U,
                .height = 1U,
                .provenance = "preparation-workshop-fixture",
                .revision = core::Revision{1U},
            })),
        "workshop texture enters the catalog");
    render::MaterialAsset paint;
    paint.id = 71U;
    paint.name = "Workshop paint";
    paint.revision = core::Revision{1U};
    paint.definition.roughness = 0.3;
    paint.definition.base_color.asset = 501U;
    paint.definition.base_color.uv_set = 1U;
    render::MaterialAsset rubber;
    rubber.id = 72U;
    rubber.name = "Workshop rubber";
    rubber.revision = core::Revision{1U};
    rubber.definition.roughness = 0.85;
    require(catalog.insert_material(std::move(paint)) &&
            catalog.insert_material(std::move(rubber)) &&
            catalog.insert_assignment({{1U, 7U}, 71U, std::nullopt}) &&
            catalog.insert_assignment({{2U, 7U}, 71U, std::nullopt}) &&
            catalog.insert_assignment({{2U, 8U}, 72U, std::nullopt}) &&
            transaction.value().set_material_catalog(std::move(catalog)),
        "workshop two-material assignments enter authoring truth");

    require(transaction.value().insert_object(scene::SceneObject{
                scene::ObjectId{1U}, "Workshop root", {}, std::nullopt,
                std::nullopt, true, false}) &&
            transaction.value().insert_object(scene::SceneObject{
                scene::ObjectId{2U}, "Bench assembly", {},
                scene::ObjectId{1U}, 1U, true, false}) &&
            transaction.value().insert_object(scene::SceneObject{
                scene::ObjectId{3U}, "Selected gate", {},
                scene::ObjectId{2U}, 2U, true, false}) &&
            transaction.value().insert_object(scene::SceneObject{
                scene::ObjectId{4U}, "Repeated gate", {},
                scene::ObjectId{1U}, 2U, true, false}),
        "workshop hierarchy and repeated asset instances enter source truth");
    require(static_cast<bool>(transaction.value().commit(
                "prepare.workshop.create")),
        "workshop commits through one journal event");
    return document;
}

carto::production::PreparationOperationRequest request_for(
    const carto::project::ProjectDocument& document,
    const std::filesystem::path& source_root,
    const std::filesystem::path& output_root,
    std::optional<carto::assets::Sha256Digest> expected) {
    using namespace carto;
    return production::PreparationOperationRequest{
        .scope = {
            production::PreparationScopeKind::scene,
            document.revision(),
            {},
            {},
        },
        .profile = production::portable_preparation_profile(),
        .source_root = source_root,
        .output_root = output_root,
        .expected_selected_digest = expected,
        .stop_token = {},
    };
}

void one_operation_is_shared_complete_and_last_valid() {
    using namespace carto;
    TempDirectory temp;
    const auto source_root = temp.path() / "source";
    const auto output_root = temp.path() / "prepared";
    const auto texture_path = source_root / "textures" / "albedo.tga";
    const auto red_texture = one_pixel_tga(255U);
    write_bytes(texture_path, red_texture);
    const auto first_journal_path = temp.path() / "first-journal.log";
    const auto first_document = make_document(
        first_journal_path, red_texture);

    auto request = request_for(first_document, source_root, output_root, std::nullopt);
    const auto first = production::prepare_and_publish(first_document, request);
    if (!first.succeeded()) {
        for (const auto& diagnostic : first.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            for (const auto& context : diagnostic.context) {
                std::cerr << "  context: " << context << '\n';
            }
        }
    }
    require(first.succeeded() && first.resolved_scope.has_value() &&
            first.envelope.has_value() && first.publication.has_value() &&
            !first.operation_id.is_zero(),
        "shared operation resolves, executes, verifies, and publishes");
    require(first.envelope->products.size() == 4U,
        "published closure contains geometry, lookdev, source catalog, and texture");
    require(first.envelope->source_namespace == first_document.source_namespace(),
        "preparation uses the durable project namespace without a client override");
    const auto first_digest = first.publication->prepared_scene_digest;
    const auto selected = production::read_selected_prepared_scene(output_root);
    require(selected && selected.value().has_value() &&
            selected.value()->prepared_scene_digest == first_digest,
        "published operation is independently inspectable");

    const auto material_product = std::find_if(
        selected.value()->envelope.products.begin(),
        selected.value()->envelope.products.end(),
        [](const production::PreparedProductReference& product) {
            return product.kind == production::PreparedProductKind::material_catalog;
        });
    require(material_product != selected.value()->envelope.products.end(),
        "prepared material catalog product is declared");
    std::ifstream material_input(
        selected.value()->revision_path / material_product->relative_path,
        std::ios::binary);
    const std::string material_text{
        std::istreambuf_iterator<char>{material_input},
        std::istreambuf_iterator<char>{}};
    const auto reopened_materials = project::MaterialCatalog::deserialize(material_text);
    require(reopened_materials && reopened_materials.value().materials().contains(71U) &&
            reopened_materials.value().textures().contains(501U),
        "destination-neutral source material meaning reopens from publication");

    const auto relocated = temp.path() / "relocated";
    std::filesystem::copy(
        output_root, relocated, std::filesystem::copy_options::recursive);
    std::error_code remove_error;
    std::filesystem::remove_all(source_root, remove_error);
    require(!remove_error &&
            production::read_selected_prepared_scene(relocated) &&
            production::read_selected_prepared_scene(relocated).value().has_value(),
        "relocated publication remains complete after source removal");

    write_bytes(texture_path, red_texture);
    request.expected_selected_digest = first_digest;
    const auto replay = production::prepare_and_publish(first_document, request);
    require(replay.succeeded() && replay.publication->reused_immutable_revision,
        "identical preparation reuses the immutable complete revision");

    project::ProjectDocument second_document = first_document;
    journal::Journal second_journal(first_journal_path);
    auto update = project::ProjectTransaction::begin(
        second_document, second_journal, "preparation-operation-test");
    require(update && update.value().set_material_catalog(
                material_catalog(1U, red_texture, 0.72, 2U)) &&
            update.value().commit("prepare.fixture.material-update"),
        "second source revision changes material intent through a transaction");
    auto second_request = request_for(
        second_document, source_root, output_root, first_digest);
    const auto second = production::prepare_and_publish(
        second_document, second_request);
    require(second.succeeded(), "newer source revision publishes from current baseline");
    const auto second_digest = second.publication->prepared_scene_digest;
    require(second_digest != first_digest,
        "material edit produces a distinct prepared revision");

    request.expected_selected_digest = first_digest;
    const auto late_first = production::prepare_and_publish(first_document, request);
    require(!late_first.succeeded() &&
            late_first.terminal_stage == production::PreparationStage::publish &&
            !late_first.diagnostics.empty() &&
            late_first.diagnostics.front().code ==
                "carto.prepare.publish.stale_data",
        "late older operation is rejected by publication compare-and-swap");
    require(production::read_selected_prepared_scene(output_root).value()
                ->prepared_scene_digest == second_digest,
        "late older operation cannot regress the last valid output");

    std::filesystem::remove(texture_path, remove_error);
    require(!remove_error, "test removes source texture");
    second_request.expected_selected_digest = second_digest;
    const auto missing = production::prepare_and_publish(
        second_document, second_request);
    require(!missing.succeeded() &&
            missing.terminal_stage == production::PreparationStage::verify &&
            missing.diagnostics.front().source_primary == 501U,
        "missing texture is attached to its stable source identity");
    require(production::read_selected_prepared_scene(output_root).value()
                ->prepared_scene_digest == second_digest,
        "missing dependency leaves the last valid output selected");

    write_bytes(texture_path, one_pixel_tga(64U));
    const auto drifted = production::prepare_and_publish(
        second_document, second_request);
    require(!drifted.succeeded() &&
            drifted.terminal_stage == production::PreparationStage::verify &&
            drifted.diagnostics.front().code ==
                "carto.prepare.verify.stale_data",
        "changed source bytes fail against the captured texture digest");
    require(production::read_selected_prepared_scene(output_root).value()
                ->prepared_scene_digest == second_digest,
        "texture drift does not replace the last valid output");
}

void invalid_or_stale_requests_fail_before_mutation() {
    using namespace carto;
    TempDirectory temp;
    const auto texture = one_pixel_tga(255U);
    const auto source_root = temp.path() / "source";
    write_bytes(source_root / "textures" / "albedo.tga", texture);
    const auto document = make_document(temp.path() / "journal.log", texture);
    auto request = request_for(
        document, source_root, temp.path() / "prepared", std::nullopt);

    request.profile.include_lookdev = false;
    request.profile.require_lookdev_for_material_regions = true;
    const auto invalid_profile = production::prepare_and_publish(document, request);
    require(!invalid_profile.succeeded() &&
            invalid_profile.terminal_stage == production::PreparationStage::resolve &&
            !std::filesystem::exists(request.output_root),
        "incoherent profile fails before creating publication state");

    request = request_for(document, source_root, temp.path() / "prepared", std::nullopt);
    request.scope.expected_source_revision = core::Revision{
        document.revision().value() - 1U};
    const auto stale_scope = production::prepare_and_publish(document, request);
    require(!stale_scope.succeeded() &&
            stale_scope.terminal_stage == production::PreparationStage::resolve &&
            !std::filesystem::exists(request.output_root),
        "stale captured scope fails before creating publication state");
}

void portable_source_meaning_survives_narrower_optional_lookdev() {
    using namespace carto;
    TempDirectory temp;
    const auto texture = one_pixel_tga(220U);
    const auto source_root = temp.path() / "source";
    write_bytes(source_root / "textures" / "albedo.tga", texture);
    const auto document = make_document(
        temp.path() / "journal.log", texture,
        render::TextureInterpretation::srgb);
    const auto output_root = temp.path() / "prepared";
    const auto report = production::prepare_and_publish(
        document, request_for(document, source_root, output_root, std::nullopt));
    require(report.succeeded() && report.diagnostics.size() == 1U &&
            report.diagnostics.front().severity ==
                production::PreparationDiagnosticSeverity::warning &&
            report.diagnostics.front().code ==
                "carto.prepare.execute.optional_lookdev_unsupported",
        "portable preparation reports a narrower optional native lookdev path");
    require(report.envelope->products.size() == 3U &&
            std::none_of(report.envelope->products.begin(), report.envelope->products.end(),
                [](const production::PreparedProductReference& product) {
                    return product.kind ==
                        production::PreparedProductKind::lookdev_draw_package;
                }),
        "unsupported optional lookdev is omitted without fabricating a product");
    const auto selected = production::read_selected_prepared_scene(output_root);
    require(selected && selected.value().has_value(),
        "source-preserving publication remains valid without optional lookdev");
    const auto material_product = std::find_if(
        selected.value()->envelope.products.begin(),
        selected.value()->envelope.products.end(),
        [](const production::PreparedProductReference& product) {
            return product.kind == production::PreparedProductKind::material_catalog;
        });
    require(material_product != selected.value()->envelope.products.end(),
        "source material catalog remains included");
    std::ifstream input(
        selected.value()->revision_path / material_product->relative_path,
        std::ios::binary);
    const std::string encoded{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto catalog = project::MaterialCatalog::deserialize(encoded);
    require(catalog && catalog.value().materials().at(71U)
                           .definition.base_color.interpretation ==
            render::TextureInterpretation::srgb,
        "unsupported optional lookdev does not flatten source texture interpretation");
}

void profiles_round_trip_and_hostile_inputs_fail_closed() {
    using namespace carto;
    auto profile = production::portable_preparation_profile();
    profile.vertex_color_layer = "paint.vertex-color";
    const auto encoded = profile.serialize();
    const auto decoded = production::PreparationProfile::deserialize(encoded);
    require(decoded && decoded.value().serialize() == encoded &&
            decoded.value().canonical_digest().value() ==
                profile.canonical_digest().value(),
        "saved preparation profile reopens with stable meaning and identity");

    std::string future = encoded;
    future.replace(0U,
        std::string{"CARTOGRAPHER_PREPARATION_PROFILE 1"}.size(),
        "CARTOGRAPHER_PREPARATION_PROFILE 99");
    require(!production::PreparationProfile::deserialize(future),
        "future preparation profile versions fail closed");

    std::string non_boolean = encoded;
    const auto channels = non_boolean.find("CHANNELS 1");
    require(channels != std::string::npos, "profile fixture contains channel policy");
    non_boolean.replace(channels, std::string{"CHANNELS 1"}.size(), "CHANNELS 2");
    require(!production::PreparationProfile::deserialize(non_boolean),
        "non-Boolean preparation profile fields fail closed");
    require(!production::PreparationProfile::deserialize(encoded + "TRAILING\n"),
        "preparation profile rejects trailing records");
    require(!production::PreparationProfile::deserialize(std::string(
                production::kMaxPreparationProfileBytes + 1U, 'x')),
        "preparation profile rejects oversized input before parsing");
}

void cancellation_never_replaces_last_valid_output() {
    using namespace carto;
    TempDirectory temp;
    const auto texture = one_pixel_tga(180U);
    const auto source_root = temp.path() / "source";
    write_bytes(source_root / "textures" / "albedo.tga", texture);
    const auto document = make_document(temp.path() / "journal.log", texture);
    const auto output_root = temp.path() / "prepared";
    auto first_request = request_for(
        document, source_root, output_root, std::nullopt);
    const auto first = production::prepare_and_publish(document, first_request);
    require(first.succeeded(), "cancellation fixture publishes a valid baseline");

    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelled_request = request_for(
        document, source_root, output_root,
        first.publication->prepared_scene_digest);
    cancelled_request.stop_token = cancellation.get_token();
    const auto cancelled = production::prepare_and_publish(
        document, cancelled_request);
    require(!cancelled.succeeded() && !cancelled.diagnostics.empty() &&
            cancelled.diagnostics.front().code ==
                "carto.prepare.resolve.cancelled",
        "cancelled operation returns a structured cancellation result");
    const auto selected = production::read_selected_prepared_scene(output_root);
    require(selected && selected.value().has_value() &&
            selected.value()->prepared_scene_digest ==
                first.publication->prepared_scene_digest,
        "cancelled operation leaves the last valid output selected");
}

void texture_capture_rejects_linked_source_ancestors() {
    using namespace carto;
    TempDirectory temp;
    const auto texture = one_pixel_tga(190U);
    const auto actual_parent = temp.path() / "actual-parent";
    const auto source_root = actual_parent / "source";
    write_bytes(source_root / "textures" / "albedo.tga", texture);
    const auto document = make_document(temp.path() / "journal.log", texture);

    const auto linked_parent = temp.path() / "linked-parent";
    std::error_code link_error;
    require(test_support::create_directory_link(
                actual_parent, linked_parent, link_error),
        "texture safety test creates a symbolic-link or junction ancestor");

    const auto output_root = temp.path() / "prepared";
    const auto linked = production::prepare_and_publish(
        document, request_for(document, linked_parent / "source",
                      output_root, std::nullopt));
    require(!linked.succeeded() &&
            linked.terminal_stage == production::PreparationStage::verify &&
            !linked.diagnostics.empty() &&
            linked.diagnostics.front().code ==
                "carto.prepare.verify.validation_failed" &&
            linked.diagnostics.front().source_primary == 501U,
        "texture capture rejects a linked source-root ancestor with source identity");
    require(!std::filesystem::exists(output_root),
        "linked texture source rejection creates no publication state");

    const auto direct = production::prepare_and_publish(
        document, request_for(document, source_root, output_root, std::nullopt));
    require(direct.succeeded(),
        "direct texture source remains valid after linked-source rejection");
}

void selected_second_mesh_and_complete_assembly_prepare_exactly() {
    using namespace carto;
    TempDirectory temp;
    const auto texture = one_pixel_tga(205U);
    const auto source_root = temp.path() / "source";
    write_bytes(source_root / "textures" / "albedo.tga", texture);
    const auto authored = make_two_mesh_workshop_document(
        temp.path() / "workshop-journal.log", texture);
    const auto project_path = temp.path() / "workshop.carto";
    require(static_cast<bool>(authored.save_atomic(project_path)),
        "two-material workshop saves without derived preparation state");
    const auto reopened = project::ProjectDocument::load(project_path);
    require(reopened && reopened.value().source_namespace() ==
                authored.source_namespace() &&
            reopened.value().material_catalog().materials().size() == 2U,
        "two-material workshop reopens with source identity and catalog intact");
    const auto& document = reopened.value();

    auto selected_request = request_for(
        document, source_root, temp.path() / "selected-prepared", std::nullopt);
    selected_request.scope.kind = production::PreparationScopeKind::asset;
    selected_request.scope.asset_ids = {2U};
    const auto selected = production::prepare_and_publish(
        document, selected_request);
    require(selected.succeeded() && selected.resolved_scope->asset_ids ==
                std::vector<std::uint64_t>{2U} &&
            selected.envelope->assets.size() == 1U &&
            selected.envelope->assets.front().id == 2U &&
            selected.envelope->nodes.empty() &&
            selected.envelope->material_bindings.size() == 2U,
        "explicit asset scope prepares the selected second mesh, not the first");
    require(std::none_of(selected.envelope->products.begin(),
                selected.envelope->products.end(),
                [](const production::PreparedProductReference& product) {
                    return product.identity.starts_with("mesh.1.");
                }),
        "selected second-mesh closure does not disclose first-mesh products");

    auto assembly_request = request_for(
        document, source_root, temp.path() / "assembly-prepared", std::nullopt);
    assembly_request.scope.kind = production::PreparationScopeKind::assembly;
    assembly_request.scope.node_ids = {2U};
    const auto assembly = production::prepare_and_publish(
        document, assembly_request);
    require(assembly.succeeded() && assembly.resolved_scope->asset_ids ==
                std::vector<std::uint64_t>({1U, 2U}) &&
            assembly.resolved_scope->node_ids ==
                std::vector<std::uint64_t>({1U, 2U, 3U}) &&
            assembly.envelope->assets.size() == 2U &&
            assembly.envelope->nodes.size() == 3U &&
            assembly.envelope->material_bindings.size() == 3U,
        "assembly scope prepares ancestors, descendants, both assets, and both materials");
    require(std::none_of(assembly.envelope->nodes.begin(),
                assembly.envelope->nodes.end(),
                [](const production::PreparedSceneNode& node) {
                    return node.id == 4U;
                }),
        "assembly scope excludes an out-of-assembly repeated placement");

    auto clean_request = request_for(
        document, source_root, temp.path() / "assembly-clean", std::nullopt);
    clean_request.scope.kind = production::PreparationScopeKind::assembly;
    clean_request.scope.node_ids = {2U};
    const auto clean = production::prepare_and_publish(document, clean_request);
    require(clean.succeeded() && clean.operation_id == assembly.operation_id &&
            clean.publication->prepared_scene_digest ==
                assembly.publication->prepared_scene_digest &&
            clean.envelope->serialize() == assembly.envelope->serialize(),
        "clean and repeated preparation resolve identical workshop meaning");
}

} // namespace

int main() {
    one_operation_is_shared_complete_and_last_valid();
    invalid_or_stale_requests_fail_before_mutation();
    portable_source_meaning_survives_narrower_optional_lookdev();
    profiles_round_trip_and_hostile_inputs_fail_closed();
    cancellation_never_replaces_last_valid_output();
    texture_capture_rejects_linked_source_ancestors();
    selected_second_mesh_and_complete_assembly_prepare_exactly();
    std::cout << "preparation operation tests passed\n";
    return EXIT_SUCCESS;
}
