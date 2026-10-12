#include <carto/assets/blob_store.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/identity.hpp>
#include <carto/kinematic/limits.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

kinematic::MeshRevisionBinding revision_binding(
    std::uint64_t revision,
    const assets::Sha256Digest& mesh_digest,
    std::uint64_t anchor,
    const core::Transform& rest) {
    kinematic::MeshRevisionBinding result;
    result.mesh_revision = core::Revision{revision};
    result.mesh_digest = mesh_digest;
    result.surface_anchor = anchor;
    result.binding_rest = rest;
    return result;
}

core::Transform rest(double y) {
    core::Transform transform;
    transform.translation = {0.0, y, 0.0};
    return transform;
}

kinematic::JointIdentityRecord identity_record(
    std::uint64_t joint,
    std::string stable_name,
    std::uint64_t parent,
    const core::Transform& authored_rest,
    std::vector<kinematic::MeshRevisionBinding> bindings) {
    kinematic::JointIdentityRecord record;
    record.joint = kinematic::JointId{joint};
    record.stable_name = std::move(stable_name);
    record.parent = parent == 0U
        ? std::optional<kinematic::JointId>{}
        : std::optional<kinematic::JointId>{kinematic::JointId{parent}};
    record.authored_rest = authored_rest;
    record.bindings = std::move(bindings);
    return record;
}

// Correspondence at mesh revision 1: pelvis root, spine, head, and tail.
kinematic::JointCorrespondence correspondence_v1() {
    kinematic::JointCorrespondence correspondence;
    correspondence.source_namespace = "condition.motel8";
    correspondence.mesh_asset = 7U;
    correspondence.mesh_revision = core::Revision{1U};
    correspondence.mesh_digest = digest("being-mesh-r1");
    const assets::Sha256Digest mesh = correspondence.mesh_digest;

    require(static_cast<bool>(correspondence.insert(identity_record(
                1U, "being.pelvis", 0U, rest(0.0),
                {revision_binding(1U, mesh, 100U, rest(0.0))}))),
        "pelvis identity enters");
    require(static_cast<bool>(correspondence.insert(identity_record(
                2U, "being.spine", 1U, rest(0.5),
                {revision_binding(1U, mesh, 110U, rest(0.5))}))),
        "spine identity enters");
    require(static_cast<bool>(correspondence.insert(identity_record(
                3U, "being.head", 2U, rest(1.5),
                {revision_binding(1U, mesh, 120U, rest(1.5))}))),
        "head identity enters");
    require(static_cast<bool>(correspondence.insert(identity_record(
                5U, "being.tail", 1U, rest(0.8),
                {revision_binding(1U, mesh, 130U, rest(0.8))}))),
        "tail identity enters");
    return correspondence;
}

// Authored graph at mesh revision 2: head rest moved (topology edit changed
// the neck), tail removed with the topology, antenna added.
kinematic::KinematicGraph graph_v2() {
    kinematic::KinematicGraph graph;
    kinematic::KinematicNode pelvis;
    pelvis.id = kinematic::JointId{1U};
    pelvis.kind = kinematic::JointKind::fixed;
    pelvis.node_class = kinematic::NodeClass::skinned;
    pelvis.semantic_type = "being.pelvis";
    pelvis.rest = rest(0.0);

    kinematic::KinematicNode spine;
    spine.id = kinematic::JointId{2U};
    spine.parent = kinematic::JointId{1U};
    spine.kind = kinematic::JointKind::ball;
    spine.node_class = kinematic::NodeClass::skinned;
    spine.semantic_type = "being.spine";
    spine.rest = rest(0.5);

    kinematic::KinematicNode head;
    head.id = kinematic::JointId{3U};
    head.parent = kinematic::JointId{2U};
    head.kind = kinematic::JointKind::ball;
    head.node_class = kinematic::NodeClass::skinned;
    head.semantic_type = "being.head";
    head.rest = rest(1.7);

    kinematic::KinematicNode antenna;
    antenna.id = kinematic::JointId{4U};
    antenna.parent = kinematic::JointId{1U};
    antenna.kind = kinematic::JointKind::hinge;
    antenna.node_class = kinematic::NodeClass::rigid;
    antenna.semantic_type = "being.antenna";
    antenna.rest = rest(0.9);

    require(static_cast<bool>(graph.insert_node(pelvis)), "pelvis node enters");
    require(static_cast<bool>(graph.insert_node(spine)), "spine node enters");
    require(static_cast<bool>(graph.insert_node(head)), "head node enters");
    require(static_cast<bool>(graph.insert_node(antenna)), "antenna node enters");
    return graph;
}

