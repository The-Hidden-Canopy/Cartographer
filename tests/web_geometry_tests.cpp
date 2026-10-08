#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/type.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/web_geometry/compiler.hpp>
#include <carto/web_geometry/diagnostics.hpp>
#include <carto/web_geometry/evaluation.hpp>
#include <carto/web_geometry/overlay.hpp>
#include <carto/web_geometry/page_interest.hpp>
#include <carto/web_geometry/package.hpp>
#include <carto/web_geometry/page_payload.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            throw TestFailure(std::string("requirement failed: ") + #condition);            \
        }                                                                                   \
    } while (false)

carto::web_geometry::WebGeometryPackage make_box_package() {
    auto editable = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(editable);
    const auto compiled = editable.value().compile();
    REQUIRE(compiled);
    const auto provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
        compiled.value());
    REQUIRE(provenance);
    const auto package = carto::web_geometry::compile_web_geometry(
        compiled.value(), {}, {}, provenance.value());
    if (!package) throw TestFailure(package.error().message);
    return package.value();
}

carto::geometry::EditableMesh two_triangle_mesh() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto d = mesh.add_vertex({1.0, 1.0, 0.0});
    REQUIRE(a && b && c && d);
    REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
    REQUIRE(mesh.add_face({b.value(), d.value(), c.value()}));
    return mesh;
}

void options_are_bounded_and_digest_deterministic() {
    carto::web_geometry::WebGeometryCompileOptions options;
    REQUIRE(options.validate());
    REQUIRE(options.digest() == options.digest());
    options.target_triangles_per_leaf = 0U;
    REQUIRE(!options.validate());
    options.target_triangles_per_leaf = 2U;
    options.hard_max_triangles_per_leaf = 1U;
    REQUIRE(!options.validate());
    options.hard_max_triangles_per_leaf = 2U;
    options.target_page_payload_bytes = 11U;
    REQUIRE(!options.validate());
}

void compiled_mesh_produces_deterministic_leaf_hierarchy() {
    auto editable = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(editable);
    const auto compiled = editable.value().compile();
    REQUIRE(compiled);
    const auto provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
        compiled.value());
    REQUIRE(provenance);
    const carto::attributes::AttributeSet attributes;
    carto::web_geometry::WebGeometryCompileOptions options;
    const auto first = carto::web_geometry::compile_web_geometry(
        compiled.value(), attributes, options, provenance.value());
    const auto second = carto::web_geometry::compile_web_geometry(
        compiled.value(), attributes, options, provenance.value());
    if (!first) throw TestFailure(first.error().message);
    if (!second) throw TestFailure(second.error().message);
    REQUIRE(first && second);
    REQUIRE(first.value().validate());
    REQUIRE(first.value().digest == second.value().digest);
    REQUIRE(first.value().hierarchy.digest == second.value().hierarchy.digest);
    if (first.value().hierarchy.clusters.size() != 7U) {
        throw TestFailure("unexpected cluster count: " +
                          std::to_string(first.value().hierarchy.clusters.size()));
    }
    REQUIRE(first.value().hierarchy.clusters.front().triangle_count == 2U);
    REQUIRE(first.value().provenance.size() == first.value().hierarchy.clusters.size());
    REQUIRE(!first.value().pages.empty());
    REQUIRE(!first.value().payload.empty());
    REQUIRE(first.value().pages.front().payload_bytes > 0U);
}

