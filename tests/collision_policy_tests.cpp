#include <carto/assets/blob_store.hpp>
#include <carto/collision/benchmark.hpp>
#include <carto/collision/family.hpp>
#include <carto/collision/metrics.hpp>
#include <carto/collision/policy.hpp>
#include <carto/collision/primitive.hpp>
#include <carto/collision/profile.hpp>
#include <carto/core/math.hpp>
#include <carto/core/revision.hpp>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

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

carto::collision::CollisionMotionBinding binding(
    std::string identity,
    carto::collision::CollisionMotionKind motion) {
    carto::collision::CollisionMotionBinding value;
    value.node_identity = std::move(identity);
    value.motion = motion;
    return value;
}

carto::collision::CollisionMotionPolicy motion_policy_fixture() {
    using namespace carto;
    collision::CollisionMotionPolicy policy;
    require(static_cast<bool>(policy.insert(binding("crane.base",
                collision::CollisionMotionKind::stationary))),
        "stationary node enters the motion policy");
    collision::CollisionMotionBinding arm =
        binding("crane.arm", collision::CollisionMotionKind::kinematic);
    arm.parent_identity = "crane.base";
    arm.rest_transform = core::Transform{{0.0, 4.0, 0.0}};
    arm.runtime_mapping = "vanta.joint.crane_arm";
    require(static_cast<bool>(policy.insert(std::move(arm))),
        "kinematic node with parent, rest transform, and mapping enters");
    require(static_cast<bool>(policy.validate()),
        "motion policy fixture validates");
    return policy;
}

void motion_policy_enforces_kinematic_contract() {
    using namespace carto;
    collision::CollisionMotionPolicy policy;

    collision::CollisionMotionBinding orphan =
        binding("orphan", collision::CollisionMotionKind::kinematic);
    orphan.rest_transform = core::Transform{{1.0, 0.0, 0.0}};
    orphan.runtime_mapping = "vanta.joint.orphan";
    const auto refused_orphan = policy.insert(orphan);
    require(!refused_orphan &&
            refused_orphan.error().code == core::ErrorCode::validation_failed,
        "kinematic node without a parent identity is refused");

    collision::CollisionMotionBinding dynamic_with_parent =
        binding("prop.crate", collision::CollisionMotionKind::dynamic);
    dynamic_with_parent.parent_identity = "crane.base";
    require(!policy.insert(dynamic_with_parent),
        "dynamic node must not declare a kinematic parent");

    collision::CollisionMotionBinding stationary_with_mapping =
        binding("prop.tarp", collision::CollisionMotionKind::stationary);
    stationary_with_mapping.runtime_mapping = "vanta.joint.tarp";
    require(!policy.insert(stationary_with_mapping),
        "stationary node must not declare a runtime mapping");

    collision::CollisionMotionPolicy unresolved;
    collision::CollisionMotionBinding ghost =
        binding("ghost.child", collision::CollisionMotionKind::kinematic);
    ghost.parent_identity = "missing.parent";
    ghost.rest_transform = core::Transform{{0.0, 1.0, 0.0}};
    ghost.runtime_mapping = "vanta.joint.ghost";
    require(static_cast<bool>(unresolved.insert(std::move(ghost))),
        "kinematic node enters even before its parent is present");
    require(!unresolved.validate(),
        "policy validation refuses an unresolved kinematic parent");

    collision::CollisionMotionPolicy duplicates;
    require(static_cast<bool>(duplicates.insert(
                binding("shared", collision::CollisionMotionKind::stationary))),
        "first node enters the duplicate fixture");
    const auto duplicate = duplicates.insert(
        binding("shared", collision::CollisionMotionKind::dynamic));
    require(!duplicate && duplicate.error().code == core::ErrorCode::invalid_argument,
        "duplicate node identity refuses with invalid_argument");
}

