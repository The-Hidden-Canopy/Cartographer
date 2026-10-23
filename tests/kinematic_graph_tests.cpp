#include <carto/assets/blob_store.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/kinematic/lowering.hpp>
#include <carto/kinematic/pose_preview.hpp>
#include <carto/kinematic/skin.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
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

using namespace carto;

kinematic::KinematicNode node(
    std::uint64_t id,
    std::uint64_t parent,
    kinematic::JointKind kind,
    kinematic::NodeClass node_class = kinematic::NodeClass::rigid,
    std::string semantic_type = {}) {
    kinematic::KinematicNode result;
    result.id = kinematic::JointId{id};
    result.parent = parent == 0U
        ? std::optional<kinematic::JointId>{}
        : std::optional<kinematic::JointId>{kinematic::JointId{parent}};
    result.kind = kind;
    result.node_class = node_class;
    result.semantic_type = std::move(semantic_type);
    return result;
}

// Door: one rigid frame root and one hinge leaf, the mechanical fixture from
// the docuseries implementation slice.
kinematic::KinematicGraph door_graph() {
    kinematic::KinematicGraph graph;
    kinematic::KinematicNode frame = node(
        1U, 0U, kinematic::JointKind::fixed, kinematic::NodeClass::rigid,
        "door.frame");
    kinematic::KinematicNode hinge = node(
        2U, 1U, kinematic::JointKind::hinge, kinematic::NodeClass::rigid,
        "door.hinge");
    hinge.rest.translation = {0.0, 1.0, 0.0};
    require(static_cast<bool>(graph.insert_node(frame)), "door frame enters");
    require(static_cast<bool>(graph.insert_node(hinge)), "door hinge enters");
    return graph;
}

kinematic::JointLimit door_hinge_limit() {
    kinematic::JointLimit limit;
    limit.joint = kinematic::JointId{2U};
    limit.angular_enabled = true;
    limit.angular_min = {-1.5707963267948966, 0.0, 0.0};
    limit.angular_max = {1.5707963267948966, 0.0, 0.0};
    return limit;
}

kinematic::JointLimitSet door_limits() {
    kinematic::JointLimitSet set;
    require(static_cast<bool>(set.insert(door_hinge_limit())),
        "door hinge limit enters");
    return set;
}

void valid_graphs_validate_and_round_trip() {
    kinematic::KinematicGraph graph = door_graph();
    require(static_cast<bool>(graph.validate()), "door graph validates");
    const auto first = graph.canonical_digest();
    require(static_cast<bool>(first), "door graph digest computes");

    // Same logical graph built in a different insertion order must produce the
    // same canonical serialization and digest.
    kinematic::KinematicGraph reordered;
    {
        kinematic::KinematicNode hinge = node(
            2U, 1U, kinematic::JointKind::hinge, kinematic::NodeClass::rigid,
            "door.hinge");
        hinge.rest.translation = {0.0, 1.0, 0.0};
        kinematic::KinematicNode frame = node(
            1U, 0U, kinematic::JointKind::fixed, kinematic::NodeClass::rigid,
            "door.frame");
        require(static_cast<bool>(reordered.insert_node(hinge)),
            "reordered hinge enters");
        require(static_cast<bool>(reordered.insert_node(frame)),
            "reordered frame enters");
    }
    const auto second = reordered.canonical_digest();
    require(static_cast<bool>(second), "reordered digest computes");
    require(first.value() == second.value(),
        "canonical digest is independent of insertion order");

    const auto decoded = kinematic::KinematicGraph::deserialize(graph.serialize());
    require(static_cast<bool>(decoded), "door graph deserializes");
    const auto third = decoded.value().canonical_digest();
    require(third.value() == first.value(),
        "round trip preserves the canonical digest");
    require(graph.serialize() == decoded.value().serialize(),
        "round trip preserves the canonical serialization");
}

