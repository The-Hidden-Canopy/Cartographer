#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/multires.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
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
            ("cartographer-multires-tests-" + std::to_string(stamp) + "-" +
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

carto::geometry::EditableMesh make_adjacent_quads() {
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

carto::geometry::EditableMesh make_concave_polygon() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {2.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {2.0, 2.0, 0.0}},
         {geometry::VertexId{4U}, {1.0, 0.75, 0.0}},
         {geometry::VertexId{5U}, {0.0, 2.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}, geometry::VertexId{4U}, geometry::VertexId{5U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_interior_detail_grid() {
    using namespace carto;
    geometry::EditableMesh mesh;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    for (std::uint64_t row = 0U; row < 4U; ++row) {
        for (std::uint64_t column = 0U; column < 4U; ++column) {
            const std::uint64_t id = row * 4U + column + 1U;
            vertices.push_back({geometry::VertexId{id},
                {static_cast<double>(column), static_cast<double>(row), 0.0}});
        }
    }
    for (std::uint64_t row = 0U; row < 3U; ++row) {
        for (std::uint64_t column = 0U; column < 3U; ++column) {
            const std::uint64_t top_left = row * 4U + column + 1U;
            const std::uint64_t top_right = top_left + 1U;
            const std::uint64_t bottom_left = top_left + 4U;
            const std::uint64_t bottom_right = bottom_left + 1U;
            const std::uint64_t face_id = row * 3U + column + 1U;
            faces.push_back({geometry::FaceId{face_id}, {
                geometry::VertexId{top_left}, geometry::VertexId{top_right},
                geometry::VertexId{bottom_right}, geometry::VertexId{bottom_left}}});
        }
    }
    REQUIRE(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_triangle_budget_mesh() {
    using namespace carto;
    geometry::EditableMesh mesh;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    vertices.reserve(1500U);
    faces.reserve(500U);
    for (std::uint64_t triangle = 0U; triangle < 500U; ++triangle) {
        const std::uint64_t first = triangle * 3U + 1U;
        const double x = static_cast<double>(triangle) * 3.0;
        vertices.push_back({geometry::VertexId{first}, {x, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first + 1U}, {x + 1.0, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first + 2U}, {x, 1.0, 0.0}});
        faces.push_back({geometry::FaceId{triangle + 1U},
            {geometry::VertexId{first}, geometry::VertexId{first + 1U},
             geometry::VertexId{first + 2U}}});
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
    auto document = project::ProjectDocument::create("Multires Test");
    REQUIRE(document);
    const std::string snapshot = document.value().serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(snapshot.data());
    REQUIRE(journal.append(journal::JournalAppend{
        core::Revision{}, document.value().revision(), "project.create",
        std::span<const std::uint8_t>(bytes, snapshot.size()), std::nullopt}));
    auto transaction = project::ProjectTransaction::begin(
        document.value(), journal, "multires-tests");
    REQUIRE(transaction);
    const auto added = transaction.value().add_mesh(std::move(mesh));
    REQUIRE(added);
    mesh_asset = added.value();
    REQUIRE(transaction.value().commit("mesh.fixture"));
    return std::move(document).value();
}

carto::production::SculptEvaluationArtifact evaluate_empty_stack(
    const carto::project::ProjectDocument& document,
    std::uint64_t mesh_asset) {
    using namespace carto;
    auto stack = production::make_sculpt_stack(document, mesh_asset);
    REQUIRE(stack);
    const auto evaluated = production::compile_sculpt_stack(
        stack.value(), document.meshes().at(mesh_asset));
    REQUIRE(evaluated);
    return evaluated.value();
}

carto::production::SculptEvaluationArtifact evaluate_central_detail(
    const carto::project::ProjectDocument& document,
    std::uint64_t mesh_asset) {
    using namespace carto;
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    auto stack = production::make_sculpt_stack(document, mesh_asset);
    REQUIRE(stack);
    const production::SculptSurfaceAnchor anchor{
        geometry::FaceId{5U},
        {geometry::VertexId{6U}, geometry::VertexId{7U}, geometry::VertexId{11U}},
        {0.0, 0.0, 1.0}};
    const auto sample = production::make_sculpt_surface_sample(source, anchor);
    REQUIRE(sample);
    production::SculptLayer layer;
    layer.identity = "detail.layer";
    layer.name = "Detail";
    production::SculptStroke stroke;
    stroke.identity = "detail.stroke";
    stroke.mode = production::SculptStrokeMode::add;
    stroke.radius = 0.5;
    stroke.strength = 0.2;
    stroke.samples.push_back(sample.value());
    layer.strokes.push_back(std::move(stroke));
    stack.value().layers.push_back(std::move(layer));
    const auto evaluated = production::compile_sculpt_stack(stack.value(), source);
    REQUIRE(evaluated);
    return evaluated.value();
}

void deterministic_levels_keep_seam_side_anchors_and_fail_closed() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "multires-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, make_adjacent_quads(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const core::Revision source_revision = source.revision();
    const production::SculptEvaluationArtifact sculpted = evaluate_empty_stack(document, mesh_asset);

    const production::MultiresSettings settings{1U};
    const auto first = production::compile_multires_surface_hierarchy(sculpted, source, settings);
    const auto second = production::compile_multires_surface_hierarchy(sculpted, source, settings);
    REQUIRE(first && second);
    REQUIRE(first.value().canonical() == second.value().canonical());
    REQUIRE(first.value().artifact_digest == second.value().artifact_digest);
    REQUIRE(first.value().levels.size() == 2U);
    REQUIRE(first.value().levels[0].faces.size() == 4U);
    REQUIRE(first.value().levels[1].faces.size() == 16U);
    REQUIRE(first.value().byte_size > 0U);
    REQUIRE(source.revision() == source_revision);
    REQUIRE(first.value().validate(sculpted, source));
    REQUIRE(first.value().source_reconstruction_errors.size() == first.value().levels.size());
    for (std::size_t index = 0U; index < first.value().levels.size(); ++index) {
        const auto& error = first.value().source_reconstruction_errors[index];
        REQUIRE(error.level == index);
        REQUIRE(error.sample_count == first.value().levels[index].vertices.size());
        REQUIRE(error.maximum_anchor_reconstruction_error < 1e-9);
        REQUIRE(error.rms_anchor_reconstruction_error < 1e-9);
    }
    REQUIRE(!first.value().detail_classifications.empty());
    REQUIRE(std::all_of(first.value().detail_classifications.begin(),
        first.value().detail_classifications.end(), [](const auto& detail) {
            return detail.target == production::MultiresDetailTarget::unchanged;
        }));

    const auto midpoint = std::find_if(first.value().levels[1].vertices.begin(),
        first.value().levels[1].vertices.end(), [](const auto& vertex) {
            return std::abs(vertex.position.x - 1.0) < 1e-12 &&
                std::abs(vertex.position.y - 0.5) < 1e-12 &&
                std::abs(vertex.position.z) < 1e-12;
        });
    REQUIRE(midpoint != first.value().levels[1].vertices.end());
    bool left_source_anchor = false;
    bool right_source_anchor = false;
    for (const production::MultiresHierarchyFace& face : first.value().levels[1].faces) {
        for (std::size_t corner = 0U; corner < face.vertices.size(); ++corner) {
            if (face.vertices[corner] != midpoint->id) continue;
            left_source_anchor = left_source_anchor ||
                face.source_corners[corner].face == geometry::FaceId{1U};
            right_source_anchor = right_source_anchor ||
                face.source_corners[corner].face == geometry::FaceId{2U};
        }
    }
    REQUIRE(left_source_anchor && right_source_anchor);

    auto tampered = first.value();
    tampered.levels.back().faces.front().source_corners.front().barycentric[0] = 2.0;
    REQUIRE(!tampered.validate(sculpted, source));
    tampered = first.value();
    tampered.output_digest.bytes[0] ^= 0x80U;
    REQUIRE(!tampered.validate(sculpted, source));
    tampered = first.value();
    tampered.detail_classifications.front().target = production::MultiresDetailTarget::geometry;
    REQUIRE(!tampered.validate(sculpted, source));
    tampered = first.value();
    tampered.source_reconstruction_errors.front().maximum_anchor_reconstruction_error += 1e-9;
    REQUIRE(tampered.refresh_digests());
    REQUIRE(tampered.output_digest != first.value().output_digest);
    REQUIRE(!tampered.validate(sculpted, source));

    auto stale_source = source;
    const auto original = stale_source.find_vertex(geometry::VertexId{1U})->position;
    REQUIRE(stale_source.set_vertex_position(geometry::VertexId{1U},
        {original.x, original.y, original.z + 0.25}));
    REQUIRE(!first.value().validate(sculpted, stale_source));
    REQUIRE(!production::compile_multires_surface_hierarchy(
        sculpted, source, production::MultiresSettings{5U}));
}

void face_budget_rejects_before_unbounded_subdivision() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "multires-budget-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_triangle_budget_mesh(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const production::SculptEvaluationArtifact sculpted = evaluate_empty_stack(document, mesh_asset);
    const auto result = production::compile_multires_surface_hierarchy(
        sculpted, source, production::MultiresSettings{4U});
    REQUIRE(!result);
    REQUIRE(result.error().code == core::ErrorCode::unsupported);
}

void concave_source_fan_is_rejected_without_mutating_source() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "multires-concave-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_concave_polygon(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const core::Revision revision_before = source.revision();
    const std::string source_before = document.serialize();
    const production::SculptEvaluationArtifact sculpted = evaluate_empty_stack(document, mesh_asset);

    const auto result = production::compile_multires_surface_hierarchy(
        sculpted, source, production::MultiresSettings{0U});
    REQUIRE(!result);
    REQUIRE(result.error().code == core::ErrorCode::unsupported);
    REQUIRE(source.revision() == revision_before);
    REQUIRE(document.serialize() == source_before);
}

void published_hierarchy_is_content_addressed_and_rebuilt_from_current_source() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "multires-publication-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, make_adjacent_quads(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const production::SculptEvaluationArtifact sculpted = evaluate_empty_stack(document, mesh_asset);
    const production::MultiresSettings settings{1U};
    const auto hierarchy = production::compile_multires_surface_hierarchy(
        sculpted, source, settings);
    REQUIRE(hierarchy);

    assets::BlobStore blobs(directory.path() / "multires-output-blobs");
    production::ProductionArtifactStore store(blobs);
    const auto key = production::multires_production_cook_key(
        hierarchy.value(), "cpu-reference");
    REQUIRE(key);
    REQUIRE(key.value().kind == production::ArtifactKind::multires_hierarchy);
    const std::string payload = hierarchy.value().canonical();
    const auto* payload_bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
    const std::span<const std::uint8_t> payload_span{payload_bytes, payload.size()};

    production::ProductionArtifactReceipt receipt;
    receipt.key = key.value();
    receipt.status = production::ReceiptStatus::admitted;
    receipt.output_digest = assets::sha256(payload_span);
    receipt.output_bytes = static_cast<std::uint64_t>(payload.size());
    receipt.cost.disk_bytes = receipt.output_bytes;
    REQUIRE(receipt.refresh_digest());
    production::AdmissionPolicy policy;
    policy.max_output_bytes = production::kMaxMultiresArtifactBytes;
    REQUIRE(production::publish_multires_surface_hierarchy(
        store, receipt, hierarchy.value(), sculpted, source, policy));

    const auto loaded = production::read_multires_surface_hierarchy(
        store, sculpted, source, settings, "cpu-reference", policy);
    REQUIRE(loaded);
    REQUIRE(loaded.value().canonical() == payload);
    const auto ledger = production::ProductionReceiptLedger::deserialize(
        store.ledger().serialize());
    REQUIRE(ledger);
    const auto persisted = ledger.value().query(key.value());
    REQUIRE(persisted && persisted.value().has_value());
    REQUIRE(persisted.value()->key.kind == production::ArtifactKind::multires_hierarchy);

    auto wrong_key = receipt;
    wrong_key.key.spatial_scope = "different-mesh-scope";
    REQUIRE(wrong_key.refresh_digest());
    REQUIRE(!production::publish_multires_surface_hierarchy(
        store, wrong_key, hierarchy.value(), sculpted, source, policy));
    auto wrong_metric = receipt;
    wrong_metric.quality.measured_error = 0.01;
    REQUIRE(wrong_metric.refresh_digest());
    REQUIRE(!production::publish_multires_surface_hierarchy(
        store, wrong_metric, hierarchy.value(), sculpted, source, policy));
    const auto retained = production::read_multires_surface_hierarchy(
        store, sculpted, source, settings, "cpu-reference", policy);
    REQUIRE(retained);
    REQUIRE(retained.value().artifact_digest == hierarchy.value().artifact_digest);

    auto stale_source = source;
    const auto original = stale_source.find_vertex(geometry::VertexId{1U})->position;
    REQUIRE(stale_source.set_vertex_position(geometry::VertexId{1U},
        {original.x, original.y, original.z + 0.25}));
    const auto stale_read = production::read_multires_surface_hierarchy(
        store, sculpted, stale_source, settings, "cpu-reference", policy);
    REQUIRE(!stale_read);
    REQUIRE(stale_read.error().code == core::ErrorCode::stale_data);
}

void detail_frequency_importance_and_boundaries_select_conservative_targets() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "multires-detail-map-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_interior_detail_grid(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const production::SculptEvaluationArtifact sculpted = evaluate_central_detail(
        document, mesh_asset);

    production::MultiresSettings displacement_settings{0U};
    displacement_settings.runtime_importance = 0.0;
    displacement_settings.displacement_frequency_threshold = 2.0;
    const auto displacement = production::compile_multires_surface_hierarchy(
        sculpted, source, displacement_settings);
    const auto displacement_again = production::compile_multires_surface_hierarchy(
        sculpted, source, displacement_settings);
    REQUIRE(displacement && displacement_again);
    REQUIRE(displacement.value().canonical() == displacement_again.value().canonical());
    const auto central_displacement = std::find_if(
        displacement.value().detail_classifications.begin(),
        displacement.value().detail_classifications.end(), [](const auto& item) {
            return item.source_face == geometry::FaceId{5U};
        });
    REQUIRE(central_displacement != displacement.value().detail_classifications.end());
    REQUIRE(!central_displacement->boundary_sensitive);
    REQUIRE(central_displacement->estimated_frequency > 0.0);
    REQUIRE(central_displacement->target == production::MultiresDetailTarget::displacement);
    bool displacement_sample_is_nonzero = false;
    for (std::size_t corner = 0U; corner < central_displacement->source_vertices.size(); ++corner) {
        const geometry::VertexId vertex_id = central_displacement->source_vertices[corner];
        const geometry::Vertex* source_vertex = source.find_vertex(vertex_id);
        REQUIRE(source_vertex != nullptr);
        const auto evaluated_vertex = std::find_if(
            sculpted.vertices.begin(), sculpted.vertices.end(), [vertex_id](const auto& vertex) {
                return vertex.id == vertex_id;
            });
        REQUIRE(evaluated_vertex != sculpted.vertices.end());
        const core::Vec3d displacement_vector = central_displacement->displacement_vectors[corner];
        displacement_sample_is_nonzero = displacement_sample_is_nonzero ||
            displacement_vector.length_squared() > 0.0;
        const core::Vec3d reconstructed = source_vertex->position + displacement_vector;
        REQUIRE((evaluated_vertex->position - reconstructed).length() < 1e-9);
        REQUIRE(central_displacement->object_space_normal.length_squared() == 0.0);
    }
    REQUIRE(displacement_sample_is_nonzero);

    production::MultiresSettings normal_settings = displacement_settings;
    normal_settings.displacement_frequency_threshold = 0.5;
    const auto normal = production::compile_multires_surface_hierarchy(
        sculpted, source, normal_settings);
    REQUIRE(normal);
    const auto central_normal = std::find_if(
        normal.value().detail_classifications.begin(), normal.value().detail_classifications.end(),
        [](const auto& item) { return item.source_face == geometry::FaceId{5U}; });
    REQUIRE(central_normal != normal.value().detail_classifications.end());
    REQUIRE(central_normal->target == production::MultiresDetailTarget::normal_field);
    for (std::size_t corner = 0U; corner < central_normal->source_vertices.size(); ++corner) {
        REQUIRE(central_normal->displacement_vectors[corner].length_squared() == 0.0);
        REQUIRE(std::abs(central_normal->object_space_normal.length_squared() - 1.0) < 1e-9);
    }
    const auto central_normal_index = static_cast<std::size_t>(std::distance(
        normal.value().detail_classifications.begin(), central_normal));
    auto tampered_normal = normal.value();
    tampered_normal.detail_classifications[central_normal_index].object_space_normal =
        {1.0, 0.0, 0.0};
    REQUIRE(tampered_normal.refresh_digests());
    REQUIRE(!tampered_normal.validate(sculpted, source));

    production::MultiresSettings important_settings = displacement_settings;
    important_settings.runtime_importance = 1.0;
    const auto important = production::compile_multires_surface_hierarchy(
        sculpted, source, important_settings);
    REQUIRE(important);
    const auto central_geometry = std::find_if(
        important.value().detail_classifications.begin(),
        important.value().detail_classifications.end(), [](const auto& item) {
            return item.source_face == geometry::FaceId{5U};
        });
    REQUIRE(central_geometry != important.value().detail_classifications.end());
    REQUIRE(central_geometry->target == production::MultiresDetailTarget::geometry);

    REQUIRE(std::any_of(displacement.value().detail_classifications.begin(),
        displacement.value().detail_classifications.end(), [](const auto& item) {
            return item.boundary_sensitive &&
                item.target == production::MultiresDetailTarget::geometry;
        }));

    production::MultiresSettings invalid_settings = displacement_settings;
    invalid_settings.runtime_importance = 1.1;
    REQUIRE(!production::compile_multires_surface_hierarchy(
        sculpted, source, invalid_settings));
}

} // namespace

int main() {
    try {
        deterministic_levels_keep_seam_side_anchors_and_fail_closed();
        std::cout << "PASS deterministic multires hierarchy, seam-side provenance, stale-input and tamper rejection\n";
        face_budget_rejects_before_unbounded_subdivision();
        std::cout << "PASS bounded multires subdivision rejects excessive face growth\n";
        concave_source_fan_is_rejected_without_mutating_source();
        std::cout << "PASS concave polygons fail closed when fan triangulation would invert\n";
        published_hierarchy_is_content_addressed_and_rebuilt_from_current_source();
        std::cout << "PASS multires publication is content-addressed and stale-source bound\n";
        detail_frequency_importance_and_boundaries_select_conservative_targets();
        std::cout << "PASS detail frequency, runtime importance, and boundary policy classify migration targets\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL multires tests: " << error.what() << '\n';
        return 1;
    }
}