void motion_policy_detects_cycles() {
    using namespace carto;
    collision::CollisionMotionPolicy policy;
    collision::CollisionMotionBinding first =
        binding("link.a", collision::CollisionMotionKind::kinematic);
    first.parent_identity = "link.b";
    first.rest_transform = core::Transform{{1.0, 0.0, 0.0}};
    first.runtime_mapping = "vanta.joint.a";
    collision::CollisionMotionBinding second =
        binding("link.b", collision::CollisionMotionKind::kinematic);
    second.parent_identity = "link.a";
    second.rest_transform = core::Transform{{0.0, 1.0, 0.0}};
    second.runtime_mapping = "vanta.joint.b";
    require(static_cast<bool>(policy.insert(std::move(first))) &&
            static_cast<bool>(policy.insert(std::move(second))),
        "cyclic kinematic pair enters");
    const auto validated = policy.validate();
    require(!validated && validated.error().code == core::ErrorCode::cycle_detected,
        "kinematic parent cycles refuse with cycle_detected");
}

void motion_policy_round_trips_and_rejects_hostile_input() {
    using namespace carto;
    const collision::CollisionMotionPolicy policy = motion_policy_fixture();
    const std::string encoded = policy.serialize();
    const auto reopened = collision::CollisionMotionPolicy::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "motion policy survives a deterministic round trip");
    require(reopened.value().nodes().size() == 2U,
        "round-tripped motion policy keeps every node");
    const collision::CollisionMotionBinding* arm =
        reopened.value().find("crane.arm");
    require(arm != nullptr && arm->motion == collision::CollisionMotionKind::kinematic &&
            arm->parent_identity.has_value() &&
            arm->parent_identity.value() == "crane.base" &&
            arm->runtime_mapping.has_value() &&
            arm->rest_transform.translation.y == 4.0,
        "kinematic binding survives the round trip intact");

    require(!collision::CollisionMotionPolicy::deserialize(encoded + "trailing"),
        "motion policy parser rejects trailing data");
    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_COLLISION_MOTION_POLICY 1"}.size(),
        "CARTOGRAPHER_COLLISION_MOTION_POLICY 99");
    require(!collision::CollisionMotionPolicy::deserialize(future),
        "future motion policy versions fail precisely");
    require(!collision::CollisionMotionPolicy::deserialize(
                "CARTOGRAPHER_COLLISION_MOTION_POLICY 1\nNODES 65537\n"),
        "oversized motion policy counts are rejected before allocation");
    require(!collision::CollisionMotionPolicy::deserialize(
                "CARTOGRAPHER_COLLISION_MOTION_POLICY 1\n"
                "NODES 2\n"
                "NODE \"dup\" stationary PARENT - REST 0 0 0 0 0 0 1 1 1 1 MAPPING -\n"
                "NODE \"dup\" dynamic PARENT - REST 0 0 0 0 0 0 1 1 1 1 MAPPING -\n"
                "END\n"),
        "duplicate node identities in hostile text refuse");
}

carto::collision::CollisionProfile profile_fixture() {
    using namespace carto;
    collision::CollisionProfile profile;
    profile.game_identity = "condition";
    profile.permitted_primitive_kinds = {
        collision::PrimitiveCollisionKind::sphere,
        collision::PrimitiveCollisionKind::box,
    };
    profile.family_preference = {
        collision::CollisionRepresentationFamily::primitive,
        collision::CollisionRepresentationFamily::triangle_mesh,
    };
    profile.error_budget.max_one_sided_hausdorff = 0.5;
    profile.default_motion = collision::CollisionMotionKind::stationary;
    profile.convex_proxy_capability = false;
    profile.triangle_mesh_capability = true;
    return profile;
}