void graph_validation_failures_are_reported() {
    {
        kinematic::KinematicGraph cyclic;
        kinematic::KinematicNode root = node(1U, 0U, kinematic::JointKind::fixed);
        kinematic::KinematicNode first = node(2U, 3U, kinematic::JointKind::hinge);
        kinematic::KinematicNode second = node(3U, 2U, kinematic::JointKind::hinge);
        require(static_cast<bool>(cyclic.insert_node(root)), "cycle root enters");
        require(static_cast<bool>(cyclic.insert_node(first)), "cycle first enters");
        require(static_cast<bool>(cyclic.insert_node(second)), "cycle second enters");
        const auto result = cyclic.validate();
        require(!result, "cycle is rejected");
        require(result.error().code == core::ErrorCode::cycle_detected,
            "cycle reports cycle_detected");
    }
    {
        kinematic::KinematicGraph two_roots;
        require(static_cast<bool>(
            two_roots.insert_node(node(1U, 0U, kinematic::JointKind::fixed))),
            "first root enters");
        require(static_cast<bool>(
            two_roots.insert_node(node(2U, 0U, kinematic::JointKind::fixed))),
            "second root enters");
        require(!two_roots.validate(), "two roots are rejected");
    }
    {
        // Inserts stay order-independent (a child may enter before its
        // parent); dangling parentage is rejected by validate().
        kinematic::KinematicGraph dangling;
        require(static_cast<bool>(
            dangling.insert_node(node(1U, 0U, kinematic::JointKind::fixed))),
            "dangling parent owner enters");
        require(static_cast<bool>(
            dangling.insert_node(node(2U, 9U, kinematic::JointKind::hinge))),
            "child with unresolved parent may still enter");
        const auto result = dangling.validate();
        require(!result, "missing parent is rejected at validation");
        require(result.error().code == core::ErrorCode::validation_failed,
            "missing parent reports validation_failed");
    }
    {
        kinematic::KinematicGraph nan_rest;
        kinematic::KinematicNode bad = node(
            1U, 0U, kinematic::JointKind::fixed);
        bad.rest.translation.x = std::numeric_limits<double>::quiet_NaN();
        require(!nan_rest.insert_node(bad), "non-finite rest transform is rejected");
    }
    {
        kinematic::KinematicGraph graph;
        require(static_cast<bool>(
            graph.insert_node(node(1U, 0U, kinematic::JointKind::fixed))),
            "duplicate probe root enters");
        require(!graph.insert_node(node(1U, 0U, kinematic::JointKind::hinge)),
            "duplicate joint identity is rejected");
    }
    {
        kinematic::KinematicGraph invalid_kind;
        kinematic::KinematicNode bad = node(7U, 0U, kinematic::JointKind::fixed);
        bad.kind = static_cast<kinematic::JointKind>(200U);
        require(!invalid_kind.insert_node(bad), "unknown joint kind is rejected");
    }
}

void hostile_graph_text_fails_closed() {
    const kinematic::KinematicGraph graph = door_graph();
    const std::string encoded = graph.serialize();

    auto future = kinematic::KinematicGraph::deserialize(
        "CARTOGRAPHER_KINEMATIC_GRAPH 99\n");
    require(!future, "future graph version is rejected");
    require(future.error().code == core::ErrorCode::version_mismatch,
        "future graph version reports version_mismatch");

    require(!kinematic::KinematicGraph::deserialize(encoded + "trailing"),
        "graph parser rejects trailing unbound data");
    require(!kinematic::KinematicGraph::deserialize("not-a-graph 1\n"),
        "graph parser rejects wrong framing");
    require(!kinematic::KinematicGraph::deserialize(
                "CARTOGRAPHER_KINEMATIC_GRAPH 1\nNODES 65537\n"),
        "hostile node count is rejected before allocation");
    require(!kinematic::KinematicGraph::deserialize(
                "CARTOGRAPHER_KINEMATIC_GRAPH 1\nNODES 1\nNODE 0 0 0 0 \"x\" 0 0 0 0 0 0 1 1 1 1\n"),
        "zero joint identity is rejected");
}

