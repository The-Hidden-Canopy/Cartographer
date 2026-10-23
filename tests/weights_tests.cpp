#include <carto/assets/blob_store.hpp>
#include <carto/character/weights_record.hpp>
#include <carto/core/math.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/skin.hpp>
#include <carto/weights/auto_weight.hpp>
#include <carto/weights/inverse.hpp>
#include <carto/weights/lowering.hpp>
#include <carto/weights/paint.hpp>
#include <carto/weights/transfer.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
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

using carto::core::Vec3d;
using carto::geometry::CompiledMesh;
using carto::geometry::FaceId;
using carto::geometry::VertexId;
using carto::kinematic::JointId;
using carto::kinematic::JointInfluence;
using carto::kinematic::KinematicGraph;
using carto::kinematic::KinematicNode;
using carto::kinematic::SkinBinding;

// Plane grid z = 0 spanning x in [0, 1], y in [0, 0.5], with a vertex column
// exactly on the x = 0.5 segment seam. Vertex array index is the canonical
// influence key used by SkinBinding. Quad diagonals alternate in checkerboard
// parity so the triangulation is mirror-symmetric across the x = 0.5 plane;
// a uniform diagonal direction would skew geodesic distances across the seam.
CompiledMesh segmented_box_mesh() {
    constexpr std::uint32_t columns = 5U; // x = 0, .25, .5, .75, 1
    constexpr std::uint32_t rows = 4U;    // y = 0, 1/6, 2/6, .5
    CompiledMesh mesh;
    for (std::uint32_t row = 0U; row < rows; ++row) {
        for (std::uint32_t column = 0U; column < columns; ++column) {
            mesh.positions.push_back(
                {static_cast<double>(column) * 0.25,
                 static_cast<double>(row) * (0.5 / 3.0), 0.0});
            mesh.normals.push_back({0.0, 0.0, 1.0});
            mesh.vertex_ids.push_back(VertexId{
                static_cast<std::uint64_t>(row * columns + column) + 1U});
            mesh.bounds.include(mesh.positions.back());
        }
    }
    std::uint64_t face = 1U;
    for (std::uint32_t row = 0U; row + 1U < rows; ++row) {
        for (std::uint32_t column = 0U; column + 1U < columns; ++column) {
            const std::uint32_t lower_left = row * columns + column;
            const std::uint32_t lower_right = lower_left + 1U;
            const std::uint32_t upper_left = lower_left + columns;
            const std::uint32_t upper_right = upper_left + 1U;
            if ((row + column) % 2U == 0U) {
                mesh.indices.insert(mesh.indices.end(),
                    {lower_left, lower_right, upper_right});
                mesh.indices.insert(mesh.indices.end(),
                    {lower_left, upper_right, upper_left});
            } else {
                mesh.indices.insert(mesh.indices.end(),
                    {lower_right, upper_right, upper_left});
                mesh.indices.insert(mesh.indices.end(),
                    {lower_left, lower_right, upper_left});
            }
            mesh.triangle_faces.push_back(FaceId{face++});
            mesh.triangle_faces.push_back(FaceId{face++});
            mesh.triangle_edges.emplace_back();
            mesh.triangle_edges.emplace_back();
        }
    }
    require(mesh.valid(), "segmented box fixture mesh is valid");
    return mesh;
}

KinematicGraph two_joint_graph() {
    KinematicGraph graph;
    KinematicNode root;
    root.id = JointId{1U};
    root.rest.translation = {0.25, 0.25, 0.0};
    KinematicNode child;
    child.id = JointId{2U};
    child.parent = JointId{1U};
    child.rest.translation = {0.75, 0.25, 0.0};
    require(static_cast<bool>(graph.insert_node(root)), "root joint inserts");
    require(static_cast<bool>(graph.insert_node(child)), "child joint inserts");
    require(static_cast<bool>(graph.validate()), "two joint graph validates");
    return graph;
}

// Chain of eight joints with distinct rest anchors along +x.
KinematicGraph eight_joint_graph() {
    KinematicGraph graph;
    for (std::uint64_t index = 1U; index <= 8U; ++index) {
        KinematicNode node;
        node.id = JointId{index};
        if (index > 1U) node.parent = JointId{index - 1U};
        node.rest.translation = {static_cast<double>(index) * 0.01, 0.0, 0.0};
        require(static_cast<bool>(graph.insert_node(node)), "chain joint inserts");
    }
    require(static_cast<bool>(graph.validate()), "eight joint graph validates");
    return graph;
}

carto::weights::AutoWeightRecipe rigid_recipe() {
    carto::weights::AutoWeightRecipe recipe;
    recipe.output_layer_id = carto::character::RigLayerId{7U};
    recipe.max_influences_per_vertex = 1U;
    recipe.nearest_joint_candidates = 1U;
    recipe.smoothing_iterations = 0U;
    return recipe;
}

bool influences_equal(
    const std::vector<JointInfluence>& first,
    const std::vector<JointInfluence>& second) {
    if (first.size() != second.size()) return false;
    for (std::size_t index = 0U; index < first.size(); ++index) {
        if (!(first[index].joint == second[index].joint) ||
            first[index].weight != second[index].weight) {
            return false;
        }
    }
    return true;
}

void auto_weights_produce_rigid_segment_answer() {
    const CompiledMesh mesh = segmented_box_mesh();
    const KinematicGraph graph = two_joint_graph();
    const carto::assets::Sha256Digest mesh_digest = digest("segmented-box-v1");

    const auto generated = carto::weights::generate_auto_weights(
        mesh, graph, mesh_digest, 42U, rigid_recipe());
    require(static_cast<bool>(generated), "auto weight generation succeeds");

    // Known answer: every vertex is rigid to exactly one joint; the seam
    // vertex at x = 0.5 ties on geodesic distance and resolves to the
    // lower joint id.
    for (std::size_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        const auto found = generated.value().binding.influences.find(
            static_cast<std::uint32_t>(vertex));
        require(found != generated.value().binding.influences.end(),
            "every mesh vertex receives influences");
        require(found->second.size() == 1U, "rigid answer has one influence");
        const JointId expected = mesh.positions[vertex].x <= 0.5 + 1e-12
            ? JointId{1U}
            : JointId{2U};
        require(found->second.front().joint == expected,
            "vertex is rigidly bound to its segment joint");
        require(std::abs(found->second.front().weight - 1.0) < 1e-9,
            "rigid weight is exactly one");
        require(static_cast<bool>(
                carto::character::validate_influences(found->second)),
            "generated influences pass the shared admission contract");
    }
    require(static_cast<bool>(generated.value().binding.validate()),
        "generated binding validates");
    require(static_cast<bool>(generated.value().layer.validate()),
        "generated layer validates");
    require(generated.value().layer.provenance ==
            carto::character::WeightProvenance::automatic_proposal,
        "layer provenance is an automatic proposal");
    require(static_cast<bool>(generated.value().receipt.validate()),
        "auto weight receipt validates");
    require(generated.value().receipt.influenced_vertex_count ==
            mesh.positions.size(),
        "receipt counts every influenced vertex");

    // Determinism: a second run over identical inputs reproduces identical
    // digests.
    const auto repeat = carto::weights::generate_auto_weights(
        mesh, graph, mesh_digest, 42U, rigid_recipe());
    require(static_cast<bool>(repeat), "repeat generation succeeds");
    const auto first_digest = generated.value().binding.canonical_digest();
    const auto second_digest = repeat.value().binding.canonical_digest();
    require(static_cast<bool>(first_digest) && static_cast<bool>(second_digest),
        "binding digests compute");
    require(first_digest.value() == second_digest.value(),
        "auto weight generation is deterministic");
    const auto first_receipt = generated.value().receipt.canonical_digest();
    const auto second_receipt = repeat.value().receipt.canonical_digest();
    require(static_cast<bool>(first_receipt) && static_cast<bool>(second_receipt),
        "receipt digests compute");
    require(first_receipt.value() == second_receipt.value(),
        "auto weight receipt is deterministic");

    // Serialization round trip.
    const auto reparsed = carto::weights::AutoWeightReceipt::deserialize(
        generated.value().receipt.serialize());
    require(static_cast<bool>(reparsed), "auto weight receipt round trips");
    require(reparsed.value().serialize() == generated.value().receipt.serialize(),
        "auto weight receipt canonical form is stable");
    const auto recipe_reparsed = carto::weights::AutoWeightRecipe::deserialize(
        rigid_recipe().serialize());
    require(static_cast<bool>(recipe_reparsed), "auto weight recipe round trips");

    // Hostile inputs fail closed before allocation.
    require(!carto::weights::AutoWeightRecipe::deserialize(
                "CARTOGRAPHER_WEIGHTS_AUTO_RECIPE 1\nLAYER 7\nANCHORS 99999999\n"),
        "oversized anchor counts are rejected");
    require(!carto::weights::AutoWeightRecipe::deserialize("not a recipe"),
        "malformed recipe framing fails closed");
    require(!carto::weights::AutoWeightReceipt::deserialize(
                std::string(64U * 1024U + 1U, 'x')),
        "oversized receipt text is rejected");
}

void auto_weights_heat_and_smoothing_stay_admitted() {
    const CompiledMesh mesh = segmented_box_mesh();
    const KinematicGraph graph = two_joint_graph();
    carto::weights::AutoWeightRecipe recipe;
    recipe.output_layer_id = carto::character::RigLayerId{9U};
    recipe.max_influences_per_vertex = 2U;
    recipe.nearest_joint_candidates = 2U;
    recipe.distance = carto::weights::DistanceMode::heat;
    recipe.heat_steps = 16U;
    recipe.smoothing_iterations = 4U;
    recipe.smoothing_strength = 0.25;

    const auto generated = carto::weights::generate_auto_weights(
        mesh, graph, digest("segmented-box-v1"), 42U, recipe);
    require(static_cast<bool>(generated), "heat-mode generation succeeds");
    require(generated.value().receipt.iterations_used >= 1U &&
            generated.value().receipt.iterations_used <= 4U,
        "receipt iteration count is bounded by the recipe");
    require(generated.value().binding.influences.size() == mesh.positions.size(),
        "every vertex is influenced");
    for (const auto& [vertex, influences] : generated.value().binding.influences) {
        static_cast<void>(vertex);
        require(influences.size() <= 2U, "influence cap is respected");
        require(static_cast<bool>(
                carto::character::validate_influences(influences)),
            "smoothed influences stay admitted");
    }

    const auto repeat = carto::weights::generate_auto_weights(
        mesh, graph, digest("segmented-box-v1"), 42U, recipe);
    require(static_cast<bool>(repeat), "repeat heat-mode generation succeeds");
    const auto first_digest = generated.value().receipt.canonical_digest();
    const auto second_digest = repeat.value().receipt.canonical_digest();
    require(static_cast<bool>(first_digest) && static_cast<bool>(second_digest),
        "heat receipt digests compute");
    require(first_digest.value() == second_digest.value(),
        "heat-mode generation is deterministic");
}