void profile_constrains_generation_and_admission() {
    using namespace carto;
    const collision::CollisionProfile profile = profile_fixture();
    require(static_cast<bool>(profile.validate()), "profile fixture validates");
    require(profile.permits(collision::PrimitiveCollisionKind::sphere) &&
            profile.permits(collision::PrimitiveCollisionKind::box) &&
            !profile.permits(collision::PrimitiveCollisionKind::cylinder),
        "profile permits exactly its primitive set");
    require(profile.prefers(
                collision::CollisionRepresentationFamily::primitive,
                collision::CollisionRepresentationFamily::triangle_mesh) &&
            !profile.prefers(
                collision::CollisionRepresentationFamily::triangle_mesh,
                collision::CollisionRepresentationFamily::primitive),
        "family preference order is honored");

    const auto first_digest = profile.canonical_digest();
    const auto second_digest = profile.canonical_digest();
    require(first_digest && second_digest &&
            first_digest.value() == second_digest.value(),
        "profile digest is deterministic");

    collision::CollisionProfile unsorted = profile;
    unsorted.permitted_primitive_kinds = {
        collision::PrimitiveCollisionKind::box,
        collision::PrimitiveCollisionKind::sphere,
    };
    require(!unsorted.validate(),
        "unsorted permitted kinds refuse validation");

    collision::CollisionProfile repeated_kind = profile;
    repeated_kind.permitted_primitive_kinds = {
        collision::PrimitiveCollisionKind::sphere,
        collision::PrimitiveCollisionKind::sphere,
    };
    require(!repeated_kind.validate(),
        "duplicate permitted kinds refuse validation");

    collision::CollisionProfile repeated_family = profile;
    repeated_family.family_preference = {
        collision::CollisionRepresentationFamily::primitive,
        collision::CollisionRepresentationFamily::primitive,
    };
    require(!repeated_family.validate(),
        "repeated family preference refuses validation");

    collision::CollisionProfile kinematic_default = profile;
    kinematic_default.default_motion = collision::CollisionMotionKind::kinematic;
    require(!kinematic_default.validate(),
        "kinematic motion defaults refuse validation");
}

void profile_round_trips_and_rejects_hostile_input() {
    using namespace carto;
    const collision::CollisionProfile profile = profile_fixture();
    const std::string encoded = profile.serialize();
    const auto reopened = collision::CollisionProfile::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "profile survives a deterministic round trip");
    require(reopened.value().game_identity == "condition" &&
            !reopened.value().convex_proxy_capability &&
            reopened.value().triangle_mesh_capability,
        "profile capabilities survive the round trip");

    require(!collision::CollisionProfile::deserialize(encoded + "trailing"),
        "profile parser rejects trailing data");
    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_COLLISION_PROFILE 1"}.size(),
        "CARTOGRAPHER_COLLISION_PROFILE 99");
    require(!collision::CollisionProfile::deserialize(future),
        "future profile versions fail precisely");
    require(!collision::CollisionProfile::deserialize(
                "CARTOGRAPHER_COLLISION_PROFILE 1\n"
                "GAME \"g\"\n"
                "DEFAULT_MOTION stationary\n"
                "CAPABILITIES CONVEX 0 TRIANGLE 1\n"
                "PRIMITIVES 17 sphere\n"
                "FAMILIES 1 primitive\n"
                "THRESHOLDS VOLUME_OVERLAP - PENETRATION - HAUSDORFF - CLEARANCE -\n"
                "END\n"),
        "oversized primitive counts are rejected before allocation");
}

carto::collision::CollisionVariantSelection selection_fixture() {
    using namespace carto;
    collision::CollisionVariantSelection selection;
    selection.basis.source_identity = "public.crate";
    selection.basis.source_revision = core::Revision{2U};
    selection.basis.source_digest = digest("crate-source");
    selection.basis.algorithm = "primitive-fit/v1";
    selection.basis.settings_digest = digest("settings");
    selection.basis.target_profile_identity = "condition/collision";
    selection.basis.target_profile_digest = digest("profile");
    selection.basis.admission_policy_identity = "condition/collision-admission";
    selection.basis.admission_policy_digest = digest("admission");
    selection.basis.hardware_profile = "cpu-reference";

    collision::CollisionVariantCandidate selected;
    selected.family = collision::CollisionRepresentationFamily::primitive;
    selected.purpose = collision::CollisionPurpose::gameplay_query;
    selected.candidate_digest = digest("crate-primitive");
    selected.resident_cost_bytes = 128U;
    selected.disposition = collision::CollisionCandidateDisposition::selected;
    require(static_cast<bool>(selection.insert_candidate(std::move(selected))),
        "selected variant enters the selection");

    collision::CollisionVariantCandidate rejected;
    rejected.family = collision::CollisionRepresentationFamily::compound;
    rejected.purpose = collision::CollisionPurpose::vehicle;
    rejected.candidate_digest = digest("crate-compound");
    rejected.resident_cost_bytes = 512U;
    rejected.disposition = collision::CollisionCandidateDisposition::rejected_policy;
    rejected.reason_code = "error_budget";
    require(static_cast<bool>(selection.insert_candidate(std::move(rejected))),
        "policy-rejected variant stays inspectable in the selection");

    selection.status = collision::CollisionReceiptStatus::admitted;
    selection.selected_index = 0U;
    require(static_cast<bool>(selection.refresh_digest()),
        "selection refreshes its decision digest");
    require(static_cast<bool>(selection.validate()),
        "selection fixture validates");
    return selection;
}

