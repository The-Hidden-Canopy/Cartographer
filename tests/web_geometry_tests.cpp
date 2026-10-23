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

carto::geometry::EditableMesh single_triangle_mesh() {
    carto::geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c);
    REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
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
    const auto& parent_lod = first.value().hierarchy.clusters.back();
    REQUIRE(parent_lod.child_count > 0U);
    REQUIRE(parent_lod.triangle_count > 0U);
    REQUIRE(parent_lod.triangle_count < first.value().hierarchy.source_faces.size());
    REQUIRE(parent_lod.geometric_error > 0.0);
    REQUIRE(parent_lod.geometric_error == 2.0 * parent_lod.bounds.sphere_radius);
    REQUIRE(parent_lod.page != carto::web_geometry::kUnassignedRuntimePage);
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
    REQUIRE(split.value().hierarchy.clusters.back().triangle_count == 0U);

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
    REQUIRE(semantic_split.value().hierarchy.clusters.back().triangle_count == 0U);

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

void consumer_package_admission_is_revision_bound_and_budgeted() {
    const auto package = make_box_package();
    const auto serialized = carto::web_geometry::serialize_package(package);
    REQUIRE(serialized);

    carto::web_geometry::WebGeometryImportBudget budget;
    budget.expected_source_revision = package.source_revision;
    budget.expected_options_digest = package.options.digest();
    budget.max_clusters = package.hierarchy.clusters.size();
    budget.max_triangles = package.hierarchy.triangle_indices.size();
    budget.max_pages = package.pages.size();
    budget.max_payload_bytes = package.payload.size();
    budget.max_serialized_bytes = serialized.value().size();
    const auto admitted = carto::web_geometry::admit_package_for_consumer(
        serialized.value(), budget);
    REQUIRE(admitted);
    REQUIRE(admitted.value().package.digest == package.digest);
    REQUIRE(admitted.value().receipt.package_digest == package.digest);
    REQUIRE(admitted.value().receipt.source_revision == package.source_revision);
    REQUIRE(admitted.value().receipt.options_digest == package.options.digest());
    REQUIRE(admitted.value().receipt.cluster_count == package.hierarchy.clusters.size());
    REQUIRE(admitted.value().receipt.triangle_count == package.hierarchy.triangle_indices.size());
    REQUIRE(admitted.value().receipt.payload_bytes == package.payload.size());
    const auto reserialized = carto::web_geometry::serialize_package(admitted.value().package);
    REQUIRE(reserialized && reserialized.value() == serialized.value());

    carto::web_geometry::WebGeometryRuntimeBudget runtime_budget;
    runtime_budget.device_name = "fixture GPU (unit test only)";
    runtime_budget.driver = "fixture driver (unit test only)";
    runtime_budget.viewport_width = 1280U;
    runtime_budget.viewport_height = 720U;
    runtime_budget.max_clusters = package.hierarchy.clusters.size();
    runtime_budget.max_triangles = package.hierarchy.triangle_indices.size();
    runtime_budget.max_pages = package.pages.size();
    runtime_budget.max_resident_payload_bytes = package.payload.size();
    const auto runtime_admitted = carto::web_geometry::admit_package_for_runtime(
        serialized.value(), budget, runtime_budget);
    REQUIRE(runtime_admitted);
    REQUIRE(runtime_admitted.value().runtime_receipt.validate());
    REQUIRE(runtime_admitted.value().runtime_receipt.device_name == runtime_budget.device_name);
    REQUIRE(runtime_admitted.value().runtime_receipt.driver == runtime_budget.driver);
    REQUIRE(runtime_admitted.value().runtime_receipt.viewport_width == 1280U);
    REQUIRE(runtime_admitted.value().runtime_receipt.cluster_count ==
            package.hierarchy.clusters.size());

    auto runtime_over_budget = runtime_budget;
    --runtime_over_budget.max_clusters;
    REQUIRE(!carto::web_geometry::admit_package_for_runtime(
        serialized.value(), budget, runtime_over_budget));
    runtime_over_budget = runtime_budget;
    --runtime_over_budget.max_resident_payload_bytes;
    REQUIRE(!carto::web_geometry::admit_package_for_runtime(
        serialized.value(), budget, runtime_over_budget));
    auto invalid_runtime_budget = runtime_budget;
    invalid_runtime_budget.device_name.clear();
    REQUIRE(!carto::web_geometry::admit_package_for_runtime(
        serialized.value(), budget, invalid_runtime_budget));
    auto forged_runtime_receipt = runtime_admitted.value().runtime_receipt;
    forged_runtime_receipt.driver = "different driver";
    REQUIRE(!forged_runtime_receipt.validate());

    auto stale_revision = budget;
    stale_revision.expected_source_revision =
        carto::core::Revision{package.source_revision.value() + 1U};
    const auto stale = carto::web_geometry::admit_package_for_consumer(
        serialized.value(), stale_revision);
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);

    auto stale_options = budget;
    auto changed_options = package.options;
    ++changed_options.target_triangles_per_leaf;
    stale_options.expected_options_digest = changed_options.digest();
    const auto option_mismatch = carto::web_geometry::admit_package_for_consumer(
        serialized.value(), stale_options);
    REQUIRE(!option_mismatch);
    REQUIRE(option_mismatch.error().code == carto::core::ErrorCode::stale_data);

    auto over_budget = budget;
    --over_budget.max_clusters;
    const auto too_many_clusters = carto::web_geometry::admit_package_for_consumer(
        serialized.value(), over_budget);
    REQUIRE(!too_many_clusters);

    auto too_large = budget;
    too_large.max_serialized_bytes = serialized.value().size() - 1U;
    REQUIRE(!carto::web_geometry::admit_package_for_consumer(
        serialized.value(), too_large));

    REQUIRE(!carto::web_geometry::admit_package_for_consumer(
        serialized.value() + "\n", budget));
    REQUIRE(!carto::web_geometry::admit_package_for_consumer(
        serialized.value() + "invalid", budget));

    auto invalid_budget = budget;
    invalid_budget.expected_source_revision = carto::core::Revision{};
    REQUIRE(!carto::web_geometry::admit_package_for_consumer(
        serialized.value(), invalid_budget));
}