void auto_weights_refuse_unreachable_components() {
    // Two disjoint triangles, one joint anchored on the first: the second
    // component is unreachable and must refuse, not silently deform.
    CompiledMesh mesh;
    mesh.positions = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
        {10.0, 0.0, 0.0}, {11.0, 0.0, 0.0}, {10.0, 1.0, 0.0}};
    for (std::size_t index = 0U; index < mesh.positions.size(); ++index) {
        mesh.normals.push_back({0.0, 0.0, 1.0});
        mesh.vertex_ids.push_back(VertexId{index + 1U});
        mesh.bounds.include(mesh.positions[index]);
    }
    mesh.indices = {0U, 1U, 2U, 3U, 4U, 5U};
    mesh.triangle_faces = {FaceId{1U}, FaceId{2U}};
    mesh.triangle_edges.emplace_back();
    mesh.triangle_edges.emplace_back();
    require(mesh.valid(), "disjoint mesh fixture is valid");

    KinematicGraph graph;
    KinematicNode joint;
    joint.id = JointId{1U};
    joint.rest.translation = {0.1, 0.1, 0.0};
    require(static_cast<bool>(graph.insert_node(joint)), "joint inserts");
    require(static_cast<bool>(graph.validate()), "graph validates");

    const auto refused = carto::weights::generate_auto_weights(
        mesh, graph, digest("disjoint-v1"), 7U, rigid_recipe());
    require(!refused, "unreachable vertices refuse generation");
    require(std::string(refused.error().message).find("unreachable") !=
            std::string::npos,
        "refusal names the unreachable-region diagnostic");

    // An explicit anchor override on the detached component makes the same
    // mesh solvable: deliberate artist assignment, not silent fallback.
    carto::weights::AutoWeightRecipe with_override = rigid_recipe();
    with_override.anchor_overrides.emplace(JointId{1U}, Vec3d{10.1, 0.1, 0.0});
    const auto assigned = carto::weights::generate_auto_weights(
        mesh, graph, digest("disjoint-v1"), 7U, with_override);
    require(static_cast<bool>(assigned),
        "anchor override assigns the detached component");
    for (const auto& [vertex, influences] : assigned.value().binding.influences) {
        static_cast<void>(vertex);
        require(static_cast<bool>(
                carto::character::validate_influences(influences)),
            "override-assigned influences stay admitted");
    }
}

void paint_respects_mask_locks_and_radius() {
    const CompiledMesh mesh = segmented_box_mesh();
    const KinematicGraph graph = two_joint_graph();
    const auto generated = carto::weights::generate_auto_weights(
        mesh, graph, digest("segmented-box-v1"), 42U, rigid_recipe());
    require(static_cast<bool>(generated), "fixture generation succeeds");
    const SkinBinding& original = generated.value().binding;

    carto::weights::WeightMask mask;
    // Mask out the first two columns entirely (x = 0 and x = 0.25).
    for (std::uint32_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        if (mesh.positions[vertex].x < 0.4) {
            require(static_cast<bool>(mask.set(vertex, 0.0)), "mask entry sets");
        }
    }
    carto::weights::WeightLocks locks;
    // Lock the seam vertex column (x = 0.5) against joint 2 changes.
    for (std::uint32_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        if (std::abs(mesh.positions[vertex].x - 0.5) < 1e-12) {
            require(static_cast<bool>(locks.lock(vertex, JointId{2U})),
                "lock entry sets");
        }
    }

    carto::weights::BrushStamp stamp;
    stamp.center = {0.65, 0.25, 0.0};
    stamp.radius = 0.3;
    stamp.strength = 0.6;
    stamp.falloff = carto::weights::BrushFalloff::spherical;
    stamp.op = carto::weights::PaintOp::add;

    const auto painted = carto::weights::apply_brush_stamp(
        original, mesh, stamp, JointId{2U}, &mask, &locks);
    require(static_cast<bool>(painted), "masked locked stamp succeeds");
    require(static_cast<bool>(painted.value().receipt.validate()),
        "paint receipt validates");
    require(painted.value().receipt.affected_vertex_count > 0U,
        "stamp affects vertices");

    bool observed_change = false;
    for (std::uint32_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        const auto before = original.influences.find(vertex);
        const auto after = painted.value().binding.influences.find(vertex);
        require(before != original.influences.end() &&
                after != painted.value().binding.influences.end(),
            "painted binding keeps vertex coverage");
        const bool masked = mesh.positions[vertex].x < 0.4;
        const double distance =
            (mesh.positions[vertex] - stamp.center).length();
        if (masked || distance > stamp.radius) {
            require(influences_equal(before->second, after->second),
                "masked and out-of-radius vertices are untouched");
        } else if (!influences_equal(before->second, after->second)) {
            observed_change = true;
        }
        require(static_cast<bool>(
                carto::character::validate_influences(after->second)),
            "painted influences stay admitted");
    }
    require(observed_change, "unmasked in-radius vertices change");

    // Determinism: identical stroke from an identical binding reproduces the
    // receipt digest.
    const auto repeat = carto::weights::apply_brush_stamp(
        original, mesh, stamp, JointId{2U}, &mask, &locks);
    require(static_cast<bool>(repeat), "repeat stamp succeeds");
    const auto first = painted.value().receipt.canonical_digest();
    const auto second = repeat.value().receipt.canonical_digest();
    require(static_cast<bool>(first) && static_cast<bool>(second),
        "paint receipt digests compute");
    require(first.value() == second.value(),
        "paint receipts are deterministic");

    const auto reparsed = carto::weights::PaintReceipt::deserialize(
        painted.value().receipt.serialize());
    require(static_cast<bool>(reparsed), "paint receipt round trips");
    require(reparsed.value().serialize() == painted.value().receipt.serialize(),
        "paint receipt canonical form is stable");

    // A stroke applies its stamps in order with per-stamp receipts.
    carto::weights::PaintStroke stroke;
    stroke.stamps.push_back(stamp);
    carto::weights::BrushStamp second_stamp = stamp;
    second_stamp.center = {0.8, 0.25, 0.0};
    second_stamp.op = carto::weights::PaintOp::smooth;
    stroke.stamps.push_back(second_stamp);
    const auto stroked = carto::weights::apply_paint_stroke(
        original, mesh, stroke, JointId{2U}, &mask, &locks);
    require(static_cast<bool>(stroked), "two-stamp stroke succeeds");
    require(stroked.value().receipts.size() == 2U,
        "stroke produces one receipt per stamp");
    require(stroked.value().receipts[0U].stamp_index == 0U &&
            stroked.value().receipts[1U].stamp_index == 1U,
        "stroke receipts carry stamp indices");

    // Hostile stamps refuse.
    carto::weights::BrushStamp hostile = stamp;
    hostile.radius = -1.0;
    require(!carto::weights::apply_brush_stamp(
                original, mesh, hostile, JointId{2U}, &mask, &locks),
        "negative radius refuses");
    require(!carto::weights::apply_brush_stamp(
                original, mesh, stamp, JointId{99U}, &mask, &locks),
        "painting an unknown joint refuses");
    carto::weights::WeightMask hostile_mask;
    require(!hostile_mask.set(0U, 2.0), "out-of-range mask factor refuses");
}

SkinBinding make_dense_binding(const carto::assets::Sha256Digest& graph_digest) {
    SkinBinding binding;
    binding.mesh_asset = 5U;
    binding.mesh_digest = digest("dense-mesh-v1");
    binding.graph_digest = graph_digest;
    // Three vertices, eight influences each, generic normalized weights.
    const std::vector<double> weights =
        {0.20, 0.20, 0.20, 0.10, 0.10, 0.10, 0.05, 0.05};
    for (std::uint32_t vertex = 0U; vertex < 3U; ++vertex) {
        std::vector<JointInfluence> influences;
        for (std::uint64_t joint = 1U; joint <= 8U; ++joint) {
            influences.push_back(
                JointInfluence{JointId{joint}, weights[joint - 1U]});
        }
        binding.influences.emplace(vertex, std::move(influences));
    }
    require(static_cast<bool>(binding.validate()),
        "dense fixture binding validates");
    return binding;
}

void lowering_selects_known_subset_and_bounds_error() {
    const KinematicGraph graph = eight_joint_graph();
    const auto graph_digest = graph.canonical_digest();
    require(static_cast<bool>(graph_digest), "graph digest computes");
    const SkinBinding dense = make_dense_binding(graph_digest.value());
    const std::vector<Vec3d> positions =
        {{0.05, 0.02, 0.0}, {0.06, -0.01, 0.01}, {0.04, 0.01, -0.02}};

    // Joints 1..4 stay at identity; joints 5..8 carry large distinct
    // translations (displacements 50, 60, 70, 80 weighted 0.10, 0.10, 0.05,
    // 0.05, i.e. a full deformation of p + 18.5*x). No four-subset reproduces
    // that exactly: dropping a mover loses deformation, and renormalizing the
    // kept weights scales what remains. The honest optimum keeps the two
    // strongest movers (5 and 6) plus identity filler joints 1 and 2 to
    // dilute the translation toward 18.5*x: the exact best four-subset is
    // {1,2,5,6} with loss |18.5 - 55/3| = 1/6, inside a declared envelope
    // of 1.0. The tied alternatives {1,3,5,6} and {2,3,5,6} resolve to the
    // lexicographically smallest subset.
    constexpr double kEnvelope = 1.0;
    carto::weights::LoweringPose pose;
    for (std::uint64_t joint = 5U; joint <= 8U; ++joint) {
        carto::core::Transform transform;
        transform.translation =
            {static_cast<double>(joint) * 10.0, 0.0, 0.0};
        pose.joint_transforms.emplace(JointId{joint}, transform);
    }

    const auto lowered = carto::weights::lower_eight_to_four(
        dense, graph, positions, kEnvelope, std::vector<carto::weights::LoweringPose>{pose});
    require(static_cast<bool>(lowered), "bounded lowering succeeds");
    require(static_cast<bool>(lowered.value().receipt.validate()),
        "lowering receipt validates");
    require(lowered.value().receipt.evaluated_max_error <= kEnvelope,
        "evaluated error stays inside the declared envelope");
    require(lowered.value().receipt.evaluated_max_error > 0.1 &&
            lowered.value().receipt.evaluated_max_error < 0.2,
        "evaluated error is the honest nonzero loss of the best subset");
    require(lowered.value().receipt.per_vertex.size() == 3U,
        "receipt records every lowered vertex");
    for (std::uint32_t vertex = 0U; vertex < 3U; ++vertex) {
        const auto found = lowered.value().binding.influences.find(vertex);
        require(found != lowered.value().binding.influences.end(),
            "lowered binding keeps the vertex");
        require(found->second.size() == 4U, "vertex is lowered to four influences");
        double weight_sum = 0.0;
        for (const JointInfluence& influence : found->second) {
            const bool in_best_subset =
                influence.joint == JointId{1U} || influence.joint == JointId{2U} ||
                influence.joint == JointId{5U} || influence.joint == JointId{6U};
            require(in_best_subset, "best subset is the known exact answer");
            weight_sum += influence.weight;
        }
        require(std::abs(weight_sum - 1.0) < 1e-9,
            "lowered weights renormalize to one");
        require(static_cast<bool>(
                carto::character::validate_influences(found->second)),
            "lowered influences stay admitted");
    }

    // Reconciliation: lowering renormalizes its kept weights (0.2 -> 1/3
    // here), so the lowered layer reconciles against itself exactly, and
    // honestly does NOT reconcile against the unnormalized full source
    // within the contract tolerance; validate_upcast_reconciliation remains
    // an upcast-direction contract (weights must survive unchanged).
    require(static_cast<bool>(carto::weights::validate_upcast_reconciliation(
                lowered.value().binding, lowered.value().binding, 1e-9)),
        "lowered binding reconciles against itself");
    require(!carto::weights::validate_upcast_reconciliation(
                lowered.value().binding, dense, 1e-9),
        "renormalized lowered weights honestly fail full-source reconciliation");

    // Determinism.
    const auto repeat = carto::weights::lower_eight_to_four(
        dense, graph, positions, kEnvelope,
        std::vector<carto::weights::LoweringPose>{pose});
    require(static_cast<bool>(repeat), "repeat lowering succeeds");
    const auto first = lowered.value().receipt.canonical_digest();
    const auto second = repeat.value().receipt.canonical_digest();
    require(static_cast<bool>(first) && static_cast<bool>(second),
        "lowering receipt digests compute");
    require(first.value() == second.value(),
        "lowering receipts are deterministic");

    const auto reparsed = carto::weights::LowerEightToFourReceipt::deserialize(
        lowered.value().receipt.serialize());
    require(static_cast<bool>(reparsed), "lowering receipt round trips");
    require(reparsed.value().serialize() == lowered.value().receipt.serialize(),
        "lowering receipt canonical form is stable");

    // Tampered sources fail reconciliation.
    SkinBinding tampered_valid = lowered.value().binding;
    for (const auto& [vertex, influences] : lowered.value().binding.influences) {
        std::vector<JointInfluence> fixed = influences;
        fixed[0U].weight += 0.005;
        fixed[1U].weight -= 0.005;
        tampered_valid.influences[vertex] = std::move(fixed);
    }
    require(!carto::weights::validate_upcast_reconciliation(
                lowered.value().binding, tampered_valid, 1e-9),
        "tampered sources fail reconciliation");
}