void variant_selection_certifies_one_variant() {
    using namespace carto;
    const collision::CollisionVariantSelection selection = selection_fixture();
    require(selection.selected_candidate() != nullptr &&
            selection.selected_candidate()->purpose ==
                collision::CollisionPurpose::gameplay_query,
        "selected candidate resolves through the selected index");
    require(!selection.decision_digest.is_zero(),
        "selection is digest-bound");

    const std::string encoded = selection.serialize();
    const auto reopened = collision::CollisionVariantSelection::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "selection survives a deterministic round trip");
    require(reopened.value().decision_digest == selection.decision_digest,
        "selection identity survives reopen");

    collision::CollisionVariantSelection tampered = selection;
    tampered.candidates[1].resident_cost_bytes = 999U;
    require(static_cast<bool>(tampered.validate()),
        "tampered selection stays structurally valid");
    require(static_cast<bool>(tampered.refresh_digest()),
        "tampered selection refreshes cleanly");
    require(tampered.decision_digest != selection.decision_digest,
        "mutated content changes the decision digest");

    collision::CollisionVariantSelection conflict = selection;
    collision::CollisionVariantCandidate extra_selected;
    extra_selected.family = collision::CollisionRepresentationFamily::triangle_mesh;
    extra_selected.purpose = collision::CollisionPurpose::projectile;
    extra_selected.candidate_digest = digest("crate-triangles");
    extra_selected.disposition = collision::CollisionCandidateDisposition::selected;
    require(!conflict.insert_candidate(std::move(extra_selected)),
        "a selection admits at most one selected candidate");

    collision::CollisionVariantSelection unselected = selection;
    unselected.status = collision::CollisionReceiptStatus::failed;
    require(!unselected.validate(),
        "failed selection cannot keep a selected index");

    collision::CollisionVariantCandidate reasoned_selected;
    reasoned_selected.family = collision::CollisionRepresentationFamily::primitive;
    reasoned_selected.purpose = collision::CollisionPurpose::debris;
    reasoned_selected.candidate_digest = digest("other");
    reasoned_selected.disposition = collision::CollisionCandidateDisposition::selected;
    reasoned_selected.reason_code = "not-allowed";
    collision::CollisionVariantSelection fresh;
    fresh.basis = selection.basis;
    require(!fresh.insert_candidate(std::move(reasoned_selected)),
        "selected candidate must not carry a reason code");
}

void variant_selection_rejects_hostile_input() {
    using namespace carto;
    const collision::CollisionVariantSelection selection = selection_fixture();
    const std::string encoded = selection.serialize();

    require(!collision::CollisionVariantSelection::deserialize(encoded + "trailing"),
        "selection parser rejects trailing data");
    std::string future = encoded;
    future.replace(
        0U, std::string{"CARTOGRAPHER_COLLISION_VARIANT_SELECTION 1"}.size(),
        "CARTOGRAPHER_COLLISION_VARIANT_SELECTION 99");
    require(!collision::CollisionVariantSelection::deserialize(future),
        "future selection versions fail precisely");

    std::string oversized = encoded;
    const std::string original = "CANDIDATES 2";
    require(oversized.find(original) != std::string::npos,
        "fixture carries the candidate count");
    oversized.replace(oversized.find(original), original.size(), "CANDIDATES 65");
    require(!collision::CollisionVariantSelection::deserialize(oversized),
        "oversized candidate counts are rejected before allocation");

    std::string out_of_range = encoded;
    const std::string selected_token = "SELECTED 0";
    require(out_of_range.find(selected_token) != std::string::npos,
        "fixture carries the selected index");
    out_of_range.replace(
        out_of_range.find(selected_token), selected_token.size(), "SELECTED 9");
    require(!collision::CollisionVariantSelection::deserialize(out_of_range),
        "out-of-range selected indices fail closed");

    std::string corrupt_digest = encoded;
    const std::string hex = digest("crate-compound").hex();
    const std::size_t digest_position = corrupt_digest.find(hex);
    require(digest_position != std::string::npos,
        "fixture carries the candidate digest");
    const char original_char = corrupt_digest[digest_position];
    corrupt_digest[digest_position] = original_char == '0' ? '1' : '0';
    const auto corrupt = collision::CollisionVariantSelection::deserialize(corrupt_digest);
    require(!corrupt && corrupt.error().code == core::ErrorCode::stale_data,
        "digest-mismatched selections fail with stale_data");
}

