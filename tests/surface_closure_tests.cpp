#include <carto/assets/blob_store.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/surface/core.hpp>
#include <carto/surface/inheritance.hpp>
#include <carto/surface/process.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
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

carto::geometry::EditableMesh two_surface_patch() {
    carto::geometry::EditableMesh mesh;
    const auto a0 = mesh.add_vertex({-1.0, 0.0, 0.0});
    const auto a1 = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto a2 = mesh.add_vertex({0.0, 1.0, 0.0});
    const auto b0 = mesh.add_vertex({-1.0, 0.0, 1.0});
    const auto b1 = mesh.add_vertex({1.0, 0.0, 1.0});
    const auto b2 = mesh.add_vertex({0.0, 1.0, 2.0});
    REQUIRE(a0 && a1 && a2 && b0 && b1 && b2);
    REQUIRE(mesh.add_face({a0.value(), a1.value(), a2.value()}));
    REQUIRE(mesh.add_face({b0.value(), b1.value(), b2.value()}));
    return mesh;
}

std::string test_digest(std::string value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

void domain_and_analysis_are_revision_bound() {
    auto mesh = two_surface_patch();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{17U}, "wall", "concrete");
    REQUIRE(domain);
    REQUIRE(domain.value().validate(compiled.value()));

    const auto analysis = carto::surface::analyze_surface(compiled.value(), domain.value());
    REQUIRE(analysis);
    REQUIRE(analysis.value().validate(compiled.value()));
    REQUIRE(analysis.value().patch.area > 0.0);
    REQUIRE(analysis.value().ndf.alpha_x >= 0.001);
    REQUIRE(analysis.value().ndf.alpha_y >= 0.001);
    REQUIRE(analysis.value().visibility.ambient_visibility >= 0.0);
    REQUIRE(analysis.value().visibility.ambient_visibility <= 1.0);
    REQUIRE(analysis.value().edges.total() == 6U);

    auto stale = domain.value();
    stale.source_revision = carto::core::Revision{999U};
    REQUIRE(!stale.validate(compiled.value()));
    auto forged = domain.value();
    forged.digest.assign(64U, '0');
    REQUIRE(!forged.validate(compiled.value()));

    auto same_revision_geometry = compiled.value();
    same_revision_geometry.positions.front().x += 0.25;
    REQUIRE(!domain.value().validate(same_revision_geometry));
}

void frequency_and_proxy_are_deterministic_and_bounded() {
    auto mesh = two_surface_patch();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{18U});
    REQUIRE(domain);
    const auto first = carto::surface::decompose_frequency(compiled.value(), domain.value());
    const auto second = carto::surface::decompose_frequency(compiled.value(), domain.value());
    REQUIRE(first && second);
    REQUIRE(first.value().size() == 4U);
    REQUIRE(first.value().size() == second.value().size());
    for (std::size_t index = 0U; index < first.value().size(); ++index) {
        REQUIRE(first.value()[index].validate());
        REQUIRE(first.value()[index].triangle_count == second.value()[index].triangle_count);
        REQUIRE(first.value()[index].rms_normal_change == second.value()[index].rms_normal_change);
    }

    const auto proxy = carto::surface::build_geometry_proxy(compiled.value(), 1U);
    REQUIRE(proxy);
    REQUIRE(proxy.value().triangle_count() == 1U);
    REQUIRE(proxy.value().validate(compiled.value()));
    auto invalid = carto::surface::build_geometry_proxy(compiled.value(), 0U);
    REQUIRE(!invalid);
    invalid = carto::surface::build_geometry_proxy(compiled.value(), 3U);
    REQUIRE(!invalid);
}

void closure_beats_geometry_only_in_bounded_reference_space() {
    auto mesh = two_surface_patch();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{19U}, "patch", "rough");
    REQUIRE(domain);
    carto::surface::SurfaceClosureCompileOptions options;
    options.target_triangles = 1U;
    options.analysis.visibility_sample_count = 32U;
    options.evaluation.view_count = 8U;
    options.evaluation.light_count = 8U;
    const auto closure = carto::surface::compile_surface_representation(
        compiled.value(), domain.value(), options, carto::surface::RepresentationMode::fidelity_closure);
    const auto baseline = carto::surface::compile_surface_representation(
        compiled.value(), domain.value(), options, carto::surface::RepresentationMode::geometry_only);
    REQUIRE(closure && baseline);
    REQUIRE(closure.value().validate(compiled.value(), domain.value()));
    REQUIRE(baseline.value().validate(compiled.value(), domain.value()));
    REQUIRE(closure.value().error.rms_radiance_error <= baseline.value().error.rms_radiance_error);
    REQUIRE(closure.value().error.specular_error <= baseline.value().error.specular_error);

    carto::surface::SurfaceQualityPolicy permissive;
    permissive.max_rms_radiance_error = 10.0;
    permissive.max_radiance_error = 10.0;
    permissive.max_diffuse_error = 10.0;
    permissive.max_specular_error = 10.0;
    permissive.max_visibility_error = 10.0;
    permissive.max_silhouette_error = 10.0;
    REQUIRE(carto::surface::admit_surface_representation(
        compiled.value(), domain.value(), closure.value(), permissive));

    auto rejected = closure.value();
    rejected.error.max_radiance_error = 100.0;
    REQUIRE(!carto::surface::admit_surface_representation(
        compiled.value(), domain.value(), rejected));
}