void lowering_refuses_beyond_bound_and_without_poses() {
    const KinematicGraph graph = eight_joint_graph();
    const auto graph_digest = graph.canonical_digest();
    require(static_cast<bool>(graph_digest), "graph digest computes");
    const SkinBinding dense = make_dense_binding(graph_digest.value());
    const std::vector<Vec3d> positions =
        {{0.05, 0.02, 0.0}, {0.06, -0.01, 0.01}, {0.04, 0.01, -0.02}};

    // Every joint carries a distinct large translation: any four-subset loses
    // deformation, so a zero bound must refuse with the named diagnostic.
    carto::weights::LoweringPose pose;
    for (std::uint64_t joint = 1U; joint <= 8U; ++joint) {
        carto::core::Transform transform;
        transform.translation =
            {static_cast<double>(joint) * 10.0, 0.0, 0.0};
        pose.joint_transforms.emplace(JointId{joint}, transform);
    }
    const auto refused = carto::weights::lower_eight_to_four(
        dense, graph, positions, 0.0,
        std::vector<carto::weights::LoweringPose>{pose});
    require(!refused, "excessive influence loss refuses publication");
    require(std::string(refused.error().message).find(
                "INFLUENCE_LOSS_EXCEEDS_BUDGET") != std::string::npos,
        "refusal carries the named budget diagnostic");

    // Without representative poses the loss is unmeasurable: honest refusal,
    // never silent truncation.
    const auto no_poses = carto::weights::lower_eight_to_four(
        dense, graph, positions, 100.0, {});
    require(!no_poses, "lowering without evaluation poses refuses");
}

Vec3d fixture_deform(
    const Vec3d& point,
    const Vec3d& anchor,
    const carto::core::Transform& transform) {
    const Vec3d local = point - anchor;
    return transform.translation +
        transform.rotation.normalized().rotate(local) + anchor;
}

void inverse_recovers_known_joint_transforms() {
    const CompiledMesh mesh = segmented_box_mesh();
    const KinematicGraph graph = two_joint_graph();
    const auto generated = carto::weights::generate_auto_weights(
        mesh, graph, digest("segmented-box-v1"), 42U, rigid_recipe());
    require(static_cast<bool>(generated), "fixture generation succeeds");
    const SkinBinding& binding = generated.value().binding;

    // Ground-truth pose: joint 1 stays at identity; joint 2 rotates 40 degrees
    // about +z through its anchor and translates.
    carto::core::Transform truth;
    const double angle = 40.0 * 3.14159265358979323846 / 180.0;
    const double half = angle * 0.5;
    truth.rotation = {0.0, 0.0, std::sin(half), std::cos(half)};
    truth.translation = {0.1, -0.05, 0.0};
    const Vec3d anchor_two = {0.75, 0.25, 0.0};

    std::vector<Vec3d> posed;
    posed.reserve(mesh.positions.size());
    for (std::size_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        const auto& influences = binding.influences.at(
            static_cast<std::uint32_t>(vertex));
        require(influences.size() == 1U, "fixture weights are rigid");
        Vec3d deformed = influences.front().joint == JointId{2U}
            ? fixture_deform(mesh.positions[vertex], anchor_two, truth)
            : mesh.positions[vertex];
        posed.push_back(deformed);
    }

    carto::weights::InverseSolveOptions options;
    options.max_iterations = 16U;
    options.tolerance = 1e-12;
    const auto solved = carto::weights::recover_joint_transforms(
        binding, graph, mesh.positions, posed, options);
    require(static_cast<bool>(solved), "inverse solve succeeds");
    require(static_cast<bool>(solved.value().receipt.validate()),
        "inverse receipt validates");
    require(solved.value().receipt.converged, "inverse solve converges");
    require(solved.value().receipt.final_rms_residual < 1e-9,
        "recovered pose reproduces the observation");

    const carto::core::Transform& recovered_two =
        solved.value().joint_transforms.at(JointId{2U});
    require((recovered_two.translation - truth.translation).length() < 1e-6,
        "recovered translation matches the known transform");
    const carto::core::Quaternion delta =
        recovered_two.rotation * carto::core::Quaternion{
            -truth.rotation.x, -truth.rotation.y, -truth.rotation.z,
            truth.rotation.w};
    const double rotation_error =
        2.0 * std::acos(std::min(1.0, std::abs(delta.normalized().w)));
    require(rotation_error < 1e-6, "recovered rotation matches the known transform");
    const carto::core::Transform& recovered_one =
        solved.value().joint_transforms.at(JointId{1U});
    require(recovered_one.translation.length() < 1e-9 &&
            std::abs(recovered_one.rotation.w - 1.0) < 1e-9,
        "untouched joint stays at identity");

    // Determinism and serialization round trip.
    const auto repeat = carto::weights::recover_joint_transforms(
        binding, graph, mesh.positions, posed, options);
    require(static_cast<bool>(repeat), "repeat inverse solve succeeds");
    const auto first = solved.value().receipt.canonical_digest();
    const auto second = repeat.value().receipt.canonical_digest();
    require(static_cast<bool>(first) && static_cast<bool>(second),
        "inverse receipt digests compute");
    require(first.value() == second.value(),
        "inverse solve is deterministic");
    const auto reparsed = carto::weights::InverseSolveReceipt::deserialize(
        solved.value().receipt.serialize());
    require(static_cast<bool>(reparsed), "inverse receipt round trips");
    require(reparsed.value().serialize() == solved.value().receipt.serialize(),
        "inverse receipt canonical form is stable");

    // Bounded hostile options refuse.
    carto::weights::InverseSolveOptions hostile = options;
    hostile.max_iterations = 1000000U;
    require(!carto::weights::recover_joint_transforms(
                binding, graph, mesh.positions, posed, hostile),
        "unbounded inverse options refuse");
}