void leaf_hard_cap_and_material_boundaries_are_enforced() {
    auto editable = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(editable);
    const auto compiled = editable.value().compile();
    REQUIRE(compiled);
    const auto provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
        compiled.value());
    REQUIRE(provenance);
    carto::web_geometry::WebGeometryCompileOptions options;
    options.target_triangles_per_leaf = 2U;
    options.hard_max_triangles_per_leaf = 3U;
    const carto::attributes::AttributeSet empty_attributes;
    const auto capped = carto::web_geometry::compile_web_geometry(
        compiled.value(), empty_attributes, options, provenance.value());
    if (!capped) throw TestFailure(capped.error().message);
    REQUIRE(capped);
    REQUIRE(capped.value().hierarchy.clusters.size() > 2U);
    for (std::size_t index = 0U; index + 1U < capped.value().hierarchy.clusters.size(); ++index) {
        REQUIRE(capped.value().hierarchy.clusters[index].triangle_count <= 3U);
    }

    auto two_triangles = two_triangle_mesh();
    const auto two_compiled = two_triangles.compile();
    REQUIRE(two_compiled);
    const auto two_provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
        two_compiled.value());
    REQUIRE(two_provenance);
    carto::attributes::AttributeSet attributes;
    REQUIRE(attributes.set_domain_count(carto::attributes::AttributeDomain::face, 2U));
    REQUIRE(attributes.add_layer(carto::attributes::AttributeLayer{
        {"material.class_id", carto::attributes::AttributeDomain::face,
         carto::attributes::AttributeType::uint32},
        {std::uint32_t{1U}, std::uint32_t{2U}}}));
    carto::web_geometry::WebGeometryCompileOptions boundary_options;
    const auto split = carto::web_geometry::compile_web_geometry(
        two_compiled.value(), attributes, boundary_options, two_provenance.value());
    REQUIRE(split);
    REQUIRE(split.value().hierarchy.clusters.size() == 3U);
    REQUIRE(split.value().hierarchy.clusters[0].triangle_count == 1U);
    REQUIRE(split.value().hierarchy.clusters[1].triangle_count == 1U);

    carto::attributes::AttributeSet semantic_attributes;
    REQUIRE(semantic_attributes.set_domain_count(
        carto::attributes::AttributeDomain::face, 2U));
    REQUIRE(semantic_attributes.add_layer(carto::attributes::AttributeLayer{
        {"semantic.class_id", carto::attributes::AttributeDomain::face,
         carto::attributes::AttributeType::uint32},
        {std::uint32_t{3U}, std::uint32_t{4U}}}));
    const auto semantic_split = carto::web_geometry::compile_web_geometry(
        two_compiled.value(), semantic_attributes, boundary_options, two_provenance.value());
    REQUIRE(semantic_split);
    REQUIRE(semantic_split.value().hierarchy.clusters.size() == 3U);

    carto::attributes::AttributeSet locked_attributes;
    REQUIRE(locked_attributes.set_domain_count(
        carto::attributes::AttributeDomain::face, 2U));
    REQUIRE(locked_attributes.add_layer(carto::attributes::AttributeLayer{
        {"web.cluster.lock_group", carto::attributes::AttributeDomain::face,
         carto::attributes::AttributeType::uint32},
        {std::uint32_t{9U}, std::uint32_t{9U}}}));
    carto::web_geometry::WebGeometryCompileOptions locked_options;
    locked_options.target_triangles_per_leaf = 1U;
    locked_options.hard_max_triangles_per_leaf = 2U;
    const auto locked = carto::web_geometry::compile_web_geometry(
        two_compiled.value(), locked_attributes, locked_options, two_provenance.value());
    REQUIRE(locked);
    REQUIRE(locked.value().hierarchy.clusters.front().triangle_count == 2U);
}

void stale_or_incomplete_provenance_is_rejected() {
    auto editable = two_triangle_mesh();
    const auto compiled = editable.compile();
    if (!compiled) throw TestFailure(compiled.error().message);
    REQUIRE(compiled);
    const auto provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
        compiled.value());
    REQUIRE(provenance);
    auto stale = provenance.value();
    stale.source_revision = carto::core::Revision{999U};
    REQUIRE(!carto::web_geometry::compile_web_geometry(
        compiled.value(), {}, {}, stale));
    auto incomplete = provenance.value();
    incomplete.source_faces.pop_back();
    REQUIRE(!carto::web_geometry::compile_web_geometry(
        compiled.value(), {}, {}, incomplete));
}

