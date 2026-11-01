#include <carto/character/autorig_graph.hpp>
#include <carto/character/autorig_graph_record.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

using namespace carto;
using namespace carto::character;

namespace {
void require(bool ok, std::string_view message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
assets::Sha256Digest hash(std::string_view s) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
}
AutorigConstraintProvenance human(std::string id) {
    return {std::move(id), "artist", "reviewed", "", AutorigConstraintOrigin::human_authored};
}
void accepted_graph_requires_complete_current_human_input() {
    geometry::EditableMesh mesh;
    const auto root = mesh.add_vertex({0, 0, 0});
    const auto child = mesh.add_vertex({0, 2, 0});
    const auto side = mesh.add_vertex({1, 0, 0});
    require(root && child && side && mesh.add_face({root.value(), child.value(), side.value()}), "mesh fixture");
    AutorigAnalysisRequest request{{51U, hash("source")}, mesh.revision(),
        AutorigAxis::x, 0.0, 1e-7, {}, {}};
    auto analysis = analyze_autorig_features(mesh, request);
    require(static_cast<bool>(analysis), "analysis fixture");
    AutorigConstraintSet constraints{request.source, mesh.revision(), analysis.value().analysis_digest,
        {{"root", "root", std::nullopt, AutorigPartSide::center},
         {"child", "child", std::string("root"), AutorigPartSide::center}},
        {{"a", "root", "root", AutorigPartSide::center, root.value(), human("root")},
         {"b", "child", "child", AutorigPartSide::center, child.value(), human("child")}}, {}, {}};
    auto opened = AutorigCandidateReview::begin(mesh, request, constraints);
    require(static_cast<bool>(opened), "review opens");
    auto review = std::move(opened).value();
    const auto& set = review.candidates();
    const auto& candidate = set.ranked_candidates.front();
    require(static_cast<bool>(review.select_candidate(candidate.digest)), "review selects candidate");
    auto accepted = review.accept_review(human("decision"));
    require(static_cast<bool>(accepted), "human accepts candidate");
    std::vector<AutorigFinalNode> nodes;
    std::uint64_t next = 100U;
    for (const auto& joint : candidate.joints) {
        AutorigFinalNode node;
        node.preview_joint = joint.id;
        node.durable_joint = {next++};
        node.local_rest.translation = joint.parent ? core::Vec3d{0, 2, 0} : core::Vec3d{};
        node.node_class = kinematic::NodeClass::skinned;
        node.semantic_type = joint.semantic_role;
        nodes.push_back(node);
    }
    auto prepared = prepare_accepted_autorig_graph(mesh, request, set, accepted.value(), nodes);
    require(prepared && prepared.value().graph.validate() &&
        prepared.value().graph.nodes().size() == candidate.joints.size() &&
        prepared.value().acceptance_digest == accepted.value().digest, "prepare accepted graph");
    const auto bytes = prepared.value().graph.serialize();
    auto reopened = kinematic::KinematicGraph::deserialize(bytes);
    require(reopened && reopened.value().canonical_digest() &&
        reopened.value().canonical_digest().value() == prepared.value().graph_digest,
        "canonical graph survives serialization");
    const auto& first = *prepared.value().graph.find(nodes.front().durable_joint);
    require(!first.parent, "review root remains root under new durable identity");
    const auto& second = *prepared.value().graph.find(nodes.back().durable_joint);
    require(second.parent && *second.parent == nodes.front().durable_joint,
        "review parentage maps to durable identities");
    auto record = make_accepted_autorig_graph_record(mesh, request, set, accepted.value(), nodes);
    require(record && record.value().validate() && record.value().canonical_digest(),
        "current accepted graph prepares provenance");
    const auto record_bytes = record.value().serialize();
    auto cold = AcceptedAutorigGraphRecord::deserialize(record_bytes);
    require(cold && cold.value().acceptance.digest == accepted.value().digest &&
        cold.value().graph_digest == prepared.value().graph_digest &&
        cold.value().canonical_digest().value() == record.value().canonical_digest().value(),
        "canonical record survives cold decode with graph and review bound");
    require(!AcceptedAutorigGraphRecord::deserialize(record_bytes + "trailing"),
        "trailing record bytes refused");
    auto bad_record = record.value();
    bad_record.graph_bytes += "corruption";
    require(!bad_record.validate(), "changed graph bytes refuse against recorded digest");
    bad_record = record.value();
    bad_record.joints.back().durable = bad_record.joints.front().durable;
    require(!bad_record.validate(), "duplicate record mapping refused");
    bad_record = record.value();
    bad_record.joints.back().preview_parent.reset();
    require(!bad_record.validate(), "record parent must agree with graph parent");
    bad_record = record.value();
    bad_record.acceptance.reviewer_decision.rationale = "changed";
    require(!bad_record.validate(), "changed recorded reviewer assertion refused");
    auto duplicate = nodes;
    duplicate.back().durable_joint = duplicate.front().durable_joint;
    require(!prepare_accepted_autorig_graph(mesh, request, set, accepted.value(), duplicate), "duplicate durable IDs refused");
    duplicate = nodes;
    duplicate.back().preview_joint = {999U};
    require(!prepare_accepted_autorig_graph(mesh, request, set, accepted.value(), duplicate), "missing preview mapping refused");
    duplicate = nodes;
    duplicate.pop_back();
    require(!prepare_accepted_autorig_graph(mesh, request, set, accepted.value(), duplicate), "partial mapping refused");
    duplicate = nodes;
    duplicate.front().local_rest.translation.x = std::numeric_limits<double>::infinity();
    require(!prepare_accepted_autorig_graph(mesh, request, set, accepted.value(), duplicate), "non-finite final transform refused");
    duplicate = nodes;
    duplicate.back().local_rest.translation.x += 0.1;
    require(!prepare_accepted_autorig_graph(mesh, request, set, accepted.value(), duplicate),
        "final positions must match reviewed anchors");
    auto forged = accepted.value();
    forged.reviewer_decision.rationale = "tampered";
    require(!prepare_accepted_autorig_graph(mesh, request, set, forged, nodes), "tampered acceptance refused");
    forged = accepted.value();
    forged.candidate_set_digest = hash("other set");
    forged.digest = forged.canonical_digest();
    require(forged.validate() && !prepare_accepted_autorig_graph(mesh, request, set, forged, nodes),
        "internally consistent receipt for a different set is refused");
    auto different_request = request;
    different_request.symmetry_tolerance = 0.02;
    require(!prepare_accepted_autorig_graph(mesh, different_request, set, accepted.value(), nodes),
        "different analysis request is refused");
    auto changed = mesh;
    require(static_cast<bool>(changed.add_vertex({4, 4, 4})), "changed mesh fixture");
    require(!prepare_accepted_autorig_graph(changed, request, set, accepted.value(), nodes), "stale mesh refused");

    constraints.parts.push_back({"unrepresented", "unknown", std::string("root"), AutorigPartSide::center});
    auto incomplete = AutorigCandidateReview::begin(mesh, request, constraints);
    require(static_cast<bool>(incomplete), "unrepresented fixture opens");
    auto partial_review = std::move(incomplete).value();
    const auto& partial_set = partial_review.candidates();
    require(!partial_set.ranked_candidates.front().unrepresented_parts.empty(), "fixture has an omitted part");
    require(static_cast<bool>(partial_review.select_candidate(partial_set.ranked_candidates.front().digest)), "select incomplete candidate");
    auto partial_acceptance = partial_review.accept_review(human("partial-decision"));
    require(static_cast<bool>(partial_acceptance), "accept incomplete candidate for review");
    require(!prepare_accepted_autorig_graph(
        mesh, request, partial_set, partial_acceptance.value(), nodes), "omitted authored parts are not silently dropped");
}
}
int main() { accepted_graph_requires_complete_current_human_input(); }
