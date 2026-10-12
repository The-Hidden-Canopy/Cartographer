#include <carto/assets/blob_store.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/production/bake_ray_worker.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <optional>
#include <span>
#include <vector>

namespace {

#define REQUIRE(condition) \
    do { \
        if (!(condition)) throw std::runtime_error(#condition); \
    } while (false)

using carto::attributes::AttributeDomain;
using carto::attributes::AttributeLayer;
using carto::attributes::AttributeSet;
using carto::attributes::AttributeType;
using carto::attributes::UvSetDescriptor;
using carto::attributes::UvSetTable;
using carto::core::Vec2d;
using carto::core::Vec3d;
using carto::geometry::EditableMesh;
using carto::geometry::Face;
using carto::geometry::FaceId;
using carto::geometry::Vertex;
using carto::geometry::VertexId;
using carto::production::BakeRayEvidenceClass;
using carto::production::BakeRayQueryCompileResult;
using carto::production::BakeRayQueryOutcome;
using carto::production::BakeRayQueryRay;
using carto::production::BakeRayQueryReport;
using carto::production::BakeRayQueryRequest;
using carto::production::BakeRayQueryTriangle;
using carto::production::BakeRecipe;

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-bake-ray-worker-tests-" + std::to_string(stamp) + "-" +
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

struct Fixture {
    EditableMesh source;
    EditableMesh target;
    BakeRecipe recipe;
};

EditableMesh make_triangle(bool with_uv) {
    EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{VertexId{1U}, {0.0, 0.0, 0.0}},
         {VertexId{2U}, {1.0, 0.0, 0.0}},
         {VertexId{3U}, {0.0, 1.0, 0.0}}},
        {{FaceId{1U}, {VertexId{1U}, VertexId{2U}, VertexId{3U}}}}));
    if (with_uv) {
        AttributeSet attributes({{AttributeDomain::corner, 3U}});
        REQUIRE(attributes.add_layer(AttributeLayer{
            {"uv0.corners", AttributeDomain::corner, AttributeType::vec2},
            {Vec2d{0.0, 0.0}, Vec2d{1.0, 0.0}, Vec2d{0.0, 1.0}}}));
        UvSetTable sets;
        REQUIRE(sets.add(UvSetDescriptor{
            7U, "UV0", "uv0.corners", true, true, false, 1U}));
        REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(sets)));
    }
    REQUIRE(mesh.validate());
    return mesh;
}

Fixture make_fixture() {
    Fixture fixture{make_triangle(false), make_triangle(true), {}};
    const auto source = carto::production::bind_bake_source("ray-test/source", fixture.source);
    const auto target = carto::production::bind_bake_target(
        "ray-test/target", fixture.target, 7U);
    REQUIRE(source && target);
    fixture.recipe.identity = "ray-test/normal-map";
    fixture.recipe.source = source.value();
    fixture.recipe.target = target.value();
    fixture.recipe.target_uv_set = 7U;
    fixture.recipe.width = 8U;
    fixture.recipe.height = 8U;
    fixture.recipe.projection_half_extent = 0.5;
    REQUIRE(fixture.recipe.validate());
    return fixture;
}

Fixture make_parallel_layers_fixture() {
    Fixture fixture = make_fixture();
    EditableMesh source;
    REQUIRE(source.insert_bulk(
        {{VertexId{1U}, {0.0, 0.0, 0.0}},
         {VertexId{2U}, {1.0, 0.0, 0.0}},
         {VertexId{3U}, {0.0, 1.0, 0.0}},
         {VertexId{4U}, {0.0, 0.0, -0.1}},
         {VertexId{5U}, {1.0, 0.0, -0.1}},
         {VertexId{6U}, {0.0, 1.0, -0.1}}},
        {{FaceId{1U}, {VertexId{1U}, VertexId{2U}, VertexId{3U}}},
         {FaceId{2U}, {VertexId{4U}, VertexId{5U}, VertexId{6U}}}}));
    REQUIRE(source.validate());
    fixture.source = std::move(source);
    const auto binding = carto::production::bind_bake_source(
        "ray-test/source", fixture.source);
    REQUIRE(binding);
    fixture.recipe.source = binding.value();
    REQUIRE(fixture.recipe.validate());
    return fixture;
}

carto::assets::Sha256Digest text_digest(std::string_view value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
    return carto::assets::sha256(
        std::span<const std::uint8_t>{bytes, value.size()});
}

struct Intersection {
    double distance = 0.0;
    std::array<double, 3U> barycentric{};
    bool front_facing = true;
};

