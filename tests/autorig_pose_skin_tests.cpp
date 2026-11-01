#include <carto/character/autorig_graph.hpp>
#include <carto/deform/skin.hpp>
#include <carto/pose/fk.hpp>
#include <carto/pose/ik.hpp>
#include <carto/pose/ik_fk_match.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace carto;
using namespace carto::character;

namespace {
void require(bool ok, std::string_view what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; std::exit(EXIT_FAILURE); }
}
assets::Sha256Digest hash(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}
AutorigConstraintProvenance human(std::string id) {
    return {std::move(id), "artist", "reviewed", "", AutorigConstraintOrigin::human_authored};
}
bool near(core::Vec3d a, core::Vec3d b) {
    return std::abs(a.x - b.x) < 1e-8 && std::abs(a.y - b.y) < 1e-8 &&
        std::abs(a.z - b.z) < 1e-8;
}
} // namespace

int main() {
    geometry::EditableMesh mesh;
    auto root = mesh.add_vertex({0, 0, 0});
    auto mid = mesh.add_vertex({0, 1, 0});
    auto tip = mesh.add_vertex({0, 2, 0});
    auto side = mesh.add_vertex({1, 0, 0});
    require(root && mid && tip && side && mesh.add_face({root.value(), mid.value(), side.value()}) &&
        mesh.add_face({mid.value(), tip.value(), side.value()}), "mesh");
    AutorigAnalysisRequest request{{51U, hash("source")}, mesh.revision(),
        AutorigAxis::x, 0.0, 1e-7, {}, {}};
    auto analysis = analyze_autorig_features(mesh, request);
    require(static_cast<bool>(analysis), "analysis");
    AutorigConstraintSet constraints{request.source, mesh.revision(), analysis.value().analysis_digest,
        {{"root", "root", std::nullopt, AutorigPartSide::center},
         {"mid", "mid", std::string("root"), AutorigPartSide::center},
         {"tip", "tip", std::string("mid"), AutorigPartSide::center}},
        {{"a", "root", "root", AutorigPartSide::center, root.value(), human("root")},
         {"b", "mid", "mid", AutorigPartSide::center, mid.value(), human("mid")},
         {"c", "tip", "tip", AutorigPartSide::center, tip.value(), human("tip")}}, {}, {}};
    auto opened = AutorigCandidateReview::begin(mesh, request, constraints);
    require(static_cast<bool>(opened), "review opens");
    auto review = std::move(opened).value();
    const auto& set = review.candidates();
    const auto& candidate = set.ranked_candidates.front();
    require(static_cast<bool>(review.select_candidate(candidate.digest)), "candidate selected");
    auto acceptance = review.accept_review(human("decision"));
    require(static_cast<bool>(acceptance), "candidate accepted");
    std::vector<AutorigFinalNode> nodes;
    for (const auto& joint : candidate.joints) {
        AutorigFinalNode node;
        node.preview_joint = joint.id;
        node.durable_joint = {joint.part_id == "root" ? 1U : joint.part_id == "mid" ? 2U : 3U};
        node.local_rest.translation = joint.parent ? core::Vec3d{0, 1, 0} : core::Vec3d{};
        node.kind = kinematic::JointKind::ball;
        node.node_class = kinematic::NodeClass::skinned;
        node.semantic_type = joint.semantic_role;
        nodes.push_back(node);
    }
    auto prepared = prepare_accepted_autorig_graph(mesh, request, set, acceptance.value(), nodes);
    require(static_cast<bool>(prepared), "accepted graph prepared");
    auto rotated_scaled = nodes;
    for (auto& node : rotated_scaled) {
        if (node.durable_joint.value == 1U) {
            const double half = std::sqrt(0.5);
            node.local_rest.rotation = {0, 0, half, half};
            node.local_rest.scale = {2, 2, 2};
        } else {
            node.local_rest.translation = {0.5, 0, 0};
        }
    }
    auto transformed = prepare_accepted_autorig_graph(mesh, request, set, acceptance.value(), rotated_scaled);
    require(static_cast<bool>(transformed), "rotated and scaled parent composes to reviewed anchors");
    auto transformed_fk = pose::evaluate_fk(transformed.value().graph);
    require(transformed_fk && near(transformed_fk.value().world.at({2}).translation, {0, 1, 0}) &&
        near(transformed_fk.value().world.at({3}).translation, {0, 2, 0}),
        "anchor comparison agrees with FK for rotated and scaled parent");
    for (auto& node : rotated_scaled)
        if (node.durable_joint.value == 2U) node.local_rest.translation = {0, 1, 0};
    require(!prepare_accepted_autorig_graph(mesh, request, set, acceptance.value(), rotated_scaled),
        "parent-relative translation that misses the anchor is refused");
    // A large origin must not make a world-space unit of anchor drift acceptable.
    auto distant_mesh = mesh;
    constexpr double origin = 100'000'000.0;
    require(distant_mesh.set_vertex_position(root.value(), {origin, 0, 0}) &&
        distant_mesh.set_vertex_position(mid.value(), {origin, 1, 0}) &&
        distant_mesh.set_vertex_position(tip.value(), {origin, 2, 0}) &&
        distant_mesh.set_vertex_position(side.value(), {origin + 1, 0, 0}), "distant mesh");
    auto distant_request = request;
    distant_request.expected_mesh_revision = distant_mesh.revision();
    distant_request.symmetry_plane_offset = origin;
    auto distant_analysis = analyze_autorig_features(distant_mesh, distant_request);
    require(static_cast<bool>(distant_analysis), "distant analysis");
    auto distant_constraints = constraints;
    distant_constraints.mesh_revision = distant_mesh.revision();
    distant_constraints.analysis_digest = distant_analysis.value().analysis_digest;
    auto distant_opened = AutorigCandidateReview::begin(distant_mesh, distant_request, distant_constraints);
    require(static_cast<bool>(distant_opened), "distant review");
    auto distant_review = std::move(distant_opened).value();
    const auto& distant_set = distant_review.candidates();
    const auto& distant_candidate = distant_set.ranked_candidates.front();
    require(static_cast<bool>(distant_review.select_candidate(distant_candidate.digest)), "distant selection");
    auto distant_acceptance = distant_review.accept_review(human("distant-decision"));
    require(static_cast<bool>(distant_acceptance), "distant acceptance");
    auto distant_nodes = nodes;
    for (auto& node : distant_nodes) {
        for (const auto& joint : distant_candidate.joints)
            if ((joint.part_id == "root" && node.durable_joint.value == 1U) ||
                (joint.part_id == "mid" && node.durable_joint.value == 2U) ||
                (joint.part_id == "tip" && node.durable_joint.value == 3U)) node.preview_joint = joint.id;
        if (node.durable_joint.value == 1U) node.local_rest.translation.x = origin;
    }
    require(static_cast<bool>(prepare_accepted_autorig_graph(distant_mesh, distant_request, distant_set,
        distant_acceptance.value(), distant_nodes)), "distant anchor positions accepted");
    for (auto& node : distant_nodes)
        if (node.durable_joint.value == 1U) node.local_rest.translation.x += 1;
    require(!prepare_accepted_autorig_graph(distant_mesh, distant_request, distant_set,
        distant_acceptance.value(), distant_nodes), "large origin does not permit unit anchor drift");
    const auto& graph = prepared.value().graph;
    auto rest = pose::evaluate_fk(graph);
    require(rest && near(rest.value().world.at({3}).translation, {0, 2, 0}), "FK composes authored rest");
    pose::TwoBoneIkRequest ik;
    ik.root = {1}; ik.mid = {2}; ik.effector = {3};
    ik.target = {1, 0.5, 0};
    auto solved = pose::solve_two_bone(graph, {}, nullptr, ik);
    require(solved && solved.value().converged, "IK converges on accepted graph");
    auto posed = pose::evaluate_fk(graph, solved.value().solved_locals);
    require(posed && near(posed.value().world.at({3}).translation, ik.target), "FK reaches IK target");
    // The chain and IK controls here are explicitly supplied test inputs; the
    // accepted graph does not persist an IK/FK control setup.
    pose::IkFkMatchRequest match_request;
    match_request.chain = {{1}, {2}, {3}};
    auto ik_match = pose::match_ik_to_fk(graph, solved.value().solved_locals, nullptr, match_request);
    require(ik_match && ik_match.value().position_jump <= match_request.max_position_jump,
        "FK pose matches IK controls");
    auto fk_match = pose::match_fk_to_ik(graph, {}, nullptr, ik_match.value().ik, match_request);
    require(fk_match && fk_match.value().position_jump <= match_request.max_position_jump,
        "IK controls match FK pose");
    auto matched_pose = pose::evaluate_fk(graph, fk_match.value().fk_locals);
    require(matched_pose && near(matched_pose.value().world.at({3}).translation, ik.target),
        "matched FK retains the effector position");
    kinematic::SkinBinding binding;
    binding.mesh_asset = request.source.asset_id;
    binding.mesh_digest = request.source.content_digest;
    binding.graph_digest = prepared.value().graph_digest;
    binding.influences[2] = {{{3}, 1.0}};
    require(static_cast<bool>(binding.validate()), "authored binding validates");
    deform::JointPoseSet poses{rest.value().world, posed.value().world};
    const std::array<core::Vec3d, 4> vertices{{{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {1, 0, 0}}};
    deform::SkinEvaluationInput input{vertices, binding.mesh_digest, true, &binding, &poses};
    auto skinned = deform::evaluate_lbs(input);
    require(skinned && skinned.value().positions.size() == vertices.size() &&
        near(skinned.value().positions[2], ik.target) &&
        near(skinned.value().positions[0], vertices[0]), "LBS follows posed tip and leaves rigid vertices alone");
    input.mesh_content_digest = hash("wrong-mesh");
    require(!deform::evaluate_lbs(input), "LBS rejects stale mesh digest");
    std::cout << "autorig pose and skin tests passed\n";
}