void correspondence_round_trips_and_resolves() {
    const kinematic::JointCorrespondence correspondence = correspondence_v1();
    require(static_cast<bool>(correspondence.validate()),
        "correspondence validates");
    const auto first = correspondence.canonical_digest();
    require(static_cast<bool>(first), "correspondence digest computes");

    const auto decoded =
        kinematic::JointCorrespondence::deserialize(correspondence.serialize());
    require(static_cast<bool>(decoded), "correspondence deserializes");
    require(decoded.value().canonical_digest().value() == first.value(),
        "correspondence round trip preserves the digest");

    const auto resolved = correspondence.resolve(
        kinematic::JointId{2U}, core::Revision{1U});
    require(static_cast<bool>(resolved), "spine resolves at revision 1");
    require(resolved.value()->surface_anchor == 110U,
        "resolved binding carries the per-revision anchor");

    const auto unknown = correspondence.resolve(
        kinematic::JointId{99U}, core::Revision{1U});
    require(!unknown, "unknown joint refuses");
    require(unknown.error().code == core::ErrorCode::not_found,
        "unknown joint reports not_found");

    const auto stale = correspondence.resolve(
        kinematic::JointId{2U}, core::Revision{9U});
    require(!stale, "unrecorded revision refuses");
    require(stale.error().code == core::ErrorCode::stale_data,
        "unrecorded revision reports stale_data");

    const kinematic::JointCorrespondence reordered = [&] {
        kinematic::JointCorrespondence value;
        value.source_namespace = "condition.motel8";
        value.mesh_asset = 7U;
        value.mesh_revision = core::Revision{1U};
        value.mesh_digest = digest("being-mesh-r1");
        const assets::Sha256Digest mesh = value.mesh_digest;
        require(static_cast<bool>(value.insert(identity_record(
                    5U, "being.tail", 1U, rest(0.8),
                    {revision_binding(1U, mesh, 130U, rest(0.8))}))),
            "tail identity re-enters");
        require(static_cast<bool>(value.insert(identity_record(
                    1U, "being.pelvis", 0U, rest(0.0),
                    {revision_binding(1U, mesh, 100U, rest(0.0))}))),
            "pelvis identity re-enters");
        require(static_cast<bool>(value.insert(identity_record(
                    3U, "being.head", 2U, rest(1.5),
                    {revision_binding(1U, mesh, 120U, rest(1.5))}))),
            "head identity re-enters");
        require(static_cast<bool>(value.insert(identity_record(
                    2U, "being.spine", 1U, rest(0.5),
                    {revision_binding(1U, mesh, 110U, rest(0.5))}))),
            "spine identity re-enters");
        return value;
    }();
    require(reordered.canonical_digest().value() == first.value(),
        "correspondence digest is independent of insertion order");
}

