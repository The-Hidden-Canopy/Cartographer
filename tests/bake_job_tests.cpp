#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/production/bake_job.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stop_token>
#include <stdexcept>
#include <utility>

namespace {

#define REQUIRE(condition) \
    do { \
        if (!(condition)) throw std::runtime_error(#condition); \
    } while (false)

carto::geometry::EditableMesh make_triangle(double z, bool with_uvs) {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, z}},
         {geometry::VertexId{2U}, {1.0, 0.0, z}},
         {geometry::VertexId{3U}, {0.0, 1.0, z}}},
        {{geometry::FaceId{1U}, {
            geometry::VertexId{1U}, geometry::VertexId{2U}, geometry::VertexId{3U}}}}));
    if (with_uvs) {
        attributes::AttributeSet values({
            {attributes::AttributeDomain::corner, 3U}});
        REQUIRE(values.add_layer(attributes::AttributeLayer{
            {"uv0.corners", attributes::AttributeDomain::corner,
                attributes::AttributeType::vec2},
            {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0}, core::Vec2d{0.0, 1.0}}}));
        attributes::UvSetTable uv_sets;
        REQUIRE(uv_sets.add(attributes::UvSetDescriptor{
            7U, "UV0", "uv0.corners", true, true, false, 1U}));
        REQUIRE(mesh.set_attribute_payload(std::move(values), std::move(uv_sets)));
    }
    REQUIRE(mesh.validate());
    return mesh;
}

carto::production::BakeRecipe make_recipe(
    const carto::geometry::EditableMesh& source,
    const carto::geometry::EditableMesh& target) {
    using namespace carto;
    const auto source_binding = production::bind_bake_source("scene/source", source);
    const auto target_binding = production::bind_bake_target("scene/target", target, 7U);
    REQUIRE(source_binding);
    REQUIRE(target_binding);
    production::BakeRecipe recipe;
    recipe.identity = "bake/target/normal";
    recipe.source = source_binding.value();
    recipe.target = target_binding.value();
    recipe.target_uv_set = 7U;
    recipe.width = 32U;
    recipe.height = 32U;
    recipe.projection_half_extent = 0.1;
    REQUIRE(recipe.validate());
    return recipe;
}

void background_output_matches_reference_and_uses_mesh_snapshots() {
    using namespace carto;
    auto source = make_triangle(0.05, false);
    auto target = make_triangle(0.0, true);
    const auto recipe = make_recipe(source, target);
    const auto direct = production::compile_tangent_normal_map_cpu(
        recipe, source, target);
    REQUIRE(direct.succeeded());

    auto started = production::BakeJob::start(
        recipe, production::BakeNormalSpace::tangent, source, target);
    REQUIRE(started);
    auto job = std::move(started.value());
    REQUIRE(!job->report().has_value());

    // The job must retain the meshes as they were when start() returned.
    REQUIRE(source.set_vertex_position(geometry::VertexId{1U}, {0.0, 0.0, 0.08}));
    REQUIRE(target.set_vertex_position(geometry::VertexId{1U}, {0.0, 0.0, 0.02}));
    job->wait();

    const auto snapshot = job->snapshot();
    const auto report = job->report();
    REQUIRE(snapshot.state == production::BakeJobState::completed);
    REQUIRE(snapshot.recipe_identity == recipe.identity);
    REQUIRE(report.has_value() && report->succeeded());
    REQUIRE(report->artifact.has_value());
    const auto direct_bytes = direct.artifact->serialize();
    const auto background_bytes = report->artifact->serialize();
    REQUIRE(direct_bytes && background_bytes);
    REQUIRE(direct_bytes.value() == background_bytes.value());
    REQUIRE(!job->request_cancel());

    const auto object_direct = production::compile_object_space_normal_map_cpu(
        recipe, make_triangle(0.05, false), make_triangle(0.0, true));
    REQUIRE(object_direct.succeeded());
    auto object_started = production::BakeJob::start(
        recipe,
        production::BakeNormalSpace::object,
        make_triangle(0.05, false),
        make_triangle(0.0, true));
    REQUIRE(object_started);
    auto object_job = std::move(object_started.value());
    object_job->wait();
    const auto object_report = object_job->report();
    REQUIRE(object_job->snapshot().state == production::BakeJobState::completed);
    REQUIRE(object_report.has_value() && object_report->succeeded());
    const auto object_direct_bytes = object_direct.artifact->serialize();
    const auto object_background_bytes = object_report->artifact->serialize();
    REQUIRE(object_direct_bytes && object_background_bytes);
    REQUIRE(object_direct_bytes.value() == object_background_bytes.value());
}

void in_flight_cancellation_suppresses_artifact() {
    using namespace carto;
    const auto source = make_triangle(0.05, false);
    const auto target = make_triangle(0.0, true);
    auto recipe = make_recipe(source, target);
    recipe.width = 2048U;
    recipe.height = 2048U;

    auto started = production::BakeJob::start(
        recipe, production::BakeNormalSpace::tangent, source, target);
    REQUIRE(started);
    auto job = std::move(started.value());
    REQUIRE(job->request_cancel());
    job->wait();

    REQUIRE(job->snapshot().state == production::BakeJobState::cancelled);
    const auto report = job->report();
    REQUIRE(report.has_value() && !report->succeeded());
    REQUIRE(!report->artifact.has_value());
    REQUIRE(std::any_of(report->diagnostics.begin(), report->diagnostics.end(),
        [](const production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "bake_cancelled";
        }));
}

void cancellation_is_failure_closed() {
    using namespace carto;
    const auto source = make_triangle(0.05, false);
    const auto target = make_triangle(0.0, true);
    const auto recipe = make_recipe(source, target);
    std::stop_source external_stop;
    external_stop.request_stop();

    auto started = production::BakeJob::start(
        recipe,
        production::BakeNormalSpace::tangent,
        source,
        target,
        external_stop.get_token());
    REQUIRE(started);
    auto job = std::move(started.value());
    job->wait();

    const auto snapshot = job->snapshot();
    const auto report = job->report();
    REQUIRE(snapshot.state == production::BakeJobState::cancelled);
    REQUIRE(report.has_value() && !report->succeeded());
    REQUIRE(!report->artifact.has_value());
    REQUIRE(snapshot.diagnostic_count == report->diagnostics.size());
    REQUIRE(std::any_of(report->diagnostics.begin(), report->diagnostics.end(),
        [](const production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "bake_cancelled" && diagnostic.is_error;
        }));
    REQUIRE(!job->request_cancel());
}

void invalid_recipe_finishes_failed_without_artifact() {
    using namespace carto;
    const auto source = make_triangle(0.05, false);
    const auto target = make_triangle(0.0, true);
    auto recipe = make_recipe(source, target);
    recipe.width = 0U;

    auto started = production::BakeJob::start(
        recipe, production::BakeNormalSpace::tangent, source, target);
    REQUIRE(started);
    auto job = std::move(started.value());
    job->wait();

    REQUIRE(job->snapshot().state == production::BakeJobState::failed);
    const auto report = job->report();
    REQUIRE(report.has_value() && !report->succeeded());
    REQUIRE(!report->artifact.has_value());
}

} // namespace

int main() {
    try {
        background_output_matches_reference_and_uses_mesh_snapshots();
        in_flight_cancellation_suppresses_artifact();
        cancellation_is_failure_closed();
        invalid_recipe_finishes_failed_without_artifact();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "bake job tests passed\n";
    return EXIT_SUCCESS;
}