carto::collision::CollisionBenchmarkReceipt benchmark_fixture(
    carto::collision::CollisionReceiptStatus status) {
    using namespace carto;
    collision::CollisionBenchmarkReceipt receipt;
    receipt.basis.source_identity = "public.crate";
    receipt.basis.source_revision = core::Revision{2U};
    receipt.basis.source_digest = digest("crate-source");
    receipt.basis.algorithm = "primitive-fit/v1";
    receipt.basis.settings_digest = digest("settings");
    receipt.basis.target_profile_identity = "condition/collision";
    receipt.basis.target_profile_digest = digest("profile");
    receipt.basis.hardware_profile = "cpu-reference";
    receipt.candidate_digest = digest("crate-primitive");
    receipt.cpu_reference_milliseconds = 0.5;
    receipt.evidence = collision::CollisionBenchmarkEvidence::cpu_reference;
    receipt.status = status;
    require(static_cast<bool>(receipt.refresh_digest()),
        "benchmark receipt refreshes its digest");
    return receipt;
}

void benchmark_promotion_never_replaces_last_good_with_failure() {
    using namespace carto;
    const collision::CollisionBenchmarkReceipt admitted =
        benchmark_fixture(collision::CollisionReceiptStatus::admitted);
    require(static_cast<bool>(admitted.validate()),
        "admitted CPU reference receipt validates");

    collision::CollisionPromotionGate gate;
    gate.max_cpu_reference_milliseconds = 1.0;
    const auto promoted = collision::promote_collision_candidate(
        admitted, std::nullopt, gate);
    require(promoted && promoted.value().promoted_new_candidate &&
            promoted.value().selected_candidate_digest == admitted.candidate_digest,
        "in-budget admitted candidate promotes");

    const collision::CollisionBenchmarkReceipt failed =
        benchmark_fixture(collision::CollisionReceiptStatus::failed);
    const auto retained = collision::promote_collision_candidate(
        failed, admitted.candidate_digest, gate);
    require(retained && retained.value().retained_last_good &&
            !retained.value().promoted_new_candidate &&
            retained.value().selected_candidate_digest == admitted.candidate_digest,
        "failed candidate retains the last-good selection untouched");

    const auto no_fallback = collision::promote_collision_candidate(
        failed, std::nullopt, gate);
    require(!no_fallback,
        "failed candidate without a last-good selection fails closed");

    collision::CollisionBenchmarkReceipt slow = admitted;
    slow.cpu_reference_milliseconds = 2.0;
    require(static_cast<bool>(slow.refresh_digest()),
        "slow receipt stays internally consistent");
    const auto retained_slow = collision::promote_collision_candidate(
        slow, admitted.candidate_digest, gate);
    require(retained_slow && retained_slow.value().retained_last_good,
        "over-budget timing retains the last-good selection");

    collision::CollisionBenchmarkReceipt inaccurate = admitted;
    inaccurate.measured_error.one_sided_hausdorff = 5.0;
    require(static_cast<bool>(inaccurate.refresh_digest()),
        "inaccurate receipt stays internally consistent");
    collision::CollisionPromotionGate strict_gate;
    strict_gate.error_thresholds.max_one_sided_hausdorff = 1.0;
    const auto retained_inaccurate = collision::promote_collision_candidate(
        inaccurate, admitted.candidate_digest, strict_gate);
    require(retained_inaccurate && retained_inaccurate.value().retained_last_good,
        "over-budget error retains the last-good selection");
}