void representation_inheritance_serialization_is_deterministic_and_fail_closed() {
    using carto::surface::RepresentationInheritanceRecord;
    using carto::surface::RepresentationPolicyField;
    const RepresentationInheritanceRecord record{
        carto::surface::RepresentationId{30U},
        carto::surface::RepresentationId{20U},
        {RepresentationPolicyField::view, RepresentationPolicyField::constraint},
        {RepresentationPolicyField::material, RepresentationPolicyField::fracture},
    };
    REQUIRE(record.validate());
    const auto serialized = record.serialize();
    REQUIRE(serialized);
    REQUIRE(serialized.value() ==
        "surface-representation-inheritance-v1\n"
        "representation=30\n"
        "parent=20\n"
        "inherited=view,constraint\n"
        "overrides=material,fracture");
    const auto parsed = RepresentationInheritanceRecord::parse(serialized.value());
    REQUIRE(parsed);
    REQUIRE(parsed.value().representation == record.representation);
    REQUIRE(parsed.value().parent == record.parent);
    REQUIRE(parsed.value().inherited_fields == record.inherited_fields);
    REQUIRE(parsed.value().override_fields == record.override_fields);
    REQUIRE(parsed.value().serialize().value() == serialized.value());

    auto unknown = serialized.value();
    const auto inherited = unknown.find("view,constraint");
    REQUIRE(inherited != std::string::npos);
    unknown.replace(inherited, std::string("view,constraint").size(), "view,lighting");
    REQUIRE(!RepresentationInheritanceRecord::parse(unknown));

    auto unknown_version = serialized.value();
    unknown_version.replace(0U, std::string("surface-representation-inheritance-v1").size(),
        "surface-representation-inheritance-v2");
    REQUIRE(!RepresentationInheritanceRecord::parse(unknown_version));

    auto overlap = record;
    overlap.override_fields = {RepresentationPolicyField::view};
    REQUIRE(!overlap.validate());
    auto invalid_enum = record;
    invalid_enum.inherited_fields = {static_cast<RepresentationPolicyField>(99U)};
    REQUIRE(!invalid_enum.serialize());
    auto same_identity = record;
    same_identity.parent = same_identity.representation;
    REQUIRE(!same_identity.validate());
}

void inherited_view_policy_is_deterministic_and_fails_closed() {
    using carto::surface::RepresentationInheritanceRecord;
    using carto::surface::RepresentationPolicyField;
    using carto::surface::RepresentationViewPolicyBinding;
    const carto::surface::SurfaceDomainId domain{31U};
    const carto::surface::RepresentationId root{41U};
    const carto::surface::RepresentationId child{42U};
    carto::surface::SurfaceRepresentationTree tree;
    REQUIRE(tree.add({root, domain, std::nullopt, {child},
        carto::surface::SurfaceRepresentationKind::authored, {}, test_digest("view-root")}));
    carto::surface::SurfaceErrorEnvelope child_error;
    child_error.max_silhouette_error = 0.4;
    REQUIRE(tree.add({child, domain, root, {},
        carto::surface::SurfaceRepresentationKind::simplified_geometry,
        child_error, test_digest("view-child")}));

    carto::surface::SurfaceQualityPolicy permissive;
    permissive.max_rms_radiance_error = 10.0;
    permissive.max_radiance_error = 10.0;
    permissive.max_diffuse_error = 10.0;
    permissive.max_specular_error = 10.0;
    permissive.max_visibility_error = 10.0;
    permissive.max_silhouette_error = 10.0;
    const std::vector<RepresentationInheritanceRecord> inherited{{
        child, root, {RepresentationPolicyField::view}, {}}};
    const std::vector<RepresentationViewPolicyBinding> root_binding{{root, permissive}};
    const auto first = carto::surface::evaluate_inherited_view_policy(
        tree, inherited, root_binding, child);
    const auto repeated = carto::surface::evaluate_inherited_view_policy(
        tree, inherited, root_binding, child);
    REQUIRE(first && repeated);
    REQUIRE(first.value().accepted);
    REQUIRE(first.value().digest == repeated.value().digest);
    REQUIRE(first.value().effective_policy_digest == permissive.digest().hex());
    REQUIRE(first.value().inheritance_digest == repeated.value().inheritance_digest);

    REQUIRE(!carto::surface::evaluate_inherited_view_policy(
        tree, inherited, {}, child));
    const std::vector<RepresentationInheritanceRecord> unsupported{{child, root, {}, {}}};
    REQUIRE(!carto::surface::evaluate_inherited_view_policy(
        tree, unsupported, root_binding, child));
    const std::vector<RepresentationInheritanceRecord> overridden{{
        child, root, {}, {RepresentationPolicyField::view}}};
    REQUIRE(!carto::surface::evaluate_inherited_view_policy(
        tree, overridden, root_binding, child));

    carto::surface::SurfaceQualityPolicy strict;
    strict.max_silhouette_error = 0.1;
    const std::vector<RepresentationViewPolicyBinding> override_bindings{
        {root, permissive}, {child, strict}};
    const auto rejected = carto::surface::evaluate_inherited_view_policy(
        tree, overridden, override_bindings, child);
    REQUIRE(rejected);
    REQUIRE(!rejected.value().accepted);
    REQUIRE(rejected.value().digest != first.value().digest);

    const std::vector<RepresentationViewPolicyBinding> contradictory_bindings{
        {root, permissive}, {child, strict}};
    REQUIRE(!carto::surface::evaluate_inherited_view_policy(
        tree, inherited, contradictory_bindings, child));
    const std::vector<RepresentationViewPolicyBinding> unknown_binding{
        {root, permissive}, {carto::surface::RepresentationId{99U}, strict}};
    REQUIRE(!carto::surface::evaluate_inherited_view_policy(
        tree, inherited, unknown_binding, child));
}