void skin_binding_normalization_is_enforced() {
    kinematic::KinematicGraph graph = door_graph();
    const auto graph_digest = graph.canonical_digest();
    require(static_cast<bool>(graph_digest), "fixture graph digest computes");

    kinematic::SkinBinding binding;
    binding.mesh_asset = 42U;
    binding.mesh_digest = digest("door-mesh-v1");
    binding.graph_digest = graph_digest.value();
    binding.influences.emplace(
        0U,
        std::vector<kinematic::JointInfluence>{
            {kinematic::JointId{2U}, 1.0}});
    require(static_cast<bool>(binding.validate()), "normalized binding validates");
    const auto binding_digest = binding.canonical_digest();
    require(static_cast<bool>(binding_digest), "binding digest computes");

    const auto decoded = kinematic::SkinBinding::deserialize(binding.serialize());
    require(static_cast<bool>(decoded), "binding deserializes");
    require(decoded.value().canonical_digest().value() == binding_digest.value(),
        "binding round trip preserves the digest");

    {
        kinematic::SkinBinding under_weighted = binding;
        under_weighted.influences[0U] = {
            {kinematic::JointId{2U}, 0.5}};
        require(!under_weighted.validate(),
            "under-normalized weights are rejected");
    }
    {
        kinematic::SkinBinding zero_weight = binding;
        zero_weight.influences[0U] = {
            {kinematic::JointId{2U}, 0.0}};
        require(!zero_weight.validate(), "zero weight is rejected");
    }
    {
        kinematic::SkinBinding unsorted = binding;
        unsorted.influences.emplace(
            1U,
            std::vector<kinematic::JointInfluence>{
                {kinematic::JointId{2U}, 0.75},
                {kinematic::JointId{1U}, 0.25}});
        require(!unsorted.validate(),
            "influences must be sorted by unique joint id");
    }
    {
        kinematic::SkinBinding too_many = binding;
        std::vector<kinematic::JointInfluence> influences;
        for (std::uint64_t joint = 1U; joint <= 9U; ++joint) {
            influences.push_back(
                {kinematic::JointId{joint}, 1.0 / 9.0});
        }
        too_many.influences[2U] = std::move(influences);
        require(!too_many.validate(),
            "influence count per vertex is bounded");
    }
    {
        kinematic::SkinBinding wrong_graph = binding;
        wrong_graph.graph_digest = digest("different-graph");
        const kinematic::JointLimitSet limits;
        const auto lowered = kinematic::describe_deforming_subgraph(
            42U, graph, wrong_graph, limits);
        require(!lowered, "stale graph binding fails closed");
        require(lowered.error().code == core::ErrorCode::stale_data,
            "stale graph binding reports stale_data");
    }
    require(!kinematic::SkinBinding::deserialize(
                "CARTOGRAPHER_SKIN_BINDING 1\nMESH 0 0a\n"),
        "malformed skin headers fail closed");
}

void door_fixture_lowers_to_rigid_archetype() {
    kinematic::KinematicGraph graph = door_graph();
    kinematic::JointLimitSet limits = door_limits();
    require(static_cast<bool>(limits.validate_for_graph(graph)),
        "door limits resolve against the door graph");

    const auto rigid = kinematic::describe_rigid_subgraph(graph);
    require(static_cast<bool>(rigid), "door lowers to a rigid archetype");
    require(rigid.value().nodes.size() == 2U,
        "door archetype keeps both rigid nodes");
    require(rigid.value().nodes.at(kinematic::JointId{2U}).kind ==
            kinematic::JointKind::hinge,
        "door hinge kind survives lowering");
    require(!rigid.value().archetype_identity.empty(),
        "door archetype carries a structural identity");
    require(rigid.value().canonical_digest().value() ==
                kinematic::ArchetypeDescriptor::deserialize(
                    rigid.value().serialize()).value().canonical_digest().value(),
        "door archetype descriptor round trips");

    kinematic::SkinBinding skin;
    skin.mesh_asset = 42U;
    skin.mesh_digest = digest("door-mesh-v1");
    skin.graph_digest = graph.canonical_digest().value();
    const auto deforming = kinematic::describe_deforming_subgraph(
        42U, graph, skin, limits);
    require(!deforming, "door has no deforming subgraph");

    const std::vector<kinematic::KinematicCapabilityCheck> supported{{
        .feature = kinematic::KinematicCapability::rigid_articulation,
        .required = true,
        .disposition = kinematic::KinematicCapabilityDisposition::supported,
        .detail = "consumer profile 19",
    }};
    require(static_cast<bool>(kinematic::require_capabilities(supported)),
        "supported required capability admits");
    const std::vector<kinematic::KinematicCapabilityCheck> unsupported{{
        .feature = kinematic::KinematicCapability::rigid_articulation,
        .required = true,
        .disposition = kinematic::KinematicCapabilityDisposition::unsupported,
        .detail = "headless capture profile",
    }};
    const auto admission = kinematic::require_capabilities(unsupported);
    require(!admission, "unsupported required capability refuses");
    require(admission.error().code == core::ErrorCode::unsupported,
        "unsupported capability reports unsupported");
}