void benchmark_evidence_tiers_never_promote_silently() {
    using namespace carto;
    collision::CollisionBenchmarkReceipt native = benchmark_fixture(
        collision::CollisionReceiptStatus::admitted);
    native.evidence = collision::CollisionBenchmarkEvidence::native_backend_evidence;
    require(static_cast<bool>(native.refresh_digest()),
        "native-tier receipt stays internally consistent");
    const auto native_validated = native.validate();
    require(!native_validated &&
            native_validated.error().code == core::ErrorCode::validation_failed,
        "neutral preparation cannot claim native backend evidence");

    collision::CollisionBenchmarkReceipt full_game = benchmark_fixture(
        collision::CollisionReceiptStatus::admitted);
    full_game.evidence =
        collision::CollisionBenchmarkEvidence::full_game_acceptance;
    require(static_cast<bool>(full_game.refresh_digest()),
        "full-game-tier receipt stays internally consistent");
    require(!full_game.validate(),
        "neutral preparation cannot claim full-game acceptance evidence");

    collision::CollisionBenchmarkReceipt structural_only = benchmark_fixture(
        collision::CollisionReceiptStatus::admitted);
    structural_only.evidence =
        collision::CollisionBenchmarkEvidence::structural_validation;
    require(static_cast<bool>(structural_only.refresh_digest()),
        "structural-tier receipt stays internally consistent");
    require(!structural_only.validate(),
        "structural validation alone never admits a candidate");
    structural_only.status = collision::CollisionReceiptStatus::failed;
    require(static_cast<bool>(structural_only.refresh_digest()) &&
            static_cast<bool>(structural_only.validate()),
        "structural validation records failure without overclaiming");
}

void benchmark_receipt_round_trips_and_rejects_hostile_input() {
    using namespace carto;
    const collision::CollisionBenchmarkReceipt receipt =
        benchmark_fixture(collision::CollisionReceiptStatus::admitted);
    const std::string encoded = receipt.serialize();
    const auto reopened = collision::CollisionBenchmarkReceipt::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "benchmark receipt survives a deterministic round trip");
    require(reopened.value().receipt_digest == receipt.receipt_digest,
        "receipt identity survives reopen");

    require(!collision::CollisionBenchmarkReceipt::deserialize(encoded + "trailing"),
        "benchmark parser rejects trailing data");
    std::string future = encoded;
    future.replace(
        0U, std::string{"CARTOGRAPHER_COLLISION_BENCHMARK_RECEIPT 1"}.size(),
        "CARTOGRAPHER_COLLISION_BENCHMARK_RECEIPT 99");
    require(!collision::CollisionBenchmarkReceipt::deserialize(future),
        "future receipt versions fail precisely");

    std::string corrupt = encoded;
    const std::string token = "CPU_MS 0.5";
    require(corrupt.find(token) != std::string::npos,
        "fixture carries the CPU timing");
    corrupt.replace(corrupt.find(token), token.size(), "CPU_MS 0.75");
    const auto mismatch = collision::CollisionBenchmarkReceipt::deserialize(corrupt);
    require(!mismatch && mismatch.error().code == core::ErrorCode::stale_data,
        "digest-mismatched receipts fail with stale_data");
}

} // namespace

int main() {
    motion_policy_enforces_kinematic_contract();
    motion_policy_detects_cycles();
    motion_policy_round_trips_and_rejects_hostile_input();
    profile_constrains_generation_and_admission();
    profile_round_trips_and_rejects_hostile_input();
    variant_selection_certifies_one_variant();
    variant_selection_rejects_hostile_input();
    benchmark_promotion_never_replaces_last_good_with_failure();
    benchmark_evidence_tiers_never_promote_silently();
    benchmark_receipt_round_trips_and_rejects_hostile_input();
    std::cout << "collision policy tests passed\n";
    return EXIT_SUCCESS;
}