void inherited_constraints_report_stable_violations_and_reject_invalid_policy() {
    using namespace carto::surface;
    const SurfaceDomainId domain{51U};
    const RepresentationId root{61U};
    const RepresentationId child{62U};
    SurfaceRepresentationTree tree;
    REQUIRE(tree.add({root, domain, std::nullopt, {child},
        SurfaceRepresentationKind::authored, {}, test_digest("constraint-root")}));
    SurfaceErrorEnvelope child_error;
    child_error.max_position_error = 0.3;
    child_error.max_silhouette_error = 0.4;
    REQUIRE(tree.add({child, domain, root, {},
        SurfaceRepresentationKind::simplified_geometry, child_error,
        test_digest("constraint-child")}));

    const std::vector<SurfaceRepresentationConstraint> root_constraints{
        {10U, SurfaceConstraintMetric::max_position_error, 0.1},
        {20U, SurfaceConstraintMetric::max_silhouette_error, 0.2},
    };
    const std::vector<RepresentationConstraintBinding> root_binding{{root, root_constraints}};
    const std::vector<RepresentationInheritanceRecord> inherited{{
        child, root, {RepresentationPolicyField::constraint}, {}}};
    const auto first = evaluate_inherited_constraint_policy(
        tree, inherited, root_binding, child);
    const auto repeated = evaluate_inherited_constraint_policy(
        tree, inherited, root_binding, child);
    REQUIRE(first && repeated);
    REQUIRE(!first.value().accepted);
    REQUIRE((first.value().violated_constraint_ids == std::vector<std::uint64_t>{10U, 20U}));
    REQUIRE(first.value().digest == repeated.value().digest);

    const std::vector<SurfaceRepresentationConstraint> child_constraints{
        {30U, SurfaceConstraintMetric::max_position_error, 0.5},
    };
    const std::vector<RepresentationConstraintBinding> override_bindings{
        {root, root_constraints}, {child, child_constraints}};
    const std::vector<RepresentationInheritanceRecord> overridden{{
        child, root, {}, {RepresentationPolicyField::constraint}}};
    const auto override_result = evaluate_inherited_constraint_policy(
        tree, overridden, override_bindings, child);
    REQUIRE(override_result && override_result.value().accepted);
    REQUIRE(override_result.value().violated_constraint_ids.empty());
    REQUIRE(override_result.value().digest != first.value().digest);

    REQUIRE(!evaluate_inherited_constraint_policy(tree, inherited, {}, child));
    const std::vector<RepresentationInheritanceRecord> unsupported{{child, root, {}, {}}};
    REQUIRE(!evaluate_inherited_constraint_policy(tree, unsupported, root_binding, child));
    REQUIRE(!evaluate_inherited_constraint_policy(
        tree, overridden, root_binding, child));

    auto invalid_constraint = root_constraints;
    invalid_constraint.front().maximum = std::numeric_limits<double>::infinity();
    const std::vector<RepresentationConstraintBinding> invalid_numeric{{root, invalid_constraint}};
    REQUIRE(!evaluate_inherited_constraint_policy(tree, inherited, invalid_numeric, child));
    auto duplicate_ids = root_constraints;
    duplicate_ids[1U].id = duplicate_ids[0U].id;
    const std::vector<RepresentationConstraintBinding> duplicate{{root, duplicate_ids}};
    REQUIRE(!evaluate_inherited_constraint_policy(tree, inherited, duplicate, child));
    auto unknown_metric = root_constraints;
    unknown_metric.front().metric = static_cast<SurfaceConstraintMetric>(99U);
    const std::vector<RepresentationConstraintBinding> invalid_enum{{root, unknown_metric}};
    REQUIRE(!evaluate_inherited_constraint_policy(tree, inherited, invalid_enum, child));
}