void reimport_preserves_stable_joint_identity() {
    const kinematic::JointCorrespondence before = correspondence_v1();
    const kinematic::KinematicGraph current = graph_v2();
    const assets::Sha256Digest digest_v2 = digest("being-mesh-r2");

    const auto reconciliation = kinematic::reconcile_reimport(
        before, current, core::Revision{2U}, digest_v2);
    require(static_cast<bool>(reconciliation),
        "reimport reconciles across the mesh revision");

    const kinematic::KinematicReimportReceipt& receipt =
        reconciliation.value().receipt;
    require(static_cast<bool>(receipt.validate()),
        "reimport receipt validates with its refreshed digest");
    require(receipt.revision_before.value() == 1U &&
            receipt.revision_after.value() == 2U,
        "receipt records the revision step");
    require(receipt.preserved_joints.size() == 2U &&
            receipt.preserved_joints[0U] == kinematic::JointId{1U} &&
            receipt.preserved_joints[1U] == kinematic::JointId{2U},
        "pelvis and spine ids are preserved");
    require(receipt.rebound_joints.size() == 1U &&
            receipt.rebound_joints[0U] == kinematic::JointId{3U},
        "head id survives with rebound evidence");
    require(receipt.removed_joints.size() == 1U &&
            receipt.removed_joints[0U] == kinematic::JointId{5U},
        "tail id is removed, not resurrected");
    require(receipt.added_joints.size() == 1U &&
            receipt.added_joints[0U] == kinematic::JointId{4U},
        "antenna id is added");

    const kinematic::JointCorrespondence& after =
        reconciliation.value().correspondence;
    require(static_cast<bool>(after.validate()),
        "advanced correspondence validates");
    require(after.mesh_revision.value() == 2U,
        "advanced correspondence reflects the new revision");

    const kinematic::JointIdentityRecord* pelvis = after.find(kinematic::JointId{1U});
    require(pelvis != nullptr && pelvis->bindings.size() == 2U &&
            pelvis->bindings[0U].mesh_revision.value() == 1U &&
            pelvis->bindings[1U].mesh_revision.value() == 2U &&
            pelvis->bindings[1U].mesh_digest == digest_v2 &&
            pelvis->bindings[1U].surface_anchor == 100U,
        "pelvis keeps its stable id and gains a revision-2 binding");

    const kinematic::JointIdentityRecord* head = after.find(kinematic::JointId{3U});
    require(head != nullptr && head->bindings.size() == 2U &&
            head->authored_rest.translation.y == 1.7,
        "head keeps its stable id with the rebound rest transform");
    require(after.find(kinematic::JointId{5U}) == nullptr,
        "removed tail identity is not carried forward");
    const kinematic::JointIdentityRecord* antenna =
        after.find(kinematic::JointId{4U});
    require(antenna != nullptr && antenna->stable_name == "being.antenna" &&
            antenna->bindings.size() == 1U,
        "added antenna identity is recorded at the new revision");

    const auto resolved = after.resolve(kinematic::JointId{1U}, core::Revision{2U});
    require(static_cast<bool>(resolved) &&
            resolved.value()->mesh_digest == digest_v2,
        "stable joint id resolves at the new revision");

    // The receipt is the durable evidence that reimport never touched runtime
    // pose state: it carries identity classifications only.
    require(static_cast<bool>(
                kinematic::require_no_motion_state(receipt.serialize())),
        "reimport receipt carries no motion state");
    require(static_cast<bool>(
                kinematic::require_no_motion_state(after.serialize())),
        "advanced correspondence carries no motion state");

    const auto receipt_round_trip =
        kinematic::KinematicReimportReceipt::deserialize(receipt.serialize());
    require(static_cast<bool>(receipt_round_trip),
        "reimport receipt deserializes");
    require(static_cast<bool>(receipt_round_trip.value().validate()),
        "reimport receipt round trip validates");
    require(receipt_round_trip.value().receipt_digest == receipt.receipt_digest,
        "reimport receipt digest survives the round trip");

    // A no-op reimport preserves every identity and appends nothing.
    const auto noop = kinematic::reconcile_reimport(
        after, current, core::Revision{2U}, digest_v2);
    require(static_cast<bool>(noop), "no-op reimport reconciles");
    require(noop.value().receipt.revision_before.value() ==
                noop.value().receipt.revision_after.value(),
        "no-op reimport records an unchanged revision");
    require(noop.value().receipt.preserved_joints.size() == 4U &&
            noop.value().receipt.rebound_joints.empty() &&
            noop.value().receipt.removed_joints.empty() &&
            noop.value().receipt.added_joints.empty(),
        "no-op reimport preserves all stable joint ids");
    require(noop.value().correspondence.find(kinematic::JointId{1U})
                ->bindings.size() == 2U,
        "no-op reimport appends no duplicate bindings");
}

void reimport_rejects_regession_and_in_place_changes() {
    const kinematic::JointCorrespondence before = correspondence_v1();
    const kinematic::KinematicGraph current = graph_v2();

    const auto regression = kinematic::reconcile_reimport(
        before, current, core::Revision{0U}, digest("being-mesh-r2"));
    require(!regression, "revision regression refuses");
    require(regression.error().code == core::ErrorCode::invalid_argument,
        "revision regression reports invalid_argument");

    const auto in_place = kinematic::reconcile_reimport(
        before, current, core::Revision{1U}, digest("being-mesh-r1b"));
    require(!in_place, "in-place content change at a pinned revision refuses");
    require(in_place.error().code == core::ErrorCode::stale_data,
        "in-place content change reports stale_data");
}