std::optional<Intersection> intersect(
    const BakeRayQueryRay& ray,
    const BakeRayQueryTriangle& triangle) {
    const Vec3d edge_one = triangle.positions[1U] - triangle.positions[0U];
    const Vec3d edge_two = triangle.positions[2U] - triangle.positions[0U];
    const Vec3d p = carto::core::cross(ray.direction, edge_two);
    const double determinant = carto::core::dot(edge_one, p);
    if (std::abs(determinant) <= 1e-12) return std::nullopt;
    const double inverse = 1.0 / determinant;
    const Vec3d from_first = ray.origin - triangle.positions[0U];
    const double u = carto::core::dot(from_first, p) * inverse;
    if (u < -1e-12 || u > 1.0 + 1e-12) return std::nullopt;
    const Vec3d q = carto::core::cross(from_first, edge_one);
    const double v = carto::core::dot(ray.direction, q) * inverse;
    if (v < -1e-12 || u + v > 1.0 + 1e-12) return std::nullopt;
    const double distance = carto::core::dot(edge_two, q) * inverse;
    if (distance < 0.0 || distance > ray.maximum_distance) return std::nullopt;
    return Intersection{distance, {1.0 - u - v, u, v}, determinant > 0.0};
}

BakeRayQueryReport make_worker_report(const BakeRayQueryRequest& request) {
    BakeRayQueryReport report;
    report.worker_version = 1U;
    report.evidence_class = BakeRayEvidenceClass::simulated;
    report.worker_id = "carto.test.closed-form-ray-worker";
    report.worker_build_digest = text_digest("closed-form-ray-worker-test-build-v1");
    report.success = true;
    report.outcomes.reserve(request.rays.size());
    for (const BakeRayQueryRay& ray : request.rays) {
        BakeRayQueryOutcome outcome;
        outcome.sample_index = ray.sample_index;
        outcome.target_face_id = ray.target_face_id;
        outcome.texel_x = ray.texel_x;
        outcome.texel_y = ray.texel_y;
        std::optional<Intersection> closest;
        const BakeRayQueryTriangle* selected = nullptr;
        for (const BakeRayQueryTriangle& triangle : request.triangles) {
            const auto candidate = intersect(ray, triangle);
            if (candidate && (!closest || candidate->distance < closest->distance)) {
                closest = candidate;
                selected = &triangle;
            }
        }
        if (closest != std::nullopt && selected != nullptr) {
            outcome.hit = true;
            outcome.source_face_id = selected->face_id;
            outcome.source_fan_index = selected->fan_index;
            outcome.backface = !closest->front_facing;
            outcome.distance = closest->distance;
            outcome.barycentric = closest->barycentric;
        }
        report.outcomes.push_back(outcome);
    }
    REQUIRE(carto::production::refresh_bake_ray_query_report(request, report));
    return report;
}

BakeRayQueryCompileResult compile_valid(Fixture& fixture) {
    return carto::production::compile_tangent_normal_map_with_ray_worker(
        fixture.recipe, fixture.source, fixture.target,
        [](const BakeRayQueryRequest& request) { return make_worker_report(request); });
}

void tangent_worker_matches_cpu_and_publishes_under_derived_profile() {
    Fixture fixture = make_fixture();
    const auto compiled = compile_valid(fixture);
    REQUIRE(compiled.succeeded());
    REQUIRE(compiled.worker.matches_request(compiled.request));
    REQUIRE(compiled.bake.artifact.has_value());
    REQUIRE(compiled.cpu_reference_artifact_digest ==
        compiled.bake.artifact->artifact_digest);
    REQUIRE(!compiled.request.rays.empty());
    REQUIRE(std::is_sorted(compiled.request.rays.begin(), compiled.request.rays.end(),
        [](const BakeRayQueryRay& left, const BakeRayQueryRay& right) {
            return std::tuple{left.target_face_id, left.texel_y, left.texel_x,
                left.sample_index} <
                std::tuple{right.target_face_id, right.texel_y, right.texel_x,
                    right.sample_index};
        }));

    const auto profile = carto::production::bake_ray_worker_platform_profile(
        compiled.worker);
    REQUIRE(profile);
    const auto key = carto::production::bake_production_cook_key(
        fixture.recipe, carto::production::BakeNormalSpace::tangent, profile.value());
    REQUIRE(key);
    const auto payload = compiled.bake.artifact->serialize();
    REQUIRE(payload);

    TempDirectory directory;
    carto::assets::BlobStore blobs(directory.path() / "blobs");
    carto::production::ProductionArtifactStore store(blobs);
    carto::production::ProductionArtifactReceipt receipt;
    receipt.key = key.value();
    receipt.status = carto::production::ReceiptStatus::admitted;
    receipt.output_digest = carto::assets::sha256(payload.value());
    receipt.output_bytes = static_cast<std::uint64_t>(payload.value().size());
    receipt.quality.confidence = 1.0;
    REQUIRE(receipt.refresh_digest());
    carto::production::AdmissionPolicy policy;
    REQUIRE(carto::production::publish_tangent_normal_map_with_ray_worker(
        store, receipt, compiled, fixture.source, fixture.target, policy));
    REQUIRE(store.ledger().size() == 1U);

    auto wrong_profile_receipt = receipt;
    const auto cpu_key = carto::production::bake_production_cook_key(fixture.recipe);
    REQUIRE(cpu_key);
    wrong_profile_receipt.key = cpu_key.value();
    REQUIRE(wrong_profile_receipt.refresh_digest());
    REQUIRE(!carto::production::publish_tangent_normal_map_with_ray_worker(
        store, wrong_profile_receipt, compiled,
        fixture.source, fixture.target, policy));
    REQUIRE(store.ledger().size() == 1U);
}