void inherited_fracture_policy_produces_stable_bounded_partition_ids() {
    using namespace carto::surface;
    const SurfaceDomainId domain{71U};
    const RepresentationId root{81U};
    const RepresentationId child{82U};
    SurfaceRepresentationTree tree;
    REQUIRE(tree.add({root, domain, std::nullopt, {child},
        SurfaceRepresentationKind::authored, {}, test_digest("fracture-root")}));
    REQUIRE(tree.add({child, domain, root, {},
        SurfaceRepresentationKind::simplified_geometry, {}, test_digest("fracture-child")}));
    const std::vector<RepresentationInheritanceRecord> inherited{{
        child, root, {RepresentationPolicyField::fracture}, {}}};
    const std::vector<RepresentationFracturePolicyBinding> root_binding{{
        root, SurfaceFractureMode::balanced_face_id_order, 2U}};
    const std::vector<carto::geometry::FaceId> source_faces{
        {1U}, {2U}, {3U}, {4U}, {5U}};
    const auto first = evaluate_inherited_fracture_policy(
        tree, inherited, root_binding, source_faces, child);
    const auto second = evaluate_inherited_fracture_policy(
        tree, inherited, root_binding, source_faces, child);
    REQUIRE(first && second);
    REQUIRE(first.value().shards.size() == 2U);
    REQUIRE(first.value().shards[0].source_faces.size() == 3U);
    REQUIRE(first.value().shards[1].source_faces.size() == 2U);
    REQUIRE(first.value().digest == second.value().digest);
    REQUIRE(first.value().shards[0].stable_id == second.value().shards[0].stable_id);
    REQUIRE(first.value().shards[1].stable_id == second.value().shards[1].stable_id);
    std::vector<carto::geometry::FaceId> partitioned;
    for (const auto& shard : first.value().shards) {
        REQUIRE(shard.stable_id.size() == 64U);
        partitioned.insert(partitioned.end(), shard.source_faces.begin(), shard.source_faces.end());
    }
    REQUIRE(partitioned == source_faces);
    REQUIRE(tree.nodes().size() == 2U);
    REQUIRE(inherited.front().serialize());

    const std::vector<RepresentationInheritanceRecord> overridden{{
        child, root, {}, {RepresentationPolicyField::fracture}}};
    const std::vector<RepresentationFracturePolicyBinding> override_bindings{
        {root, SurfaceFractureMode::balanced_face_id_order, 2U},
        {child, SurfaceFractureMode::balanced_face_id_order, 3U},
    };
    const auto override_result = evaluate_inherited_fracture_policy(
        tree, overridden, override_bindings, source_faces, child);
    REQUIRE(override_result && override_result.value().shards.size() == 3U);
    REQUIRE(override_result.value().digest != first.value().digest);

    REQUIRE(!evaluate_inherited_fracture_policy(tree, inherited, {}, source_faces, child));
    const std::vector<RepresentationInheritanceRecord> unsupported{{child, root, {}, {}}};
    REQUIRE(!evaluate_inherited_fracture_policy(
        tree, unsupported, root_binding, source_faces, child));
    const std::vector<RepresentationFracturePolicyBinding> unsupported_mode{{
        root, static_cast<SurfaceFractureMode>(99U), 2U}};
    REQUIRE(!evaluate_inherited_fracture_policy(
        tree, inherited, unsupported_mode, source_faces, child));
    const std::vector<carto::geometry::FaceId> repeated_faces{{1U}, {1U}};
    REQUIRE(!evaluate_inherited_fracture_policy(
        tree, inherited, root_binding, repeated_faces, child));
    const std::vector<RepresentationFracturePolicyBinding> too_many_shards{{
        root, SurfaceFractureMode::balanced_face_id_order, 6U}};
    REQUIRE(!evaluate_inherited_fracture_policy(
        tree, inherited, too_many_shards, source_faces, child));
}