void hinge_pose_preview_is_bounded_and_read_only() {
    const auto graph = door_graph();
    const auto limits = door_limits();
    const auto closed = kinematic::preview_hinge_pose(
        graph, limits, kinematic::JointId{2U}, 0.0);
    const auto halfway = kinematic::preview_hinge_pose(
        graph, limits, kinematic::JointId{2U}, 0.5);
    const auto open = kinematic::preview_hinge_pose(
        graph, limits, kinematic::JointId{2U}, 1.0);
    require(closed && halfway && open,
        "bounded authored door hinge supports read-only pose samples");
    require(closed.value().pivot_in_root.x == 0.0 &&
            closed.value().pivot_in_root.y == 1.0 &&
            closed.value().pivot_in_root.z == 0.0 &&
            std::abs(closed.value().angle_radians + 1.5707963267948966) < 1.0e-12,
        "closed hinge preview preserves authored pivot and lower bound");
    require(std::abs(halfway.value().angle_radians) < 1.0e-12 &&
            std::abs(halfway.value().axis_in_root.x - 1.0) < 1.0e-12,
        "midpoint hinge preview interpolates the authored travel on local X");
    require(std::abs(open.value().angle_radians - 1.5707963267948966) < 1.0e-12,
        "open hinge preview reaches but does not exceed its authored upper bound");

    require(!kinematic::preview_hinge_pose(
                graph, limits, kinematic::JointId{2U}, -0.01) &&
            !kinematic::preview_hinge_pose(
                graph, limits, kinematic::JointId{2U}, 1.01) &&
            !kinematic::preview_hinge_pose(
                graph, limits, kinematic::JointId{2U},
                std::numeric_limits<double>::quiet_NaN()),
        "out-of-range and non-finite preview controls fail closed");
    require(!kinematic::preview_hinge_pose(
                graph, limits, kinematic::JointId{999U}, 0.5),
        "unknown hinge identity cannot produce a pose");
    require(!kinematic::preview_hinge_pose(
                graph, limits, kinematic::JointId{1U}, 0.5),
        "fixed root cannot be previewed as a hinge");

    auto unsupported_axis_limit = door_hinge_limit();
    unsupported_axis_limit.angular_max.y = 0.25;
    kinematic::JointLimitSet unsupported_axis_limits;
    require(static_cast<bool>(unsupported_axis_limits.insert(unsupported_axis_limit)),
        "test-only unsupported-axis limit remains a valid authored limit");
    const auto unsupported_axis = kinematic::preview_hinge_pose(
        graph, unsupported_axis_limits, kinematic::JointId{2U}, 0.5);
    require(!unsupported_axis &&
            unsupported_axis.error().code == core::ErrorCode::unsupported,
        "unimplemented hinge-axis conventions fail closed");
}