void inverse_rig_hypotheses_are_bounded_source_bound_and_reviewed() {
    const CompiledMesh mesh = segmented_box_mesh();
    const KinematicGraph graph = two_joint_graph();
    const auto source_mesh_digest = digest("segmented-box-v1");
    const auto generated = carto::weights::generate_auto_weights(
        mesh, graph, source_mesh_digest, 42U, rigid_recipe());
    require(static_cast<bool>(generated), "hypothesis fixture binding generates");
    const SkinBinding binding = generated.value().binding;

    carto::core::Transform truth;
    const double angle = 40.0 * 3.14159265358979323846 / 180.0;
    const double half = angle * 0.5;
    truth.rotation = {0.0, 0.0, std::sin(half), std::cos(half)};
    truth.translation = {0.1, -0.05, 0.0};
    const Vec3d anchor_two = {0.75, 0.25, 0.0};
    std::vector<Vec3d> posed;
    posed.reserve(mesh.positions.size());
    for (std::size_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        const auto& influences = binding.influences.at(
            static_cast<std::uint32_t>(vertex));
        posed.push_back(influences.front().joint == JointId{2U}
            ? fixture_deform(mesh.positions[vertex], anchor_two, truth)
            : mesh.positions[vertex]);
    }

    // A valid but deliberately poorer alternative binds the whole mesh to the
    // unmoved root joint. The evaluator should rank by residual, not input order.
    SkinBinding poorer_binding = binding;
    for (auto& [vertex, influences] : poorer_binding.influences) {
        static_cast<void>(vertex);
        influences = {{JointId{1U}, 1.0}};
    }
    require(static_cast<bool>(poorer_binding.validate()),
        "alternative hypothesis binding validates");
    SkinBinding stale_graph_binding = binding;
    stale_graph_binding.graph_digest = digest("superseded-hypothesis-graph");
    SkinBinding wrong_source_binding = binding;
    wrong_source_binding.mesh_digest = digest("other-observation-source");

    const auto graph_digest_before = graph.canonical_digest();
    const auto binding_digest_before = binding.canonical_digest();
    const auto poorer_digest_before = poorer_binding.canonical_digest();
    require(static_cast<bool>(graph_digest_before) &&
            static_cast<bool>(binding_digest_before) &&
            static_cast<bool>(poorer_digest_before),
        "hypothesis fixture digests compute");
    const std::vector<Vec3d> rest_before = mesh.positions;
    const std::vector<Vec3d> posed_before = posed;

    const std::vector<carto::weights::InverseRigHypothesisInput> hypotheses{
        {4U, &graph, &stale_graph_binding},
        {3U, nullptr, &binding},
        {2U, &graph, &poorer_binding},
        {1U, &graph, &binding},
        {5U, &graph, &wrong_source_binding},
    };
    const std::vector<carto::weights::InverseRigHypothesisInput> single{hypotheses[3U]};
    carto::weights::InverseRigHypothesisOptions options;
    options.solver.max_iterations = 16U;
    options.solver.tolerance = 1e-12;
    const auto search = carto::weights::evaluate_inverse_rig_hypotheses(
        hypotheses, source_mesh_digest, mesh.positions, posed, options);
    require(static_cast<bool>(search), "bounded rig hypothesis search succeeds");
    require(search.value().source_mesh_digest == source_mesh_digest,
        "search receipt binds the expected source revision");
    require(search.value().ranked_hypotheses.size() == hypotheses.size(),
        "search returns one evidence row for each supplied hypothesis");

    const auto row_for = [&](std::uint64_t id)
        -> const carto::weights::InverseRigHypothesisResult* {
        for (const auto& row : search.value().ranked_hypotheses) {
            if (row.id == id) return &row;
        }
        return nullptr;
    };
    const auto* best = row_for(1U);
    const auto* poorer = row_for(2U);
    const auto* stale_graph = row_for(4U);
    const auto* wrong_source = row_for(5U);
    const auto* missing = row_for(3U);
    require(best != nullptr && poorer != nullptr && stale_graph != nullptr &&
            wrong_source != nullptr && missing != nullptr,
        "search preserves identities for accepted and rejected hypotheses");
    require(search.value().ranked_hypotheses.front().id == 1U &&
            best->status == carto::weights::InverseHypothesisStatus::converged &&
            poorer->final_rms_residual.has_value() &&
            best->final_rms_residual.has_value() &&
            *best->final_rms_residual < *poorer->final_rms_residual,
        "candidate ranking prefers the lower residual independent of input order");
    require(stale_graph->status == carto::weights::InverseHypothesisStatus::rejected &&
            !stale_graph->diagnostic.empty(),
        "binding with a stale graph digest remains visible as rejected evidence");
    require(wrong_source->status == carto::weights::InverseHypothesisStatus::rejected &&
            !wrong_source->diagnostic.empty(),
        "candidate bound to another mesh revision is rejected with a diagnostic");
    require(missing->status == carto::weights::InverseHypothesisStatus::rejected &&
            !missing->diagnostic.empty(),
        "candidate missing its graph is rejected with a diagnostic");

    const auto repeated = carto::weights::evaluate_inverse_rig_hypotheses(
        hypotheses, source_mesh_digest, mesh.positions, posed, options);
    require(static_cast<bool>(repeated), "repeated hypothesis search succeeds");
    require(repeated.value().observation_digest == search.value().observation_digest &&
            repeated.value().ranked_hypotheses.size() ==
                search.value().ranked_hypotheses.size(),
        "repeated search has identical observation lineage and row count");
    for (std::size_t index = 0U;
         index < search.value().ranked_hypotheses.size(); ++index) {
        const auto& first = search.value().ranked_hypotheses[index];
        const auto& second = repeated.value().ranked_hypotheses[index];
        require(first.id == second.id && first.input_ordinal == second.input_ordinal &&
                first.status == second.status &&
                first.final_rms_residual == second.final_rms_residual &&
                first.hypothesis_digest == second.hypothesis_digest,
            "candidate ordering and evidence are deterministic");
    }
    const auto graph_digest_after = graph.canonical_digest();
    const auto binding_digest_after = binding.canonical_digest();
    const auto poorer_digest_after = poorer_binding.canonical_digest();
    require(graph_digest_after && binding_digest_after && poorer_digest_after &&
            graph_digest_after.value() == graph_digest_before.value() &&
            binding_digest_after.value() == binding_digest_before.value() &&
            poorer_digest_after.value() == poorer_digest_before.value(),
        "search leaves supplied graphs and bindings unchanged");
    require(mesh.positions.size() == rest_before.size() && posed.size() == posed_before.size(),
        "search leaves observation array lengths unchanged");
    for (std::size_t index = 0U; index < posed.size(); ++index) {
        require(mesh.positions[index].x == rest_before[index].x &&
                mesh.positions[index].y == rest_before[index].y &&
                mesh.positions[index].z == rest_before[index].z &&
                posed[index].x == posed_before[index].x &&
                posed[index].y == posed_before[index].y &&
                posed[index].z == posed_before[index].z,
            "search leaves source and posed vertex coordinates unchanged");
    }

    const auto lock = carto::weights::lock_inverse_rig_hypothesis(
        search.value(), hypotheses, 1U, "reviewer-7", "known pose fixture accepted");
    require(static_cast<bool>(lock) && static_cast<bool>(lock.value().validate()),
        "human selection requires and records valid reviewer lineage");
    require(lock.value().source_mesh_digest == source_mesh_digest,
        "review lock carries the exact source mesh revision");
    require(!carto::weights::lock_inverse_rig_hypothesis(
                search.value(), hypotheses, 1U, "", "reviewed"),
        "missing reviewer identity is rejected");
    require(!carto::weights::lock_inverse_rig_hypothesis(
                search.value(), hypotheses, 1U, "reviewer-7", ""),
        "missing justification is rejected");
    require(!carto::weights::lock_inverse_rig_hypothesis(
                search.value(), hypotheses, 4U, "reviewer-7", "stale proposal"),
        "rejected candidate cannot be locked");
    auto tampered_lock = lock.value();
    tampered_lock.justification = "changed after digest";
    require(!tampered_lock.validate(),
        "selection fields cannot change without invalidating the review digest");

    const auto rerun = carto::weights::rerun_locked_inverse_rig_hypothesis(
        lock.value(), hypotheses[3U], source_mesh_digest,
        mesh.positions, posed, options);
    require(static_cast<bool>(rerun) &&
            rerun.value().status == carto::weights::InverseHypothesisStatus::converged &&
            rerun.value().selection_digest == lock.value().selection_digest &&
            rerun.value().observation_digest == search.value().observation_digest &&
            rerun.value().solution.receipt.final_rms_residual < 1e-9,
        "locked source-bound rerun reproduces its reviewed candidate");
    const auto graph_digest_after_rerun = graph.canonical_digest();
    const auto binding_digest_after_rerun = binding.canonical_digest();
    require(graph_digest_after_rerun && binding_digest_after_rerun &&
            graph_digest_after_rerun.value() == graph_digest_before.value() &&
            binding_digest_after_rerun.value() == binding_digest_before.value(),
        "locked rerun preserves the reviewed graph and binding");
    for (std::size_t index = 0U; index < posed.size(); ++index) {
        require(mesh.positions[index].x == rest_before[index].x &&
                mesh.positions[index].y == rest_before[index].y &&
                mesh.positions[index].z == rest_before[index].z &&
                posed[index].x == posed_before[index].x &&
                posed[index].y == posed_before[index].y &&
                posed[index].z == posed_before[index].z,
            "locked rerun preserves the source and observation arrays");
    }
    require(!carto::weights::rerun_locked_inverse_rig_hypothesis(
                lock.value(), hypotheses[3U], digest("other-source"),
                mesh.positions, posed, options),
        "rerun refuses a different source mesh digest");
    std::vector<Vec3d> changed_observation = posed;
    changed_observation.front().z += 0.01;
    require(!carto::weights::rerun_locked_inverse_rig_hypothesis(
                lock.value(), hypotheses[3U], source_mesh_digest,
                mesh.positions, changed_observation, options),
        "rerun refuses a stale observation even when vertex count is unchanged");
    SkinBinding changed_binding = binding;
    changed_binding.influences.begin()->second.front().joint = JointId{2U};
    const carto::weights::InverseRigHypothesisInput changed_candidate{
        1U, &graph, &changed_binding};
    require(static_cast<bool>(changed_binding.validate()),
        "changed binding remains a valid but different hypothesis");
    require(!carto::weights::rerun_locked_inverse_rig_hypothesis(
                lock.value(), changed_candidate, source_mesh_digest,
                mesh.positions, posed, options),
        "rerun refuses a changed binding after review");
    const carto::weights::InverseRigHypothesisInput wrong_id_candidate{
        99U, &graph, &binding};
    require(!carto::weights::rerun_locked_inverse_rig_hypothesis(
                lock.value(), wrong_id_candidate, source_mesh_digest,
                mesh.positions, posed, options),
        "rerun refuses a different candidate identity");

    auto bounded_preview_options = options;
    bounded_preview_options.solver.max_iterations = 1U;
    const auto incomplete_search = carto::weights::evaluate_inverse_rig_hypotheses(
        single, source_mesh_digest, mesh.positions, posed, bounded_preview_options);
    require(static_cast<bool>(incomplete_search) &&
            incomplete_search.value().ranked_hypotheses.front().status ==
                carto::weights::InverseHypothesisStatus::incomplete,
        "iteration exhaustion remains explicitly incomplete");
    const auto incomplete_lock = carto::weights::lock_inverse_rig_hypothesis(
        incomplete_search.value(), single, 1U,
        "reviewer-7", "review bounded preview");
    require(static_cast<bool>(incomplete_lock) &&
            incomplete_lock.value().preview_status ==
                carto::weights::InverseHypothesisStatus::incomplete,
        "review lock preserves incomplete preview quality");
    const auto incomplete_rerun = carto::weights::rerun_locked_inverse_rig_hypothesis(
        incomplete_lock.value(), single.front(), source_mesh_digest,
        mesh.positions, posed, bounded_preview_options);
    require(static_cast<bool>(incomplete_rerun) &&
            incomplete_rerun.value().status ==
                carto::weights::InverseHypothesisStatus::incomplete,
        "bounded rerun does not promote an incomplete solve");

    const std::vector<carto::weights::InverseRigHypothesisInput> duplicate_ids{
        {8U, &graph, &binding}, {8U, &graph, &binding}};
    const auto duplicate_search = carto::weights::evaluate_inverse_rig_hypotheses(
        duplicate_ids, source_mesh_digest, mesh.positions, posed, options);
    require(static_cast<bool>(duplicate_search) &&
            duplicate_search.value().ranked_hypotheses.size() == 2U,
        "duplicate candidate identities produce evidence rows");
    for (const auto& row : duplicate_search.value().ranked_hypotheses) {
        require(row.status == carto::weights::InverseHypothesisStatus::rejected &&
                !row.diagnostic.empty(),
            "every ambiguous duplicate identity is rejected explicitly");
    }
    require(!carto::weights::evaluate_inverse_rig_hypotheses(
                single, {}, mesh.positions, posed, options),
        "search refuses a missing source mesh digest");
    auto over_budget = options;
    over_budget.max_solver_work_units = 1U;
    require(!carto::weights::evaluate_inverse_rig_hypotheses(
                single, source_mesh_digest, mesh.positions, posed, over_budget),
        "search refuses a solver workload beyond its configured cap");
    std::vector<carto::weights::InverseRigHypothesisInput> too_many(
        carto::weights::InverseRigHypothesisOptions::kHardMaxHypotheses + 1U,
        hypotheses[3U]);
    require(!carto::weights::evaluate_inverse_rig_hypotheses(
                too_many, source_mesh_digest, mesh.positions, posed, options),
        "search refuses more than the hard candidate limit");
    std::vector<Vec3d> non_finite = posed;
    non_finite.front().x = std::numeric_limits<double>::infinity();
    require(!carto::weights::evaluate_inverse_rig_hypotheses(
                single, source_mesh_digest, mesh.positions, non_finite, options),
        "search refuses non-finite observation data");
}