void ray_worker_rejects_bad_provenance_order_and_geometry() {
    Fixture fixture = make_fixture();
    const auto bad_digest = carto::production::compile_tangent_normal_map_with_ray_worker(
        fixture.recipe, fixture.source, fixture.target,
        [](const BakeRayQueryRequest& request) {
            auto report = make_worker_report(request);
            report.request_digest.bytes[0U] ^= 0x80U;
            return report;
        });
    REQUIRE(!bad_digest.succeeded());
    REQUIRE(!bad_digest.bake.artifact.has_value());

    const auto bad_order = carto::production::compile_tangent_normal_map_with_ray_worker(
        fixture.recipe, fixture.source, fixture.target,
        [](const BakeRayQueryRequest& request) {
            auto report = make_worker_report(request);
            REQUIRE(report.outcomes.size() > 1U);
            std::swap(report.outcomes[0U], report.outcomes[1U]);
            REQUIRE(carto::production::refresh_bake_ray_query_report(request, report));
            return report;
        });
    REQUIRE(!bad_order.succeeded());

    const auto false_hit = carto::production::compile_tangent_normal_map_with_ray_worker(
        fixture.recipe, fixture.source, fixture.target,
        [](const BakeRayQueryRequest& request) {
            auto report = make_worker_report(request);
            report.outcomes[0U].barycentric = {1.0, 0.0, 0.0};
            REQUIRE(carto::production::refresh_bake_ray_query_report(request, report));
            return report;
        });
    REQUIRE(!false_hit.succeeded());
    REQUIRE(!false_hit.worker.matches_request(false_hit.request));

    const auto provenance_compile = compile_valid(fixture);
    auto changed_provenance = provenance_compile.worker;
    changed_provenance.worker_id += ".spoofed";
    REQUIRE(!changed_provenance.matches_request(
        provenance_compile.request));
    REQUIRE(!carto::production::bake_ray_worker_platform_profile(changed_provenance));

    const auto foreign_triangle = carto::production::compile_tangent_normal_map_with_ray_worker(
        fixture.recipe, fixture.source, fixture.target,
        [](const BakeRayQueryRequest& request) {
            auto report = make_worker_report(request);
            report.outcomes[0U].source_face_id = 999U;
            REQUIRE(carto::production::refresh_bake_ray_query_report(request, report));
            return report;
        });
    REQUIRE(!foreign_triangle.succeeded());

    Fixture layered = make_parallel_layers_fixture();
    const auto farther_same_normal =
        carto::production::compile_tangent_normal_map_with_ray_worker(
            layered.recipe, layered.source, layered.target,
            [](const BakeRayQueryRequest& request) {
                auto report = make_worker_report(request);
                for (BakeRayQueryOutcome& outcome : report.outcomes) {
                    outcome.source_face_id = 2U;
                    outcome.distance += 0.1;
                }
                REQUIRE(carto::production::refresh_bake_ray_query_report(request, report));
                REQUIRE(report.matches_request(request));
                return report;
            });
    REQUIRE(!farther_same_normal.succeeded());
    REQUIRE(std::any_of(farther_same_normal.bake.diagnostics.begin(),
        farther_same_normal.bake.diagnostics.end(),
        [](const carto::production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "ray_worker_cpu_projection_mismatch";
        }));

    const auto valid_layered = compile_valid(layered);
    REQUIRE(valid_layered.succeeded());
    auto forged_after_compile = valid_layered;
    for (BakeRayQueryOutcome& outcome : forged_after_compile.worker.outcomes) {
        if (!outcome.hit) continue;
        outcome.source_face_id = 2U;
        outcome.distance += 0.1;
    }
    REQUIRE(carto::production::refresh_bake_ray_query_report(
        forged_after_compile.request, forged_after_compile.worker));
    REQUIRE(forged_after_compile.worker.matches_request(forged_after_compile.request));
    REQUIRE(!forged_after_compile.succeeded());

    TempDirectory forged_directory;
    carto::assets::BlobStore forged_blobs(forged_directory.path() / "blobs");
    carto::production::ProductionArtifactStore forged_store(forged_blobs);
    carto::production::AdmissionPolicy forged_policy;
    REQUIRE(!carto::production::publish_tangent_normal_map_with_ray_worker(
        forged_store, {}, forged_after_compile,
        layered.source, layered.target, forged_policy));
    REQUIRE(forged_store.ledger().size() == 0U);

    const auto thrown = carto::production::compile_tangent_normal_map_with_ray_worker(
        fixture.recipe, fixture.source, fixture.target,
        [](const BakeRayQueryRequest&) -> BakeRayQueryReport {
            throw std::runtime_error("injected worker failure");
        });
    REQUIRE(!thrown.succeeded());
    REQUIRE(std::any_of(thrown.bake.diagnostics.begin(), thrown.bake.diagnostics.end(),
        [](const carto::production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "ray_worker_exception";
        }));
}