void package_round_trip_and_corruption_are_bounded() {
    auto editable = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(editable);
    const auto compiled = editable.value().compile();
    REQUIRE(compiled);
    const auto provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
        compiled.value());
    REQUIRE(provenance);
    const auto compiled_package = carto::web_geometry::compile_web_geometry(
        compiled.value(), {}, {}, provenance.value());
    REQUIRE(compiled_package);
    const auto serialized = carto::web_geometry::serialize_package(compiled_package.value());
    REQUIRE(serialized);
    const auto reopened = carto::web_geometry::deserialize_package(serialized.value());
    REQUIRE(reopened);
    REQUIRE(reopened.value().digest == compiled_package.value().digest);
    REQUIRE(reopened.value().hierarchy.canonical() ==
            compiled_package.value().hierarchy.canonical());
    REQUIRE(reopened.value().payload == compiled_package.value().payload);
    REQUIRE(reopened.value().pages.size() == compiled_package.value().pages.size());
    const auto& page = reopened.value().pages.front();
    const auto decoded = carto::web_geometry::decode_runtime_page_payload(
        std::span<const std::uint8_t>{reopened.value().payload}.subspan(
            static_cast<std::size_t>(page.payload_offset),
            static_cast<std::size_t>(page.payload_bytes)));
    REQUIRE(decoded);
    REQUIRE(decoded.value().clusters.size() == page.cluster_count);
    std::vector<std::uint8_t> bad_page(
        reopened.value().payload.begin() + static_cast<std::ptrdiff_t>(page.payload_offset),
        reopened.value().payload.begin() + static_cast<std::ptrdiff_t>(
            page.payload_offset + page.payload_bytes));
    bad_page[4] = 2U;
    REQUIRE(!carto::web_geometry::decode_runtime_page_payload(bad_page));
    REQUIRE(!carto::web_geometry::deserialize_package(serialized.value() + " trailing"));
    std::string tampered = serialized.value();
    const auto marker = tampered.find("PACKAGE_DIGEST ");
    REQUIRE(marker != std::string::npos);
    tampered[marker + std::string("PACKAGE_DIGEST ").size()] =
        tampered[marker + std::string("PACKAGE_DIGEST ").size()] == '0' ? '1' : '0';
    REQUIRE(!carto::web_geometry::deserialize_package(tampered));
    std::string truncated = serialized.value();
    truncated.resize(truncated.size() - 8U);
    REQUIRE(!carto::web_geometry::deserialize_package(truncated));
}

void hierarchy_and_provenance_corruption_are_rejected() {
    const auto original = make_box_package();

    auto broken_root_link = original;
    broken_root_link.hierarchy.child_ids[0] =
        static_cast<carto::web_geometry::ClusterId>(
            broken_root_link.hierarchy.clusters.size() - 1U);
    REQUIRE(!broken_root_link.validate());

    auto broken_parent_bounds = original;
    auto& root = broken_parent_bounds.hierarchy.clusters.back();
    root.bounds.aabb.minimum.x = 0.0;
    REQUIRE(!broken_parent_bounds.validate());

    auto duplicated_triangle = original;
    duplicated_triangle.hierarchy.triangle_indices[1] =
        duplicated_triangle.hierarchy.triangle_indices[0];
    REQUIRE(!duplicated_triangle.validate());

    auto broken_provenance = original;
    broken_provenance.provenance.front().source_faces.front() =
        carto::geometry::FaceId{999'999'999U};
    REQUIRE(!broken_provenance.validate());

    auto broken_page_offset = original;
    broken_page_offset.hierarchy.clusters.front().page = 1U;
    carto::web_geometry::RuntimeGeometryPage page;
    page.id = 1U;
    page.first_cluster = 0U;
    page.cluster_count = 1U;
    page.payload_offset = std::numeric_limits<std::uint64_t>::max();
    page.payload_bytes = 0U;
    page.bounds = broken_page_offset.hierarchy.clusters.front().bounds.aabb;
    page.digest.bytes[0] = 1U;
    broken_page_offset.pages.push_back(page);
    REQUIRE(!broken_page_offset.refresh_digest());
    REQUIRE(!broken_page_offset.validate());
}