void weight_refinement_stub_reports_honest_quality() {
    const CompiledMesh mesh = segmented_box_mesh();
    const KinematicGraph graph = two_joint_graph();
    // A smoothed, non-rigid starting layer gives the stub something to do.
    carto::weights::AutoWeightRecipe recipe;
    recipe.output_layer_id = carto::character::RigLayerId{11U};
    recipe.max_influences_per_vertex = 2U;
    recipe.nearest_joint_candidates = 2U;
    recipe.smoothing_iterations = 6U;
    recipe.smoothing_strength = 0.4;
    const auto generated = carto::weights::generate_auto_weights(
        mesh, graph, digest("segmented-box-v1"), 42U, recipe);
    require(static_cast<bool>(generated), "smoothed fixture generates");

    carto::core::Transform pose_two;
    const double angle = 30.0 * 3.14159265358979323846 / 180.0;
    const double half = angle * 0.5;
    pose_two.rotation = {0.0, 0.0, std::sin(half), std::cos(half)};
    pose_two.translation = {0.05, 0.02, 0.0};
    const Vec3d anchor_two = {0.75, 0.25, 0.0};
    std::map<JointId, carto::core::Transform> pose;
    pose.emplace(JointId{2U}, pose_two);

    std::vector<Vec3d> posed;
    posed.reserve(mesh.positions.size());
    for (std::size_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        const auto& influences = generated.value().binding.influences.at(
            static_cast<std::uint32_t>(vertex));
        Vec3d blended{};
        for (const JointInfluence& influence : influences) {
            const carto::core::Transform& transform =
                influence.joint == JointId{2U} ? pose_two
                                               : carto::core::Transform::identity();
            const Vec3d anchor =
                influence.joint == JointId{2U} ? anchor_two : Vec3d{0.25, 0.25, 0.0};
            blended = blended +
                fixture_deform(mesh.positions[vertex], anchor, transform) *
                    influence.weight;
        }
        posed.push_back(blended);
    }

    carto::weights::WeightRefinementOptions options;
    options.max_iterations = 16U;
    options.step_size = 0.1;
    const auto refined = carto::weights::refine_weights_gradient(
        generated.value().binding, graph, mesh.positions, posed, pose, options);
    require(static_cast<bool>(refined), "weight refinement succeeds");
    require(static_cast<bool>(refined.value().receipt.validate()),
        "refinement receipt validates");
    require(refined.value().receipt.final_rms_residual <=
                refined.value().receipt.initial_rms_residual + 1e-12,
        "stub never reports a worse residual than it started from");
    require(static_cast<bool>(refined.value().binding.validate()),
        "refined binding validates");
    for (const auto& [vertex, influences] : refined.value().binding.influences) {
        static_cast<void>(vertex);
        require(static_cast<bool>(
                carto::character::validate_influences(influences)),
            "refined influences stay admitted");
    }

    const auto repeat = carto::weights::refine_weights_gradient(
        generated.value().binding, graph, mesh.positions, posed, pose, options);
    require(static_cast<bool>(repeat), "repeat refinement succeeds");
    const auto first = refined.value().receipt.canonical_digest();
    const auto second = repeat.value().receipt.canonical_digest();
    require(static_cast<bool>(first) && static_cast<bool>(second),
        "refinement receipt digests compute");
    require(first.value() == second.value(),
        "weight refinement is deterministic");
    const auto reparsed =
        carto::weights::WeightRefinementReceipt::deserialize(
            refined.value().receipt.serialize());
    require(static_cast<bool>(reparsed), "refinement receipt round trips");
}

} // namespace

// Segmented grid recentred on x = 0 so it is mirror-symmetric across the YZ
// plane, with a hand-authored asymmetric two-joint binding: joint 1 is the
// left (negative x) joint, joint 2 the right one.
SkinBinding symmetric_fixture_binding(const CompiledMesh& mesh) {
    SkinBinding binding;
    binding.mesh_asset = 9U;
    binding.mesh_digest = digest("symmetric-grid-v1");
    binding.graph_digest = digest("left-right-graph-v1");
    for (std::uint32_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        const double x = mesh.positions[vertex].x;
        const double right = x > 1e-12 ? 0.8 : (x < -1e-12 ? 0.5 : 0.7);
        binding.influences.emplace(vertex, std::vector<JointInfluence>{
            JointInfluence{JointId{1U}, 1.0 - right},
            JointInfluence{JointId{2U}, right}});
    }
    require(static_cast<bool>(binding.validate()), "symmetric fixture binding validates");
    return binding;
}

CompiledMesh symmetric_grid_mesh() {
    CompiledMesh mesh = segmented_box_mesh();
    for (Vec3d& position : mesh.positions) position.x -= 0.5;
    return mesh;
}

double weight_of(const std::vector<JointInfluence>& influences, JointId joint) {
    for (const JointInfluence& influence : influences) {
        if (influence.joint == joint) return influence.weight;
    }
    return 0.0;
}

void flood_sets_listed_vertices_only_and_refuses_hostile_requests() {
    const CompiledMesh mesh = symmetric_grid_mesh();
    const SkinBinding original = symmetric_fixture_binding(mesh);

    carto::weights::FloodRequest request;
    request.joint = JointId{2U};
    request.op = carto::weights::PaintOp::set;
    request.value = 1.0;
    request.vertices = {3U, 0U};
    const auto flooded = carto::weights::flood_weights(original, mesh, request, nullptr);
    require(static_cast<bool>(flooded), "flood succeeds");
    require(flooded.value().receipt.kind == carto::weights::WeightEditKind::flood,
        "flood receipt records its kind");
    require(flooded.value().receipt.affected_vertex_count == 2U,
        "flood receipt counts listed vertices");
    for (std::uint32_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        const auto& before = original.influences.at(vertex);
        const auto& after = flooded.value().binding.influences.at(vertex);
        require(static_cast<bool>(carto::character::validate_influences(after)),
            "flooded influences stay admitted");
        if (vertex == 0U || vertex == 3U) {
            require(weight_of(after, JointId{2U}) > weight_of(before, JointId{2U}),
                "flooded vertex gains the target joint");
        } else {
            require(influences_equal(before, after), "unlisted vertices are untouched");
        }
    }

    // A locked joint keeps its weight; the target joint absorbs the rest.
    carto::weights::WeightLocks locks;
    require(static_cast<bool>(locks.lock(0U, JointId{1U})), "flood lock sets");
    request.vertices = {0U};
    const auto locked = carto::weights::flood_weights(original, mesh, request, &locks);
    require(static_cast<bool>(locked), "locked flood succeeds");
    const auto& locked_after = locked.value().binding.influences.at(0U);
    require(weight_of(locked_after, JointId{1U}) ==
            weight_of(original.influences.at(0U), JointId{1U}),
        "locked weight survives the flood");
    require(!locked.value().receipt.locks_digest.is_zero(), "flood receipt binds locks");

    const auto reparsed = carto::weights::WeightEditReceipt::deserialize(
        flooded.value().receipt.serialize());
    require(static_cast<bool>(reparsed), "weight edit receipt round trips");
    require(reparsed.value().serialize() == flooded.value().receipt.serialize(),
        "weight edit receipt canonical form is stable");
    require(!carto::weights::WeightEditReceipt::deserialize(
                flooded.value().receipt.serialize() + "EXTRA\n"),
        "weight edit receipt trailing data refuses");

    carto::weights::FloodRequest hostile = request;
    hostile.joint = JointId{77U};
    require(!carto::weights::flood_weights(original, mesh, hostile, nullptr),
        "flooding an unknown joint refuses");
    hostile = request;
    hostile.op = carto::weights::PaintOp::smooth;
    require(!carto::weights::flood_weights(original, mesh, hostile, nullptr),
        "smooth flood refuses");
    hostile = request;
    hostile.vertices = {1U, 1U};
    require(!carto::weights::flood_weights(original, mesh, hostile, nullptr),
        "duplicate flood vertices refuse");
    hostile = request;
    hostile.vertices = {static_cast<std::uint32_t>(mesh.positions.size())};
    require(!carto::weights::flood_weights(original, mesh, hostile, nullptr),
        "out-of-mesh flood vertex refuses");
    hostile = request;
    hostile.value = 1.5;
    require(!carto::weights::flood_weights(original, mesh, hostile, nullptr),
        "out-of-range flood value refuses");
    SkinBinding partial = original;
    partial.influences.erase(5U);
    hostile = request;
    hostile.vertices = {5U};
    require(!carto::weights::flood_weights(partial, mesh, hostile, nullptr),
        "flooding a rigid vertex refuses");
}

void prune_drops_small_weights_without_losing_coverage() {
    const SkinBinding dense = make_dense_binding(digest("dense-graph-v1"));
    const auto pruned = carto::weights::prune_weights(dense, 0.08, nullptr);
    require(static_cast<bool>(pruned), "prune succeeds");
    require(pruned.value().receipt.affected_vertex_count == 3U,
        "prune affects every dense vertex");
    for (const auto& [vertex, influences] : pruned.value().binding.influences) {
        static_cast<void>(vertex);
        require(influences.size() == 6U, "the two smallest influences are pruned");
        require(static_cast<bool>(carto::character::validate_influences(influences)),
            "pruned influences stay admitted");
    }

    const auto aggressive = carto::weights::prune_weights(dense, 0.5, nullptr);
    require(static_cast<bool>(aggressive), "aggressive prune succeeds");
    for (const auto& [vertex, influences] : aggressive.value().binding.influences) {
        static_cast<void>(vertex);
        require(influences.size() == 1U && influences.front().joint == JointId{1U} &&
                std::abs(influences.front().weight - 1.0) < 1e-12,
            "a fully-below-threshold vertex keeps its strongest influence");
    }

    carto::weights::WeightLocks locks;
    require(static_cast<bool>(locks.lock(0U, JointId{8U})), "prune lock sets");
    const auto locked = carto::weights::prune_weights(dense, 0.08, &locks);
    require(static_cast<bool>(locked), "locked prune succeeds");
    require(weight_of(locked.value().binding.influences.at(0U), JointId{8U}) == 0.05,
        "locked small weight survives pruning");

    require(!carto::weights::prune_weights(dense, 0.0, nullptr), "zero threshold refuses");
    require(!carto::weights::prune_weights(dense, 1.0, nullptr), "unit threshold refuses");
}

void mirror_copies_remapped_weights_and_symmetrizes_the_plane() {
    const CompiledMesh mesh = symmetric_grid_mesh();
    const SkinBinding original = symmetric_fixture_binding(mesh);
    carto::weights::JointMirrorMap joints;
    require(static_cast<bool>(joints.pair(JointId{1U}, JointId{2U})), "mirror pair sets");
    require(static_cast<bool>(joints.pair(JointId{2U}, JointId{1U})),
        "re-adding the same pair is idempotent");
    require(!joints.pair(JointId{1U}, JointId{3U}), "a joint joins only one pair");

    carto::weights::MirrorRequest request;
    request.axis = carto::weights::MirrorAxis::x;
    request.direction = carto::weights::MirrorDirection::positive_to_negative;
    request.tolerance = 1e-6;
    const auto mirrored = carto::weights::mirror_weights(
        original, mesh, joints, request, nullptr);
    require(static_cast<bool>(mirrored), "mirror succeeds");
    std::uint64_t changed = 0U;
    for (std::uint32_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        const double x = mesh.positions[vertex].x;
        const auto& before = original.influences.at(vertex);
        const auto& after = mirrored.value().binding.influences.at(vertex);
        if (x > 1e-12) {
            require(influences_equal(before, after), "source half is untouched");
        } else if (x < -1e-12) {
            require(std::abs(weight_of(after, JointId{1U}) - 0.8) < 1e-12 &&
                    std::abs(weight_of(after, JointId{2U}) - 0.2) < 1e-12,
                "destination half takes the remapped source weights");
            ++changed;
        } else {
            require(std::abs(weight_of(after, JointId{1U}) - 0.5) < 1e-12 &&
                    std::abs(weight_of(after, JointId{2U}) - 0.5) < 1e-12,
                "plane vertices are symmetrized");
            ++changed;
        }
    }
    require(mirrored.value().receipt.affected_vertex_count == changed,
        "mirror receipt counts changed vertices");
    require(!mirrored.value().receipt.joint_map_digest.is_zero(),
        "mirror receipt binds the joint map");

    // Mirroring twice is a fixed point: the result is already symmetric.
    const auto again = carto::weights::mirror_weights(
        mirrored.value().binding, mesh, joints, request, nullptr);
    require(static_cast<bool>(again), "second mirror succeeds");
    require(again.value().receipt.affected_vertex_count == 0U,
        "double mirror changes nothing");

    CompiledMesh asymmetric = mesh;
    asymmetric.positions[0U].x -= 0.01;
    require(!carto::weights::mirror_weights(original, asymmetric, joints, request, nullptr),
        "an unmatched destination vertex refuses");

    carto::weights::WeightLocks locks;
    require(static_cast<bool>(locks.lock(0U, JointId{1U})), "mirror lock sets");
    require(!carto::weights::mirror_weights(original, mesh, joints, request, &locks),
        "mirroring over a locked weight refuses");

    carto::weights::JointMirrorMap dangling;
    require(static_cast<bool>(dangling.pair(JointId{1U}, JointId{9U})), "dangling pair sets");
    require(!carto::weights::mirror_weights(original, mesh, dangling, request, nullptr),
        "a pair onto an unbound joint refuses");

    carto::weights::MirrorRequest hostile = request;
    hostile.tolerance = 0.0;
    require(!carto::weights::mirror_weights(original, mesh, joints, hostile, nullptr),
        "zero mirror tolerance refuses");
}

