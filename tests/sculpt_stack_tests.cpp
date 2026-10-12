#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/sculpt_stack.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
            ("cartographer-sculpt-stack-tests-" + std::to_string(stamp) + "-" +
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

carto::geometry::EditableMesh make_masked_plane() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {-1.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{4U}, {-1.0, 1.0, 0.0}},
         {geometry::VertexId{5U}, {0.0, 1.0, 0.0}},
         {geometry::VertexId{6U}, {1.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{5U}, geometry::VertexId{4U}}},
         {geometry::FaceId{2U}, {geometry::VertexId{2U}, geometry::VertexId{3U},
             geometry::VertexId{6U}, geometry::VertexId{5U}}}}));
    attributes::AttributeSet attributes({{attributes::AttributeDomain::vertex, 6U}});
    REQUIRE(attributes.add_layer(attributes::AttributeLayer{
        {"mask.protect", attributes::AttributeDomain::vertex,
            attributes::AttributeType::float32},
        {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F}}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), attributes::UvSetTable{}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_raised_pair() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {-1.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {0.0, 0.0, 1.0}},
         {geometry::VertexId{3U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{4U}, {-1.0, 1.0, 0.0}},
         {geometry::VertexId{5U}, {0.0, 1.0, 1.0}},
         {geometry::VertexId{6U}, {1.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{5U}, geometry::VertexId{4U}}},
         {geometry::FaceId{2U}, {geometry::VertexId{2U}, geometry::VertexId{3U},
             geometry::VertexId{6U}, geometry::VertexId{5U}}}}));
    attributes::AttributeSet attributes({{attributes::AttributeDomain::vertex, 6U}});
    REQUIRE(attributes.add_layer(attributes::AttributeLayer{
        {"mask.protect", attributes::AttributeDomain::vertex,
            attributes::AttributeType::float32},
        {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F}}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), attributes::UvSetTable{}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_scrape_fixture() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {-1.0, -1.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, -1.0, 0.0}},
         {geometry::VertexId{3U}, {1.0, 1.0, 0.0}},
         {geometry::VertexId{4U}, {-1.0, 1.0, 0.0}},
         {geometry::VertexId{5U}, {-1.0, -1.0, 1.0}},
         {geometry::VertexId{6U}, {1.0, -1.0, 1.0}},
         {geometry::VertexId{7U}, {1.0, 1.0, 1.0}},
         {geometry::VertexId{8U}, {-1.0, 1.0, 1.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}, geometry::VertexId{4U}}},
         {geometry::FaceId{2U}, {geometry::VertexId{5U}, geometry::VertexId{6U},
             geometry::VertexId{7U}, geometry::VertexId{8U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_local_radial_fixture() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {11.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {10.0, 1.0, 0.0}},
         {geometry::VertexId{3U}, {9.0, 0.0, 0.0}},
         {geometry::VertexId{4U}, {10.0, -1.0, 0.0}},
         {geometry::VertexId{5U}, {10.0, 0.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{5U}, geometry::VertexId{1U},
             geometry::VertexId{2U}}},
         {geometry::FaceId{2U}, {geometry::VertexId{5U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{3U}, {geometry::VertexId{5U}, geometry::VertexId{3U},
             geometry::VertexId{4U}}},
         {geometry::FaceId{4U}, {geometry::VertexId{5U}, geometry::VertexId{4U},
             geometry::VertexId{1U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_radial_budget_fixture() {
    using namespace carto;
    constexpr std::uint32_t ring_count = 17U;
    geometry::EditableMesh mesh;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    vertices.reserve(static_cast<std::size_t>(ring_count) + 1U);
    faces.reserve(ring_count);
    vertices.push_back({geometry::VertexId{1U}, {0.0, 0.0, 0.0}});
    for (std::uint32_t index = 0U; index < ring_count; ++index) {
        const double angle = 2.0 * std::numbers::pi_v<double> *
            static_cast<double>(index) / static_cast<double>(ring_count);
        vertices.push_back({geometry::VertexId{static_cast<std::uint64_t>(index) + 2U},
            {std::cos(angle), std::sin(angle), 0.0}});
        const std::uint64_t first_ring = static_cast<std::uint64_t>(index) + 2U;
        const std::uint64_t second_ring = static_cast<std::uint64_t>(
            (index + 1U) % ring_count) + 2U;
        faces.push_back({geometry::FaceId{static_cast<std::uint64_t>(index) + 1U},
            {geometry::VertexId{1U}, geometry::VertexId{first_ring},
                geometry::VertexId{second_ring}}});
    }
    REQUIRE(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::project::ProjectDocument make_project(
    carto::journal::Journal& journal,
    carto::geometry::EditableMesh mesh,
    std::uint64_t& mesh_asset) {
    using namespace carto;
    auto document = project::ProjectDocument::create("SculptStack Test");
    REQUIRE(document);
    const std::string initial_snapshot = document.value().serialize();
    const auto* snapshot_bytes = reinterpret_cast<const std::uint8_t*>(initial_snapshot.data());
    REQUIRE(journal.append(journal::JournalAppend{
        core::Revision{}, document.value().revision(), "project.create",
        std::span<const std::uint8_t>(snapshot_bytes, initial_snapshot.size()), std::nullopt}));
    auto transaction = project::ProjectTransaction::begin(
        document.value(), journal, "sculpt-stack-tests");
    REQUIRE(transaction);
    const auto added = transaction.value().add_mesh(std::move(mesh));
    REQUIRE(added);
    mesh_asset = added.value();
    REQUIRE(transaction.value().commit("mesh.fixture"));
    return std::move(document).value();
}

carto::production::SculptStack make_stack(const carto::project::ProjectDocument& document,
    std::uint64_t mesh_asset) {
    using namespace carto;
    auto stack = production::make_sculpt_stack(document, mesh_asset);
    REQUIRE(stack);
    production::SculptLayer layer;
    layer.identity = "surface.primary";
    layer.name = "Primary form";
    layer.visible = true;
    layer.weight = 1.0;
    production::SculptStroke stroke;
    stroke.identity = "stroke.0001";
    stroke.mode = production::SculptStrokeMode::add;
    stroke.symmetry = production::SculptSymmetryMode::mirror_x;
    stroke.symmetry_plane_x = 0.0;
    stroke.radius = 1.5;
    stroke.strength = 0.2;
    stroke.mask_layer_id = "mask.protect";
    stroke.samples = {{{-0.5, 0.0, 0.0}, 1.0}};
    layer.strokes.push_back(std::move(stroke));
    stack.value().layers.push_back(std::move(layer));
    REQUIRE(stack.value().validate());
    return stack.value();
}

void stack_serialization_and_evaluation_are_deterministic() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "stack-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, make_masked_plane(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const production::SculptStack stack = make_stack(document, mesh_asset);

    const std::string serialized = stack.serialize();
    REQUIRE(!serialized.empty());
    const auto decoded = production::SculptStack::deserialize(serialized);
    REQUIRE(decoded);
    REQUIRE(decoded.value().serialize() == serialized);
    REQUIRE(decoded.value().source == stack.source);
    REQUIRE(decoded.value().layers.size() == 1U);
    REQUIRE(decoded.value().layers.front().strokes.front().samples.front().position.x == -0.5);

    const auto first = production::compile_sculpt_stack(stack, source);
    const auto second = production::compile_sculpt_stack(decoded.value(), source);
    REQUIRE(first && second);
    REQUIRE(first.value().settings_digest == second.value().settings_digest);
    REQUIRE(first.value().output_digest == second.value().output_digest);
    REQUIRE(first.value().artifact_digest == second.value().artifact_digest);
    REQUIRE(first.value().vertices.size() == source.vertex_count());
    REQUIRE(first.value().validate(source));
    REQUIRE(first.value().maximum_displacement > 0.0);
    REQUIRE(source.find_vertex(geometry::VertexId{1U})->position.z == 0.0);
    REQUIRE(source.find_vertex(geometry::VertexId{3U})->position.z == 0.0);

    const auto output_position = [&](geometry::VertexId id) {
        const auto vertex = std::find_if(first.value().vertices.begin(), first.value().vertices.end(),
            [id](const geometry::Vertex& candidate) { return candidate.id == id; });
        REQUIRE(vertex != first.value().vertices.end());
        return vertex->position;
    };
    const core::Vec3d left = output_position(geometry::VertexId{1U});
    const core::Vec3d right = output_position(geometry::VertexId{3U});
    REQUIRE(left.z > 0.0);
    REQUIRE(right.z == 0.0); // The authored mask protects this vertex.
    REQUIRE(output_position(geometry::VertexId{4U}).z ==
        output_position(geometry::VertexId{6U}).z);

    auto multi_sample_stack = stack;
    multi_sample_stack.layers.front().strokes.front().samples.assign(
        16U, production::SculptStrokeSample{{-0.5, 0.0, 0.0}, 1.0});
    const auto indexed_result = production::compile_sculpt_stack(multi_sample_stack, source);
    REQUIRE(indexed_result);
    const auto indexed_left = std::find_if(
        indexed_result.value().vertices.begin(), indexed_result.value().vertices.end(),
        [](const geometry::Vertex& candidate) {
            return candidate.id == geometry::VertexId{1U};
        });
    REQUIRE(indexed_left != indexed_result.value().vertices.end());
    REQUIRE(std::abs(indexed_left->position.z - 64.0 / 27.0) < 1e-12);
    const auto indexed_protected = std::find_if(
        indexed_result.value().vertices.begin(), indexed_result.value().vertices.end(),
        [](const geometry::Vertex& candidate) {
            return candidate.id == geometry::VertexId{3U};
        });
    REQUIRE(indexed_protected != indexed_result.value().vertices.end());
    REQUIRE(indexed_protected->position.z == 0.0);

    auto hidden = stack;
    hidden.layers.front().visible = false;
    const auto hidden_result = production::compile_sculpt_stack(hidden, source);
    REQUIRE(hidden_result);
    REQUIRE(hidden_result.value().maximum_displacement == 0.0);
    REQUIRE(hidden_result.value().output_digest != first.value().output_digest);

    auto stale_source = source;
    REQUIRE(stale_source.set_vertex_position(geometry::VertexId{2U}, {0.1, 0.0, 0.0}));
    const auto stale_result = production::compile_sculpt_stack(stack, stale_source);
    REQUIRE(!stale_result);
    REQUIRE(stale_result.error().code == core::ErrorCode::stale_data);

    auto missing_mask = stack;
    missing_mask.layers.front().strokes.front().mask_layer_id = "mask.missing";
    const auto missing_mask_result = production::compile_sculpt_stack(missing_mask, source);
    REQUIRE(!missing_mask_result);
    REQUIRE(missing_mask_result.error().code == core::ErrorCode::not_found);

    auto invalid_pressure = stack;
    invalid_pressure.layers.front().strokes.front().samples.front().pressure = 1.5;
    REQUIRE(!invalid_pressure.validate());
    REQUIRE(invalid_pressure.serialize().empty());

    std::string tampered = serialized;
    const std::size_t sample = tampered.find("SAMPLE ");
    REQUIRE(sample != std::string::npos);
    const std::size_t digit = sample + 7U;
    tampered[digit] = 'g';
    REQUIRE(!production::SculptStack::deserialize(tampered));
}

void transaction_persists_stack_and_requires_expected_digest_for_updates() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "transaction-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, make_masked_plane(), mesh_asset);
    const geometry::EditableMesh& base = document.meshes().at(mesh_asset);
    const core::Revision mesh_revision = base.revision();
    const core::Revision project_revision = document.revision();
    production::SculptStack stack = make_stack(document, mesh_asset);
    const auto digest = stack.digest();
    REQUIRE(digest);

    auto transaction = project::ProjectTransaction::begin(
        document, journal, "sculpt-stack-tests");
    REQUIRE(transaction);
    REQUIRE(production::stage_sculpt_stack(transaction.value(), stack));
    REQUIRE(transaction.value().staged_document().meshes().at(mesh_asset).revision() == mesh_revision);
    REQUIRE(transaction.value().staged_document().sculpt_stacks().contains(mesh_asset));
    REQUIRE(transaction.value().staged_document().revision() > project_revision);
    REQUIRE(transaction.value().commit(
        production::sculpt_stack_transaction_operation(digest.value())));
    REQUIRE(document.meshes().at(mesh_asset).revision() == mesh_revision);

    const auto saved = document.save_atomic(directory.path() / "sculpt.carto");
    REQUIRE(saved);
    REQUIRE(saved.value().schema_version == 13U);
    const auto reopened = project::ProjectDocument::load(directory.path() / "sculpt.carto");
    REQUIRE(reopened);
    REQUIRE(reopened.value().sculpt_stacks().contains(mesh_asset));
    const auto& stored = reopened.value().sculpt_stacks().at(mesh_asset);
    REQUIRE(stored.digest == digest.value());
    const auto decoded = production::SculptStack::deserialize(stored.bytes);
    REQUIRE(decoded);
    REQUIRE(decoded.value().serialize() == stack.serialize());
    REQUIRE(production::compile_sculpt_stack(
        decoded.value(), reopened.value().meshes().at(mesh_asset)));

    std::string corrupt_project = reopened.value().serialize();
    const std::size_t stack_offset = corrupt_project.find(stored.bytes);
    REQUIRE(stack_offset != std::string::npos);
    const std::size_t sample_offset = stored.bytes.find("SAMPLE ");
    REQUIRE(sample_offset != std::string::npos);
    const std::size_t corrupt_offset = stack_offset + sample_offset + 7U;
    corrupt_project[corrupt_offset] = corrupt_project[corrupt_offset] == '0' ? '1' : '0';
    REQUIRE(!project::ProjectDocument::deserialize(corrupt_project));

    std::string mislabeled_v10 = reopened.value().serialize();
    constexpr std::string_view schema_v13 = "CARTOGRAPHER_PROJECT 13\n";
    constexpr std::string_view schema_v10 = "CARTOGRAPHER_PROJECT 10\n";
    REQUIRE(mislabeled_v10.starts_with(schema_v13));
    mislabeled_v10.replace(0U, schema_v13.size(), schema_v10);
    constexpr std::string_view cloth_receipt_table = "CLOTH_REST_POSE_RECEIPTS 0\n";
    const std::size_t cloth_receipts = mislabeled_v10.find(cloth_receipt_table);
    REQUIRE(cloth_receipts != std::string::npos);
    mislabeled_v10.erase(cloth_receipts, cloth_receipt_table.size());
    REQUIRE(!project::ProjectDocument::deserialize(mislabeled_v10));

    auto update_without_expected = stack;
    update_without_expected.layers.front().weight = 0.5;
    auto rejected_update = project::ProjectTransaction::begin(
        document, journal, "sculpt-stack-tests");
    REQUIRE(rejected_update);
    const auto missing_expected = production::stage_sculpt_stack(
        rejected_update.value(), update_without_expected);
    REQUIRE(!missing_expected);
    REQUIRE(missing_expected.error().code == core::ErrorCode::stale_data);
    REQUIRE(rejected_update.value().rollback());
    REQUIRE(document.sculpt_stacks().at(mesh_asset).digest == digest.value());

    auto update = project::ProjectTransaction::begin(document, journal, "sculpt-stack-tests");
    REQUIRE(update);
    REQUIRE(production::stage_sculpt_stack(
        update.value(), update_without_expected, digest.value()));
    const auto updated_digest = update_without_expected.digest();
    REQUIRE(updated_digest);
    REQUIRE(update.value().commit(
        production::sculpt_stack_transaction_operation(updated_digest.value())));
    const auto changed = production::compile_sculpt_stack(
        update_without_expected, document.meshes().at(mesh_asset));
    const auto previous = production::compile_sculpt_stack(
        stack, document.meshes().at(mesh_asset));
    REQUIRE(changed && previous);
    REQUIRE(changed.value().output_digest != previous.value().output_digest);
}

void brush_modes_round_trip_and_cpu_kernels_are_deterministic() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "brush-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_raised_pair(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const auto base_stack = production::make_sculpt_stack(document, mesh_asset);
    REQUIRE(base_stack);

    const production::SculptSurfaceAnchor source_anchor{
        geometry::FaceId{1U},
        {geometry::VertexId{1U}, geometry::VertexId{2U}, geometry::VertexId{5U}},
        {0.25, 0.25, 0.5}};
    const auto anchored_sample = production::make_sculpt_surface_sample(
        source, source_anchor, 0.8);
    REQUIRE(anchored_sample);
    REQUIRE(anchored_sample.value().position.x == -0.25);
    REQUIRE(anchored_sample.value().position.y == 0.5);
    REQUIRE(anchored_sample.value().position.z == 0.75);
    auto anchored_stack = base_stack.value();
    production::SculptLayer anchored_layer;
    anchored_layer.identity = "surface.layer";
    anchored_layer.name = "Surface anchored";
    production::SculptStroke anchored_stroke;
    anchored_stroke.identity = "surface.stroke";
    anchored_stroke.mode = production::SculptStrokeMode::inflate;
    anchored_stroke.radius = 1.0;
    anchored_stroke.strength = 0.1;
    anchored_stroke.samples.push_back(anchored_sample.value());
    anchored_layer.strokes.push_back(anchored_stroke);
    anchored_stack.layers.push_back(anchored_layer);
    const std::string anchored_serialized = anchored_stack.serialize();
    const auto anchored_decoded = production::SculptStack::deserialize(anchored_serialized);
    REQUIRE(anchored_decoded);
    REQUIRE(anchored_decoded.value().serialize() == anchored_serialized);
    REQUIRE(anchored_decoded.value().layers.front().strokes.front().samples.front() ==
        anchored_sample.value());
    const auto anchored_result = production::compile_sculpt_stack(anchored_stack, source);
    const auto anchored_result_again = production::compile_sculpt_stack(
        anchored_decoded.value(), source);
    REQUIRE(anchored_result && anchored_result_again);
    REQUIRE(anchored_result.value().artifact_digest ==
        anchored_result_again.value().artifact_digest);
    const auto anchored_digest = anchored_stack.digest();
    REQUIRE(anchored_digest);
    auto anchored_transaction = project::ProjectTransaction::begin(
        document, journal, "sculpt-stack-tests");
    REQUIRE(anchored_transaction);
    REQUIRE(production::stage_sculpt_stack(anchored_transaction.value(), anchored_stack));
    REQUIRE(anchored_transaction.value().commit(
        production::sculpt_stack_transaction_operation(anchored_digest.value())));
    const auto anchored_save = document.save_atomic(directory.path() / "surface-stack.carto");
    REQUIRE(anchored_save);
    const auto anchored_reopened = project::ProjectDocument::load(anchored_save.value().path);
    REQUIRE(anchored_reopened);
    const auto& persisted_stack = anchored_reopened.value().sculpt_stacks().at(mesh_asset);
    REQUIRE(persisted_stack.digest == anchored_digest.value());
    REQUIRE(persisted_stack.bytes == anchored_serialized);
    const auto persisted_decoded = production::SculptStack::deserialize(persisted_stack.bytes);
    REQUIRE(persisted_decoded);
    REQUIRE(persisted_decoded.value().layers.front().strokes.front().samples.front() ==
        anchored_sample.value());

    auto mismatched_surface_point = anchored_stack;
    mismatched_surface_point.layers.front().strokes.front().samples.front().position.x += 0.01;
    const auto mismatched_result = production::compile_sculpt_stack(
        mismatched_surface_point, source);
    REQUIRE(!mismatched_result);
    REQUIRE(mismatched_result.error().code == core::ErrorCode::validation_failed);

    auto stale_surface_identity = anchored_stack;
    stale_surface_identity.layers.front().strokes.front().samples.front().surface_anchor->face =
        geometry::FaceId{999U};
    const auto stale_anchor_result = production::compile_sculpt_stack(
        stale_surface_identity, source);
    REQUIRE(!stale_anchor_result);
    REQUIRE(stale_anchor_result.error().code == core::ErrorCode::stale_data);

    auto non_fan_anchor = anchored_stack;
    non_fan_anchor.layers.front().strokes.front().samples.front().surface_anchor->vertices = {
        geometry::VertexId{2U}, geometry::VertexId{5U}, geometry::VertexId{4U}};
    const auto invalid_anchor_result = production::compile_sculpt_stack(non_fan_anchor, source);
    REQUIRE(!invalid_anchor_result);
    REQUIRE(invalid_anchor_result.error().code == core::ErrorCode::validation_failed);

    const std::vector<production::SculptStrokeMode> modes{
        production::SculptStrokeMode::add,
        production::SculptStrokeMode::subtract,
        production::SculptStrokeMode::inflate,
        production::SculptStrokeMode::clay,
        production::SculptStrokeMode::grab,
        production::SculptStrokeMode::smooth,
        production::SculptStrokeMode::pinch,
        production::SculptStrokeMode::flatten,
        production::SculptStrokeMode::scrape,
        production::SculptStrokeMode::polish,
        production::SculptStrokeMode::crease,
        production::SculptStrokeMode::surface_noise,
    };
    for (const auto mode : modes) {
        auto stack = base_stack.value();
        production::SculptLayer layer;
        layer.identity = "brush.layer";
        layer.name = "Brush layer";
        production::SculptStroke stroke;
        stroke.identity = "brush.stroke";
        stroke.mode = mode;
        stroke.radius = 2.0;
        stroke.strength = 0.25;
        stroke.samples = {{{-0.25, 0.0, 0.0}, 0.75}, {{0.25, 0.0, 0.0}, 1.0}};
        layer.strokes.push_back(stroke);
        stack.layers.push_back(layer);
        REQUIRE(stack.validate());
        const std::string serialized = stack.serialize();
        const auto decoded = production::SculptStack::deserialize(serialized);
        REQUIRE(decoded);
        REQUIRE(decoded.value().serialize() == serialized);
        REQUIRE(decoded.value().layers.front().strokes.front().mode == mode);
    }

    auto legacy_stack = base_stack.value();
    legacy_stack.format_version = 1U;
    production::SculptLayer legacy_layer;
    legacy_layer.identity = "legacy.layer";
    legacy_layer.name = "Legacy brush";
    production::SculptStroke legacy_stroke;
    legacy_stroke.identity = "legacy.stroke";
    legacy_stroke.mode = production::SculptStrokeMode::add;
    legacy_stroke.samples = {{{0.0, 0.0, 1.0}, 1.0}};
    legacy_layer.strokes.push_back(legacy_stroke);
    legacy_stack.layers.push_back(legacy_layer);
    const std::string legacy_bytes = legacy_stack.serialize();
    REQUIRE(legacy_bytes.starts_with("CARTO_SCULPT_STACK 1\n"));
    const auto legacy_decoded = production::SculptStack::deserialize(legacy_bytes);
    REQUIRE(legacy_decoded);
    REQUIRE(legacy_decoded.value().format_version == 1U);
    REQUIRE(legacy_decoded.value().serialize() == legacy_bytes);

    auto version_two_stack = legacy_stack;
    version_two_stack.format_version = 2U;
    const std::string version_two_bytes = version_two_stack.serialize();
    REQUIRE(version_two_bytes.starts_with("CARTO_SCULPT_STACK 2\n"));
    const auto version_two_decoded = production::SculptStack::deserialize(version_two_bytes);
    REQUIRE(version_two_decoded);
    REQUIRE(version_two_decoded.value().format_version == 2U);
    REQUIRE(version_two_decoded.value().serialize() == version_two_bytes);

    auto version_three_stack = version_two_stack;
    version_three_stack.format_version = 3U;
    const std::string version_three_bytes = version_three_stack.serialize();
    REQUIRE(version_three_bytes.starts_with("CARTO_SCULPT_STACK 3\n"));
    const auto version_three_decoded = production::SculptStack::deserialize(version_three_bytes);
    REQUIRE(version_three_decoded);
    REQUIRE(version_three_decoded.value().format_version == 3U);
    REQUIRE(version_three_decoded.value().serialize() == version_three_bytes);

    legacy_stack.layers.front().strokes.front().mode =
        production::SculptStrokeMode::surface_noise;
    REQUIRE(legacy_stack.serialize().empty());

    auto malformed = base_stack.value();
    production::SculptLayer malformed_layer;
    malformed_layer.identity = "bad.layer";
    malformed_layer.name = "Bad layer";
    production::SculptStroke malformed_stroke;
    malformed_stroke.identity = "bad.stroke";
    malformed_stroke.samples = {{{0.0, 0.0, 0.0}, 1.0}};
    malformed_layer.strokes.push_back(malformed_stroke);
    malformed.layers.push_back(malformed_layer);
    std::string malformed_bytes = malformed.serialize();
    const std::size_t stroke_record = malformed_bytes.find("STROKE ");
    REQUIRE(stroke_record != std::string::npos);
    const std::size_t identity_end = malformed_bytes.find(' ', stroke_record + 7U);
    REQUIRE(identity_end != std::string::npos);
    const std::size_t mode_start = identity_end + 1U;
    const std::size_t mode_end = malformed_bytes.find(' ', mode_start);
    REQUIRE(mode_end != std::string::npos);
    malformed_bytes.replace(mode_start, mode_end - mode_start, "256");
    REQUIRE(!production::SculptStack::deserialize(malformed_bytes));

    auto missing_tangent_axis = version_two_stack;
    missing_tangent_axis.format_version = 3U;
    missing_tangent_axis.layers.front().strokes.front().direction_mode =
        production::SculptStrokeDirectionMode::surface_tangent;
    REQUIRE(!missing_tangent_axis.validate());

    auto zero_view_axis = missing_tangent_axis;
    zero_view_axis.layers.front().strokes.front().direction_mode =
        production::SculptStrokeDirectionMode::view_aligned;
    zero_view_axis.layers.front().strokes.front().samples.front().direction_axis =
        core::Vec3d{};
    REQUIRE(!zero_view_axis.validate());

    auto non_finite_view_axis = zero_view_axis;
    non_finite_view_axis.layers.front().strokes.front().samples.front().direction_axis =
        core::Vec3d{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
    REQUIRE(!non_finite_view_axis.validate());

    auto invalid_direction_enum = missing_tangent_axis;
    invalid_direction_enum.layers.front().strokes.front().direction_mode =
        static_cast<production::SculptStrokeDirectionMode>(255U);
    REQUIRE(!invalid_direction_enum.validate());

    auto unsupported_direction = missing_tangent_axis;
    unsupported_direction.layers.front().strokes.front().mode =
        production::SculptStrokeMode::smooth;
    unsupported_direction.layers.front().strokes.front().samples.front().direction_axis =
        core::Vec3d{1.0, 0.0, 0.0};
    REQUIRE(!unsupported_direction.validate());

    auto direction_in_old_version = missing_tangent_axis;
    direction_in_old_version.format_version = 2U;
    direction_in_old_version.layers.front().strokes.front().samples.front().direction_axis =
        core::Vec3d{1.0, 0.0, 0.0};
    REQUIRE(direction_in_old_version.serialize().empty());

    auto malformed_local_plane = version_two_stack;
    malformed_local_plane.format_version = 4U;
    malformed_local_plane.layers.front().strokes.front().symmetry =
        production::SculptSymmetryMode::mirror_plane;
    malformed_local_plane.layers.front().strokes.front().symmetry_origin_local =
        core::Vec3d{10.0, 0.0, 0.0};
    malformed_local_plane.layers.front().strokes.front().symmetry_axis_local = core::Vec3d{};
    REQUIRE(!malformed_local_plane.validate());

    auto invalid_radial_count = version_two_stack;
    invalid_radial_count.format_version = 4U;
    invalid_radial_count.layers.front().strokes.front().symmetry =
        production::SculptSymmetryMode::radial;
    invalid_radial_count.layers.front().strokes.front().symmetry_origin_local =
        core::Vec3d{10.0, 0.0, 0.0};
    invalid_radial_count.layers.front().strokes.front().symmetry_axis_local =
        core::Vec3d{0.0, 0.0, 1.0};
    invalid_radial_count.layers.front().strokes.front().radial_symmetry_count = 1U;
    REQUIRE(!invalid_radial_count.validate());

    auto excessive_radial_count = invalid_radial_count;
    excessive_radial_count.layers.front().strokes.front().radial_symmetry_count =
        production::kMaxSculptRadialSymmetryCopies + 1U;
    REQUIRE(!excessive_radial_count.validate());

    auto local_symmetry_in_v3 = invalid_radial_count;
    local_symmetry_in_v3.format_version = 3U;
    local_symmetry_in_v3.layers.front().strokes.front().radial_symmetry_count = 4U;
    REQUIRE(!local_symmetry_in_v3.validate());

    const auto find_output = [](const production::SculptEvaluationArtifact& artifact,
                                geometry::VertexId id) {
        const auto found = std::find_if(artifact.vertices.begin(), artifact.vertices.end(),
            [id](const geometry::Vertex& vertex) { return vertex.id == id; });
        REQUIRE(found != artifact.vertices.end());
        return found->position;
    };
    const auto compile_mode = [&](production::SculptStrokeMode mode,
                                  std::vector<production::SculptStrokeSample> samples,
                                  double radius,
                                  double strength,
                                  production::SculptSymmetryMode symmetry =
                                      production::SculptSymmetryMode::none,
                                  std::string mask_layer = {}) {
        auto stack = base_stack.value();
        production::SculptLayer layer;
        layer.identity = "compiled.layer";
        layer.name = "Compiled brush";
        production::SculptStroke stroke;
        stroke.identity = "compiled.stroke";
        stroke.mode = mode;
        stroke.symmetry = symmetry;
        stroke.radius = radius;
        stroke.strength = strength;
        stroke.mask_layer_id = std::move(mask_layer);
        stroke.samples = std::move(samples);
        layer.strokes.push_back(std::move(stroke));
        stack.layers.push_back(std::move(layer));
        return production::compile_sculpt_stack(stack, source);
    };

    TempDirectory direction_directory;
    journal::Journal direction_journal(direction_directory.path() / "direction-journal.log");
    std::uint64_t direction_mesh_asset = 0U;
    project::ProjectDocument direction_document = make_project(
        direction_journal, make_masked_plane(), direction_mesh_asset);
    const geometry::EditableMesh& direction_source =
        direction_document.meshes().at(direction_mesh_asset);
    const auto direction_base = production::make_sculpt_stack(
        direction_document, direction_mesh_asset);
    REQUIRE(direction_base);
    const auto compile_direction = [&](production::SculptStrokeDirectionMode direction_mode,
                                       core::Vec3d axis,
                                       production::SculptSymmetryMode symmetry =
                                           production::SculptSymmetryMode::none,
                                       core::Vec3d center = core::Vec3d{0.0, 0.0, 0.0}) {
        auto stack = direction_base.value();
        production::SculptLayer layer;
        layer.identity = "direction.layer";
        layer.name = "Captured direction";
        production::SculptStroke stroke;
        stroke.identity = "direction.stroke";
        stroke.mode = production::SculptStrokeMode::add;
        stroke.symmetry = symmetry;
        stroke.direction_mode = direction_mode;
        stroke.radius = 2.0;
        stroke.strength = 0.25;
        stroke.samples = {{center, 1.0}};
        if (direction_mode != production::SculptStrokeDirectionMode::surface_normal) {
            stroke.samples.front().direction_axis = axis;
        }
        layer.strokes.push_back(std::move(stroke));
        stack.layers.push_back(std::move(layer));
        return std::pair{stack, production::compile_sculpt_stack(stack, direction_source)};
    };

    const auto [normal_direction_stack, normal_direction] = compile_direction(
        production::SculptStrokeDirectionMode::surface_normal, {});
    const auto [tangent_direction_stack, tangent_direction] = compile_direction(
        production::SculptStrokeDirectionMode::surface_tangent, {1.0, 0.0, 0.0});
    const auto [view_direction_stack, view_direction] = compile_direction(
        production::SculptStrokeDirectionMode::view_aligned, {0.0, 1.0, 0.0});
    REQUIRE(normal_direction && tangent_direction && view_direction);
    REQUIRE(find_output(normal_direction.value(), geometry::VertexId{2U}).z > 0.0);
    REQUIRE(find_output(tangent_direction.value(), geometry::VertexId{2U}).x > 0.0);
    REQUIRE(find_output(view_direction.value(), geometry::VertexId{2U}).y > 0.0);
    REQUIRE(normal_direction.value().output_digest != tangent_direction.value().output_digest);
    REQUIRE(tangent_direction.value().output_digest != view_direction.value().output_digest);
    const std::string tangent_bytes = tangent_direction_stack.serialize();
    REQUIRE(tangent_bytes.starts_with("CARTO_SCULPT_STACK 4\n"));
    auto version_three_tangent_stack = tangent_direction_stack;
    version_three_tangent_stack.format_version = 3U;
    const std::string version_three_tangent_bytes =
        version_three_tangent_stack.serialize();
    REQUIRE(version_three_tangent_bytes.starts_with("CARTO_SCULPT_STACK 3\n"));
    const auto version_three_tangent_decoded =
        production::SculptStack::deserialize(version_three_tangent_bytes);
    REQUIRE(version_three_tangent_decoded);
    REQUIRE(version_three_tangent_decoded.value().serialize() ==
        version_three_tangent_bytes);
    const auto version_three_tangent_result = production::compile_sculpt_stack(
        version_three_tangent_decoded.value(), direction_source);
    REQUIRE(version_three_tangent_result);
    REQUIRE(version_three_tangent_result.value().output_digest ==
        tangent_direction.value().output_digest);
    std::string invalid_direction_bytes = tangent_bytes;
    std::size_t direction_record = invalid_direction_bytes.find("STROKE ");
    REQUIRE(direction_record != std::string::npos);
    direction_record += 7U;
    const std::size_t direction_identity_end =
        invalid_direction_bytes.find(' ', direction_record);
    REQUIRE(direction_identity_end != std::string::npos);
    const std::size_t direction_mode_end =
        invalid_direction_bytes.find(' ', direction_identity_end + 1U);
    REQUIRE(direction_mode_end != std::string::npos);
    const std::size_t direction_symmetry_end =
        invalid_direction_bytes.find(' ', direction_mode_end + 1U);
    REQUIRE(direction_symmetry_end != std::string::npos);
    const std::size_t direction_start = direction_symmetry_end + 1U;
    const std::size_t direction_end = invalid_direction_bytes.find(' ', direction_start);
    REQUIRE(direction_end != std::string::npos);
    invalid_direction_bytes.replace(
        direction_start, direction_end - direction_start, "255");
    REQUIRE(!production::SculptStack::deserialize(invalid_direction_bytes));
    const auto tangent_decoded = production::SculptStack::deserialize(tangent_bytes);
    REQUIRE(tangent_decoded);
    REQUIRE(tangent_decoded.value().serialize() == tangent_bytes);
    const auto& decoded_axis =
        tangent_decoded.value().layers.front().strokes.front().samples.front().direction_axis;
    REQUIRE(decoded_axis.has_value());
    REQUIRE(decoded_axis->x == 1.0 && decoded_axis->y == 0.0 && decoded_axis->z == 0.0);
    const auto tangent_rebuilt = production::compile_sculpt_stack(
        tangent_decoded.value(), direction_source);
    REQUIRE(tangent_rebuilt);
    REQUIRE(tangent_rebuilt.value().artifact_digest == tangent_direction.value().artifact_digest);

    const auto singular_tangent = compile_direction(
        production::SculptStrokeDirectionMode::surface_tangent, {0.0, 0.0, 1.0});
    REQUIRE(singular_tangent.second);
    REQUIRE(singular_tangent.second.value().maximum_displacement == 0.0);

    const auto mirrored_view = compile_direction(
        production::SculptStrokeDirectionMode::view_aligned, {1.0, 0.0, 0.0},
        production::SculptSymmetryMode::mirror_x, {-0.5, 0.0, 0.0});
    REQUIRE(mirrored_view.second);
    REQUIRE(find_output(mirrored_view.second.value(), geometry::VertexId{1U}).x > -1.0);
    REQUIRE(find_output(mirrored_view.second.value(), geometry::VertexId{3U}).x < 1.0);

    TempDirectory symmetry_directory;
    journal::Journal symmetry_journal(symmetry_directory.path() / "symmetry-journal.log");
    std::uint64_t symmetry_mesh_asset = 0U;
    project::ProjectDocument symmetry_document = make_project(
        symmetry_journal, make_local_radial_fixture(), symmetry_mesh_asset);
    const geometry::EditableMesh& symmetry_source =
        symmetry_document.meshes().at(symmetry_mesh_asset);
    const auto symmetry_base = production::make_sculpt_stack(
        symmetry_document, symmetry_mesh_asset);
    REQUIRE(symmetry_base);
    const auto compile_local_symmetry = [&](production::SculptSymmetryMode mode,
                                            core::Vec3d axis,
                                            std::uint32_t radial_count,
                                            std::optional<core::Vec3d> direction_axis =
                                                std::nullopt) {
        auto stack = symmetry_base.value();
        production::SculptLayer layer;
        layer.identity = "local-symmetry.layer";
        layer.name = "Local symmetry";
        production::SculptStroke stroke;
        stroke.identity = "local-symmetry.stroke";
        stroke.mode = production::SculptStrokeMode::add;
        stroke.symmetry = mode;
        stroke.direction_mode = direction_axis.has_value()
            ? production::SculptStrokeDirectionMode::view_aligned
            : production::SculptStrokeDirectionMode::surface_normal;
        stroke.symmetry_origin_local = {10.0, 0.0, 0.0};
        stroke.symmetry_axis_local = axis;
        stroke.radial_symmetry_count = radial_count;
        stroke.radius = 0.2;
        stroke.strength = 0.25;
        stroke.samples = {{{11.0, 0.0, 0.0}, 1.0}};
        stroke.samples.front().direction_axis = direction_axis;
        layer.strokes.push_back(std::move(stroke));
        stack.layers.push_back(std::move(layer));
        return std::pair{stack, production::compile_sculpt_stack(stack, symmetry_source)};
    };

    const auto local_plane = compile_local_symmetry(
        production::SculptSymmetryMode::mirror_plane, {5.0, 0.0, 0.0}, 4U);
    REQUIRE(local_plane.second);
    REQUIRE(find_output(local_plane.second.value(), geometry::VertexId{1U}).z > 0.0);
    REQUIRE(std::abs(
        find_output(local_plane.second.value(), geometry::VertexId{1U}).z -
        find_output(local_plane.second.value(), geometry::VertexId{3U}).z) < 1e-12);

    auto overflowing_local_plane = local_plane.first;
    overflowing_local_plane.layers.front().strokes.front().symmetry_origin_local =
        {-1.0e9, 0.0, 0.0};
    overflowing_local_plane.layers.front().strokes.front().samples.front().position =
        {1.0e9, 0.0, 0.0};
    const auto rejected_local_plane = production::compile_sculpt_stack(
        overflowing_local_plane, symmetry_source);
    REQUIRE(!rejected_local_plane);
    REQUIRE(rejected_local_plane.error().code == core::ErrorCode::validation_failed);
    REQUIRE(symmetry_source.find_vertex(geometry::VertexId{1U})->position.x == 11.0);

    const auto directional_local_plane = compile_local_symmetry(
        production::SculptSymmetryMode::mirror_plane, {1.0, 0.0, 0.0}, 4U,
        core::Vec3d{1.0, 0.0, 0.0});
    REQUIRE(directional_local_plane.second);
    REQUIRE(find_output(directional_local_plane.second.value(),
        geometry::VertexId{1U}).x > 11.0);
    REQUIRE(find_output(directional_local_plane.second.value(),
        geometry::VertexId{3U}).x < 9.0);

    const auto local_radial = compile_local_symmetry(
        production::SculptSymmetryMode::radial, {0.0, 0.0, 5.0}, 4U);
    REQUIRE(local_radial.second);
    const double radial_height =
        find_output(local_radial.second.value(), geometry::VertexId{1U}).z;
    REQUIRE(radial_height > 0.0);
    for (std::uint64_t id = 2U; id <= 4U; ++id) {
        REQUIRE(std::abs(find_output(local_radial.second.value(),
            geometry::VertexId{id}).z - radial_height) < 1e-12);
    }
    const auto directional_local_radial = compile_local_symmetry(
        production::SculptSymmetryMode::radial, {0.0, 0.0, 1.0}, 4U,
        core::Vec3d{1.0, 0.0, 0.0});
    REQUIRE(directional_local_radial.second);
    REQUIRE(find_output(directional_local_radial.second.value(),
        geometry::VertexId{1U}).x > 11.0);
    REQUIRE(find_output(directional_local_radial.second.value(),
        geometry::VertexId{2U}).y > 1.0);
    REQUIRE(find_output(directional_local_radial.second.value(),
        geometry::VertexId{3U}).x < 9.0);
    REQUIRE(find_output(directional_local_radial.second.value(),
        geometry::VertexId{4U}).y < -1.0);
    const auto local_radial_again = production::compile_sculpt_stack(
        local_radial.first, symmetry_source);
    REQUIRE(local_radial_again);
    REQUIRE(local_radial_again.value().artifact_digest ==
        local_radial.second.value().artifact_digest);
    const std::string radial_bytes = local_radial.first.serialize();
    REQUIRE(radial_bytes.starts_with("CARTO_SCULPT_STACK 4\n"));
    const auto radial_decoded = production::SculptStack::deserialize(radial_bytes);
    REQUIRE(radial_decoded);
    REQUIRE(radial_decoded.value().serialize() == radial_bytes);
    const auto radial_rebuilt = production::compile_sculpt_stack(
        radial_decoded.value(), symmetry_source);
    REQUIRE(radial_rebuilt);
    REQUIRE(radial_rebuilt.value().artifact_digest ==
        local_radial.second.value().artifact_digest);

    const auto symmetry_digest = local_radial.first.digest();
    REQUIRE(symmetry_digest);
    auto symmetry_transaction = project::ProjectTransaction::begin(
        symmetry_document, symmetry_journal, "sculpt-symmetry-tests");
    REQUIRE(symmetry_transaction);
    REQUIRE(production::stage_sculpt_stack(
        symmetry_transaction.value(), local_radial.first));
    REQUIRE(symmetry_transaction.value().commit(
        production::sculpt_stack_transaction_operation(symmetry_digest.value())));
    const auto symmetry_save = symmetry_document.save_atomic(
        symmetry_directory.path() / "local-symmetry.carto");
    REQUIRE(symmetry_save);
    const auto symmetry_reopened = project::ProjectDocument::load(symmetry_save.value().path);
    REQUIRE(symmetry_reopened);
    const auto& persisted_symmetry_stack =
        symmetry_reopened.value().sculpt_stacks().at(symmetry_mesh_asset);
    REQUIRE(persisted_symmetry_stack.digest == symmetry_digest.value());
    REQUIRE(persisted_symmetry_stack.bytes == radial_bytes);

    TempDirectory symmetry_budget_directory;
    journal::Journal symmetry_budget_journal(
        symmetry_budget_directory.path() / "symmetry-budget-journal.log");
    std::uint64_t symmetry_budget_mesh_asset = 0U;
    project::ProjectDocument symmetry_budget_document = make_project(
        symmetry_budget_journal, make_radial_budget_fixture(), symmetry_budget_mesh_asset);
    const auto symmetry_budget_source =
        symmetry_budget_document.meshes().at(symmetry_budget_mesh_asset);
    auto symmetry_budget_stack = production::make_sculpt_stack(
        symmetry_budget_document, symmetry_budget_mesh_asset);
    REQUIRE(symmetry_budget_stack);
    production::SculptLayer symmetry_budget_layer;
    symmetry_budget_layer.identity = "radial-budget.layer";
    symmetry_budget_layer.name = "Radial budget";
    for (std::uint32_t stroke_index = 0U; stroke_index < 32U; ++stroke_index) {
        production::SculptStroke stroke;
        stroke.identity = "radial-budget.stroke." + std::to_string(stroke_index);
        stroke.mode = production::SculptStrokeMode::add;
        stroke.symmetry = production::SculptSymmetryMode::radial;
        stroke.symmetry_origin_local = {};
        stroke.symmetry_axis_local = {0.0, 0.0, 1.0};
        stroke.radial_symmetry_count = production::kMaxSculptRadialSymmetryCopies;
        stroke.samples.assign(production::kMaxSculptSamplesPerStroke,
            production::SculptStrokeSample{{1.0, 0.0, 0.0}, 1.0});
        symmetry_budget_layer.strokes.push_back(std::move(stroke));
    }
    symmetry_budget_stack.value().layers.push_back(std::move(symmetry_budget_layer));
    REQUIRE(symmetry_budget_stack.value().validate());
    const auto over_budget = production::compile_sculpt_stack(
        symmetry_budget_stack.value(), symmetry_budget_source);
    REQUIRE(!over_budget);
    REQUIRE(over_budget.error().code == core::ErrorCode::unsupported);

    const auto direction_digest = tangent_direction_stack.digest();
    REQUIRE(direction_digest);
    auto direction_transaction = project::ProjectTransaction::begin(
        direction_document, direction_journal, "sculpt-direction-tests");
    REQUIRE(direction_transaction);
    REQUIRE(production::stage_sculpt_stack(
        direction_transaction.value(), tangent_direction_stack));
    REQUIRE(direction_transaction.value().commit(
        production::sculpt_stack_transaction_operation(direction_digest.value())));
    const auto direction_save = direction_document.save_atomic(
        direction_directory.path() / "direction-stack.carto");
    REQUIRE(direction_save);
    const auto direction_reopened = project::ProjectDocument::load(direction_save.value().path);
    REQUIRE(direction_reopened);
    const auto& persisted_direction_stack =
        direction_reopened.value().sculpt_stacks().at(direction_mesh_asset);
    REQUIRE(persisted_direction_stack.digest == direction_digest.value());
    REQUIRE(persisted_direction_stack.bytes == tangent_bytes);
    const auto persisted_direction =
        production::SculptStack::deserialize(persisted_direction_stack.bytes);
    REQUIRE(persisted_direction);
    const auto& persisted_axis = persisted_direction.value().layers.front().strokes.front()
        .samples.front().direction_axis;
    REQUIRE(persisted_axis.has_value());
    REQUIRE(persisted_axis->x == 1.0 && persisted_axis->y == 0.0 && persisted_axis->z == 0.0);

    const auto smooth = compile_mode(production::SculptStrokeMode::smooth,
        {{{0.0, 0.0, 1.0}, 1.0}}, 0.75, 1.0);
    REQUIRE(smooth);
    REQUIRE(std::abs(find_output(smooth.value(), geometry::VertexId{2U}).z - 1.0 / 3.0) < 1e-12);
    const auto smooth_again = compile_mode(production::SculptStrokeMode::smooth,
        {{{0.0, 0.0, 1.0}, 1.0}}, 0.75, 1.0);
    REQUIRE(smooth_again);
    REQUIRE(smooth.value().artifact_digest == smooth_again.value().artifact_digest);

    const auto grab = compile_mode(production::SculptStrokeMode::grab,
        {{{0.0, 0.0, 1.0}, 1.0}, {{0.25, 0.0, 1.0}, 1.0}}, 2.0, 1.0);
    REQUIRE(grab);
    REQUIRE(find_output(grab.value(), geometry::VertexId{2U}).x > 0.0);

    const auto pinch = compile_mode(production::SculptStrokeMode::pinch,
        {{{0.0, 0.5, 0.0}, 1.0}}, 2.0, 0.2);
    REQUIRE(pinch);
    REQUIRE(find_output(pinch.value(), geometry::VertexId{1U}).x > -1.0);

    const auto flatten = compile_mode(production::SculptStrokeMode::flatten,
        {{{-1.0, 0.0, 0.0}, 1.0}}, 3.0, 1.0);
    REQUIRE(flatten);
    REQUIRE(flatten.value().maximum_displacement > 0.0);

    const auto clay = compile_mode(production::SculptStrokeMode::clay,
        {{{0.0, 0.0, 1.0}, 1.0}}, 1.0, 0.2);
    REQUIRE(clay);
    REQUIRE(clay.value().maximum_displacement > 0.0);

    TempDirectory scrape_directory;
    journal::Journal scrape_journal(scrape_directory.path() / "scrape-journal.log");
    std::uint64_t scrape_mesh_asset = 0U;
    project::ProjectDocument scrape_document = make_project(
        scrape_journal, make_scrape_fixture(), scrape_mesh_asset);
    const auto scrape_source = scrape_document.meshes().at(scrape_mesh_asset);
    auto scrape_stack = production::make_sculpt_stack(scrape_document, scrape_mesh_asset);
    REQUIRE(scrape_stack);
    production::SculptLayer scrape_layer;
    scrape_layer.identity = "scrape.layer";
    scrape_layer.name = "Scrape";
    production::SculptStroke scrape_stroke;
    scrape_stroke.identity = "scrape.stroke";
    scrape_stroke.mode = production::SculptStrokeMode::scrape;
    scrape_stroke.radius = 4.0;
    scrape_stroke.strength = 1.0;
    scrape_stroke.samples = {{{-1.0, -1.0, 0.0}, 1.0}};
    scrape_layer.strokes.push_back(std::move(scrape_stroke));
    scrape_stack.value().layers.push_back(std::move(scrape_layer));
    const auto scrape = production::compile_sculpt_stack(scrape_stack.value(), scrape_source);
    REQUIRE(scrape);
    REQUIRE(scrape.value().maximum_displacement > 0.0);
    REQUIRE(find_output(scrape.value(), geometry::VertexId{5U}).z < 1.0);

    const auto polish = compile_mode(production::SculptStrokeMode::polish,
        {{{0.0, 0.0, 1.0}, 1.0}}, 0.75, 1.0);
    REQUIRE(polish);
    REQUIRE(polish.value().maximum_displacement > 0.0);

    const auto crease = compile_mode(production::SculptStrokeMode::crease,
        {{{0.0, 0.0, 1.0}, 1.0}}, 1.0, 0.2);
    REQUIRE(crease);
    REQUIRE(crease.value().maximum_displacement > 0.0);

    const auto inflate = compile_mode(production::SculptStrokeMode::inflate,
        {{{0.0, 0.0, 1.0}, 1.0}}, 1.0, 0.2);
    REQUIRE(inflate);
    REQUIRE(inflate.value().maximum_displacement > 0.0);

    const auto surface_noise = compile_mode(production::SculptStrokeMode::surface_noise,
        {{{0.0, 0.0, 1.0}, 1.0}}, 2.0, 0.1, production::SculptSymmetryMode::none,
        "mask.protect");
    const auto surface_noise_again = compile_mode(
        production::SculptStrokeMode::surface_noise,
        {{{0.0, 0.0, 1.0}, 1.0}}, 2.0, 0.1,
        production::SculptSymmetryMode::none, "mask.protect");
    REQUIRE(surface_noise && surface_noise_again);
    REQUIRE(surface_noise.value().artifact_digest == surface_noise_again.value().artifact_digest);
    REQUIRE(surface_noise.value().maximum_displacement > 0.0);
    REQUIRE(find_output(surface_noise.value(), geometry::VertexId{3U}).x == 1.0);

    const auto mirrored_grab = compile_mode(production::SculptStrokeMode::grab,
        {{{-0.5, 0.0, 0.0}, 1.0}, {{-0.25, 0.0, 0.0}, 1.0}}, 2.0, 0.5,
        production::SculptSymmetryMode::mirror_x, "mask.protect");
    REQUIRE(mirrored_grab);
    REQUIRE(find_output(mirrored_grab.value(), geometry::VertexId{1U}).x > -1.0);
    REQUIRE(find_output(mirrored_grab.value(), geometry::VertexId{3U}).x == 1.0);
}

} // namespace

int main() {
    try {
        stack_serialization_and_evaluation_are_deterministic();
        std::cout << "PASS deterministic SculptStack serialization, symmetry, masks, and stale-source rejection\n";
        transaction_persists_stack_and_requires_expected_digest_for_updates();
        std::cout << "PASS transactional SculptStack persistence and compare-and-swap update boundary\n";
        brush_modes_round_trip_and_cpu_kernels_are_deterministic();
        std::cout << "PASS deterministic CPU brush modes and hostile serialized mode rejection\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL SculptStack tests: " << error.what() << '\n';
        return 1;
    }
}
