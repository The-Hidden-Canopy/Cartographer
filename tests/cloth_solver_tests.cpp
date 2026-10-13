#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/cloth_solver.hpp>
#include <carto/production/garment_asset.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
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
            ("cartographer-cloth-tests-" + std::to_string(stamp) + "-" +
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

carto::geometry::EditableMesh make_mesh() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {2.0, 0.0, 0.0}},
         {geometry::VertexId{4U}, {0.0, 1.0, 0.0}},
         {geometry::VertexId{5U}, {1.0, 1.0, 0.0}},
         {geometry::VertexId{6U}, {2.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{5U}, geometry::VertexId{4U}}},
         {geometry::FaceId{2U}, {geometry::VertexId{2U}, geometry::VertexId{3U},
             geometry::VertexId{6U}, geometry::VertexId{5U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_mesh_with_uvs() {
    using namespace carto;
    geometry::EditableMesh mesh = make_mesh();
    attributes::AttributeSet attributes({{attributes::AttributeDomain::corner, 8U}});
    REQUIRE(attributes.add_layer(attributes::AttributeLayer{
        {"uv0.corners", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
        {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0},
         core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 1.0},
         core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0},
         core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 1.0}}}));
    attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add(attributes::UvSetDescriptor{
        1U, "UV0", "uv0.corners", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_coincident_panels(double x_offset = 0.0) {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {x_offset, 0.0, 0.0}},
         {geometry::VertexId{2U}, {x_offset + 1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {x_offset + 1.0, 1.0, 0.0}},
         {geometry::VertexId{4U}, {x_offset, 1.0, 0.0}},
         {geometry::VertexId{5U}, {x_offset, 0.0, 0.0}},
         {geometry::VertexId{6U}, {x_offset + 1.0, 0.0, 0.0}},
         {geometry::VertexId{7U}, {x_offset + 1.0, 1.0, 0.0}},
         {geometry::VertexId{8U}, {x_offset, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}, geometry::VertexId{4U}}},
         {geometry::FaceId{2U}, {geometry::VertexId{5U}, geometry::VertexId{6U},
             geometry::VertexId{7U}, geometry::VertexId{8U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_vertex_face_overlap() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {-1.0, 0.0, -1.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, -1.0}},
         {geometry::VertexId{3U}, {0.0, 0.0, 1.0}},
         {geometry::VertexId{4U}, {0.0, 0.0005, 0.0}},
         {geometry::VertexId{5U}, {2.0, 1.0, 0.0}},
         {geometry::VertexId{6U}, {0.0, 2.0, 2.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{2U}, {geometry::VertexId{4U}, geometry::VertexId{5U},
             geometry::VertexId{6U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_edge_edge_crossing() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {-1.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 2.0, 0.0}},
         {geometry::VertexId{4U}, {0.0, -1.0, -1.0}},
         {geometry::VertexId{5U}, {0.0, 1.0, 1.0}},
         {geometry::VertexId{6U}, {2.0, 0.0, 2.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{2U}, {geometry::VertexId{4U}, geometry::VertexId{5U},
             geometry::VertexId{6U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_seam_adjacent_panels() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 1.0, 0.0}},
         {geometry::VertexId{4U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{5U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{6U}, {0.0, -1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{2U}, {geometry::VertexId{4U}, geometry::VertexId{5U},
             geometry::VertexId{6U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_dense_self_collision_cell() {
    using namespace carto;
    geometry::EditableMesh mesh;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    for (std::uint64_t triangle = 0U; triangle < 22U; ++triangle) {
        const std::uint64_t base = triangle * 3U + 1U;
        vertices.push_back({geometry::VertexId{base}, {0.0, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{base + 1U}, {0.0005, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{base + 2U}, {0.0, 0.0005, 0.0}});
        faces.push_back({geometry::FaceId{triangle + 1U}, {
            geometry::VertexId{base}, geometry::VertexId{base + 1U},
            geometry::VertexId{base + 2U}}});
    }
    REQUIRE(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_frame_sample_limit_mesh() {
    using namespace carto;
    constexpr std::uint64_t width = 42U;
    constexpr std::uint64_t height = 40U;
    geometry::EditableMesh mesh;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    vertices.reserve(static_cast<std::size_t>(width * height));
    faces.reserve(static_cast<std::size_t>((width - 1U) * (height - 1U)));
    for (std::uint64_t y = 0U; y < height; ++y) {
        for (std::uint64_t x = 0U; x < width; ++x) {
            const std::uint64_t id = y * width + x + 1U;
            vertices.push_back({geometry::VertexId{id},
                {static_cast<double>(x), static_cast<double>(y), 0.0}});
        }
    }
    std::uint64_t face_id = 1U;
    for (std::uint64_t y = 0U; y + 1U < height; ++y) {
        for (std::uint64_t x = 0U; x + 1U < width; ++x) {
            const std::uint64_t lower_left = y * width + x + 1U;
            const std::uint64_t lower_right = lower_left + 1U;
            const std::uint64_t upper_left = lower_left + width;
            const std::uint64_t upper_right = upper_left + 1U;
            faces.push_back({geometry::FaceId{face_id++}, {
                geometry::VertexId{lower_left}, geometry::VertexId{lower_right},
                geometry::VertexId{upper_right}, geometry::VertexId{upper_left}}});
        }
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
    auto document = project::ProjectDocument::create("CPU Cloth Test");
    REQUIRE(document);
    const std::string snapshot = document.value().serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(snapshot.data());
    REQUIRE(journal.append(journal::JournalAppend{
        core::Revision{}, document.value().revision(), "project.create",
        std::span<const std::uint8_t>(bytes, snapshot.size()), std::nullopt}));
    auto transaction = project::ProjectTransaction::begin(
        document.value(), journal, "cloth-tests");
    REQUIRE(transaction);
    const auto added = transaction.value().add_mesh(std::move(mesh));
    REQUIRE(added);
    mesh_asset = added.value();
    REQUIRE(transaction.value().commit("mesh.fixture"));
    return std::move(document).value();
}

carto::production::GarmentAsset make_garment(
    const carto::project::ProjectDocument& document,
    std::uint64_t mesh_asset) {
    using namespace carto;
    auto created = production::make_garment_asset(document, mesh_asset, "cloth.reference");
    REQUIRE(created);
    production::GarmentAsset garment = std::move(created).value();
    garment.panels = {
        {"panel.left", {geometry::FaceId{1U}}},
        {"panel.right", {geometry::FaceId{2U}}}};
    garment.collision_capsules.push_back(production::GarmentCollisionCapsule{
        "body.torso", {-1.0, -0.25, 0.0}, {3.0, -0.25, 0.0}, 0.2});
    garment.pins.push_back(production::GarmentPin{
        "pin.origin", geometry::VertexId{1U}, {0.0, 0.0, 0.0}, 0.0});
    REQUIRE(garment.validate(document.meshes().at(mesh_asset)));
    return garment;
}

void cpu_reference_is_deterministic_bounded_and_failure_closed() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "cloth-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, make_mesh(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const core::Revision source_revision = source.revision();
    const production::GarmentAsset garment = make_garment(document, mesh_asset);
    production::ClothSolverSettings settings;
    settings.frame_count = 12U;
    settings.projection_iterations = 64U;
    const auto first = production::compile_cloth_cpu(garment, source, settings);
    const auto second = production::compile_cloth_cpu(garment, source, settings);
    if (!first) throw std::runtime_error(first.error().message);
    if (!second) throw std::runtime_error(second.error().message);
    REQUIRE(first.value().algorithm == "carto.cloth.cpu.xpbd-reference.v5");
    REQUIRE(first.value().canonical() == second.value().canonical());
    REQUIRE(first.value().artifact_digest == second.value().artifact_digest);
    REQUIRE(first.value().completed_substeps == 24U);
    REQUIRE(first.value().maximum_collision_correction > 0.0);
    REQUIRE(first.value().maximum_constraint_error <= 0.05);
    REQUIRE(first.value().frames.size() == settings.frame_count);
    REQUIRE(first.value().validate(garment, source));
    REQUIRE(source.revision() == source_revision);
    for (std::size_t frame_index = 0U; frame_index < first.value().frames.size(); ++frame_index) {
        const auto& frame = first.value().frames[frame_index];
        REQUIRE(frame.index == frame_index);
        REQUIRE(frame.vertices.size() == source.vertex_count());
        for (std::size_t i = 0U; i < frame.vertices.size(); ++i) {
            REQUIRE(frame.vertices[i].position.finite());
            REQUIRE(frame.vertices[i].position.y >= -0.051001);
            if (i > 0U) REQUIRE(frame.vertices[i - 1U].id < frame.vertices[i].id);
            if (frame_index > 0U) {
                REQUIRE(frame.vertices[i].id == first.value().frames[0].vertices[i].id);
            }
        }
    }
    const auto pinned = std::find_if(first.value().frames.back().vertices.begin(),
        first.value().frames.back().vertices.end(),
        [](const geometry::Vertex& vertex) { return vertex.id == geometry::VertexId{1U}; });
    REQUIRE(pinned != first.value().frames.back().vertices.end());
    REQUIRE(std::abs(pinned->position.x) < 1e-10);
    REQUIRE(std::abs(pinned->position.y) < 1e-10);
    REQUIRE(std::any_of(first.value().frames.front().vertices.begin(),
        first.value().frames.front().vertices.end(), [&](const geometry::Vertex& vertex) {
            if (vertex.id == geometry::VertexId{1U}) return false;
            const auto last = std::find_if(first.value().frames.back().vertices.begin(),
                first.value().frames.back().vertices.end(), [&](const geometry::Vertex& value) {
                    return value.id == vertex.id;
                });
            return last != first.value().frames.back().vertices.end() &&
                (last->position - vertex.position).length() > 1e-6;
        }));

    auto changed_garment = garment;
    changed_garment.fabric.damping = 0.3;
    REQUIRE(!first.value().validate(changed_garment, source));
    auto tampered = first.value();
    tampered.frames[5].vertices.back().position.x += 0.1;
    REQUIRE(tampered.refresh_digests());
    REQUIRE(!tampered.validate(garment, source));
    tampered = first.value();
    tampered.artifact_digest.bytes[0] ^= 0x40U;
    REQUIRE(!tampered.validate(garment, source));
    auto mismatched_sequence = first.value();
    mismatched_sequence.settings.frame_count = production::kMaxClothFrames;
    mismatched_sequence.settings.substeps_per_frame = production::kMaxClothSubsteps;
    mismatched_sequence.settings.projection_iterations = production::kMaxClothIterations;
    const auto sequence_failure = mismatched_sequence.validate(garment, source);
    REQUIRE(!sequence_failure);
    REQUIRE(sequence_failure.error().message.find("frame count differs") != std::string::npos);

    auto conflicting_pin = garment;
    conflicting_pin.collision_capsules.front() = {
        "body.torso", {-0.1, 0.0, 0.0}, {0.1, 0.0, 0.0}, 0.2};
    REQUIRE(!production::compile_cloth_cpu(conflicting_pin, source, settings));
    auto invalid_settings = settings;
    invalid_settings.frame_count = 0U;
    REQUIRE(!production::compile_cloth_cpu(garment, source, invalid_settings));

    const auto serialized = first.value().serialize();
    REQUIRE(serialized);
    const auto restored = production::ClothSimulationArtifact::deserialize(
        std::span<const std::uint8_t>{serialized.value()});
    REQUIRE(restored);
    REQUIRE(restored.value().canonical() == first.value().canonical());
    REQUIRE(restored.value().frames.size() == settings.frame_count);
    const auto rejects_serialized = [](const std::string& malformed) {
        const std::vector<std::uint8_t> bytes(malformed.begin(), malformed.end());
        return !production::ClothSimulationArtifact::deserialize(
            std::span<const std::uint8_t>{bytes});
    };
    std::string wrong_frame_count = first.value().canonical();
    const std::size_t frame_count_marker = wrong_frame_count.rfind("FRAMES 12\n");
    REQUIRE(frame_count_marker != std::string::npos);
    wrong_frame_count.replace(frame_count_marker, 9U, "FRAMES 13\n");
    REQUIRE(rejects_serialized(wrong_frame_count));
    std::string overlarge_frame = first.value().canonical();
    const std::size_t frame_marker = overlarge_frame.find("FRAME 0 VERTICES 6 MOVEMENT ");
    REQUIRE(frame_marker != std::string::npos);
    const std::size_t vertex_count_start = frame_marker + std::string("FRAME 0 VERTICES ").size();
    const std::size_t movement_marker = overlarge_frame.find(" MOVEMENT ", vertex_count_start);
    REQUIRE(movement_marker != std::string::npos);
    overlarge_frame.replace(vertex_count_start, movement_marker - vertex_count_start, "100001");
    REQUIRE(rejects_serialized(overlarge_frame));
}

void attachment_constraints_bind_to_the_authored_reference_point() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "cloth-attachment-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, make_mesh(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    production::GarmentAsset garment = make_garment(document, mesh_asset);
    garment.collision_capsules.clear();
    garment.attachments.push_back(production::GarmentAttachment{
        "attachment.body", "body.anchor", geometry::VertexId{2U}, {1.0, 0.0, 0.0}, 0.0});
    REQUIRE(garment.validate(source));

    production::ClothSolverSettings settings;
    settings.frame_count = 4U;
    settings.projection_iterations = 16U;
    const auto result = production::compile_cloth_cpu(garment, source, settings);
    if (!result) throw std::runtime_error(result.error().message);
    REQUIRE(result.value().validate(garment, source));
    for (const production::ClothSimulationFrame& frame : result.value().frames) {
        const auto attached = std::find_if(frame.vertices.begin(), frame.vertices.end(),
            [](const geometry::Vertex& vertex) {
                return vertex.id == geometry::VertexId{2U};
            });
        REQUIRE(attached != frame.vertices.end());
        REQUIRE(std::abs(attached->position.x - 1.0) <= 1e-10);
        REQUIRE(std::abs(attached->position.y) <= 1e-10);
        REQUIRE(std::abs(attached->position.z) <= 1e-10);
    }
    REQUIRE(source.find_vertex(geometry::VertexId{2U})->position.y == 0.0);
}

void settling_receipt_is_deterministic_and_required_settle_fails_closed() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "cloth-settle-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, make_mesh(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const core::Revision source_revision = source.revision();
    production::GarmentAsset garment = make_garment(document, mesh_asset);
    garment.collision_capsules.clear();

    production::ClothSolverSettings settled_settings;
    settled_settings.frame_count = 4U;
    settled_settings.gravity = {};
    settled_settings.settle_window_frames = 3U;
    settled_settings.require_settled = true;
    const auto first = production::compile_cloth_cpu(garment, source, settled_settings);
    const auto second = production::compile_cloth_cpu(garment, source, settled_settings);
    if (!first) throw std::runtime_error(first.error().message);
    if (!second) throw std::runtime_error(second.error().message);
    REQUIRE(first.value().settled_frame == 2U);
    REQUIRE(first.value().artifact_digest == second.value().artifact_digest);
    REQUIRE(first.value().canonical() == second.value().canonical());
    REQUIRE(first.value().validate(garment, source));
    for (const auto& frame : first.value().frames) {
        REQUIRE(frame.maximum_frame_displacement == 0.0);
    }

    auto tampered_movement = first.value();
    tampered_movement.frames[0].maximum_frame_displacement = 0.25;
    REQUIRE(!tampered_movement.refresh_digests());
    tampered_movement = first.value();
    tampered_movement.frames[0].maximum_frame_displacement = 0.25;
    tampered_movement.settled_frame = 3U;
    REQUIRE(tampered_movement.refresh_digests());
    REQUIRE(!tampered_movement.validate(garment, source));

    const auto serialized = first.value().serialize();
    REQUIRE(serialized);
    const auto restored = production::ClothSimulationArtifact::deserialize(
        std::span<const std::uint8_t>{serialized.value()});
    REQUIRE(restored);
    REQUIRE(restored.value().settled_frame == first.value().settled_frame);
    REQUIRE(restored.value().canonical() == first.value().canonical());

    production::ClothSolverSettings unsettled_settings = settled_settings;
    unsettled_settings.frame_count = 2U;
    unsettled_settings.settle_window_frames = 2U;
    unsettled_settings.settle_displacement_tolerance = 1e-15;
    unsettled_settings.gravity = {0.0, -9.81, 0.0};
    const auto unsettled = production::compile_cloth_cpu(garment, source, unsettled_settings);
    REQUIRE(!unsettled);
    REQUIRE(unsettled.error().code == core::ErrorCode::unsupported);
    REQUIRE(unsettled.error().message.find("did not settle") != std::string::npos);
    REQUIRE(source.revision() == source_revision);
}

void settled_rest_pose_bake_is_bound_reconstructible_and_persistable() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "cloth-rest-pose-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, make_mesh_with_uvs(), mesh_asset);
    std::string serialized_v12_compat = document.serialize();
    const std::string current_schema =
        "CARTOGRAPHER_PROJECT " +
        std::to_string(project::ProjectDocument::kSchemaVersion) + "\n";
    REQUIRE(serialized_v12_compat.starts_with(current_schema));
    serialized_v12_compat.replace(0U, current_schema.size(), "CARTOGRAPHER_PROJECT 12\n");
    constexpr std::string_view empty_receipt_table = "CLOTH_REST_POSE_RECEIPTS 0\n";
    const std::size_t empty_receipts = serialized_v12_compat.find(empty_receipt_table);
    REQUIRE(empty_receipts != std::string::npos);
    serialized_v12_compat.erase(empty_receipts, empty_receipt_table.size());
    constexpr std::string_view condition_authoring_record = "CONDITION_AUTHORING NONE\n";
    const std::size_t condition_record = serialized_v12_compat.find(condition_authoring_record);
    REQUIRE(condition_record != std::string::npos);
    serialized_v12_compat.erase(condition_record, condition_authoring_record.size());
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const core::Revision source_revision = source.revision();
    production::GarmentAsset garment = make_garment(document, mesh_asset);
    garment.collision_capsules.clear();
    garment.pins.clear();
    for (const geometry::Vertex& vertex : source.vertices_sorted()) {
        garment.pins.push_back(production::GarmentPin{
            "pin." + std::to_string(vertex.id.value), vertex.id,
            vertex.position + core::Vec3d{0.0, 0.25, 0.0}, 0.0});
    }
    REQUIRE(garment.validate(source));

    production::ClothSolverSettings settings;
    settings.frame_count = 4U;
    settings.gravity = {};
    settings.settle_window_frames = 3U;
    settings.require_settled = true;
    const auto simulation = production::compile_cloth_cpu(garment, source, settings);
    if (!simulation) throw std::runtime_error(simulation.error().message);
    REQUIRE(simulation.value().settled_frame == 3U);

    const auto baked = production::bake_cloth_settled_rest_pose(
        simulation.value(), garment, source);
    const auto repeated = production::bake_cloth_settled_rest_pose(
        simulation.value(), garment, source);
    if (!baked) throw std::runtime_error(baked.error().message);
    if (!repeated) throw std::runtime_error(repeated.error().message);
    REQUIRE(baked.value().validate(simulation.value(), garment, source));
    REQUIRE(baked.value().receipt.receipt_digest == repeated.value().receipt.receipt_digest);
    REQUIRE(baked.value().receipt.canonical() == repeated.value().receipt.canonical());
    REQUIRE(baked.value().receipt.frame_index == simulation.value().settled_frame.value());
    REQUIRE(baked.value().receipt.source == garment.source);
    REQUIRE(baked.value().receipt.simulation_artifact_digest ==
        simulation.value().artifact_digest);
    REQUIRE(baked.value().mesh.vertex_count() == source.vertex_count());
    REQUIRE(baked.value().mesh.face_count() == source.face_count());
    REQUIRE(baked.value().mesh.attributes().layer_count() == source.attributes().layer_count());
    REQUIRE(baked.value().mesh.uv_sets().descriptors().size() == source.uv_sets().descriptors().size());
    const auto* source_uvs = source.attributes().find("uv0.corners");
    const auto* baked_uvs = baked.value().mesh.attributes().find("uv0.corners");
    REQUIRE(source_uvs != nullptr);
    REQUIRE(baked_uvs != nullptr);
    REQUIRE(source_uvs->values.size() == baked_uvs->values.size());
    for (std::size_t index = 0U; index < source_uvs->values.size(); ++index) {
        const core::Vec2d source_uv = std::get<core::Vec2d>(source_uvs->values[index]);
        const core::Vec2d baked_uv = std::get<core::Vec2d>(baked_uvs->values[index]);
        REQUIRE(source_uv.x == baked_uv.x);
        REQUIRE(source_uv.y == baked_uv.y);
    }
    const auto expected_uv_values = source_uvs->values;
    const std::size_t expected_attribute_layers = source.attributes().layer_count();
    const std::size_t expected_uv_sets = source.uv_sets().descriptors().size();
    const auto source_vertices = source.vertices_sorted();
    const auto baked_vertices = baked.value().mesh.vertices_sorted();
    REQUIRE(source_vertices.size() == baked_vertices.size());
    for (std::size_t index = 0U; index < source_vertices.size(); ++index) {
        REQUIRE(source_vertices[index].id == baked_vertices[index].id);
        REQUIRE(baked_vertices[index].position.x == source_vertices[index].position.x);
        REQUIRE(baked_vertices[index].position.y == source_vertices[index].position.y + 0.25);
        REQUIRE(baked_vertices[index].position.z == source_vertices[index].position.z);
    }
    const auto output_digest = production::bake_mesh_source_digest(baked.value().mesh);
    REQUIRE(output_digest);
    REQUIRE(output_digest.value() == baked.value().receipt.output.content_digest);
    REQUIRE(source.revision() == source_revision);

    const auto receipt_bytes = baked.value().receipt.serialize();
    REQUIRE(receipt_bytes);
    const std::string serialized_receipt(
        receipt_bytes.value().begin(), receipt_bytes.value().end());
    const assets::Sha256Digest serialized_receipt_digest =
        assets::sha256(std::span<const std::uint8_t>{receipt_bytes.value()});
    const auto restored_receipt = production::ClothRestPoseBakeReceipt::deserialize(
        std::span<const std::uint8_t>{receipt_bytes.value()});
    REQUIRE(restored_receipt);
    REQUIRE(restored_receipt.value().canonical() == baked.value().receipt.canonical());

    assets::BlobStore rest_pose_blobs(directory.path() / "cloth-rest-pose-blobs");
    production::ProductionArtifactStore rest_pose_store(rest_pose_blobs);
    const auto rest_pose_key = production::cloth_rest_pose_bake_cook_key(
        simulation.value(), "cpu-reference");
    REQUIRE(rest_pose_key);
    production::ProductionArtifactReceipt production_receipt;
    production_receipt.key = rest_pose_key.value();
    production_receipt.status = production::ReceiptStatus::admitted;
    production_receipt.output_digest = assets::sha256(receipt_bytes.value());
    production_receipt.output_bytes = receipt_bytes.value().size();
    production_receipt.quality.measured_error = simulation.value().maximum_constraint_error;
    production_receipt.quality.importance = 1.0;
    production_receipt.quality.confidence = 1.0;
    production_receipt.cost.disk_bytes = receipt_bytes.value().size();
    REQUIRE(production_receipt.refresh_digest());
    production::AdmissionPolicy rest_pose_policy;
    rest_pose_policy.max_output_bytes = production::kMaxClothRestPoseReceiptBytes;
    REQUIRE(production::publish_cloth_rest_pose_bake(
        rest_pose_store, production_receipt, baked.value(), simulation.value(),
        garment, source, rest_pose_policy));
    const auto published_bake = production::read_cloth_rest_pose_bake(
        rest_pose_store, simulation.value(), garment, source, "cpu-reference",
        rest_pose_policy);
    REQUIRE(published_bake);
    REQUIRE(published_bake.value().receipt.canonical() == baked.value().receipt.canonical());
    REQUIRE(published_bake.value().mesh.vertex_count() == baked.value().mesh.vertex_count());
    const auto rest_pose_ledger = production::ProductionReceiptLedger::deserialize(
        rest_pose_store.ledger().serialize());
    REQUIRE(rest_pose_ledger);
    const auto restored_production_receipt = rest_pose_ledger.value().query(rest_pose_key.value());
    REQUIRE(restored_production_receipt);
    REQUIRE(restored_production_receipt.value().has_value());
    REQUIRE(restored_production_receipt.value()->key.kind ==
        production::ArtifactKind::cloth_rest_pose);

    auto wrong_rest_pose_key = production_receipt;
    wrong_rest_pose_key.key.spatial_scope = "different-garment-scope";
    REQUIRE(wrong_rest_pose_key.refresh_digest());
    REQUIRE(!production::publish_cloth_rest_pose_bake(
        rest_pose_store, wrong_rest_pose_key, baked.value(), simulation.value(),
        garment, source, rest_pose_policy));
    auto wrong_rest_pose_metric = production_receipt;
    wrong_rest_pose_metric.quality.measured_error += 0.01;
    REQUIRE(wrong_rest_pose_metric.refresh_digest());
    REQUIRE(!production::publish_cloth_rest_pose_bake(
        rest_pose_store, wrong_rest_pose_metric, baked.value(), simulation.value(),
        garment, source, rest_pose_policy));
    REQUIRE(production::read_cloth_rest_pose_bake(
        rest_pose_store, simulation.value(), garment, source, "cpu-reference",
        rest_pose_policy));

    const auto rejects_receipt = [](const std::string& malformed) {
        const std::vector<std::uint8_t> bytes(malformed.begin(), malformed.end());
        return !production::ClothRestPoseBakeReceipt::deserialize(
            std::span<const std::uint8_t>{bytes});
    };
    REQUIRE(rejects_receipt(serialized_receipt.substr(0U, serialized_receipt.size() - 1U)));
    std::string wrong_receipt_version = serialized_receipt;
    const std::size_t receipt_header = wrong_receipt_version.find(
        "CARTO_CLOTH_REST_POSE_RECEIPT ");
    REQUIRE(receipt_header != std::string::npos);
    const std::size_t version_offset = receipt_header +
        std::string("CARTO_CLOTH_REST_POSE_RECEIPT ").size();
    wrong_receipt_version.replace(version_offset, 1U, "2");
    REQUIRE(rejects_receipt(wrong_receipt_version));
    REQUIRE(rejects_receipt(serialized_receipt + "TRAILING\n"));
    std::string wrong_receipt_digest = serialized_receipt;
    const std::size_t digest_marker = wrong_receipt_digest.rfind("RECEIPT ");
    REQUIRE(digest_marker != std::string::npos);
    const std::size_t digest_character = digest_marker + std::string("RECEIPT ").size();
    wrong_receipt_digest[digest_character] =
        wrong_receipt_digest[digest_character] == '0' ? '1' : '0';
    REQUIRE(rejects_receipt(wrong_receipt_digest));
    auto tampered_receipt = baked.value();
    tampered_receipt.receipt.frame_index = 2U;
    REQUIRE(tampered_receipt.receipt.refresh_digest());
    REQUIRE(!tampered_receipt.validate(simulation.value(), garment, source));
    auto tampered_mesh = baked.value();
    REQUIRE(tampered_mesh.mesh.set_vertex_position(
        geometry::VertexId{1U}, baked_vertices.front().position + core::Vec3d{0.1, 0.0, 0.0}));
    REQUIRE(!tampered_mesh.validate(simulation.value(), garment, source));

    auto stale_source = source;
    REQUIRE(stale_source.set_vertex_position(geometry::VertexId{1U}, {0.0, 0.0, 0.1}));
    REQUIRE(!production::bake_cloth_settled_rest_pose(
        simulation.value(), garment, stale_source));
    const auto stale_published_bake = production::read_cloth_rest_pose_bake(
        rest_pose_store, simulation.value(), garment, stale_source, "cpu-reference",
        rest_pose_policy);
    REQUIRE(!stale_published_bake);
    REQUIRE(stale_published_bake.error().code == core::ErrorCode::stale_data);

    auto transaction = project::ProjectTransaction::begin(
        document, journal, "cloth-rest-pose-tests");
    REQUIRE(transaction);
    const auto baked_asset = transaction.value().add_mesh(baked.value().mesh);
    REQUIRE(baked_asset);
    const std::vector<std::uint8_t> wrong_digest_bytes{0U};
    const assets::Sha256Digest wrong_serialized_receipt_digest =
        assets::sha256(std::span<const std::uint8_t>{wrong_digest_bytes});
    REQUIRE(!transaction.value().set_cloth_rest_pose_receipt(
        baked_asset.value(), serialized_receipt, wrong_serialized_receipt_digest));
    REQUIRE(!transaction.value().set_cloth_rest_pose_receipt(
        baked_asset.value(), serialized_receipt, assets::Sha256Digest{}));
    REQUIRE(!transaction.value().set_cloth_rest_pose_receipt(
        baked_asset.value() + 1U, serialized_receipt, serialized_receipt_digest));
    REQUIRE(transaction.value().staged_document().cloth_rest_pose_receipts().empty());
    REQUIRE(production::stage_cloth_rest_pose_bake_receipt(
        transaction.value(), baked_asset.value(), baked.value(), simulation.value(),
        garment, source));
    REQUIRE(transaction.value().staged_document().cloth_rest_pose_receipts().contains(
        baked_asset.value()));
    REQUIRE(transaction.value().commit("cloth.rest-pose.bake"));
    const auto reopened = project::ProjectDocument::deserialize(document.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().schema_version() == project::ProjectDocument::kSchemaVersion);
    const auto saved_project = document.save_atomic(directory.path() / "cloth-rest-pose.carto");
    REQUIRE(saved_project);
    REQUIRE(saved_project.value().schema_version == project::ProjectDocument::kSchemaVersion);
    const auto file_reopened = project::ProjectDocument::load(
        directory.path() / "cloth-rest-pose.carto");
    REQUIRE(file_reopened);
    REQUIRE(file_reopened.value().cloth_rest_pose_receipts().at(baked_asset.value()) ==
        reopened.value().cloth_rest_pose_receipts().at(baked_asset.value()));
    const auto reopened_mesh = reopened.value().meshes().find(baked_asset.value());
    REQUIRE(reopened_mesh != reopened.value().meshes().end());
    REQUIRE(reopened_mesh->second.attributes().layer_count() == expected_attribute_layers);
    REQUIRE(reopened_mesh->second.uv_sets().descriptors().size() == expected_uv_sets);
    const auto* reopened_uvs = reopened_mesh->second.attributes().find("uv0.corners");
    REQUIRE(reopened_uvs != nullptr);
    REQUIRE(reopened_uvs->values.size() == expected_uv_values.size());
    for (std::size_t index = 0U; index < expected_uv_values.size(); ++index) {
        const core::Vec2d source_uv = std::get<core::Vec2d>(expected_uv_values[index]);
        const core::Vec2d reopened_uv = std::get<core::Vec2d>(reopened_uvs->values[index]);
        REQUIRE(source_uv.x == reopened_uv.x);
        REQUIRE(source_uv.y == reopened_uv.y);
    }
    const auto reopened_digest = production::bake_mesh_source_digest(reopened_mesh->second);
    REQUIRE(reopened_digest);
    REQUIRE(reopened_digest.value() == baked.value().receipt.output.content_digest);
    const auto& saved_receipt = reopened.value().cloth_rest_pose_receipts().at(
        baked_asset.value());
    REQUIRE(saved_receipt.digest == assets::sha256(std::span<const std::uint8_t>{
        receipt_bytes.value()}));
    const std::vector<std::uint8_t> saved_receipt_bytes(
        saved_receipt.bytes.begin(), saved_receipt.bytes.end());
    const auto reopened_receipt = production::ClothRestPoseBakeReceipt::deserialize(
        std::span<const std::uint8_t>{saved_receipt_bytes});
    REQUIRE(reopened_receipt);
    REQUIRE(reopened_receipt.value().canonical() == baked.value().receipt.canonical());
    REQUIRE(reopened_receipt.value().output == baked.value().receipt.output);
    const auto reopened_v12 = project::ProjectDocument::deserialize(serialized_v12_compat);
    REQUIRE(reopened_v12);
    REQUIRE(reopened_v12.value().cloth_rest_pose_receipts().empty());
    std::string corrupt_project_receipt = reopened.value().serialize();
    const std::size_t receipt_record = corrupt_project_receipt.find(
        "CLOTH_REST_POSE_RECEIPT ");
    REQUIRE(receipt_record != std::string::npos);
    const std::size_t project_receipt_digest_character = corrupt_project_receipt.find(
        ' ', receipt_record +
        std::string("CLOTH_REST_POSE_RECEIPT ").size()) + 1U;
    REQUIRE(project_receipt_digest_character < corrupt_project_receipt.size());
    corrupt_project_receipt[project_receipt_digest_character] =
        corrupt_project_receipt[project_receipt_digest_character] == '0' ? '1' : '0';
    REQUIRE(!project::ProjectDocument::deserialize(corrupt_project_receipt));

    auto wrong_mesh_transaction = project::ProjectTransaction::begin(
        document, journal, "cloth-rest-pose-tests");
    REQUIRE(wrong_mesh_transaction);
    const auto unrelated_mesh = wrong_mesh_transaction.value().add_mesh(source);
    REQUIRE(unrelated_mesh);
    const auto mismatched_receipt = production::stage_cloth_rest_pose_bake_receipt(
        wrong_mesh_transaction.value(), unrelated_mesh.value(), baked.value(),
        simulation.value(), garment, source);
    REQUIRE(!mismatched_receipt);
    REQUIRE(mismatched_receipt.error().code == core::ErrorCode::stale_data);
    REQUIRE(!wrong_mesh_transaction.value().staged_document().cloth_rest_pose_receipts().contains(
        unrelated_mesh.value()));
    REQUIRE(wrong_mesh_transaction.value().rollback());

    auto replacement_transaction = project::ProjectTransaction::begin(
        document, journal, "cloth-rest-pose-tests");
    REQUIRE(replacement_transaction);
    auto edited_output = document.meshes().at(baked_asset.value());
    REQUIRE(edited_output.set_vertex_position(
        geometry::VertexId{1U}, baked_vertices.front().position + core::Vec3d{0.05, 0.0, 0.0}));
    REQUIRE(replacement_transaction.value().replace_mesh(
        baked_asset.value(), std::move(edited_output)));
    REQUIRE(!replacement_transaction.value().staged_document().cloth_rest_pose_receipts().contains(
        baked_asset.value()));
    REQUIRE(replacement_transaction.value().commit("cloth.rest-pose.mesh-edit"));
    REQUIRE(!document.cloth_rest_pose_receipts().contains(baked_asset.value()));

    const geometry::EditableMesh& current_source = document.meshes().at(mesh_asset);
    production::ClothSolverSettings unsettled_settings = settings;
    unsettled_settings.frame_count = 1U;
    unsettled_settings.settle_window_frames = 3U;
    unsettled_settings.require_settled = false;
    const auto unsettled = production::compile_cloth_cpu(
        garment, current_source, unsettled_settings);
    REQUIRE(unsettled);
    REQUIRE(!unsettled.value().settled_frame.has_value());
    const auto refused = production::bake_cloth_settled_rest_pose(
        unsettled.value(), garment, current_source);
    REQUIRE(!refused);
    REQUIRE(refused.error().code == core::ErrorCode::unsupported);
}

void frame_sequence_sample_bound_fails_before_simulation() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "cloth-frame-limit-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_frame_sample_limit_mesh(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const auto garment = production::make_garment_asset(
        document, mesh_asset, "frame-sample-limit");
    REQUIRE(garment);
    production::ClothSolverSettings settings;
    settings.frame_count = production::kMaxClothFrames;
    settings.substeps_per_frame = 1U;
    settings.projection_iterations = 1U;
    settings.gravity = {};
    const auto rejected = production::compile_cloth_cpu(garment.value(), source, settings);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::unsupported);
    REQUIRE(rejected.error().message.find("sample") != std::string::npos);
}

void vertex_self_collision_is_deterministic_bounded_and_failure_closed() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "cloth-self-collision-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_coincident_panels(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const core::Revision source_revision = source.revision();
    auto created = production::make_garment_asset(document, mesh_asset, "self-contact");
    REQUIRE(created);
    production::GarmentAsset garment = std::move(created).value();
    garment.panels = {
        {"panel.first", {geometry::FaceId{1U}}},
        {"panel.second", {geometry::FaceId{2U}}}};
    production::ClothSolverSettings settings;
    settings.frame_count = 2U;
    settings.substeps_per_frame = 1U;
    settings.projection_iterations = 8U;
    settings.gravity = {};
    settings.enable_self_collision = true;
    const auto first = production::compile_cloth_cpu(garment, source, settings);
    const auto second = production::compile_cloth_cpu(garment, source, settings);
    if (!first) throw std::runtime_error(first.error().message);
    if (!second) throw std::runtime_error(second.error().message);
    REQUIRE(first.value().canonical() == second.value().canonical());
    REQUIRE(first.value().artifact_digest == second.value().artifact_digest);
    REQUIRE(first.value().frames.size() == settings.frame_count);
    REQUIRE(first.value().maximum_collision_correction > 0.0);
    REQUIRE(first.value().maximum_constraint_error <= garment.fabric.thickness * 0.1);
    REQUIRE(first.value().validate(garment, source));
    const auto serialized = first.value().serialize();
    REQUIRE(serialized);
    const auto decoded = production::ClothSimulationArtifact::deserialize(
        std::span<const std::uint8_t>{serialized.value()});
    REQUIRE(decoded);
    REQUIRE(decoded.value().canonical() == first.value().canonical());
    REQUIRE(decoded.value().artifact_digest == first.value().artifact_digest);
    auto tampered_serialization = serialized.value();
    tampered_serialization.back() ^= 0x01U;
    REQUIRE(!production::ClothSimulationArtifact::deserialize(
        std::span<const std::uint8_t>{tampered_serialization}));
    auto truncated_serialization = serialized.value();
    truncated_serialization.pop_back();
    REQUIRE(!production::ClothSimulationArtifact::deserialize(
        std::span<const std::uint8_t>{truncated_serialization}));

    assets::BlobStore blobs(directory.path() / "cloth-output-blobs");
    production::ProductionArtifactStore store(blobs);
    const auto cook_key = production::cloth_production_cook_key(
        garment, settings, "cpu-reference");
    REQUIRE(cook_key);
    production::ProductionArtifactReceipt receipt;
    receipt.key = cook_key.value();
    receipt.status = production::ReceiptStatus::admitted;
    receipt.output_digest = assets::sha256(serialized.value());
    receipt.output_bytes = serialized.value().size();
    receipt.quality.measured_error = first.value().maximum_constraint_error;
    receipt.quality.importance = 1.0;
    receipt.quality.confidence = 1.0;
    receipt.cost.cpu_milliseconds = 0.5;
    receipt.cost.disk_bytes = serialized.value().size();
    REQUIRE(receipt.refresh_digest());
    production::AdmissionPolicy policy;
    policy.max_output_bytes = production::kMaxGarmentAssetBytes + 4096U;
    REQUIRE(production::publish_cloth_simulation(
        store, receipt, first.value(), garment, source, policy));
    const auto loaded = production::read_cloth_simulation(
        store, garment, source, settings, "cpu-reference", policy);
    REQUIRE(loaded);
    REQUIRE(loaded.value().canonical() == first.value().canonical());
    const auto ledger_roundtrip = production::ProductionReceiptLedger::deserialize(
        store.ledger().serialize());
    REQUIRE(ledger_roundtrip);
    const auto restored_receipt = ledger_roundtrip.value().query(cook_key.value());
    REQUIRE(restored_receipt);
    REQUIRE(restored_receipt.value().has_value());
    REQUIRE(restored_receipt.value()->key.kind == production::ArtifactKind::cloth_simulation);
    auto wrong_key_receipt = receipt;
    wrong_key_receipt.key.spatial_scope = "different-garment-scope";
    REQUIRE(wrong_key_receipt.refresh_digest());
    REQUIRE(!production::publish_cloth_simulation(
        store, wrong_key_receipt, first.value(), garment, source, policy));
    auto incorrect_metric_receipt = receipt;
    incorrect_metric_receipt.quality.measured_error += 0.01;
    REQUIRE(incorrect_metric_receipt.refresh_digest());
    REQUIRE(!production::publish_cloth_simulation(
        store, incorrect_metric_receipt, first.value(), garment, source, policy));
    const auto retained_last_good = production::read_cloth_simulation(
        store, garment, source, settings, "cpu-reference", policy);
    REQUIRE(retained_last_good);
    REQUIRE(retained_last_good.value().artifact_digest == first.value().artifact_digest);
    auto stale_source = source;
    REQUIRE(stale_source.set_vertex_position(geometry::VertexId{1U}, {0.1, 0.0, 0.0}));
    const auto stale_read = production::read_cloth_simulation(
        store, garment, stale_source, settings, "cpu-reference", policy);
    REQUIRE(!stale_read);
    REQUIRE(stale_read.error().code == core::ErrorCode::stale_data);
    REQUIRE(source.revision() == source_revision);
    for (const auto& [first_id, second_id] : {
             std::pair{geometry::VertexId{1U}, geometry::VertexId{5U}},
             std::pair{geometry::VertexId{2U}, geometry::VertexId{6U}},
             std::pair{geometry::VertexId{3U}, geometry::VertexId{7U}},
             std::pair{geometry::VertexId{4U}, geometry::VertexId{8U}}}) {
        const auto first_vertex = std::find_if(first.value().frames.back().vertices.begin(),
            first.value().frames.back().vertices.end(), [first_id](const geometry::Vertex& vertex) {
                return vertex.id == first_id;
            });
        const auto second_vertex = std::find_if(first.value().frames.back().vertices.begin(),
            first.value().frames.back().vertices.end(), [second_id](const geometry::Vertex& vertex) {
                return vertex.id == second_id;
            });
        REQUIRE(first_vertex != first.value().frames.back().vertices.end());
        REQUIRE(second_vertex != first.value().frames.back().vertices.end());
        REQUIRE((first_vertex->position - second_vertex->position).length() >=
            garment.fabric.thickness * 2.0 - garment.fabric.thickness * 0.1);
    }

    auto fixed_overlap = garment;
    fixed_overlap.pins = {
        {"pin.first", geometry::VertexId{1U}, {0.0, 0.0, 0.0}, 0.0},
        {"pin.second", geometry::VertexId{5U}, {0.0, 0.0, 0.0}, 0.0}};
    const auto pinned_failure = production::compile_cloth_cpu(fixed_overlap, source, settings);
    REQUIRE(!pinned_failure);
    REQUIRE(pinned_failure.error().code == core::ErrorCode::validation_failed);

    const auto dense_mesh = make_dense_self_collision_cell();
    std::uint64_t dense_mesh_asset = 0U;
    journal::Journal dense_journal(directory.path() / "cloth-self-collision-dense-journal.log");
    project::ProjectDocument dense_document = make_project(
        dense_journal, dense_mesh, dense_mesh_asset);
    auto dense_garment = production::make_garment_asset(
        dense_document, dense_mesh_asset, "dense-self-contact");
    REQUIRE(dense_garment);
    const auto dense_failure = production::compile_cloth_cpu(
        dense_garment.value(), dense_document.meshes().at(dense_mesh_asset), settings);
    REQUIRE(!dense_failure);
    REQUIRE(dense_failure.error().code == core::ErrorCode::unsupported);

    auto over_budget = settings;
    over_budget.frame_count = production::kMaxClothFrames;
    over_budget.substeps_per_frame = production::kMaxClothSubsteps;
    over_budget.projection_iterations = production::kMaxClothIterations;
    const auto budget_failure = production::compile_cloth_cpu(garment, source, over_budget);
    REQUIRE(!budget_failure);
    REQUIRE(budget_failure.error().code == core::ErrorCode::unsupported);

    journal::Journal coordinate_journal(
        directory.path() / "cloth-self-collision-coordinate-journal.log");
    std::uint64_t coordinate_mesh_asset = 0U;
    project::ProjectDocument coordinate_document = make_project(
        coordinate_journal, make_coincident_panels(1e14), coordinate_mesh_asset);
    auto coordinate_garment = production::make_garment_asset(
        coordinate_document, coordinate_mesh_asset, "large-coordinate-self-contact");
    REQUIRE(coordinate_garment);
    const auto coordinate_failure = production::compile_cloth_cpu(
        coordinate_garment.value(), coordinate_document.meshes().at(coordinate_mesh_asset),
        settings);
    REQUIRE(!coordinate_failure);
    REQUIRE(coordinate_failure.error().code == core::ErrorCode::validation_failed);
}

void vertex_face_self_collision_closes_interior_penetration() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "cloth-vertex-face-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_vertex_face_overlap(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    auto created = production::make_garment_asset(document, mesh_asset, "vertex-face-contact");
    REQUIRE(created);
    production::GarmentAsset garment = std::move(created).value();
    garment.panels = {
        {"panel.crossing", {geometry::FaceId{2U}}},
        {"panel.receiver", {geometry::FaceId{1U}}}};

    production::ClothSolverSettings settings;
    settings.frame_count = 1U;
    settings.substeps_per_frame = 1U;
    settings.projection_iterations = 16U;
    settings.gravity = {};
    const auto vertex_only = production::compile_cloth_cpu(garment, source, settings);
    if (!vertex_only) throw std::runtime_error(vertex_only.error().message);
    REQUIRE(vertex_only);
    REQUIRE(vertex_only.value().maximum_collision_correction == 0.0);

    settings.enable_self_collision = true;
    const auto first = production::compile_cloth_cpu(garment, source, settings);
    const auto second = production::compile_cloth_cpu(garment, source, settings);
    if (!first) throw std::runtime_error(first.error().message);
    if (!second) throw std::runtime_error(second.error().message);
    REQUIRE(first.value().canonical() == second.value().canonical());
    REQUIRE(first.value().artifact_digest == second.value().artifact_digest);
    REQUIRE(first.value().maximum_collision_correction > 0.0);
    REQUIRE(first.value().maximum_constraint_error <= garment.fabric.thickness * 0.1);
    REQUIRE(first.value().validate(garment, source));

    const auto find_position = [&first](geometry::VertexId id) {
        const auto found = std::find_if(first.value().frames.back().vertices.begin(),
            first.value().frames.back().vertices.end(),
            [id](const geometry::Vertex& vertex) { return vertex.id == id; });
        REQUIRE(found != first.value().frames.back().vertices.end());
        return found->position;
    };
    const core::Vec3d a = find_position(geometry::VertexId{1U});
    const core::Vec3d b = find_position(geometry::VertexId{2U});
    const core::Vec3d c = find_position(geometry::VertexId{3U});
    const core::Vec3d crossing = find_position(geometry::VertexId{4U});
    const core::Vec3d normal = core::cross(b - a, c - a).normalized();
    REQUIRE(normal.finite());
    REQUIRE(std::abs(core::dot(crossing - a, normal)) >=
        garment.fabric.thickness * 2.0 * 0.9);

    auto fixed_overlap = garment;
    fixed_overlap.pins = {
        {"pin.1", geometry::VertexId{1U}, source.find_vertex(geometry::VertexId{1U})->position, 0.0},
        {"pin.2", geometry::VertexId{2U}, source.find_vertex(geometry::VertexId{2U})->position, 0.0},
        {"pin.3", geometry::VertexId{3U}, source.find_vertex(geometry::VertexId{3U})->position, 0.0},
        {"pin.4", geometry::VertexId{4U}, source.find_vertex(geometry::VertexId{4U})->position, 0.0},
        {"pin.5", geometry::VertexId{5U}, source.find_vertex(geometry::VertexId{5U})->position, 0.0},
        {"pin.6", geometry::VertexId{6U}, source.find_vertex(geometry::VertexId{6U})->position, 0.0}};
    REQUIRE(fixed_overlap.validate(source));
    const auto pinned_failure = production::compile_cloth_cpu(
        fixed_overlap, source, settings);
    REQUIRE(!pinned_failure);
    REQUIRE(pinned_failure.error().code == core::ErrorCode::validation_failed);
}

void edge_edge_self_collision_closes_crossing_edges() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "cloth-edge-edge-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_edge_edge_crossing(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    auto created = production::make_garment_asset(document, mesh_asset, "edge-edge-contact");
    REQUIRE(created);
    production::GarmentAsset garment = std::move(created).value();
    garment.panels = {
        {"panel.diagonal", {geometry::FaceId{2U}}},
        {"panel.horizontal", {geometry::FaceId{1U}}}};
    const auto garment_validation = garment.validate(source);
    if (!garment_validation) throw std::runtime_error(garment_validation.error().message);
    const double minimum_separation = garment.fabric.thickness * 2.0;
    const auto first_edge_midpoint = (source.find_vertex(geometry::VertexId{1U})->position +
        source.find_vertex(geometry::VertexId{2U})->position) * 0.5;
    const auto second_edge_midpoint = (source.find_vertex(geometry::VertexId{4U})->position +
        source.find_vertex(geometry::VertexId{5U})->position) * 0.5;
    REQUIRE((first_edge_midpoint - second_edge_midpoint).length() < 1e-12);
    const auto face_normal = [&](geometry::FaceId id) {
        const geometry::Face* face = source.find_face(id);
        REQUIRE(face != nullptr);
        const core::Vec3d a = source.find_vertex(face->vertices[0])->position;
        const core::Vec3d b = source.find_vertex(face->vertices[1])->position;
        const core::Vec3d c = source.find_vertex(face->vertices[2])->position;
        return core::cross(b - a, c - a).normalized();
    };
    const core::Vec3d first_normal = face_normal(geometry::FaceId{1U});
    const core::Vec3d second_normal = face_normal(geometry::FaceId{2U});
    for (const geometry::Vertex& first : source.vertices_sorted()) {
        for (const geometry::Vertex& second : source.vertices_sorted()) {
            if (first.id >= second.id) continue;
            REQUIRE((first.position - second.position).length() > minimum_separation);
        }
        const auto normal = first.id.value <= 3U ? second_normal : first_normal;
        const auto* anchor = source.find_vertex(first.id.value <= 3U ?
            geometry::VertexId{4U} : geometry::VertexId{1U});
        REQUIRE(anchor != nullptr);
        REQUIRE(std::abs(core::dot(first.position - anchor->position, normal)) >
            minimum_separation);
    }

    production::ClothSolverSettings settings;
    settings.frame_count = 1U;
    settings.substeps_per_frame = 1U;
    settings.projection_iterations = 16U;
    settings.gravity = {};
    const auto disabled = production::compile_cloth_cpu(garment, source, settings);
    if (!disabled) throw std::runtime_error(disabled.error().message);
    REQUIRE(disabled);
    REQUIRE(disabled.value().maximum_collision_correction == 0.0);

    settings.enable_self_collision = true;
    const auto first = production::compile_cloth_cpu(garment, source, settings);
    const auto second = production::compile_cloth_cpu(garment, source, settings);
    if (!first) throw std::runtime_error(first.error().message);
    if (!second) throw std::runtime_error(second.error().message);
    REQUIRE(first.value().canonical() == second.value().canonical());
    REQUIRE(first.value().artifact_digest == second.value().artifact_digest);
    REQUIRE(first.value().maximum_collision_correction > 0.0);
    REQUIRE(first.value().maximum_constraint_error <= 0.05);
    REQUIRE(first.value().validate(garment, source));

    auto pinned_crossing = garment;
    pinned_crossing.pins.clear();
    for (const geometry::Vertex& vertex : source.vertices_sorted()) {
        pinned_crossing.pins.push_back(production::GarmentPin{
            "pin." + std::to_string(vertex.id.value), vertex.id, vertex.position, 0.0});
    }
    REQUIRE(pinned_crossing.validate(source));
    const auto pinned_failure = production::compile_cloth_cpu(
        pinned_crossing, source, settings);
    REQUIRE(!pinned_failure);
    REQUIRE(pinned_failure.error().code == core::ErrorCode::validation_failed);
}

void edge_edge_self_collision_preserves_authored_seam_adjacency() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "cloth-seam-edge-contact-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_seam_adjacent_panels(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    auto created = production::make_garment_asset(document, mesh_asset, "stitched-panels");
    REQUIRE(created);
    production::GarmentAsset garment = std::move(created).value();
    garment.panels = {
        {"panel.first", {geometry::FaceId{1U}}},
        {"panel.second", {geometry::FaceId{2U}}}};
    garment.seams.push_back(production::GarmentSeam{
        "seam.shared-edge", {geometry::VertexId{1U}, geometry::VertexId{2U}},
        {geometry::VertexId{4U}, geometry::VertexId{5U}}, 0.0});
    REQUIRE(garment.validate(source));

    production::ClothSolverSettings settings;
    settings.frame_count = 1U;
    settings.substeps_per_frame = 1U;
    settings.projection_iterations = 8U;
    settings.gravity = {};
    settings.enable_self_collision = true;
    const auto result = production::compile_cloth_cpu(garment, source, settings);
    if (!result) throw std::runtime_error(result.error().message);
    REQUIRE(result.value().maximum_collision_correction == 0.0);
    REQUIRE(result.value().validate(garment, source));
}

void coincident_boundary_seam_constraints_remain_deterministic() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "cloth-seam-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_coincident_panels(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    auto created = production::make_garment_asset(document, mesh_asset, "joined.panels");
    REQUIRE(created);
    production::GarmentAsset garment = std::move(created).value();
    garment.panels = {
        {"panel.first", {geometry::FaceId{1U}}},
        {"panel.second", {geometry::FaceId{2U}}}};
    garment.seams.push_back(production::GarmentSeam{
        "seam.shared", {geometry::VertexId{1U}, geometry::VertexId{4U}},
        {geometry::VertexId{5U}, geometry::VertexId{8U}}, 0.0});
    garment.pins.push_back(production::GarmentPin{
        "pin.first", geometry::VertexId{1U}, {0.0, 0.0, 0.0}, 0.0});
    garment.pins.push_back(production::GarmentPin{
        "pin.second", geometry::VertexId{5U}, {0.0, 0.0, 0.0}, 0.0});
    REQUIRE(garment.validate(source));
    production::ClothSolverSettings settings;
    settings.frame_count = 4U;
    settings.projection_iterations = 16U;
    const auto first = production::compile_cloth_cpu(garment, source, settings);
    const auto second = production::compile_cloth_cpu(garment, source, settings);
    if (!first) throw std::runtime_error(first.error().message);
    if (!second) throw std::runtime_error(second.error().message);
    REQUIRE(first.value().artifact_digest == second.value().artifact_digest);
    const auto vertex = [&first](geometry::VertexId id) -> const geometry::Vertex& {
        const auto found = std::find_if(first.value().frames.back().vertices.begin(),
            first.value().frames.back().vertices.end(),
            [id](const geometry::Vertex& value) { return value.id == id; });
        REQUIRE(found != first.value().frames.back().vertices.end());
        return *found;
    };
    REQUIRE(vertex(geometry::VertexId{1U}).position.x == vertex(geometry::VertexId{5U}).position.x);
    REQUIRE(vertex(geometry::VertexId{1U}).position.y == vertex(geometry::VertexId{5U}).position.y);
    REQUIRE(vertex(geometry::VertexId{4U}).position.x == vertex(geometry::VertexId{8U}).position.x);
    REQUIRE(vertex(geometry::VertexId{4U}).position.y == vertex(geometry::VertexId{8U}).position.y);
}

} // namespace

int main() {
    try {
        cpu_reference_is_deterministic_bounded_and_failure_closed();
        std::cout << "PASS deterministic CPU XPBD stretch/shear/bend, pin and body-capsule projection\n";
        std::cout << "PASS stale-source, tamper, and conflicting pin/body-collision rejection\n";
        attachment_constraints_bind_to_the_authored_reference_point();
        std::cout << "PASS deterministic CPU attachment projection preserves the source mesh\n";
        settling_receipt_is_deterministic_and_required_settle_fails_closed();
        std::cout << "PASS deterministic settle-window receipt, serialization, tamper rejection, and required-settle failure\n";
        settled_rest_pose_bake_is_bound_reconstructible_and_persistable();
        std::cout << "PASS settled cloth pose bake, receipt round-trip, stale/tamper rejection, and transactional mesh persistence\n";
        frame_sequence_sample_bound_fails_before_simulation();
        std::cout << "PASS cloth frame sample preflight rejects oversized output\n";
        coincident_boundary_seam_constraints_remain_deterministic();
        std::cout << "PASS deterministic XPBD seam projection on coincident imported panel boundaries\n";
        vertex_self_collision_is_deterministic_bounded_and_failure_closed();
        std::cout << "PASS deterministic vertex self-contact, pin conflict, dense-cell, and work-budget boundaries\n";
        vertex_face_self_collision_closes_interior_penetration();
        std::cout << "PASS bounded vertex-face self-contact prevents interior triangle penetration\n";
        edge_edge_self_collision_closes_crossing_edges();
        std::cout << "PASS bounded edge-edge self-contact separates crossing boundary edges\n";
        edge_edge_self_collision_preserves_authored_seam_adjacency();
        std::cout << "PASS edge-edge self-contact excludes authored seam adjacency\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL CPU cloth tests: " << error.what() << '\n';
        return 1;
    }
}