void inverse_hypothesis_holdouts_report_error_tables_and_preserve_sources() {
    const auto verify_holdout_case = [](
        const char* case_name,
        std::uint64_t first_candidate_id,
        const KinematicGraph& graph,
        const SkinBinding& correct_binding,
        const SkinBinding& alternative_binding,
        const carto::assets::Sha256Digest& source_mesh_digest,
        const std::vector<Vec3d>& rest,
        const std::vector<Vec3d>& posed) {
        const std::string graph_before = graph.serialize();
        const std::string correct_binding_before = correct_binding.serialize();
        const std::string alternative_binding_before = alternative_binding.serialize();
        const std::vector<Vec3d> rest_before = rest;
        const std::vector<Vec3d> posed_before = posed;
        const std::vector<carto::weights::InverseRigHypothesisInput> candidates{
            {first_candidate_id + 1U, &graph, &alternative_binding},
            {first_candidate_id, &graph, &correct_binding},
        };
        carto::weights::InverseRigHypothesisOptions options;
        options.solver.max_iterations = 32U;
        options.solver.tolerance = 1e-10;
        const auto search = carto::weights::evaluate_inverse_rig_hypotheses(
            candidates, source_mesh_digest, rest, posed, options);
        require(static_cast<bool>(search), "holdout hypothesis search succeeds");
        require(search.value().ranked_hypotheses.size() == 2U,
            "holdout error table has one row per candidate");
        const auto& error_table = search.value().ranked_hypotheses;
        require(error_table[0U].id == first_candidate_id &&
                error_table[1U].id == first_candidate_id + 1U &&
                error_table[0U].status ==
                    carto::weights::InverseHypothesisStatus::converged &&
                error_table[1U].status !=
                    carto::weights::InverseHypothesisStatus::rejected &&
                error_table[0U].final_rms_residual.has_value() &&
                error_table[1U].final_rms_residual.has_value() &&
                *error_table[0U].final_rms_residual < 1e-8 &&
                *error_table[1U].final_rms_residual >
                    *error_table[0U].final_rms_residual + 1e-4,
            "holdout residual table ranks the recovered rig ahead of the alternative");
        std::cout << "INV holdout " << case_name << " residual table:";
        for (const auto& row : error_table) {
            std::cout << " candidate-" << row.id << '=' << *row.final_rms_residual;
        }
        std::cout << '\n';

        const auto selection = carto::weights::lock_inverse_rig_hypothesis(
            search.value(), candidates, first_candidate_id,
            "holdout-reviewer", std::string("recover ") + case_name);
        require(static_cast<bool>(selection) &&
                selection.value().source_mesh_digest == source_mesh_digest,
            "holdout winning candidate locks to its reviewer and source revision");
        const auto rerun = carto::weights::rerun_locked_inverse_rig_hypothesis(
            selection.value(), candidates[1U], source_mesh_digest,
            rest, posed, options);
        require(static_cast<bool>(rerun) &&
                rerun.value().status == carto::weights::InverseHypothesisStatus::converged &&
                rerun.value().selection_digest == selection.value().selection_digest &&
                rerun.value().observation_digest == search.value().observation_digest &&
                rerun.value().solution.receipt.final_rms_residual < 1e-8,
            "holdout locked rerun reproduces the selected low-error result");

        require(graph.serialize() == graph_before &&
                correct_binding.serialize() == correct_binding_before &&
                alternative_binding.serialize() == alternative_binding_before &&
                rest.size() == rest_before.size() && posed.size() == posed_before.size(),
            "holdout search and rerun preserve all rig source records");
        for (std::size_t index = 0U; index < rest.size(); ++index) {
            require(rest[index].x == rest_before[index].x &&
                    rest[index].y == rest_before[index].y &&
                    rest[index].z == rest_before[index].z &&
                    posed[index].x == posed_before[index].x &&
                    posed[index].y == posed_before[index].y &&
                    posed[index].z == posed_before[index].z,
                "holdout search and rerun preserve the observed vertices");
        }
    };

    // Holdout A: the segmented planar body and its two-joint authored rig.
    const CompiledMesh segmented_mesh = segmented_box_mesh();
    const KinematicGraph segmented_graph = two_joint_graph();
    const auto segmented_source = digest("holdout-segmented-creature-v1");
    const auto segmented_generated = carto::weights::generate_auto_weights(
        segmented_mesh, segmented_graph, segmented_source, 701U, rigid_recipe());
    require(static_cast<bool>(segmented_generated),
        "segmented holdout weights generate");
    const SkinBinding segmented_binding = segmented_generated.value().binding;
    SkinBinding segmented_alternative = segmented_binding;
    for (auto& [vertex, influences] : segmented_alternative.influences) {
        static_cast<void>(vertex);
        influences = {{JointId{1U}, 1.0}};
    }
    carto::core::Transform segmented_joint_pose;
    const double segmented_angle = 36.0 * 3.14159265358979323846 / 180.0;
    segmented_joint_pose.rotation = {
        0.0, 0.0, std::sin(segmented_angle * 0.5),
        std::cos(segmented_angle * 0.5)};
    segmented_joint_pose.translation = {0.08, -0.03, 0.0};
    std::vector<Vec3d> segmented_posed;
    segmented_posed.reserve(segmented_mesh.positions.size());
    for (std::size_t vertex = 0U; vertex < segmented_mesh.positions.size(); ++vertex) {
        const auto& influence = segmented_binding.influences.at(
            static_cast<std::uint32_t>(vertex)).front();
        if (influence.joint == JointId{2U}) {
            segmented_posed.push_back(fixture_deform(
                segmented_mesh.positions[vertex], {0.75, 0.25, 0.0},
                segmented_joint_pose));
        } else {
            segmented_posed.push_back(segmented_mesh.positions[vertex]);
        }
    }
    verify_holdout_case("segmented-body", 100U, segmented_graph,
        segmented_binding, segmented_alternative, segmented_source,
        segmented_mesh.positions, segmented_posed);

    // Holdout B: an irregular three-segment 3D creature and a distinct rig.
    // Each rigid segment has four non-coplanar points for a stable fit.
    KinematicGraph creature_graph;
    KinematicNode creature_root;
    creature_root.id = JointId{10U};
    creature_root.rest.translation = {-1.0, 0.0, 0.0};
    KinematicNode creature_middle;
    creature_middle.id = JointId{20U};
    creature_middle.parent = JointId{10U};
    creature_middle.rest.translation = {0.0, 0.0, 0.0};
    KinematicNode creature_tip;
    creature_tip.id = JointId{30U};
    creature_tip.parent = JointId{20U};
    creature_tip.rest.translation = {1.0, 0.0, 0.0};
    require(static_cast<bool>(creature_graph.insert_node(creature_root)) &&
            static_cast<bool>(creature_graph.insert_node(creature_middle)) &&
            static_cast<bool>(creature_graph.insert_node(creature_tip)) &&
            static_cast<bool>(creature_graph.validate()),
        "three-segment holdout graph validates");
    const auto creature_source = digest("holdout-irregular-creature-v1");
    const auto creature_graph_digest = creature_graph.canonical_digest();
    require(static_cast<bool>(creature_graph_digest),
        "three-segment holdout graph digest computes");
    const std::vector<Vec3d> segment_offsets{
        {-0.18, -0.12, 0.0}, {0.16, -0.11, 0.03},
        {-0.13, 0.14, 0.07}, {0.04, 0.02, 0.28},
    };
    std::vector<Vec3d> creature_rest;
    SkinBinding creature_binding;
    creature_binding.mesh_asset = 808U;
    creature_binding.mesh_digest = creature_source;
    creature_binding.graph_digest = creature_graph_digest.value();
    const std::vector<Vec3d> segment_centers{
        {-1.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0},
    };
    const std::vector<JointId> segment_joints{
        JointId{10U}, JointId{20U}, JointId{30U},
    };
    for (std::size_t segment = 0U; segment < segment_centers.size(); ++segment) {
        for (const Vec3d& offset : segment_offsets) {
            const auto vertex = static_cast<std::uint32_t>(creature_rest.size());
            creature_rest.push_back(segment_centers[segment] + offset);
            creature_binding.influences.emplace(vertex,
                std::vector<JointInfluence>{{segment_joints[segment], 1.0}});
        }
    }
    require(static_cast<bool>(creature_binding.validate()),
        "three-segment holdout binding validates");
    SkinBinding creature_alternative = creature_binding;
    for (auto& [vertex, influences] : creature_alternative.influences) {
        static_cast<void>(vertex);
        influences = {{JointId{10U}, 1.0}};
    }
    std::map<JointId, carto::core::Transform> creature_pose;
    carto::core::Transform middle_pose;
    const double middle_angle = 24.0 * 3.14159265358979323846 / 180.0;
    middle_pose.rotation = {
        0.0, 0.0, std::sin(middle_angle * 0.5), std::cos(middle_angle * 0.5)};
    middle_pose.translation = {0.03, 0.06, 0.0};
    carto::core::Transform tip_pose;
    const double tip_angle = 18.0 * 3.14159265358979323846 / 180.0;
    tip_pose.rotation = {
        0.0, std::sin(tip_angle * 0.5), 0.0, std::cos(tip_angle * 0.5)};
    tip_pose.translation = {-0.02, 0.0, 0.05};
    creature_pose.emplace(JointId{20U}, middle_pose);
    creature_pose.emplace(JointId{30U}, tip_pose);
    std::vector<Vec3d> creature_posed;
    creature_posed.reserve(creature_rest.size());
    for (std::size_t vertex = 0U; vertex < creature_rest.size(); ++vertex) {
        const JointId joint = segment_joints[vertex / segment_offsets.size()];
        const auto found = creature_pose.find(joint);
        const carto::core::Transform pose = found == creature_pose.end()
            ? carto::core::Transform::identity() : found->second;
        const KinematicNode* node = creature_graph.find(joint);
        require(node != nullptr, "creature holdout joint exists");
        creature_posed.push_back(fixture_deform(
            creature_rest[vertex], node->rest.translation, pose));
    }
    verify_holdout_case("irregular-three-segment", 200U, creature_graph,
        creature_binding, creature_alternative, creature_source,
        creature_rest, creature_posed);
}