void worker_completion_revalidates_stale_meshes_and_publication_is_fail_closed() {
    Fixture fixture = make_fixture();
    const auto stale = carto::production::compile_tangent_normal_map_with_ray_worker(
        fixture.recipe, fixture.source, fixture.target,
        [&fixture](const BakeRayQueryRequest& request) {
            auto report = make_worker_report(request);
            REQUIRE(fixture.source.set_vertex_position(VertexId{1U}, {0.0, 0.0, -0.1}));
            return report;
        });
    REQUIRE(!stale.succeeded());
    REQUIRE(!stale.bake.artifact.has_value());

    Fixture clean = make_fixture();
    const auto valid = compile_valid(clean);
    REQUIRE(valid.succeeded());
    const auto profile = carto::production::bake_ray_worker_platform_profile(valid.worker);
    REQUIRE(profile);
    const auto key = carto::production::bake_production_cook_key(
        clean.recipe, carto::production::BakeNormalSpace::tangent, profile.value());
    REQUIRE(key);
    const auto payload = valid.bake.artifact->serialize();
    REQUIRE(payload);
    carto::production::ProductionArtifactReceipt receipt;
    receipt.key = key.value();
    receipt.status = carto::production::ReceiptStatus::admitted;
    receipt.output_digest = carto::assets::sha256(payload.value());
    receipt.output_bytes = static_cast<std::uint64_t>(payload.value().size());
    REQUIRE(receipt.refresh_digest());

    TempDirectory directory;
    carto::assets::BlobStore blobs(directory.path() / "blobs");
    carto::production::ProductionArtifactStore store(blobs);
    carto::production::AdmissionPolicy policy;
    auto changed_source = clean.source;
    REQUIRE(changed_source.set_vertex_position(VertexId{1U}, {0.0, 0.0, -0.1}));
    REQUIRE(!carto::production::publish_tangent_normal_map_with_ray_worker(
        store, receipt, valid, changed_source, clean.target, policy));
    REQUIRE(store.ledger().size() == 0U);

    const auto failed = carto::production::compile_tangent_normal_map_with_ray_worker(
        clean.recipe, clean.source, clean.target,
        [](const BakeRayQueryRequest& request) {
            auto report = make_worker_report(request);
            report.success = false;
            report.error = "worker declined the request";
            return report;
        });
    REQUIRE(!failed.succeeded());
    REQUIRE(!carto::production::publish_tangent_normal_map_with_ray_worker(
        store, receipt, failed, clean.source, clean.target, policy));
    REQUIRE(store.ledger().size() == 0U);
}

} // namespace

int main() {
    try {
        tangent_worker_matches_cpu_and_publishes_under_derived_profile();
        ray_worker_rejects_bad_provenance_order_and_geometry();
        worker_completion_revalidates_stale_meshes_and_publication_is_fail_closed();
        std::cout << "Cartographer bake ray worker contract tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Cartographer bake ray worker contract test failure: "
                  << exception.what() << '\n';
        return 1;
    }
}