void fracture_policy_materializes_revision_bound_mesh_shards() {
    using namespace carto::surface;
    auto editable = two_surface_patch();
    const auto compiled = editable.compile();
    REQUIRE(compiled);
    std::vector<carto::geometry::FaceId> source_faces = compiled.value().triangle_faces;
    std::sort(source_faces.begin(), source_faces.end());
    source_faces.erase(std::unique(source_faces.begin(), source_faces.end()), source_faces.end());

    const SurfaceDomainId domain{91U};
    const RepresentationId root{101U};
    const RepresentationId child{102U};
    SurfaceRepresentationTree tree;
    REQUIRE(tree.add({root, domain, std::nullopt, {child},
        SurfaceRepresentationKind::authored, {}, test_digest("fracture-geometry-root")}));
    REQUIRE(tree.add({child, domain, root, {},
        SurfaceRepresentationKind::simplified_geometry, {}, test_digest("fracture-geometry-child")}));
    const std::vector<RepresentationInheritanceRecord> inherited{{
        child, root, {RepresentationPolicyField::fracture}, {}}};
    const std::vector<RepresentationFracturePolicyBinding> binding{{
        root, SurfaceFractureMode::balanced_face_id_order, 2U}};
    const auto plan = evaluate_inherited_fracture_policy(
        tree, inherited, binding, source_faces, child);
    REQUIRE(plan);

    const auto first = materialize_fracture_geometry(compiled.value(), plan.value());
    const auto second = materialize_fracture_geometry(compiled.value(), plan.value());
    REQUIRE(first && second);
    REQUIRE(first.value().representation == child);
    REQUIRE(first.value().policy_outcome_digest == plan.value().digest);
    REQUIRE(first.value().source_mesh_digest.size() == 64U);
    REQUIRE(first.value().digest == second.value().digest);
    REQUIRE(first.value().validate(compiled.value(), plan.value()));
    REQUIRE(first.value().shards.size() == plan.value().shards.size());
    std::vector<carto::geometry::FaceId> emitted_faces;
    std::size_t emitted_triangles = 0U;
    for (std::size_t index = 0U; index < first.value().shards.size(); ++index) {
        const auto& output = first.value().shards[index];
        const auto& expected = plan.value().shards[index];
        REQUIRE(output.stable_id == expected.stable_id);
        REQUIRE(output.mesh.valid());
        REQUIRE(output.mesh.source_revision == compiled.value().source_revision);
        std::vector<carto::geometry::FaceId> shard_faces = output.mesh.triangle_faces;
        std::sort(shard_faces.begin(), shard_faces.end());
        shard_faces.erase(std::unique(shard_faces.begin(), shard_faces.end()), shard_faces.end());
        REQUIRE(shard_faces == expected.source_faces);
        emitted_triangles += output.mesh.triangle_faces.size();
        emitted_faces.insert(emitted_faces.end(), shard_faces.begin(), shard_faces.end());
        for (std::size_t triangle = 0U; triangle < output.mesh.triangle_faces.size(); ++triangle) {
            const auto source_triangle = std::find(compiled.value().triangle_faces.begin(),
                compiled.value().triangle_faces.end(), output.mesh.triangle_faces[triangle]);
            REQUIRE(source_triangle != compiled.value().triangle_faces.end());
            const auto source_triangle_index = static_cast<std::size_t>(
                std::distance(compiled.value().triangle_faces.begin(), source_triangle));
            REQUIRE(output.mesh.triangle_edges[triangle] ==
                compiled.value().triangle_edges[source_triangle_index]);
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                const auto output_vertex = output.mesh.indices[triangle * 3U + corner];
                const auto source_vertex = compiled.value().indices[source_triangle_index * 3U + corner];
                REQUIRE(output.mesh.vertex_ids[output_vertex] ==
                    compiled.value().vertex_ids[source_vertex]);
            }
        }
        for (std::size_t vertex = 0U; vertex < output.mesh.vertex_ids.size(); ++vertex) {
            const auto source_vertex = std::find(compiled.value().vertex_ids.begin(),
                compiled.value().vertex_ids.end(), output.mesh.vertex_ids[vertex]);
            REQUIRE(source_vertex != compiled.value().vertex_ids.end());
            const auto source_index = static_cast<std::size_t>(
                std::distance(compiled.value().vertex_ids.begin(), source_vertex));
            REQUIRE(output.mesh.positions[vertex].x == compiled.value().positions[source_index].x);
            REQUIRE(output.mesh.positions[vertex].y == compiled.value().positions[source_index].y);
            REQUIRE(output.mesh.positions[vertex].z == compiled.value().positions[source_index].z);
            REQUIRE(output.mesh.normals[vertex].x == compiled.value().normals[source_index].x);
            REQUIRE(output.mesh.normals[vertex].y == compiled.value().normals[source_index].y);
            REQUIRE(output.mesh.normals[vertex].z == compiled.value().normals[source_index].z);
        }
    }
    std::sort(emitted_faces.begin(), emitted_faces.end());
    REQUIRE(emitted_faces == source_faces);
    REQUIRE(emitted_triangles == compiled.value().triangle_faces.size());

    auto stale_coverage = compiled.value();
    stale_coverage.triangle_faces.back() = stale_coverage.triangle_faces.front();
    REQUIRE(stale_coverage.valid());
    REQUIRE(!materialize_fracture_geometry(stale_coverage, plan.value()));
    auto invalid_source = compiled.value();
    invalid_source.indices.front() = std::numeric_limits<std::uint32_t>::max();
    REQUIRE(!materialize_fracture_geometry(invalid_source, plan.value()));
    auto forged_plan = plan.value();
    forged_plan.shards.front().source_faces.pop_back();
    REQUIRE(!materialize_fracture_geometry(compiled.value(), forged_plan));
    auto forged_geometry = first.value();
    forged_geometry.shards.front().mesh.positions.front().x += 0.25;
    REQUIRE(!forged_geometry.validate(compiled.value(), plan.value()));
    auto stale_geometry = first.value();
    REQUIRE(!stale_geometry.validate(stale_coverage, plan.value()));
}