// Identity correspondence for every vertex of the segmented box fixture:
// each target vertex gathers from the source vertex of the same index.
carto::weights::WeightCorrespondence identity_correspondence(
    std::size_t vertex_count) {
    carto::weights::WeightCorrespondence correspondence;
    for (std::uint32_t vertex = 0U; vertex < vertex_count; ++vertex) {
        require(static_cast<bool>(correspondence.map(vertex, {{vertex, 1.0}})),
            "identity correspondence maps");
    }
    return correspondence;
}

carto::weights::WeightTransferOptions transfer_options(
    carto::character::RigLayerId layer_id,
    const char* mesh_tag,
    std::uint64_t mesh_asset) {
    carto::weights::WeightTransferOptions options;
    options.output_layer_id = layer_id;
    options.output_mesh_digest = digest(mesh_tag);
    options.output_mesh_asset = mesh_asset;
    return options;
}

void transfer_identity_correspondence_reproduces_source_weights() {
    const CompiledMesh mesh = segmented_box_mesh();
    const KinematicGraph graph = two_joint_graph();
    const auto generated = carto::weights::generate_auto_weights(
        mesh, graph, digest("segmented-box-v1"), 42U, rigid_recipe());
    require(static_cast<bool>(generated), "fixture generation succeeds");
    const SkinBinding& source = generated.value().binding;

    const carto::weights::WeightCorrespondence correspondence =
        identity_correspondence(mesh.positions.size());
    const carto::weights::JointTransferMap joints;
    const carto::weights::WeightTransferOptions options =
        transfer_options(carto::character::RigLayerId{21U},
            "transferred-mesh-v1", 77U);

    const auto transferred = carto::weights::transfer_weights(
        source, generated.value().layer, correspondence, joints, options);
    require(static_cast<bool>(transferred), "identity transfer succeeds");
    require(static_cast<bool>(transferred.value().binding.validate()),
        "transferred binding validates");
    require(static_cast<bool>(transferred.value().layer.validate()),
        "transferred layer validates");
    require(static_cast<bool>(transferred.value().receipt.validate()),
        "transfer receipt validates");
    require(transferred.value().layer.provenance ==
            carto::character::WeightProvenance::transferred,
        "transferred layer provenance is transferred");
    require(transferred.value().binding.mesh_digest == options.output_mesh_digest &&
            transferred.value().binding.mesh_asset == 77U,
        "output binding pins the target mesh identity");
    require(transferred.value().binding.graph_digest == source.graph_digest,
        "identity transfer keeps the source graph digest");
    require(transferred.value().layer.mesh_topology_digest ==
            options.output_mesh_digest &&
            transferred.value().layer.joint_graph_digest == source.graph_digest,
        "transferred layer pins the target identities");
    require(transferred.value().layer.provenance_digest ==
            transferred.value().receipt.correspondence_digest,
        "transferred layer provenance binds the correspondence digest");
    require(transferred.value().receipt.joint_map_digest.is_zero(),
        "identity joint map records a zero digest");
    require(transferred.value().receipt.transferred_vertex_count ==
            mesh.positions.size(),
        "receipt counts every transferred vertex");
    require(transferred.value().receipt.rigid_sample_count == 0U,
        "identity transfer touches no rigid sample");
    for (std::uint32_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        require(influences_equal(
                transferred.value().binding.influences.at(vertex),
                source.influences.at(vertex)),
            "identity transfer reproduces source weights exactly");
    }

    // Determinism: a repeat run reproduces binding and receipt digests.
    const auto repeat = carto::weights::transfer_weights(
        source, generated.value().layer, correspondence, joints, options);
    require(static_cast<bool>(repeat), "repeat transfer succeeds");
    const auto first_receipt = transferred.value().receipt.canonical_digest();
    const auto second_receipt = repeat.value().receipt.canonical_digest();
    require(static_cast<bool>(first_receipt) && static_cast<bool>(second_receipt),
        "transfer receipt digests compute");
    require(first_receipt.value() == second_receipt.value(),
        "weight transfer is deterministic");

    // Serialization round trips stay canonical.
    const auto reparsed = carto::weights::WeightTransferReceipt::deserialize(
        transferred.value().receipt.serialize());
    require(static_cast<bool>(reparsed), "transfer receipt round trips");
    require(reparsed.value().serialize() == transferred.value().receipt.serialize(),
        "transfer receipt canonical form is stable");
    const auto corr_reparsed = carto::weights::WeightCorrespondence::deserialize(
        correspondence.serialize());
    require(static_cast<bool>(corr_reparsed), "correspondence round trips");
    require(corr_reparsed.value().serialize() == correspondence.serialize(),
        "correspondence canonical form is stable");
    const auto first_corr = correspondence.canonical_digest();
    const auto second_corr = corr_reparsed.value().canonical_digest();
    require(static_cast<bool>(first_corr) && static_cast<bool>(second_corr),
        "correspondence digests compute");
    require(first_corr.value() == second_corr.value(),
        "correspondence digest survives the round trip");
}

void transfer_blends_barycentric_samples_and_counts_rigid_ones() {
    const CompiledMesh mesh = segmented_box_mesh();
    const KinematicGraph graph = two_joint_graph();
    // A smoothed, non-rigid source layer gives the gather real blending work.
    carto::weights::AutoWeightRecipe recipe;
    recipe.output_layer_id = carto::character::RigLayerId{23U};
    recipe.max_influences_per_vertex = 2U;
    recipe.nearest_joint_candidates = 2U;
    recipe.smoothing_iterations = 4U;
    recipe.smoothing_strength = 0.25;
    const auto generated = carto::weights::generate_auto_weights(
        mesh, graph, digest("segmented-box-v1"), 42U, recipe);
    require(static_cast<bool>(generated), "smoothed fixture generates");
    const SkinBinding& source = generated.value().binding;

    // Barycentric blend of vertices 0 and 4; construction order must not
    // matter, the canonical form sorts samples by source vertex.
    carto::weights::WeightCorrespondence blended;
    require(static_cast<bool>(blended.map(100U, {{0U, 0.25}, {4U, 0.75}})),
        "blend correspondence maps");
    carto::weights::WeightCorrespondence reversed;
    require(static_cast<bool>(reversed.map(100U, {{4U, 0.75}, {0U, 0.25}})),
        "reversed blend correspondence maps");
    const auto first_digest = blended.canonical_digest();
    const auto second_digest = reversed.canonical_digest();
    require(static_cast<bool>(first_digest) && static_cast<bool>(second_digest),
        "blend correspondence digests compute");
    require(first_digest.value() == second_digest.value(),
        "correspondence canonical form is order independent");

    // A partially rigid gather: sample 5 points at a vertex with no
    // influences in this source binding; it contributes nothing, is counted
    // on the receipt, and the surviving sample still carries the vertex.
    SkinBinding sparse = source;
    sparse.influences.erase(5U);
    require(static_cast<bool>(sparse.validate()),
        "source binding with one rigid vertex validates");
    require(static_cast<bool>(blended.map(101U, {{5U, 0.3}, {0U, 0.7}})),
        "partially rigid correspondence maps");

    const carto::weights::JointTransferMap joints;
    const carto::weights::WeightTransferOptions options =
        transfer_options(carto::character::RigLayerId{24U},
            "blended-mesh-v1", 78U);
    const auto transferred = carto::weights::transfer_weights(
        sparse, generated.value().layer, blended, joints, options);
    require(static_cast<bool>(transferred), "blend transfer succeeds");
    require(transferred.value().receipt.rigid_sample_count == 1U,
        "receipt counts the rigid sample honestly");

    // Known answer: target 100 blends its two sources 0.25/0.75 and
    // renormalizes; the blend of two normalized lists needs no rescale.
    std::map<JointId, double> expected;
    for (const JointInfluence& influence : source.influences.at(0U)) {
        expected[influence.joint] += 0.25 * influence.weight;
    }
    for (const JointInfluence& influence : source.influences.at(4U)) {
        expected[influence.joint] += 0.75 * influence.weight;
    }
    const auto& blended_vertex =
        transferred.value().binding.influences.at(100U);
    require(blended_vertex.size() == expected.size(),
        "blend carries the union of source joints");
    for (const JointInfluence& influence : blended_vertex) {
        require(std::abs(influence.weight - expected.at(influence.joint)) < 1e-9,
            "blended weight matches the barycentric known answer");
    }
    require(static_cast<bool>(
            carto::character::validate_influences(blended_vertex)),
        "blended influences stay admitted");

    // Target 101 survives on its single live sample at weight 1.0.
    const auto& carried_vertex =
        transferred.value().binding.influences.at(101U);
    require(carried_vertex.size() == source.influences.at(0U).size(),
        "carried vertex keeps its live source's influence count");
    for (const JointInfluence& influence : carried_vertex) {
        require(std::abs(influence.weight -
                    weight_of(source.influences.at(0U), influence.joint)) < 1e-9,
            "carried vertex takes its live source's weights");
    }
}