void hierarchy_and_provenance_corruption_are_rejected() {
    const auto original = make_box_package();

    auto underbounded_parent_lod = original;
    underbounded_parent_lod.hierarchy.clusters.back().geometric_error =
        underbounded_parent_lod.hierarchy.clusters.back().bounds.sphere_radius;
    REQUIRE(!underbounded_parent_lod.refresh_digest());

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

void cluster_diagnostics_are_read_only_and_expose_bounded_parent_lod() {
    const auto package = make_box_package();
    const auto diagnostics = carto::web_geometry::inspect_package(package);
    REQUIRE(diagnostics);
    REQUIRE(diagnostics.value().validate());
    REQUIRE(diagnostics.value().validate_against(package));
    REQUIRE(diagnostics.value().cluster_count == package.hierarchy.clusters.size());
    REQUIRE(diagnostics.value().leaf_count + diagnostics.value().grouping_count +
            diagnostics.value().parent_lod_count == package.hierarchy.clusters.size());
    REQUIRE(diagnostics.value().leaf_count == package.hierarchy.clusters.size() - 1U);
    REQUIRE(diagnostics.value().grouping_count == 0U);
    REQUIRE(diagnostics.value().parent_lod_count == 1U);
    REQUIRE(diagnostics.value().error_receipts.size() ==
            diagnostics.value().cluster_count);
    REQUIRE(diagnostics.value().error_receipts.back().method ==
            "parent-lod-sphere-diameter-v1");
    REQUIRE(diagnostics.value().error_receipts.back().conservative);
    REQUIRE(diagnostics.value().error_receipts.back().renderable);
    REQUIRE(diagnostics.value().error_receipts.back().error_bound > 0.0);
    REQUIRE(!diagnostics.value().warnings.empty());
    REQUIRE(std::any_of(
        diagnostics.value().warnings.begin(),
        diagnostics.value().warnings.end(),
        [](const std::string& warning) {
            return warning.find("representative parent LOD") != std::string::npos;
        }));

    auto single = single_triangle_mesh();
    const auto compiled_single = single.compile();
    REQUIRE(compiled_single);
    const auto provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
        compiled_single.value());
    REQUIRE(provenance);
    const auto grouping_package = carto::web_geometry::compile_web_geometry(
        compiled_single.value(), {}, {}, provenance.value());
    REQUIRE(grouping_package);
    const auto grouping_diagnostics = carto::web_geometry::inspect_package(
        grouping_package.value());
    REQUIRE(grouping_diagnostics);
    REQUIRE(grouping_diagnostics.value().grouping_count == 1U);
    REQUIRE(grouping_diagnostics.value().parent_lod_count == 0U);
    REQUIRE(grouping_diagnostics.value().error_receipts.back().method ==
            "grouping-sphere-radius-v1");
    REQUIRE(grouping_diagnostics.value().error_receipts.back().conservative);
    REQUIRE(!grouping_diagnostics.value().error_receipts.back().renderable);

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
        package.value(), identity, {800U, 600U}, "project/object/mesh-17");
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

    const auto pointer_hits = carto::web_geometry::resolve_cluster_overlay_pointer(
        package.value(), projected.value(), 600.0, 150.0,
        "project/object/mesh-17", package.value().source_revision);
    REQUIRE(pointer_hits && !pointer_hits.value().empty());
    for (const auto& hit : pointer_hits.value()) {
        REQUIRE(hit.source_object_identity == "project/object/mesh-17");
        REQUIRE(hit.package_digest == package.value().digest.hex());
        REQUIRE(hit.source_revision == package.value().source_revision);
        REQUIRE(!hit.source_faces.empty());
    }
    const auto outside_pointer = carto::web_geometry::resolve_cluster_overlay_pointer(
        package.value(), projected.value(), 800.0, 150.0,
        "project/object/mesh-17", package.value().source_revision);
    REQUIRE(outside_pointer && outside_pointer.value().empty());
    const auto wrong_object = carto::web_geometry::resolve_cluster_overlay_pointer(
        package.value(), projected.value(), 600.0, 150.0,
        "project/object/other", package.value().source_revision);
    REQUIRE(!wrong_object && wrong_object.error().code == carto::core::ErrorCode::stale_data);
    const auto stale_revision = carto::web_geometry::resolve_cluster_overlay_pointer(
        package.value(), projected.value(), 600.0, 150.0,
        "project/object/mesh-17", carto::core::Revision{999U});
    REQUIRE(!stale_revision &&
            stale_revision.error().code == carto::core::ErrorCode::stale_data);
    auto stale_package_snapshot = projected.value();
    stale_package_snapshot.package_digest.assign(64U, '0');
    const auto stale_package = carto::web_geometry::resolve_cluster_overlay_pointer(
        package.value(), stale_package_snapshot, 600.0, 150.0,
        "project/object/mesh-17", package.value().source_revision);
    REQUIRE(!stale_package &&
            stale_package.error().code == carto::core::ErrorCode::stale_data);
    const auto invalid_pointer = carto::web_geometry::resolve_cluster_overlay_pointer(
        package.value(), projected.value(), std::numeric_limits<double>::quiet_NaN(), 150.0,
        "project/object/mesh-17", package.value().source_revision);
    REQUIRE(!invalid_pointer &&
            invalid_pointer.error().code == carto::core::ErrorCode::invalid_argument);

    const auto overlap_package = make_box_package();
    const auto overlap_snapshot = carto::web_geometry::project_cluster_overlays(
        overlap_package, identity, {800U, 600U}, "project/object/box-3");
    REQUIRE(overlap_snapshot);
    const auto overlap_hits = carto::web_geometry::resolve_cluster_overlay_pointer(
        overlap_package, overlap_snapshot.value(), 400.0, 300.0,
        "project/object/box-3", overlap_package.source_revision);
    REQUIRE(overlap_hits && overlap_hits.value().size() > 1U);

    const auto draw_plan = carto::web_geometry::build_cluster_overlay_draw_plan(
        overlap_snapshot.value(), 2.0);
    REQUIRE(draw_plan && draw_plan.value().validate());
    REQUIRE(draw_plan.value().vertices.size() == draw_plan.value().draws.size() * 24U);
    std::vector<const carto::web_geometry::ProjectedClusterOverlay*> visible_clusters;
    for (const auto& cluster : overlap_snapshot.value().clusters) {
        if (cluster.visible) visible_clusters.push_back(&cluster);
    }
    REQUIRE(draw_plan.value().draws.size() == visible_clusters.size());
    std::sort(visible_clusters.begin(), visible_clusters.end(), [](const auto* left, const auto* right) {
        if (left->minimum_depth != right->minimum_depth) {
            return left->minimum_depth > right->minimum_depth;
        }
        return left->cluster < right->cluster;
    });
    for (std::size_t index = 0U; index < visible_clusters.size(); ++index) {
        REQUIRE(draw_plan.value().draws[index].cluster == visible_clusters[index]->cluster);
        REQUIRE(draw_plan.value().draws[index].first_vertex == index * 24U);
        REQUIRE(draw_plan.value().draws[index].vertex_count == 24U);
    }
    REQUIRE(!carto::web_geometry::build_cluster_overlay_draw_plan(
        overlap_snapshot.value(), 0.0));
    REQUIRE(!carto::web_geometry::build_cluster_overlay_draw_plan(
        overlap_snapshot.value(), std::numeric_limits<double>::infinity()));
    auto invisible_snapshot = overlap_snapshot.value();
    for (auto& cluster : invisible_snapshot.clusters) {
        cluster.visible = false;
        cluster.clipped = false;
    }
    const auto empty_draw_plan = carto::web_geometry::build_cluster_overlay_draw_plan(
        invisible_snapshot);
    REQUIRE(empty_draw_plan && empty_draw_plan.value().draws.empty() &&
            empty_draw_plan.value().vertices.empty());
    auto malformed_draw_plan = draw_plan.value();
    malformed_draw_plan.draws.front().first_vertex = 1U;
    REQUIRE(!malformed_draw_plan.validate());
    carto::web_geometry::ClusterOverlaySnapshot excessive_overlay_snapshot;
    excessive_overlay_snapshot.package_digest = std::string(64U, '0');
    excessive_overlay_snapshot.source_revision = carto::core::Revision{1U};
    excessive_overlay_snapshot.viewport = {96U, 96U};
    excessive_overlay_snapshot.clusters.resize(
        carto::web_geometry::kMaxClusterOverlayDraws + 1U);
    for (std::size_t index = 0U; index < excessive_overlay_snapshot.clusters.size(); ++index) {
        auto& cluster = excessive_overlay_snapshot.clusters[index];
        cluster.cluster = static_cast<carto::web_geometry::ClusterId>(index);
        cluster.minimum_x = 1.0;
        cluster.minimum_y = 1.0;
        cluster.maximum_x = 10.0;
        cluster.maximum_y = 10.0;
        cluster.minimum_depth = 0.0;
        cluster.maximum_depth = 1.0;
        cluster.error_receipt_digest = std::string(64U, '0');
        cluster.visible = true;
    }
    const auto excessive_draw_plan = carto::web_geometry::build_cluster_overlay_draw_plan(
        excessive_overlay_snapshot);
    REQUIRE(!excessive_draw_plan);

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
        {"web geometry consumer admission is revision bound and budgeted",
         consumer_package_admission_is_revision_bound_and_budgeted},
        {"web geometry structural corruption is rejected",
         hierarchy_and_provenance_corruption_are_rejected},
        {"web geometry evaluation publication is incremental",
         evaluation_publication_is_typed_and_incremental},
        {"web geometry cluster diagnostics expose bounded parent LOD",
         cluster_diagnostics_are_read_only_and_expose_bounded_parent_lod},
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