void evaluation_publication_is_typed_and_incremental() {
    const auto original = make_box_package();
    const auto first = carto::web_geometry::publish_evaluation(original);
    if (!first) throw TestFailure(first.error().message);
    REQUIRE(first.value().validate());
    REQUIRE(first.value().graph.node_count() == carto::web_geometry::kEvaluationStageCount);
    REQUIRE(first.value().graph.connection_count() ==
            carto::web_geometry::kEvaluationStageCount - 1U);
    REQUIRE(first.value().nodes.front().value == 1U);
    REQUIRE(first.value().nodes.back().value ==
            carto::web_geometry::kEvaluationStageCount);
    REQUIRE(!first.value().graph.topological_order().value().empty());

    auto editable = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(editable);
    const auto compiled = editable.value().compile();
    REQUIRE(compiled);
    const auto provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
        compiled.value());
    REQUIRE(provenance);
    carto::web_geometry::WebGeometryCompileOptions changed_options;
    changed_options.target_triangles_per_leaf = 1U;
    changed_options.hard_max_triangles_per_leaf = 1U;
    const auto changed = carto::web_geometry::compile_web_geometry(
        compiled.value(), {}, changed_options, provenance.value());
    REQUIRE(changed);
    const auto second = carto::web_geometry::publish_evaluation(changed.value());
    REQUIRE(second);
    const auto invalidation = carto::web_geometry::compute_invalidation(
        first.value(), second.value());
    REQUIRE(invalidation);
    REQUIRE(invalidation.value().stage_dirty(
        carto::web_geometry::EvaluationStage::leaf_clusters));
    REQUIRE(invalidation.value().stage_dirty(
        carto::web_geometry::EvaluationStage::hierarchy));
    REQUIRE(invalidation.value().stage_dirty(
        carto::web_geometry::EvaluationStage::page_pack));
    REQUIRE(invalidation.value().stage_dirty(
        carto::web_geometry::EvaluationStage::package));
    REQUIRE(!invalidation.value().stage_dirty(
        carto::web_geometry::EvaluationStage::compile_mesh));
    REQUIRE(!invalidation.value().clusters.empty());

    const auto unchanged = carto::web_geometry::compute_invalidation(
        first.value(), first.value());
    REQUIRE(unchanged);
    REQUIRE(unchanged.value().empty());

    auto forged = second.value();
    forged.output_digests.back().assign(64U, '0');
    REQUIRE(!carto::web_geometry::compute_invalidation(first.value(), forged));
}

void cluster_diagnostics_are_read_only_and_expose_open_parent_work() {
    const auto package = make_box_package();
    const auto diagnostics = carto::web_geometry::inspect_package(package);
    REQUIRE(diagnostics);
    REQUIRE(diagnostics.value().validate());
    REQUIRE(diagnostics.value().validate_against(package));
    REQUIRE(diagnostics.value().cluster_count == package.hierarchy.clusters.size());
    REQUIRE(diagnostics.value().leaf_count ==
            package.hierarchy.clusters.size() - diagnostics.value().grouping_count);
    REQUIRE(diagnostics.value().grouping_count == 1U);
    REQUIRE(diagnostics.value().error_receipts.size() ==
            diagnostics.value().cluster_count);
    REQUIRE(diagnostics.value().error_receipts.back().method ==
            "grouping-sphere-radius-v1");
    REQUIRE(diagnostics.value().error_receipts.back().conservative);
    REQUIRE(!diagnostics.value().error_receipts.back().renderable);
    REQUIRE(diagnostics.value().error_receipts.back().error_bound > 0.0);
    REQUIRE(!diagnostics.value().warnings.empty());
    REQUIRE(std::any_of(
        diagnostics.value().warnings.begin(),
        diagnostics.value().warnings.end(),
        [](const std::string& warning) {
            return warning.find("parent simplification") != std::string::npos;
        }));

    auto forged_receipt = diagnostics.value();
    forged_receipt.error_receipts.back().digest.assign(64U, '0');
    REQUIRE(forged_receipt.validate());
    REQUIRE(!forged_receipt.validate_against(package));

    auto forged = package;
    forged.hierarchy.clusters.front().triangle_count = 0U;
    REQUIRE(!carto::web_geometry::inspect_package(forged));
}

void cluster_overlay_projection_is_bounded_and_fail_closed() {
    auto editable = two_triangle_mesh();
    const auto compiled = editable.compile();
    REQUIRE(compiled);
    const auto provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
        compiled.value());
    REQUIRE(provenance);
    const auto package = carto::web_geometry::compile_web_geometry(
        compiled.value(), {}, {}, provenance.value());
    REQUIRE(package);

    carto::web_geometry::OverlayProjection identity;
    identity.values[0U] = 1.0;
    identity.values[5U] = 1.0;
    identity.values[10U] = 1.0;
    identity.values[15U] = 1.0;
    const auto projected = carto::web_geometry::project_cluster_overlays(
        package.value(), identity, {800U, 600U});
    REQUIRE(projected);
    REQUIRE(projected.value().validate());
    REQUIRE(projected.value().clusters.size() == package.value().hierarchy.clusters.size());
    for (const auto& cluster : projected.value().clusters) {
        REQUIRE(cluster.visible);
        REQUIRE(cluster.minimum_x == 400.0);
        REQUIRE(cluster.maximum_x == 800.0);
        REQUIRE(cluster.minimum_y == 0.0);
        REQUIRE(cluster.maximum_y == 300.0);
        REQUIRE(!cluster.clipped);
    }

    auto behind = identity;
    behind.values[15U] = -1.0;
    const auto hidden = carto::web_geometry::project_cluster_overlays(
        package.value(), behind, {800U, 600U});
    REQUIRE(hidden);
    for (const auto& cluster : hidden.value().clusters) {
        REQUIRE(!cluster.visible);
        REQUIRE(!cluster.clipped);
    }

    auto invalid_projection = identity;
    invalid_projection.values[0U] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!carto::web_geometry::project_cluster_overlays(
        package.value(), invalid_projection, {800U, 600U}));
    REQUIRE(!carto::web_geometry::project_cluster_overlays(
        package.value(), identity, {0U, 600U}));
}