void transfer_caps_influences_and_honors_locks() {
    const CompiledMesh mesh = segmented_box_mesh();
    const KinematicGraph graph = two_joint_graph();
    carto::weights::AutoWeightRecipe recipe;
    recipe.output_layer_id = carto::character::RigLayerId{25U};
    recipe.max_influences_per_vertex = 2U;
    recipe.nearest_joint_candidates = 2U;
    recipe.smoothing_iterations = 4U;
    recipe.smoothing_strength = 0.25;
    const auto generated = carto::weights::generate_auto_weights(
        mesh, graph, digest("segmented-box-v1"), 42U, recipe);
    require(static_cast<bool>(generated), "smoothed fixture generates");
    const SkinBinding& source = generated.value().binding;

    // Deterministic cap: blend two vertices from different joint regions and
    // keep only the strongest gathered influence.
    carto::weights::WeightCorrespondence correspondence;
    require(static_cast<bool>(correspondence.map(100U, {{0U, 0.5}, {19U, 0.5}})),
        "cap correspondence maps");
    const carto::weights::JointTransferMap joints;
    carto::weights::WeightTransferOptions options =
        transfer_options(carto::character::RigLayerId{26U},
            "capped-mesh-v1", 79U);
    options.max_influences_per_vertex = 1U;

    const auto capped = carto::weights::transfer_weights(
        source, generated.value().layer, correspondence, joints, options);
    require(static_cast<bool>(capped), "capped transfer succeeds");
    const auto& capped_vertex = capped.value().binding.influences.at(100U);
    require(capped_vertex.size() == 1U, "influence cap is applied");
    require(std::abs(capped_vertex.front().weight - 1.0) < 1e-12,
        "capped vertex renormalizes to one");
    // The survivor is the strongest gathered joint (lowest id on ties).
    std::map<JointId, double> gathered;
    for (const JointInfluence& influence : source.influences.at(0U)) {
        gathered[influence.joint] += 0.5 * influence.weight;
    }
    for (const JointInfluence& influence : source.influences.at(19U)) {
        gathered[influence.joint] += 0.5 * influence.weight;
    }
    JointId strongest = gathered.begin()->first;
    for (const auto& [joint, weight] : gathered) {
        if (weight > gathered.at(strongest)) strongest = joint;
    }
    require(capped_vertex.front().joint == strongest,
        "cap keeps the strongest gathered influence");

    // Locks: a locked (vertex, joint) weight survives the transfer exactly
    // and the remaining mass renormalizes around it.
    const std::uint32_t locked_target = 100U;
    const std::uint32_t first_source_vertex = 0U;
    const std::uint32_t second_source_vertex = 19U;
    std::map<JointId, double> locked_expected;
    for (const JointInfluence& influence : source.influences.at(first_source_vertex)) {
        locked_expected[influence.joint] += 0.5 * influence.weight;
    }
    for (const JointInfluence& influence : source.influences.at(second_source_vertex)) {
        locked_expected[influence.joint] += 0.5 * influence.weight;
    }
    require(locked_expected.size() == 2U,
        "locked blend fixture spans two source joints");
    carto::weights::WeightCorrespondence locked_corr;
    require(static_cast<bool>(locked_corr.map(
                locked_target,
                {{first_source_vertex, 0.5}, {second_source_vertex, 0.5}})),
        "locked correspondence maps");
    auto weaker = locked_expected.begin();
    for (auto candidate = locked_expected.begin(); candidate != locked_expected.end();
         ++candidate) {
        if (candidate->second < weaker->second) weaker = candidate;
    }
    const JointId locked_joint = weaker->first;
    const double weaker_weight = weaker->second;
    carto::weights::WeightLocks locks;
    require(static_cast<bool>(locks.lock(locked_target, locked_joint)),
        "transfer lock sets");

    carto::weights::WeightTransferOptions lock_options =
        transfer_options(carto::character::RigLayerId{27U},
            "locked-mesh-v1", 80U);
    const auto locked = carto::weights::transfer_weights(
        source, generated.value().layer, locked_corr, joints, lock_options,
        &locks);
    require(static_cast<bool>(locked), "locked transfer succeeds");
    const auto& locked_vertex = locked.value().binding.influences.at(locked_target);
    require(weight_of(locked_vertex, locked_joint) == weaker_weight,
        "locked weight survives the transfer exactly");
    double lock_sum = 0.0;
    for (const JointInfluence& influence : locked_vertex) lock_sum += influence.weight;
    require(std::abs(lock_sum - 1.0) < 1e-9,
        "locked vertex renormalizes to one");
    require(!locked.value().receipt.locks_digest.is_zero(),
        "transfer receipt binds the locks");
    require(locked.value().layer.locks_digest.has_value(),
        "transferred layer binds the locks digest");

    // The cap must not silently drop a locked weight.
    carto::weights::WeightTransferOptions cap_lock_options = lock_options;
    cap_lock_options.output_layer_id = carto::character::RigLayerId{28U};
    cap_lock_options.output_mesh_digest = digest("cap-lock-mesh-v1");
    cap_lock_options.max_influences_per_vertex = 1U;
    require(!carto::weights::transfer_weights(
                source, generated.value().layer, locked_corr, joints,
                cap_lock_options, &locks),
        "a cap that drops a locked weight refuses");
}

void transfer_remaps_joints_across_skeletons() {
    const CompiledMesh mesh = segmented_box_mesh();
    const KinematicGraph graph = two_joint_graph();
    const auto generated = carto::weights::generate_auto_weights(
        mesh, graph, digest("segmented-box-v1"), 42U, rigid_recipe());
    require(static_cast<bool>(generated), "fixture generation succeeds");
    const SkinBinding& source = generated.value().binding;

    // Cross-skeleton mode: joint 2's region moves to joint 5 on a different
    // graph, pinned by an explicit target graph digest.
    carto::weights::JointTransferMap remap;
    require(static_cast<bool>(remap.pair(JointId{2U}, JointId{5U})),
        "remap pair sets");
    require(!remap.pair(JointId{1U}, JointId{1U}),
        "an identity remap pair refuses");
    carto::weights::JointTransferMap clash;
    require(static_cast<bool>(clash.pair(JointId{1U}, JointId{5U})),
        "first clash pair sets");
    require(!clash.pair(JointId{2U}, JointId{5U}),
        "a target joint claimed twice refuses");
    require(!clash.pair(JointId{1U}, JointId{7U}),
        "a source joint mapped twice refuses");

    const carto::weights::WeightCorrespondence correspondence =
        identity_correspondence(mesh.positions.size());
    carto::weights::WeightTransferOptions options =
        transfer_options(carto::character::RigLayerId{29U},
            "remapped-mesh-v1", 81U);
    options.output_graph_digest = digest("retarget-graph-v1");

    const auto transferred = carto::weights::transfer_weights(
        source, generated.value().layer, correspondence, remap, options);
    require(static_cast<bool>(transferred), "remapped transfer succeeds");
    require(transferred.value().binding.graph_digest == options.output_graph_digest,
        "remapped binding pins the target graph digest");
    require(!transferred.value().receipt.joint_map_digest.is_zero(),
        "remapped receipt binds the joint map");
    for (std::uint32_t vertex = 0U; vertex < mesh.positions.size(); ++vertex) {
        const JointId expected = source.influences.at(vertex).front().joint == JointId{2U}
            ? JointId{5U} : JointId{1U};
        const auto& influences =
            transferred.value().binding.influences.at(vertex);
        require(influences.size() == 1U && influences.front().joint == expected,
            "remapped vertex takes the mapped joint");
        require(static_cast<bool>(
                carto::character::validate_influences(influences)),
            "remapped influences stay admitted");
    }

    // A non-identity joint map without the target graph digest refuses:
    // the output would claim joints on an unknown graph.
    carto::weights::WeightTransferOptions bare_options =
        transfer_options(carto::character::RigLayerId{30U},
            "bare-mesh-v1", 82U);
    require(!carto::weights::transfer_weights(
                source, generated.value().layer, correspondence, remap,
                bare_options),
        "remapping without the target graph digest refuses");
}

void transfer_refuses_unmapped_and_hostile_inputs() {
    const CompiledMesh mesh = segmented_box_mesh();
    const KinematicGraph graph = two_joint_graph();
    const auto generated = carto::weights::generate_auto_weights(
        mesh, graph, digest("segmented-box-v1"), 42U, rigid_recipe());
    require(static_cast<bool>(generated), "fixture generation succeeds");
    const SkinBinding& source = generated.value().binding;
    const carto::weights::JointTransferMap joints;

    // A target vertex whose every sample is rigid has no corresponding
    // source influences: the whole transfer refuses, never a partial layer.
    SkinBinding sparse = source;
    sparse.influences.erase(5U);
    carto::weights::WeightCorrespondence unmapped;
    require(static_cast<bool>(unmapped.map(200U, {{5U, 1.0}})),
        "unmapped correspondence maps");
    const carto::weights::WeightTransferOptions options =
        transfer_options(carto::character::RigLayerId{31U},
            "refused-mesh-v1", 83U);
    const auto refused = carto::weights::transfer_weights(
        sparse, generated.value().layer, unmapped, joints, options);
    require(!refused, "an unmapped target vertex refuses the transfer");
    require(std::string(refused.error().message).find("no corresponding") !=
            std::string::npos,
        "refusal names the missing-source diagnostic");

    // The source layer must actually describe the source binding.
    carto::character::SkinWeightLayer mismatched = generated.value().layer;
    mismatched.mesh_topology_digest = digest("some-other-mesh");
    const carto::weights::WeightCorrespondence correspondence =
        identity_correspondence(mesh.positions.size());
    require(!carto::weights::transfer_weights(
                source, mismatched, correspondence, joints, options),
        "a layer pin mismatch refuses");

    // Hostile options refuse.
    carto::weights::WeightTransferOptions hostile = options;
    hostile.output_mesh_digest = carto::assets::Sha256Digest{};
    require(!carto::weights::transfer_weights(
                source, generated.value().layer, correspondence, joints, hostile),
        "a zero target mesh digest refuses");
    hostile = options;
    hostile.output_mesh_asset = 0U;
    require(!carto::weights::transfer_weights(
                source, generated.value().layer, correspondence, joints, hostile),
        "a zero target mesh asset refuses");
    hostile = options;
    hostile.output_layer_id = carto::character::RigLayerId{0U};
    require(!carto::weights::transfer_weights(
                source, generated.value().layer, correspondence, joints, hostile),
        "a zero layer identity refuses");
    hostile = options;
    hostile.max_influences_per_vertex = 9U;
    require(!carto::weights::transfer_weights(
                source, generated.value().layer, correspondence, joints, hostile),
        "an out-of-range influence cap refuses");

    // Hostile correspondence construction fails closed.
    carto::weights::WeightCorrespondence bad;
    require(!bad.map(0U, {}), "an empty sample list refuses");
    require(!bad.map(0U, {{1U, 0.5}, {2U, 0.4}}), "samples not summing to one refuse");
    require(!bad.map(0U, {{1U, 0.5}, {1U, 0.5}}), "duplicate samples refuse");
    require(!bad.map(0U, {{1U, 0.0}}), "a zero coordinate refuses");
    std::vector<carto::weights::WeightCorrespondence::Sample> too_many;
    for (std::uint32_t index = 0U; index < 9U; ++index) {
        too_many.push_back(
            carto::weights::WeightCorrespondence::Sample{index, 1.0 / 9.0});
    }
    require(!bad.map(0U, std::move(too_many)), "more than 8 samples refuse");

    // Hostile deserialization fails closed.
    require(!carto::weights::WeightCorrespondence::deserialize("not a record"),
        "malformed correspondence framing refuses");
    require(!carto::weights::WeightCorrespondence::deserialize(
                std::string(carto::weights::WeightCorrespondence::kMaxSerializedBytes + 1U, 'x')),
        "oversized correspondence text refuses");
    require(!carto::weights::WeightCorrespondence::deserialize(
                correspondence.serialize() + "EXTRA\n"),
        "correspondence trailing data refuses");
    require(!carto::weights::WeightTransferReceipt::deserialize(
                std::string(carto::weights::WeightTransferReceipt::kMaxSerializedBytes + 1U, 'x')),
        "oversized transfer receipt text refuses");
}

int main() {
    auto_weights_produce_rigid_segment_answer();
    auto_weights_heat_and_smoothing_stay_admitted();
    auto_weights_refuse_unreachable_components();
    paint_respects_mask_locks_and_radius();
    lowering_selects_known_subset_and_bounds_error();
    lowering_refuses_beyond_bound_and_without_poses();
    inverse_recovers_known_joint_transforms();
    inverse_rig_hypotheses_are_bounded_source_bound_and_reviewed();
    weight_refinement_stub_reports_honest_quality();
    flood_sets_listed_vertices_only_and_refuses_hostile_requests();
    prune_drops_small_weights_without_losing_coverage();
    mirror_copies_remapped_weights_and_symmetrizes_the_plane();
    inverse_hypothesis_holdouts_report_error_tables_and_preserve_sources();
    transfer_identity_correspondence_reproduces_source_weights();
    transfer_blends_barycentric_samples_and_counts_rigid_ones();
    transfer_caps_influences_and_honors_locks();
    transfer_remaps_joints_across_skeletons();
    transfer_refuses_unmapped_and_hostile_inputs();
    std::cout << "weights tests passed\n";
    return EXIT_SUCCESS;
}