void representation_tree_and_fidelity_package_are_fail_closed() {
    auto mesh = two_surface_patch();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{20U});
    REQUIRE(domain);
    carto::surface::SurfaceClosureCompileOptions options;
    options.target_triangles = 1U;
    const auto closure = carto::surface::compile_surface_representation(
        compiled.value(), domain.value(), options);
    REQUIRE(closure);

    const auto invalid_mode = carto::surface::compile_surface_representation(
        compiled.value(), domain.value(), options,
        static_cast<carto::surface::RepresentationMode>(99));
    REQUIRE(!invalid_mode);

    auto invalid_closure = closure.value();
    invalid_closure.kind = static_cast<carto::surface::SurfaceRepresentationKind>(99);
    REQUIRE(!invalid_closure.refresh_digest());
    REQUIRE(!invalid_closure.validate(compiled.value(), domain.value()));

    const carto::surface::RepresentationId root_id{
        closure.value().representation.value == 1U ? 2U : 1U};
    const std::string root_digest = test_digest("surface-root");
    carto::surface::SurfaceRepresentationTree tree;
    REQUIRE(tree.add({
        root_id,
        domain.value().id,
        std::nullopt,
        {closure.value().representation},
        carto::surface::SurfaceRepresentationKind::authored,
        {},
        root_digest,
    }));
    REQUIRE(tree.add({
        closure.value().representation,
        domain.value().id,
        root_id,
        {},
        carto::surface::SurfaceRepresentationKind::closure_proxy,
        closure.value().error,
        closure.value().digest,
    }));
    REQUIRE(tree.validate(domain.value().id));

    auto invalid_node = tree.nodes().front();
    invalid_node.kind = static_cast<carto::surface::SurfaceRepresentationKind>(99);
    REQUIRE(!invalid_node.validate());

    carto::surface::RepresentationMigration migration{
        domain.value().id,
        root_id,
        closure.value().representation,
        {carto::surface::MigrationTarget::ndf,
         carto::surface::MigrationTarget::directional_visibility},
        0.0,
        compiled.value().source_revision,
        "surface-test-migration/1",
        {},
    };
    REQUIRE(migration.refresh_digest());
    REQUIRE(migration.validate());

    auto invalid_migration = migration;
    invalid_migration.targets.front() = static_cast<carto::surface::MigrationTarget>(99);
    REQUIRE(!invalid_migration.refresh_digest());
    REQUIRE(!invalid_migration.validate());

    carto::surface::FidelityPackage package;
    package.source_revision = compiled.value().source_revision;
    package.domains.push_back(domain.value());
    package.representations = tree.nodes();
    package.runtime_records.push_back({
        domain.value().id,
        closure.value().representation,
        closure.value().error.max_position_error,
        closure.value().error.rms_radiance_error,
        1U,
        2U,
        3U,
    });
    package.closures.push_back(closure.value());
    package.migrations.push_back(migration);
    REQUIRE(package.refresh_digest());
    REQUIRE(package.validate(compiled.value()));

    const auto foreign_domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{21U});
    REQUIRE(foreign_domain);
    const carto::surface::RepresentationId foreign_root_id{
        closure.value().representation.value == 999U ? 1000U : 999U};
    const carto::surface::SurfaceRepresentationNode foreign_root{
        foreign_root_id,
        foreign_domain.value().id,
        std::nullopt,
        {},
        carto::surface::SurfaceRepresentationKind::authored,
        {},
        test_digest("foreign-root"),
    };
    REQUIRE(foreign_root.validate());

    auto cross_domain_base = package;
    cross_domain_base.domains.push_back(foreign_domain.value());
    cross_domain_base.representations.push_back(foreign_root);
    REQUIRE(cross_domain_base.refresh_digest());
    REQUIRE(cross_domain_base.validate(compiled.value()));

    auto cross_domain_runtime = cross_domain_base;
    cross_domain_runtime.runtime_records.front().representation = foreign_root_id;
    REQUIRE(cross_domain_runtime.refresh_digest());
    REQUIRE(!cross_domain_runtime.validate(compiled.value()));

    auto cross_domain_closure = cross_domain_base;
    cross_domain_closure.closures.front().representation = foreign_root_id;
    REQUIRE(cross_domain_closure.closures.front().refresh_digest());
    REQUIRE(cross_domain_closure.refresh_digest());
    REQUIRE(!cross_domain_closure.validate(compiled.value()));

    auto cross_domain_migration = cross_domain_base;
    cross_domain_migration.migrations.front().source_representation = foreign_root_id;
    REQUIRE(cross_domain_migration.migrations.front().refresh_digest());
    REQUIRE(cross_domain_migration.refresh_digest());
    REQUIRE(!cross_domain_migration.validate(compiled.value()));

    auto tampered = package;
    tampered.digest[0] = tampered.digest[0] == '0' ? '1' : '0';
    REQUIRE(!tampered.validate(compiled.value()));
    auto invalid_tree = tree;
    (void)invalid_tree;
}