void chain_pose_preview_composes_from_root_and_stays_derived() {
    // Base -> hinge -> slider -> fixed tip, the mechanical chain fixture for
    // the generalized read-only forward-kinematics preview.
    kinematic::KinematicGraph graph;
    require(static_cast<bool>(graph.insert_node(node(
                1U, 0U, kinematic::JointKind::fixed, kinematic::NodeClass::rigid,
                "chain.base"))),
        "chain base enters");
    kinematic::KinematicNode hinge = node(
        2U, 1U, kinematic::JointKind::hinge, kinematic::NodeClass::rigid,
        "chain.hinge");
    hinge.rest.translation = {0.0, 1.0, 0.0};
    require(static_cast<bool>(graph.insert_node(hinge)), "chain hinge enters");
    kinematic::KinematicNode slider = node(
        3U, 2U, kinematic::JointKind::slider, kinematic::NodeClass::rigid,
        "chain.slider");
    slider.rest.translation = {0.0, 0.5, 0.0};
    require(static_cast<bool>(graph.insert_node(slider)), "chain slider enters");
    kinematic::KinematicNode tip = node(
        4U, 3U, kinematic::JointKind::fixed, kinematic::NodeClass::rigid,
        "chain.tip");
    tip.rest.translation = {0.0, 0.0, 0.25};
    require(static_cast<bool>(graph.insert_node(tip)), "chain tip enters");

    kinematic::JointLimit slider_limit;
    slider_limit.joint = kinematic::JointId{3U};
    slider_limit.linear_enabled = true;
    slider_limit.linear_max = {2.0, 0.0, 0.0};
    kinematic::JointLimitSet limits;
    require(static_cast<bool>(limits.insert(door_hinge_limit())),
        "chain hinge limit enters");
    require(static_cast<bool>(limits.insert(slider_limit)),
        "chain slider limit enters");

    const auto graph_digest_before = graph.canonical_digest().value();
    const auto limits_digest_before = limits.canonical_digest().value();

    const auto lowered = kinematic::preview_chain_poses(graph, limits, {});
    require(static_cast<bool>(lowered),
        "uncontrolled chain samples every joint at its authored lower bound");
    require(std::abs(lowered.value().at(kinematic::JointId{1U}).translation.x) <
                1.0e-12,
        "root pose stays at the origin");
    const core::Transform& lowered_hinge =
        lowered.value().at(kinematic::JointId{2U});
    require(std::abs(lowered_hinge.translation.x) < 1.0e-12 &&
            std::abs(lowered_hinge.translation.y - 1.0) < 1.0e-12 &&
            std::abs(lowered_hinge.translation.z) < 1.0e-12,
        "hinge pivot matches the single-hinge preview at the lower bound");
    const core::Vec3d lowered_dir =
        lowered_hinge.rotation.rotate({0.0, 1.0, 0.0});
    require(std::abs(lowered_dir.x) < 1.0e-12 &&
            std::abs(lowered_dir.y) < 1.0e-12 &&
            std::abs(lowered_dir.z + 1.0) < 1.0e-12,
        "lower-bound hinge rotation matches the single-hinge preview angle");
    const core::Vec3d lowered_slider =
        lowered.value().at(kinematic::JointId{3U}).translation;
    require(std::abs(lowered_slider.x) < 1.0e-12 &&
            std::abs(lowered_slider.y - 1.0) < 1.0e-12 &&
            std::abs(lowered_slider.z + 0.5) < 1.0e-12,
        "slider rest offset rotates through the parent hinge frame");
    const core::Vec3d lowered_tip =
        lowered.value().at(kinematic::JointId{4U}).translation;
    require(std::abs(lowered_tip.x) < 1.0e-12 &&
            std::abs(lowered_tip.y - 1.25) < 1.0e-12 &&
            std::abs(lowered_tip.z + 0.5) < 1.0e-12,
        "tip pose composes hinge and slider deterministically from the root");

    const std::map<kinematic::JointId, double> open_controls{
        {kinematic::JointId{2U}, 1.0}, {kinematic::JointId{3U}, 1.0}};
    const auto open = kinematic::preview_chain_poses(graph, limits, open_controls);
    require(static_cast<bool>(open),
        "bounded controls map through the authored limits");
    const core::Vec3d open_dir =
        open.value().at(kinematic::JointId{2U}).rotation.rotate({0.0, 1.0, 0.0});
    require(std::abs(open_dir.x) < 1.0e-12 &&
            std::abs(open_dir.y) < 1.0e-12 &&
            std::abs(open_dir.z - 1.0) < 1.0e-12,
        "open hinge reaches its authored upper bound");
    const core::Vec3d open_slider =
        open.value().at(kinematic::JointId{3U}).translation;
    require(std::abs(open_slider.x - 2.0) < 1.0e-12 &&
            std::abs(open_slider.y - 1.0) < 1.0e-12 &&
            std::abs(open_slider.z - 0.5) < 1.0e-12,
        "slider reaches its authored upper bound in the rotated hinge frame");
    const core::Vec3d open_tip =
        open.value().at(kinematic::JointId{4U}).translation;
    require(std::abs(open_tip.x - 2.0) < 1.0e-12 &&
            std::abs(open_tip.y - 0.75) < 1.0e-12 &&
            std::abs(open_tip.z - 0.5) < 1.0e-12,
        "tip pose carries both parent motions");
    for (const auto& [joint, pose] : open.value()) {
        static_cast<void>(joint);
        const double norm = std::hypot(
            std::hypot(pose.rotation.x, pose.rotation.y),
            std::hypot(pose.rotation.z, pose.rotation.w));
        require(std::abs(norm - 1.0) < 1.0e-12 &&
                std::abs(pose.rotation.rotate({1.0, 0.0, 0.0}).length() - 1.0) <
                    1.0e-12,
            "preview orientations stay orthonormal");
    }

    const auto partial = kinematic::preview_chain_poses(
        graph, limits, {{kinematic::JointId{2U}, 1.0}});
    require(static_cast<bool>(partial),
        "partial controls leave uncontrolled joints at their lower bound");
    const core::Vec3d partial_slider =
        partial.value().at(kinematic::JointId{3U}).translation;
    require(std::abs(partial_slider.x) < 1.0e-12 &&
            std::abs(partial_slider.y - 1.0) < 1.0e-12 &&
            std::abs(partial_slider.z - 0.5) < 1.0e-12,
        "uncontrolled slider stays at its authored lower bound");

    require(graph.canonical_digest().value() == graph_digest_before &&
                limits.canonical_digest().value() == limits_digest_before,
            "chain preview leaves every authored record untouched");

    const auto out_of_range = kinematic::preview_chain_poses(
        graph, limits, {{kinematic::JointId{2U}, 1.01}});
    require(!out_of_range &&
            out_of_range.error().code == core::ErrorCode::invalid_argument,
        "out-of-range chain controls fail closed");
    const auto non_finite = kinematic::preview_chain_poses(
        graph, limits,
        {{kinematic::JointId{2U}, std::numeric_limits<double>::quiet_NaN()}});
    require(!non_finite &&
            non_finite.error().code == core::ErrorCode::invalid_argument,
        "non-finite chain controls fail closed");
    const auto unknown_joint = kinematic::preview_chain_poses(
        graph, limits, {{kinematic::JointId{999U}, 0.5}});
    require(!unknown_joint &&
            unknown_joint.error().code == core::ErrorCode::not_found,
        "controls for unknown joints fail closed");

    const kinematic::JointLimitSet empty_limits;
    const auto unbounded = kinematic::preview_chain_poses(
        graph, empty_limits, {});
    require(!unbounded &&
            unbounded.error().code == core::ErrorCode::unsupported,
        "movable joints without authored limits fail closed");

    kinematic::KinematicGraph ball_graph;
    require(static_cast<bool>(ball_graph.insert_node(node(
                1U, 0U, kinematic::JointKind::fixed))),
        "ball chain root enters");
    require(static_cast<bool>(ball_graph.insert_node(node(
                2U, 1U, kinematic::JointKind::ball))),
        "ball chain joint enters");
    const auto ball = kinematic::preview_chain_poses(ball_graph, empty_limits, {});
    require(!ball && ball.error().code == core::ErrorCode::unsupported,
        "ball joints fail closed without an invented orientation convention");

    kinematic::KinematicGraph cyclic;
    require(static_cast<bool>(cyclic.insert_node(node(
                1U, 0U, kinematic::JointKind::fixed))),
        "cycle root enters");
    require(static_cast<bool>(cyclic.insert_node(node(
                2U, 3U, kinematic::JointKind::hinge))),
        "cycle first link enters");
    require(static_cast<bool>(cyclic.insert_node(node(
                3U, 2U, kinematic::JointKind::hinge))),
        "cycle second link enters");
    require(!kinematic::preview_chain_poses(cyclic, limits, {}),
        "cyclic graphs fail closed before any pose is derived");
}