void runtime_page_interest_is_package_bound_and_deterministic() {
    auto editable = two_triangle_mesh();
    const auto compiled = editable.compile();
    REQUIRE(compiled);
    const auto provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
        compiled.value());
    REQUIRE(provenance);
    const auto package = carto::web_geometry::compile_web_geometry(
        compiled.value(), {}, {}, provenance.value());
    REQUIRE(package);

    carto::web_geometry::OverlayProjection identity;
    identity.values[0U] = 1.0;
    identity.values[5U] = 1.0;
    identity.values[10U] = 1.0;
    identity.values[15U] = 1.0;
    const auto overlays = carto::web_geometry::project_cluster_overlays(
        package.value(), identity, {800U, 600U});
    REQUIRE(overlays);

    const auto plan = carto::web_geometry::plan_runtime_page_interest(
        package.value(), overlays.value());
    REQUIRE(plan);
    REQUIRE(plan.value().validate());
    REQUIRE(plan.value().visible_clusters.size() == overlays.value().clusters.size());
    REQUIRE(!plan.value().pages.empty());

    std::vector<carto::web_geometry::RuntimePageId> expected_pages;
    for (const auto& cluster : overlays.value().clusters) {
        if (cluster.visible && cluster.page != carto::web_geometry::kUnassignedRuntimePage) {
            expected_pages.push_back(cluster.page);
        }
    }
    std::sort(expected_pages.begin(), expected_pages.end());
    expected_pages.erase(
        std::unique(expected_pages.begin(), expected_pages.end()), expected_pages.end());
    REQUIRE(plan.value().pages == expected_pages);

    auto forged = overlays.value();
    forged.clusters.front().error_receipt_digest.assign(64U, '0');
    REQUIRE(!carto::web_geometry::plan_runtime_page_interest(package.value(), forged));

    auto hidden = overlays.value();
    for (auto& cluster : hidden.clusters) cluster.visible = false;
    const auto empty_plan = carto::web_geometry::plan_runtime_page_interest(
        package.value(), hidden);
    REQUIRE(empty_plan);
    REQUIRE(empty_plan.value().pages.empty());
    REQUIRE(empty_plan.value().visible_clusters.empty());

    auto stale = overlays.value();
    stale.source_revision = carto::core::Revision(999U);
    REQUIRE(!carto::web_geometry::plan_runtime_page_interest(package.value(), stale));
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"web geometry options are bounded", options_are_bounded_and_digest_deterministic},
        {"web geometry hierarchy is deterministic", compiled_mesh_produces_deterministic_leaf_hierarchy},
        {"web geometry caps and boundaries are enforced", leaf_hard_cap_and_material_boundaries_are_enforced},
        {"web geometry stale provenance is rejected", stale_or_incomplete_provenance_is_rejected},
        {"web geometry package round trip is bounded", package_round_trip_and_corruption_are_bounded},
        {"web geometry structural corruption is rejected",
         hierarchy_and_provenance_corruption_are_rejected},
        {"web geometry evaluation publication is incremental",
         evaluation_publication_is_typed_and_incremental},
        {"web geometry cluster diagnostics expose open work",
         cluster_diagnostics_are_read_only_and_expose_open_parent_work},
        {"web geometry cluster overlays project safely",
         cluster_overlay_projection_is_bounded_and_fail_closed},
        {"web geometry page interest is package bound",
         runtime_page_interest_is_package_bound_and_deterministic},
    };
    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