void view_dependent_selection_is_camera_and_receipt_bound() {
    auto mesh = two_surface_patch();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{42U});
    REQUIRE(domain);

    carto::surface::SurfaceClosureCompileOptions dense_options;
    dense_options.target_triangles = 2U;
    carto::surface::SurfaceClosureCompileOptions coarse_options;
    coarse_options.target_triangles = 1U;
    const auto dense = carto::surface::compile_surface_representation(
        compiled.value(), domain.value(), dense_options);
    const auto coarse = carto::surface::compile_surface_representation(
        compiled.value(), domain.value(), coarse_options);
    REQUIRE(dense && coarse);

    std::vector<carto::surface::RepresentationId> children{
        dense.value().representation, coarse.value().representation};
    std::sort(children.begin(), children.end());
    carto::surface::RepresentationId root{1U};
    while (std::find(children.begin(), children.end(), root) != children.end()) {
        ++root.value;
    }
    carto::surface::SurfaceRepresentationTree tree;
    REQUIRE(tree.add({root, domain.value().id, std::nullopt, children,
        carto::surface::SurfaceRepresentationKind::authored, {},
        test_digest("view-selection-root")}));
    REQUIRE(tree.add({dense.value().representation, domain.value().id, root, {},
        dense.value().kind, dense.value().error, dense.value().digest}));
    REQUIRE(tree.add({coarse.value().representation, domain.value().id, root, {},
        coarse.value().kind, coarse.value().error, coarse.value().digest}));
    REQUIRE(tree.validate(domain.value().id));

    carto::surface::SurfaceViewSelectionOptions near_options;
    near_options.minimum_view_depth = 2.0;
    near_options.maximum_projected_radius_pixels = 100.0;
    near_options.maximum_estimated_position_error_pixels = 0.0;
    near_options.quality_policy = {1.0e6, 1.0e6, 1.0e6, 1.0e6, 1.0e6, 1.0e6};
    const std::vector<carto::surface::SurfaceClosurePolygon> candidates{
        coarse.value(), dense.value()};
    const auto near_selection = carto::surface::select_surface_representation_for_view(
        compiled.value(), domain.value(), tree, candidates, near_options);
    REQUIRE(near_selection);
    REQUIRE(near_selection.value().representation == dense.value().representation);
    REQUIRE(near_selection.value().triangle_count == 2U);
    REQUIRE(near_selection.value().validate(
        compiled.value(), domain.value(), tree, candidates, near_options));

    auto far_options = near_options;
    far_options.minimum_view_depth = 100'000.0;
    far_options.maximum_estimated_position_error_pixels = 1.0;
    const auto far_selection = carto::surface::select_surface_representation_for_view(
        compiled.value(), domain.value(), tree, candidates, far_options);
    REQUIRE(far_selection);
    REQUIRE(far_selection.value().representation == coarse.value().representation);
    REQUIRE(far_selection.value().triangle_count == 1U);

    const std::vector<carto::surface::SurfaceClosurePolygon> reordered_candidates{
        dense.value(), coarse.value()};
    const auto reordered = carto::surface::select_surface_representation_for_view(
        compiled.value(), domain.value(), tree, reordered_candidates, far_options);
    REQUIRE(reordered);
    REQUIRE(reordered.value().digest == far_selection.value().digest);

    auto appearance_options = far_options;
    appearance_options.quality_policy.max_silhouette_error = 0.0;
    const auto appearance_limited = carto::surface::select_surface_representation_for_view(
        compiled.value(), domain.value(), tree, candidates, appearance_options);
    REQUIRE(appearance_limited);
    REQUIRE(appearance_limited.value().representation == dense.value().representation);

    REQUIRE(!carto::surface::select_surface_representation_for_view(
        compiled.value(), domain.value(), tree, {coarse.value()}, near_options));
    auto invalid_camera = near_options;
    invalid_camera.vertical_fov_radians = std::numeric_limits<double>::infinity();
    REQUIRE(!carto::surface::select_surface_representation_for_view(
        compiled.value(), domain.value(), tree, candidates, invalid_camera));

    auto stale_receipt = far_selection.value();
    stale_receipt.representation_digest = dense.value().digest;
    REQUIRE(!stale_receipt.validate(
        compiled.value(), domain.value(), tree, candidates, far_options));

    auto forged_receipt = far_selection.value();
    forged_receipt.representation = dense.value().representation;
    forged_receipt.representation_digest = dense.value().digest;
    forged_receipt.triangle_count = 2U;
    forged_receipt.estimated_projected_position_error_pixels = 0.0;
    forged_receipt.digest = test_digest(forged_receipt.canonical());
    REQUIRE(!forged_receipt.validate(
        compiled.value(), domain.value(), tree, candidates, far_options));
}