void correspondence_rejects_duplicates_and_cycles() {
    kinematic::JointCorrespondence correspondence = correspondence_v1();
    require(!correspondence.insert(identity_record(
                1U, "being.pelvis", 0U, rest(0.0), {})),
        "duplicate joint identity is rejected");

    {
        kinematic::JointCorrespondence cyclic;
        cyclic.source_namespace = "test";
        cyclic.mesh_asset = 1U;
        cyclic.mesh_revision = core::Revision{1U};
        cyclic.mesh_digest = digest("cycle-mesh");
        const assets::Sha256Digest mesh = cyclic.mesh_digest;
        require(static_cast<bool>(cyclic.insert(identity_record(
                    1U, "a", 2U, rest(0.0),
                    {revision_binding(1U, mesh, 1U, rest(0.0))}))),
            "cycle first half enters");
        require(static_cast<bool>(cyclic.insert(identity_record(
                    2U, "b", 1U, rest(0.0),
                    {revision_binding(1U, mesh, 2U, rest(0.0))}))),
            "cycle second half enters");
        const auto result = cyclic.validate();
        require(!result, "identity cycle is rejected");
        require(result.error().code == core::ErrorCode::cycle_detected,
            "identity cycle reports cycle_detected");
    }
    {
        kinematic::JointCorrespondence dangling;
        dangling.source_namespace = "test";
        dangling.mesh_asset = 1U;
        dangling.mesh_revision = core::Revision{1U};
        dangling.mesh_digest = digest("dangling-mesh");
        require(static_cast<bool>(dangling.insert(identity_record(
                    1U, "a", 9U, rest(0.0),
                    {revision_binding(
                        1U, dangling.mesh_digest, 1U, rest(0.0))}))),
            "dangling parent record enters");
        const auto result = dangling.validate();
        require(!result, "dangling identity parent is rejected");
        require(result.error().code == core::ErrorCode::validation_failed,
            "dangling identity parent reports validation_failed");
    }
}

void hostile_identity_text_fails_closed() {
    const kinematic::JointCorrespondence correspondence = correspondence_v1();
    const std::string encoded = correspondence.serialize();

    const auto future = kinematic::JointCorrespondence::deserialize(
        "CARTOGRAPHER_KINEMATIC_CORRESPONDENCE 99\n");
    require(!future, "future correspondence version is rejected");
    require(future.error().code == core::ErrorCode::version_mismatch,
        "future correspondence version reports version_mismatch");
    require(!kinematic::JointCorrespondence::deserialize(encoded + "trailing"),
        "correspondence parser rejects trailing unbound data");
    require(!kinematic::JointCorrespondence::deserialize(
                "CARTOGRAPHER_KINEMATIC_CORRESPONDENCE 1\n"
                "SOURCE \"x\" 0 0 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "JOINTS 65537\n"),
        "hostile joint count is rejected before allocation");
    require(!kinematic::JointCorrespondence::deserialize(
                "CARTOGRAPHER_KINEMATIC_CORRESPONDENCE 1\n"
                "SOURCE \"x\" 0 0 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "JOINTS 1\n"
                "JOINT 0 \"x\" 0 0 0 0 0 0 0 0 1 1 1 1 0\n"),
        "zero joint identity in text is rejected");

    const kinematic::KinematicReimportReceipt& receipt =
        kinematic::reconcile_reimport(
            correspondence, graph_v2(), core::Revision{2U},
            digest("being-mesh-r2"))
            .value()
            .receipt;
    std::string tampered = receipt.serialize();
    const std::string original_hex = receipt.receipt_digest.hex();
    const std::size_t position = tampered.find(original_hex);
    require(position != std::string::npos, "receipt digest is present for tampering");
    tampered.replace(
        position, original_hex.size(),
        "0000000000000000000000000000000000000000000000000000000000000000");
    require(!kinematic::KinematicReimportReceipt::deserialize(tampered),
        "tampered receipt digest fails closed");
    require(!kinematic::KinematicReimportReceipt::deserialize(
                "CARTOGRAPHER_KINEMATIC_REIMPORT_RECEIPT 1\nREVISION 9 0\n"),
        "regressed receipt revisions fail closed");
}

} // namespace

int main() {
    correspondence_round_trips_and_resolves();
    reimport_preserves_stable_joint_identity();
    reimport_rejects_regession_and_in_place_changes();
    correspondence_rejects_duplicates_and_cycles();
    hostile_identity_text_fails_closed();
    std::cout << "kinematic identity tests passed\n";
    return EXIT_SUCCESS;
}