void skinned_fixtures_lower_to_skeletal_assets() {
    // Human: skinned pelvis root with spine, head, and one arm chain, plus a
    // rigid held item in the rigid complement.
    kinematic::KinematicGraph human;
    require(static_cast<bool>(human.insert_node(node(
                1U, 0U, kinematic::JointKind::fixed, kinematic::NodeClass::skinned,
                "human.pelvis"))),
        "human pelvis enters");
    require(static_cast<bool>(human.insert_node(node(
                2U, 1U, kinematic::JointKind::ball, kinematic::NodeClass::skinned,
                "human.spine"))),
        "human spine enters");
    require(static_cast<bool>(human.insert_node(node(
                3U, 2U, kinematic::JointKind::ball, kinematic::NodeClass::skinned,
                "human.head"))),
        "human head enters");
    require(static_cast<bool>(human.insert_node(node(
                4U, 2U, kinematic::JointKind::ball, kinematic::NodeClass::skinned,
                "human.shoulder"))),
        "human shoulder enters");
    require(static_cast<bool>(human.insert_node(node(
                5U, 4U, kinematic::JointKind::hinge, kinematic::NodeClass::skinned,
                "human.elbow"))),
        "human elbow enters");
    require(static_cast<bool>(human.insert_node(node(
                6U, 2U, kinematic::JointKind::fixed, kinematic::NodeClass::rigid,
                "human.lantern"))),
        "human lantern enters");
    require(static_cast<bool>(human.insert_node(node(
                7U, 6U, kinematic::JointKind::hinge, kinematic::NodeClass::rigid,
                "human.lantern.handle"))),
        "rigid lantern handle link enters");

    kinematic::SkinBinding human_skin;
    human_skin.mesh_asset = 7U;
    human_skin.mesh_digest = digest("human-mesh-v3");
    human_skin.graph_digest = human.canonical_digest().value();
    human_skin.influences.emplace(
        0U, std::vector<kinematic::JointInfluence>{
            {kinematic::JointId{1U}, 1.0}});
    human_skin.influences.emplace(
        1U, std::vector<kinematic::JointInfluence>{
            {kinematic::JointId{4U}, 0.25}, {kinematic::JointId{5U}, 0.75}});

    kinematic::JointLimitSet human_limits;
    kinematic::JointLimit shoulder;
    shoulder.joint = kinematic::JointId{4U};
    shoulder.angular_enabled = true;
    shoulder.angular_min = {-2.5, -1.0, -2.5};
    shoulder.angular_max = {2.5, 1.0, 2.5};
    require(static_cast<bool>(human_limits.insert(shoulder)),
        "human shoulder limit enters");

    const auto skeletal = kinematic::describe_deforming_subgraph(
        7U, human, human_skin, human_limits);
    require(static_cast<bool>(skeletal), "human lowers to a skeletal asset");
    require(skeletal.value().joints.size() == 5U,
        "human skeletal asset includes skinned nodes and their ancestors");
    require(!skeletal.value().joints.contains(kinematic::JointId{6U}),
        "rigid lantern stays out of the skeletal asset");
    require(!skeletal.value().joints.contains(kinematic::JointId{7U}),
        "rigid lantern handle stays out of the skeletal asset");
    require(skeletal.value().joints.at(kinematic::JointId{5U}).has_skin,
        "weighted elbow marks has_skin");
    require(skeletal.value().joints.at(kinematic::JointId{1U}).has_skin,
        "weighted pelvis root marks has_skin");
    require(!skeletal.value().joints.at(kinematic::JointId{2U}).has_skin,
        "unweighted spine leaves has_skin unset");
    require(skeletal.value().joints.at(kinematic::JointId{1U}).parent == std::nullopt,
        "pelvis root is the descriptor root");
    const auto skeletal_round_trip =
        kinematic::SkeletalAssetDescriptor::deserialize(skeletal.value().serialize());
    require(static_cast<bool>(skeletal_round_trip),
        "skeletal descriptor round trips");
    require(skeletal_round_trip.value().canonical_digest().value() ==
                skeletal.value().canonical_digest().value(),
        "skeletal descriptor digest survives the round trip");

    const auto complement = kinematic::describe_rigid_subgraph(human);
    require(static_cast<bool>(complement),
        "human rigid complement lowers to an archetype");
    require(complement.value().nodes.size() == 2U,
        "lantern and articulated handle form the rigid archetype");
    require(complement.value().nodes.at(kinematic::JointId{7U}).parent ==
                kinematic::JointId{6U} &&
            complement.value().nodes.at(kinematic::JointId{7U}).kind ==
                kinematic::JointKind::hinge,
        "rigid handle parent and hinge survive non-skin lowering");
    const auto rigid_round_trip = kinematic::ArchetypeDescriptor::deserialize(
        complement.value().serialize());
    require(static_cast<bool>(rigid_round_trip) &&
            rigid_round_trip.value().canonical_digest().value() ==
                complement.value().canonical_digest().value(),
        "mixed-articulation rigid export round-trips without skin payloads");

    // Animal: quadruped with four identical skinned leg chains.
    kinematic::KinematicGraph quadruped;
    require(static_cast<bool>(quadruped.insert_node(node(
                1U, 0U, kinematic::JointKind::fixed, kinematic::NodeClass::skinned,
                "animal.spine"))),
        "animal spine enters");
    for (std::uint64_t leg = 0U; leg < 4U; ++leg) {
        const std::uint64_t upper = 2U + leg * 2U;
        const std::uint64_t lower = upper + 1U;
        require(static_cast<bool>(quadruped.insert_node(node(
                    upper, 1U, kinematic::JointKind::hinge,
                    kinematic::NodeClass::skinned, "animal.leg.upper"))),
            "animal upper leg enters");
        require(static_cast<bool>(quadruped.insert_node(node(
                    lower, upper, kinematic::JointKind::hinge,
                    kinematic::NodeClass::skinned, "animal.leg.lower"))),
            "animal lower leg enters");
    }
    kinematic::SkinBinding animal_skin;
    animal_skin.mesh_asset = 8U;
    animal_skin.mesh_digest = digest("animal-mesh-v1");
    animal_skin.graph_digest = quadruped.canonical_digest().value();
    for (std::uint32_t vertex = 0U; vertex < 9U; ++vertex) {
        const std::uint64_t lower = 3U + (vertex % 4U) * 2U;
        animal_skin.influences.emplace(
            vertex,
            std::vector<kinematic::JointInfluence>{
                {kinematic::JointId{lower - 1U}, 0.4},
                {kinematic::JointId{lower}, 0.6}});
    }
    kinematic::JointLimitSet animal_limits;
    const auto animal = kinematic::describe_deforming_subgraph(
        8U, quadruped, animal_skin, animal_limits);
    require(static_cast<bool>(animal), "animal lowers to a skeletal asset");
    require(animal.value().joints.size() == 9U,
        "quadruped skeleton carries spine plus four leg chains");

    // Vehicle: rigid chassis with four hinge wheels and slider suspensions.
    kinematic::KinematicGraph vehicle;
    require(static_cast<bool>(vehicle.insert_node(node(
                1U, 0U, kinematic::JointKind::fixed, kinematic::NodeClass::rigid,
                "vehicle.chassis"))),
        "vehicle chassis enters");
    for (std::uint64_t wheel = 0U; wheel < 4U; ++wheel) {
        const std::uint64_t axle = 2U + wheel * 2U;
        require(static_cast<bool>(vehicle.insert_node(node(
                    axle, 1U, kinematic::JointKind::slider,
                    kinematic::NodeClass::rigid, "vehicle.suspension"))),
            "vehicle suspension enters");
        require(static_cast<bool>(vehicle.insert_node(node(
                    axle + 1U, axle, kinematic::JointKind::hinge,
                    kinematic::NodeClass::rigid, "vehicle.wheel"))),
            "vehicle wheel enters");
    }
    kinematic::JointLimitSet vehicle_limits;
    for (std::uint64_t wheel = 0U; wheel < 4U; ++wheel) {
        kinematic::JointLimit travel;
        travel.joint = kinematic::JointId{3U + wheel * 2U};
        travel.angular_enabled = true;
        travel.angular_min = {-6.283185307179586, 0.0, 0.0};
        travel.angular_max = {6.283185307179586, 0.0, 0.0};
        travel.linear_enabled = true;
        travel.linear_min = {0.0, -0.25, 0.0};
        travel.linear_max = {0.0, 0.25, 0.0};
        require(static_cast<bool>(vehicle_limits.insert(travel)),
            "vehicle wheel limit enters");
    }
    require(static_cast<bool>(vehicle_limits.validate_for_graph(vehicle)),
        "vehicle limits resolve against the vehicle graph");
    const auto vehicle_rigid = kinematic::describe_rigid_subgraph(vehicle);
    require(static_cast<bool>(vehicle_rigid),
        "vehicle lowers to a rigid archetype");
    require(vehicle_rigid.value().nodes.size() == 9U,
        "vehicle archetype keeps chassis, suspensions, and wheels");
    require(vehicle_rigid.value().nodes.at(kinematic::JointId{3U}).kind ==
            kinematic::JointKind::hinge,
        "wheel hinge kinds survive lowering");

    // Prop: a single rigid root lowers to a one-node archetype and has no
    // deforming subgraph.
    kinematic::KinematicGraph prop;
    require(static_cast<bool>(prop.insert_node(node(
                1U, 0U, kinematic::JointKind::fixed, kinematic::NodeClass::rigid,
                "prop.crate"))),
        "prop crate enters");
    const auto prop_rigid = kinematic::describe_rigid_subgraph(prop);
    require(static_cast<bool>(prop_rigid), "prop lowers to a rigid archetype");
    require(prop_rigid.value().nodes.size() == 1U,
        "prop archetype is a single node");
    kinematic::SkinBinding prop_skin;
    prop_skin.mesh_asset = 9U;
    prop_skin.mesh_digest = digest("prop-mesh-v1");
    prop_skin.graph_digest = prop.canonical_digest().value();
    require(!kinematic::describe_deforming_subgraph(
                 9U, prop, prop_skin, kinematic::JointLimitSet{}),
        "prop has no deforming subgraph");
}

void reimport_receipt_carries_no_runtime_state() {
    const kinematic::KinematicGraph graph = door_graph();
    const auto lowered = kinematic::describe_rigid_subgraph(graph);
    require(static_cast<bool>(lowered), "door rigid descriptor builds");
    require(static_cast<bool>(
                kinematic::require_no_motion_state(graph.serialize())),
        "graph serialization carries no motion state");
    require(static_cast<bool>(
                kinematic::require_no_motion_state(lowered.value().serialize())),
        "lowering descriptor carries no motion state");
}

} // namespace

int main() {
    valid_graphs_validate_and_round_trip();
    graph_validation_failures_are_reported();
    hostile_graph_text_fails_closed();
    skin_binding_normalization_is_enforced();
    door_fixture_lowers_to_rigid_archetype();
    hinge_pose_preview_is_bounded_and_read_only();
    chain_pose_preview_composes_from_root_and_stays_derived();
    skinned_fixtures_lower_to_skeletal_assets();
    reimport_receipt_carries_no_runtime_state();
    std::cout << "kinematic graph tests passed\n";
    return EXIT_SUCCESS;
}