void process_contracts_are_bounded_and_deterministic() {
    auto mesh = two_surface_patch();
    const auto compiled = mesh.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{21U}, "wall", "coated-steel");
    REQUIRE(domain);
    const auto analysis = carto::surface::analyze_surface(compiled.value(), domain.value());
    REQUIRE(analysis);

    carto::surface::MaterialStratigraphy stratigraphy{
        domain.value().id,
        compiled.value().source_revision,
        {
            {11U, 0.10, 0.01, "paint"},
            {12U, 0.20, 0.02, "primer"},
            {13U, 1.00, 0.00, "substrate"},
        },
        {},
    };
    REQUIRE(stratigraphy.refresh_digest());
    REQUIRE(stratigraphy.validate());
    const auto shallow = carto::surface::resolve_material_exposure(stratigraphy, 0.05);
    REQUIRE(shallow && shallow.value().material == 11U && !shallow.value().reaches_substrate);
    const auto primer = carto::surface::resolve_material_exposure(stratigraphy, 0.15);
    REQUIRE(primer && primer.value().material == 12U);
    const auto substrate = carto::surface::resolve_material_exposure(stratigraphy, 2.0);
    REQUIRE(substrate && substrate.value().material == 13U && substrate.value().reaches_substrate);

    carto::surface::WeatheringControls controls;
    controls.age = 40.0;
    controls.rain_exposure = 0.8;
    controls.solar_exposure = 0.7;
    controls.moisture = 0.6;
    controls.traffic = 0.4;
    controls.maintenance = 0.1;
    controls.seed = 7U;
    const auto weathering_a = carto::surface::derive_weathering(analysis.value(), controls);
    const auto weathering_b = carto::surface::derive_weathering(analysis.value(), controls);
    REQUIRE(weathering_a && weathering_b);
    REQUIRE(weathering_a.value().validate());
    REQUIRE(weathering_a.value().digest == weathering_b.value().digest);
    REQUIRE(weathering_a.value().oxidation > 0.0);

    carto::surface::ManufacturingHistory history{
        domain.value().id,
        compiled.value().source_revision,
        {
            {1U, carto::surface::ManufacturingProcess::cut, {1.0, 0.0, 0.0}, 0.8, "cut-plan"},
            {2U, carto::surface::ManufacturingProcess::grind, {1.0, 0.0, 0.0}, 0.4, "finish"},
        },
        {},
    };
    REQUIRE(history.refresh_digest());
    REQUIRE(history.validate());
    auto duplicate_history = history;
    duplicate_history.steps[1U].sequence = duplicate_history.steps[0U].sequence;
    REQUIRE(!duplicate_history.refresh_digest());

    carto::surface::SurfaceContact contact{
        carto::surface::SurfaceDomainId{21U},
        carto::surface::SurfaceDomainId{22U},
        carto::surface::ContactType::sealed,
        0.01,
        {},
    };
    REQUIRE(contact.refresh_digest());
    REQUIRE(contact.validate());
    carto::surface::ContactGraph contacts{
        compiled.value().source_revision,
        {contact},
        {},
    };
    REQUIRE(contacts.refresh_digest());
    REQUIRE(contacts.validate());

    carto::surface::EdgeProfile profile{
        carto::geometry::EdgeId{1U},
        carto::surface::EdgeProcess::painted_corner,
        carto::surface::ProfileShape::asymmetric,
        0.01,
        0.002,
        0.2,
        0.1,
        "paint-to-primer",
    };
    REQUIRE(profile.validate());

    carto::surface::MaterialGeometryPolicy policy{
        12U, 0.002, 0.001, 128U, 9U, "brick-generator/1"};
    REQUIRE(policy.validate());
    const auto intent = carto::surface::make_material_geometry_intent(
        domain.value().id, domain.value().source_faces.front(), compiled.value().source_revision, policy);
    REQUIRE(intent && intent.value().validate());
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"surface domain and analysis are revision bound", domain_and_analysis_are_revision_bound},
        {"frequency and proxy are deterministic", frequency_and_proxy_are_deterministic_and_bounded},
        {"closure beats geometry-only reference", closure_beats_geometry_only_in_bounded_reference_space},
        {"representation inheritance serialization is deterministic",
         representation_inheritance_serialization_is_deterministic_and_fail_closed},
        {"inherited view policy is deterministic and fail closed",
         inherited_view_policy_is_deterministic_and_fails_closed},
        {"inherited constraints report stable violations",
         inherited_constraints_report_stable_violations_and_reject_invalid_policy},
        {"inherited fracture policy produces stable partitions",
         inherited_fracture_policy_produces_stable_bounded_partition_ids},
        {"fracture policy materializes revision-bound mesh shards",
         fracture_policy_materializes_revision_bound_mesh_shards},
        {"representation tree and fidelity package are bounded",
         representation_tree_and_fidelity_package_are_fail_closed},
        {"view-dependent selection is camera and receipt bound",
         view_dependent_selection_is_camera_and_receipt_bound},
        {"material, process, contact, and edge contracts are bounded",
         process_contracts_are_bounded_and_deterministic},
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
