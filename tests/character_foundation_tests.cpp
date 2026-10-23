#include <carto/character/bindings.hpp>
#include <carto/character/constraints_record.hpp>
#include <carto/character/controls.hpp>
#include <carto/character/coordinate.hpp>
#include <carto/character/definition.hpp>
#include <carto/character/drivers.hpp>
#include <carto/character/ids.hpp>
#include <carto/character/invalidation.hpp>
#include <carto/character/manifest.hpp>
#include <carto/character/morph.hpp>
#include <carto/character/motion_document.hpp>
#include <carto/character/receipt.hpp>
#include <carto/character/rig_symmetry.hpp>
#include <carto/character/time.hpp>
#include <carto/character/weights_record.hpp>

#include <carto/assets/blob_store.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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

void id_allocation_and_tombstones() {
    using namespace carto;
    character::IdSequence<character::CharacterTag> characters;
    const auto first = characters.allocate();
    const auto second = characters.allocate();
    require(first && second && first.value().value == 1U && second.value().value == 2U,
        "character identities allocate monotonically from one");
    require(!characters.retired(first.value()) &&
            !characters.retired(second.value()),
        "live identities carry no tombstone");
    require(static_cast<bool>(characters.retire(
                first.value(), "superseded by redesign", core::Revision{7U})),
        "allocated identity retires with a reason and revision");
    require(characters.retired(first.value()), "retired identity is retained");
    require(!characters.retire(first.value(), "double retire", core::Revision{8U}),
        "a second tombstone for one identity is refused");
    require(!characters.retire(character::CharacterId{99U}, "never allocated",
                core::Revision{8U}),
        "retiring an identity the sequence never allocated is refused");
    require(!characters.retire(character::CharacterId{0U}, "invalid",
                core::Revision{8U}),
        "retiring the invalid identity is refused");
    require(static_cast<bool>(characters.tombstones().validate()),
        "tombstone set validates sorted and unique");

    character::TombstoneSet mixed;
    require(static_cast<bool>(mixed.insert(character::Tombstone{
                character::IdKind::control, 5U, "deleted control",
                core::Revision{3U}})),
        "control tombstone enters the mixed set");
    require(static_cast<bool>(mixed.insert(character::Tombstone{
                character::IdKind::character, 5U, "deleted character",
                core::Revision{4U}})),
        "same numeric value in another identity space is a distinct tombstone");
    require(mixed.contains(character::IdKind::control, 5U) &&
            mixed.contains(character::IdKind::character, 5U) &&
            !mixed.contains(character::IdKind::rig, 5U),
        "tombstone lookup is keyed by identity space and value");
    require(character::id_string(second.value()) == "character:2",
        "identity string form matches the interchange envelope");
}

void rational_time_exactness_and_conversion_receipts() {
    using namespace carto;
    const character::RationalTime one_second{24, 24};
    const character::RationalTime half_second{12, 24};
    const character::RationalTime third{10, 30};
    const auto sum = character::add(one_second, half_second);
    require(sum && sum.value().tick == 36 && sum.value().ticks_per_second == 24,
        "same-timebase addition is exact");
    const auto difference = character::subtract(one_second, half_second);
    require(difference && difference.value().tick == 12,
        "same-timebase subtraction is exact");
    const auto cross = character::add(one_second, third);
    require(cross && cross.value().tick == 160 &&
            cross.value().ticks_per_second == 120 &&
            character::compare(cross.value(), character::RationalTime{4, 3}) ==
                std::strong_ordering::equal,
        "cross-timebase addition rescales to the lcm timebase exactly");
    require(character::compare(one_second, character::RationalTime{2, 2}) ==
            std::strong_ordering::equal,
        "equality holds exactly across timebases");
    require(character::compare(third, half_second) == std::strong_ordering::less,
        "ordering holds exactly across timebases");
    require(character::compare(
                character::RationalTime{-1, 2}, character::RationalTime{1, 3}) ==
            std::strong_ordering::less,
        "negative rational times order exactly");
    require(character::compare(
                character::RationalTime{0, 24}, character::RationalTime{-5, 30}) ==
            std::strong_ordering::greater,
        "zero orders above negative times exactly");

    const auto seconds = character::to_seconds(third);
    require(std::abs(seconds.seconds - (10.0 / 30.0)) <= seconds.max_abs_error &&
            seconds.max_abs_error <=
                std::abs(std::nextafter(seconds.seconds, 2.0) - seconds.seconds),
        "rational-to-double receipt bounds the error by half an ulp");
    const auto back = character::from_seconds(seconds.seconds, 30U);
    require(back && back.value().time.tick == 10 &&
            back.value().max_abs_error > 0.0,
        "double-to-rational rounds to the nearest tick with an error receipt");
    require(!character::from_seconds(std::numeric_limits<double>::infinity(), 24U),
        "non-finite seconds fail closed");
    const auto huge = character::from_seconds(1e300, 24U);
    require(!huge, "seconds outside the tick range fail closed");

    const character::RationalTime monotone[] = {
        {0, 24}, {12, 24}, {12, 24}, {24, 24}};
    require(static_cast<bool>(character::validate_monotonic(monotone)),
        "non-decreasing same-timebase sequence is monotonic");
    const character::RationalTime decreasing[] = {{12, 24}, {0, 24}};
    require(!character::validate_monotonic(decreasing),
        "a decreasing sequence fails monotonic validation");
    const character::RationalTime mixed_timebase[] = {{12, 24}, {12, 30}};
    require(!character::validate_monotonic(mixed_timebase),
        "monotonic validation requires one shared timebase");

    const auto roundtrip = character::deserialize_rational_time(
        character::serialize(one_second));
    require(roundtrip && roundtrip.value().tick == 24 &&
            roundtrip.value().ticks_per_second == 24,
        "rational time canonical codec roundtrips");
    require(!character::deserialize_rational_time("CARTO_RATIONAL_TIME 99\n"),
        "unknown rational time schema versions fail closed");
    const auto first_digest = character::canonical_digest(one_second);
    const auto second_digest = character::canonical_digest(one_second);
    require(first_digest && second_digest &&
            first_digest.value() == second_digest.value(),
        "rational time canonical digest is deterministic");
}

void coordinate_policy_checklist_fail_closed() {
    using namespace carto;
    const character::CoordinatePolicy vanta{
        .handedness = character::Handedness::right,
        .up = character::Axis::positive_y,
        .forward = character::Axis::positive_z,
        .meters_per_unit = 1.0,
        .winding = character::WindingOrder::counter_clockwise,
        .rotation = character::RotationRepresentation::quaternion_normalized_positive_w,
        .inverse_bind = character::InverseBindSpace::world_space,
    };
    const character::CoordinatePolicy studio = [&] {
        character::CoordinatePolicy policy = vanta;
        policy.handedness = character::Handedness::left;
        policy.winding = character::WindingOrder::clockwise;
        return policy;
    }();
    require(static_cast<bool>(vanta.validate()) &&
            static_cast<bool>(studio.validate()),
        "both coordinate policies validate independently");

    character::CoordinateConversion identity{vanta, vanta, {}};
    require(identity.identity() && static_cast<bool>(identity.validate()),
        "identity conversion takes the fast path without a checklist");

    character::CoordinateConversion conversion{vanta, studio, {}};
    require(!conversion.identity() && !conversion.validate(),
        "non-identity conversion with an incomplete checklist fails closed");
    conversion.checklist.mark(character::TransformedEntityClass::mesh_positions);
    conversion.checklist.mark(character::TransformedEntityClass::normals_tangents);
    conversion.checklist.mark(character::TransformedEntityClass::joint_rest_transforms);
    conversion.checklist.mark(character::TransformedEntityClass::inverse_binds);
    conversion.checklist.mark(character::TransformedEntityClass::root_motion);
    conversion.checklist.mark(character::TransformedEntityClass::collision_geometry);
    conversion.checklist.mark(character::TransformedEntityClass::garment_anchors);
    conversion.checklist.mark(character::TransformedEntityClass::groom_anchors);
    require(!conversion.validate(),
        "one unconfirmed entity class still refuses the conversion");
    conversion.checklist.mark(character::TransformedEntityClass::mocap_frames);
    require(static_cast<bool>(conversion.validate()),
        "a fully confirmed checklist admits the conversion");

    character::CoordinatePolicy invalid = vanta;
    invalid.meters_per_unit = 0.0;
    require(!invalid.validate(),
        "non-positive meters-per-unit fails closed");
    invalid = vanta;
    invalid.up = character::Axis::positive_z;
    require(!invalid.validate(),
        "parallel up and forward axes fail closed");
}

void control_graph_cycles_and_definition_failures() {
    using namespace carto;
    character::ControlGraph graph;
    require(static_cast<bool>(graph.insert(character::ControlDefinition{
                .id = character::ControlId{1U},
                .shape = character::ControlShape::locator,
                .affected_joints = {kinematic::JointId{10U}},
                .allowed_axes = character::kAxisTranslateX,
            })),
        "first control enters the graph");
    require(static_cast<bool>(graph.insert(character::ControlDefinition{
                .id = character::ControlId{2U},
                .shape = character::ControlShape::sphere,
                .follow_space = character::ControlId{1U},
            })),
        "follow-space control enters the graph");
    require(static_cast<bool>(graph.insert_edge(
                character::ControlEdge{character::ControlId{1U},
                    character::ControlId{2U}})),
        "control edge enters the graph");
    require(static_cast<bool>(graph.validate()),
        "acyclic control graph validates");

    character::ControlGraph cyclic;
    require(static_cast<bool>(cyclic.insert(character::ControlDefinition{
                .id = character::ControlId{1U}})) &&
            static_cast<bool>(cyclic.insert(character::ControlDefinition{
                .id = character::ControlId{2U}})),
        "cycle fixture controls enter");
    require(static_cast<bool>(cyclic.insert_edge(
                character::ControlEdge{character::ControlId{1U},
                    character::ControlId{2U}})) &&
            static_cast<bool>(cyclic.insert_edge(
                character::ControlEdge{character::ControlId{2U},
                    character::ControlId{1U}})),
        "mutual control edges enter");
    require(!cyclic.validate(),
        "a control dependency cycle fails closed");
    character::ControlGraph self_follow;
    require(static_cast<bool>(self_follow.insert(character::ControlDefinition{
                .id = character::ControlId{1U}})),
        "self-cycle fixture control enters");
    require(!self_follow.insert(character::ControlDefinition{
                .id = character::ControlId{2U},
                .follow_space = character::ControlId{2U}}),
        "a self follow-space edge is refused at insert");

    character::CharacterDefinition definition;
    definition.id = character::CharacterId{7U};
    definition.project_namespace = "example-project";
    definition.source_revision = core::Revision{42U};
    definition.source_digest = digest("source-bytes");
    definition.root = scene::ObjectId{11U};
    definition.mesh_refs = {{3U, digest("mesh-a")}, {9U, digest("mesh-b")}};
    definition.rig = {character::RigId{5U}, digest("rig-graph")};
    definition.control_graph = {character::RigId{5U}, digest("control-graph")};
    definition.locale_neutral_labels = {{"display", "Hero"}};
    definition.coordinate_policy_digest = digest("policy");
    require(static_cast<bool>(definition.validate()),
        "complete character definition validates");

    auto dangling = definition;
    dangling.rig.graph_digest = {};
    require(!dangling.validate(),
        "a rig reference with a zero graph digest fails closed");
    auto duplicate = definition;
    duplicate.mesh_refs.push_back({3U, digest("mesh-a-copy")});
    require(!duplicate.validate(),
        "duplicate mesh asset identities fail closed");
    auto zero_root = definition;
    zero_root.root = scene::ObjectId{0U};
    require(!zero_root.validate(),
        "a zero root scene object fails closed");

    character::RigDefinition rig;
    rig.id = character::RigId{5U};
    rig.kinematic_graph_digest = digest("rig-graph");
    rig.joint_limits = {digest("limits")};
    rig.rest_basis.handedness = character::Handedness::right;
    rig.rest_basis.meters_per_unit = 1.0;
    rig.semantic_tags = {"arm.left", "arm.right"};
    rig.control_graph = cyclic;
    rig.symmetry = {{kinematic::JointId{3U}, kinematic::JointId{4U}}};
    require(!rig.validate(),
        "a rig whose control graph is cyclic fails closed");

    auto unsorted_tags = rig;
    unsorted_tags.control_graph = graph;
    unsorted_tags.semantic_tags = {"b", "a"};
    require(!unsorted_tags.validate(),
        "unsorted rig semantic tags fail closed");
    auto symmetric = unsorted_tags;
    symmetric.semantic_tags = {"arm.left"};
    symmetric.symmetry = {{kinematic::JointId{3U}, kinematic::JointId{3U}}};
    require(!symmetric.validate(),
        "a self-pairing symmetry map entry fails closed");
}

void influence_admission() {
    using namespace carto;
    using Influence = kinematic::JointInfluence;
    const std::vector<Influence> valid{
        {kinematic::JointId{1U}, 0.25}, {kinematic::JointId{2U}, 0.75}};
    require(static_cast<bool>(character::validate_influences(valid)),
        "one to eight normalized positive influences are admitted");

    const std::vector<Influence> empty;
    require(!character::validate_influences(empty),
        "zero influences are rejected");
    std::vector<Influence> nine = valid;
    nine.resize(9U, Influence{kinematic::JointId{10U}, 0.05});
    require(!character::validate_influences(nine),
        "nine influences are rejected");
    const std::vector<Influence> negative{
        {kinematic::JointId{1U}, 0.5}, {kinematic::JointId{2U}, -0.2},
        {kinematic::JointId{3U}, 0.7}};
    require(!character::validate_influences(negative),
        "negative weights are rejected");
    const std::vector<Influence> unnormalized{
        {kinematic::JointId{1U}, 0.2}, {kinematic::JointId{2U}, 0.3}};
    require(!character::validate_influences(unnormalized),
        "weights not normalized to one are rejected");
    const std::vector<Influence> non_finite{
        {kinematic::JointId{1U}, std::numeric_limits<double>::infinity()},
        {kinematic::JointId{2U}, 1.0}};
    require(!character::validate_influences(non_finite),
        "non-finite weights are rejected");
    const std::vector<Influence> duplicate_joint{
        {kinematic::JointId{1U}, 0.5}, {kinematic::JointId{1U}, 0.5}};
    require(!character::validate_influences(duplicate_joint),
        "duplicate joints in one vertex are rejected");

    character::SkinWeightLayer layer;
    layer.layer_id = character::RigLayerId{4U};
    layer.mesh_topology_digest = digest("topology");
    layer.joint_graph_digest = digest("graph");
    layer.influenced_vertex_count = 2U;
    layer.total_influence_count = 4U;
    layer.provenance = character::WeightProvenance::automatic_proposal;
    layer.provenance_digest = digest("recipe");
    require(static_cast<bool>(layer.validate()),
        "fresh automatic-proposal weight layer validates");
    auto stale = layer;
    stale.staleness = character::WeightStaleness::stale_mesh_topology;
    stale.stale_since = core::Revision{9U};
    require(static_cast<bool>(stale.validate()),
        "staleness with an invalid revision validates");
    auto inconsistent = layer;
    inconsistent.staleness = character::WeightStaleness::invalid;
    require(!inconsistent.validate(),
        "staleness without an invalid revision is refused");
    auto zero_provenance = layer;
    zero_provenance.provenance_digest = {};
    require(!zero_provenance.validate(),
        "weight layer provenance must be digest-bound");
}

void morph_target_bounds() {
    using namespace carto;
    character::MorphTarget morph;
    morph.id = character::MorphTargetId{3U};
    morph.source_mesh_asset = 12U;
    morph.source_mesh_digest = digest("mesh");
    morph.displaced_point_count = 1500U;
    morph.delta_blob_digest = digest("deltas");
    morph.delta_blob_bytes = 1500U * 3U * 4U;
    morph.topology_correspondence_digest = digest("correspondence");
    morph.precision_bound = 0.25;
    require(static_cast<bool>(morph.validate()),
        "bounded morph target with correspondence validates");

    auto no_correspondence = morph;
    no_correspondence.topology_correspondence_digest = {};
    require(!no_correspondence.validate(),
        "a morph target without stable topology correspondence fails closed");
    auto unbounded = morph;
    unbounded.displaced_point_count =
        character::MorphTarget::kMaxDisplacedPoints + 1U;
    require(!unbounded.validate(),
        "displaced point counts above the bound fail closed");
    auto blob_overflow = morph;
    blob_overflow.delta_blob_bytes =
        character::MorphTarget::kMaxDeltaBlobBytes + 1U;
    require(!blob_overflow.validate(),
        "delta blobs above the byte bound fail closed");
    auto bad_precision = morph;
    bad_precision.precision_bound = 0.0;
    require(!bad_precision.validate(),
        "a non-positive precision bound fails closed");
}

void constraint_cycles_and_arbitration() {
    using namespace carto;
    character::ConstraintGraph graph;
    character::ConstraintDefinition ik;
    ik.id = character::ConstraintId{1U};
    ik.type = character::ConstraintType::ik_pin;
    ik.source = {.joint = kinematic::JointId{1U}};
    ik.target = {.joint = kinematic::JointId{2U}};
    ik.tolerance = 1e-4;
    ik.validity_start = {0, 24};
    ik.validity_end = {48, 24};
    require(static_cast<bool>(graph.insert(ik)),
        "first constraint enters the graph");
    character::ConstraintDefinition orient;
    orient.id = character::ConstraintId{2U};
    orient.type = character::ConstraintType::orient;
    orient.source = {.joint = kinematic::JointId{2U}};
    orient.target = {.joint = kinematic::JointId{2U}};
    orient.tolerance = 1e-4;
    orient.validity_start = {0, 24};
    orient.validity_end = {48, 24};
    require(static_cast<bool>(graph.insert(orient)),
        "same-target constraint with a different type enters");
    require(!graph.validate(),
        "conflicting constraints without declared arbitration fail closed");
    require(static_cast<bool>(graph.declare_arbitration(
                character::ConstraintArbitration{
                    character::ConstraintType::ik_pin,
                    character::ConstraintType::orient,
                    character::ArbitrationRule::priority_wins})),
        "conflict arbitration is declared");
    require(static_cast<bool>(graph.validate()),
        "arbitrated constraint graph validates");

    character::ConstraintGraph cyclic;
    character::ConstraintDefinition first;
    first.id = character::ConstraintId{1U};
    first.type = character::ConstraintType::ik_pin;
    first.source = {.joint = kinematic::JointId{1U}};
    first.target = {.joint = kinematic::JointId{2U}};
    first.tolerance = 0.001;
    first.validity_start = {0, 24};
    first.validity_end = {24, 24};
    first.depends_on = character::ConstraintId{2U};
    character::ConstraintDefinition second = first;
    second.id = character::ConstraintId{2U};
    second.type = character::ConstraintType::contact;
    second.depends_on = character::ConstraintId{1U};
    require(static_cast<bool>(cyclic.insert(first)) &&
            static_cast<bool>(cyclic.insert(second)),
        "mutually dependent constraints enter");
    require(!cyclic.validate(),
        "a constraint dependency cycle fails closed");

    auto inverted = first;
    inverted.id = character::ConstraintId{9U};
    inverted.depends_on = std::nullopt;
    inverted.validity_start = {24, 24};
    inverted.validity_end = {0, 24};
    character::ConstraintGraph window;
    require(!window.insert(inverted),
        "an inverted validity window is refused at insert");
}

void motion_document_validation() {
    using namespace carto;
    character::MotionDocument document;
    document.id = character::MotionDocumentId{6U};
    document.timebase_ticks_per_second = 24U;
    document.clips = {{temporal::ClipId{2U}, digest("clip")}};
    document.channels = {{temporal::ChannelId{3U}, digest("channel")}};
    document.events = {{temporal::EventId{4U}, digest("event")}};
    document.targets = {{.joint = kinematic::JointId{5U}}};
    document.root_motion = character::RootMotionPolicy::proposal_only;
    document.missing_target = character::MissingTargetHandling::skip_with_receipt;
    require(static_cast<bool>(document.validate()),
        "motion document with explicit policies validates");
    auto zero_timebase = document;
    zero_timebase.timebase_ticks_per_second = 0U;
    require(!zero_timebase.validate(),
        "a zero rational timebase fails closed");
    auto dangling = document;
    dangling.clips.front().digest = {};
    require(!dangling.validate(),
        "a clip reference without a content digest fails closed");
    auto duplicate_target = document;
    duplicate_target.targets.push_back({.joint = kinematic::JointId{5U}});
    require(!duplicate_target.validate(),
        "duplicate motion targets fail closed");
}

void bindings_no_orphans() {
    using namespace carto;
    character::GarmentBinding garment;
    garment.id = character::GarmentBindingId{8U};
    garment.source_mesh = {21U, core::Revision{3U}, digest("cloth")};
    garment.attachments = {{.joint = kinematic::JointId{2U}}};
    garment.seams = {{100U, 0U}};
    garment.material_intent_ref = digest("material");
    garment.lod_mapping = {{0U, digest("lod0")}, {1U, digest("lod1")}};
    require(static_cast<bool>(garment.validate()),
        "garment binding with attached seams validates");
    auto orphan = garment;
    orphan.seams = {{100U, 1U}};
    require(!orphan.validate(),
        "an orphan seam outside the attachment table fails closed");
    auto unpinned = garment;
    unpinned.source_mesh.pinned_revision = core::Revision{0U};
    require(!unpinned.validate(),
        "a garment source without a pinned revision fails closed");

    character::GroomBinding groom;
    groom.id = character::GroomBindingId{9U};
    groom.source_guides = {33U, core::Revision{2U}, digest("guides")};
    groom.attachments = {{.control = character::ControlId{4U}}};
    groom.guides = {{7U, 0U}};
    groom.material_intent_ref = digest("groom-material");
    groom.lod_mapping = {{0U, digest("groom-lod0")}};
    require(static_cast<bool>(groom.validate()),
        "groom binding with attached guides validates");
    auto orphan_guide = groom;
    orphan_guide.guides = {{7U, 4U}};
    require(!orphan_guide.validate(),
        "an orphan guide outside the attachment table fails closed");
}

void manifest_golden_bytes_and_closure() {
    using namespace carto;
    character::CharacterPreparedManifest manifest;
    manifest.source = {"example-project", 42U, digest("project-source")};
    manifest.coordinate = {character::Handedness::right,
        character::Axis::positive_y, character::Axis::positive_z, 1.0};
    manifest.character_id = character::CharacterId{7U};
    manifest.target_profile = {"vanta-character-v1", digest("profile")};
    manifest.required_capabilities = {"animation.clips", "skeleton", "skin.lbs"};
    const auto skeleton_bytes = std::string("skeleton-payload");
    const auto weights_bytes = std::string("weights8-payload");
    const auto skeleton_digest =
        digest(skeleton_bytes);
    const auto weights_digest = digest(weights_bytes);
    manifest.products = {
        {"character.skeleton", "products/skeleton.bin", 1U, skeleton_digest},
        {"character.weights8", "products/weights.bin", 1U, weights_digest},
    };
    require(static_cast<bool>(manifest.validate()),
        "prepared manifest with real fixture digests validates");

    const std::string encoded = manifest.serialize();
    const auto reopened = character::CharacterPreparedManifest::deserialize(encoded);
    if (!reopened) {
        std::cerr << "manifest reopen diagnostic: " << reopened.error().message << '\n';
    }
    require(reopened && reopened.value().serialize() == encoded,
        "manifest survives a deterministic JSON round trip");
    require(encoded.find("\"format\": \"carto-character-prepared\"") != std::string::npos &&
            encoded.find("\"character_id\": \"character:7\"") != std::string::npos &&
            encoded.find(skeleton_digest.hex()) != std::string::npos,
        "manifest bytes carry the Chapter 04 envelope shape with real digests");

    const std::map<std::string, assets::Sha256Digest> closure{
        {"products/skeleton.bin", skeleton_digest},
        {"products/weights.bin", weights_digest},
    };
    require(static_cast<bool>(manifest.verify_closure(closure)),
        "exact payload closure verifies");
    std::map<std::string, assets::Sha256Digest> missing = closure;
    missing.erase("products/weights.bin");
    require(!manifest.verify_closure(missing),
        "a missing product payload fails closure verification");
    auto tampered = closure;
    tampered["products/skeleton.bin"] = digest("tampered");
    require(!manifest.verify_closure(tampered),
        "a tampered product digest fails closure verification");
    auto extra = closure;
    extra["products/extra.bin"] = digest("extra");
    require(!manifest.verify_closure(extra),
        "an undeclared extra payload fails closure verification");

    const std::string forged = manifest.serialize();
    const auto skeleton_at = forged.find(skeleton_digest.hex());
    require(skeleton_at != std::string::npos, "skeleton digest appears in the fixture");
    std::string corrupted_hex = forged;
    corrupted_hex[skeleton_at] = 'G';
    require(!character::CharacterPreparedManifest::deserialize(corrupted_hex),
        "a product digest that stops being exact lowercase hex fails the codec");

    auto hostile_path = manifest;
    hostile_path.products.front().path = "../outside.bin";
    require(!character::is_safe_product_path("../outside.bin") &&
            !hostile_path.validate(),
        "a parent-escaping product path fails closed");
    require(!character::is_safe_product_path("products\\weights.bin"),
        "backslash product paths are rejected");
    require(!character::is_safe_product_path("/abs/weights.bin"),
        "absolute product paths are rejected");
    auto hostile = manifest;
    hostile.products.back().path = "products\\evil.bin";
    require(!character::CharacterPreparedManifest::deserialize(hostile.serialize()),
        "a backslash path cannot survive manifest serialization");

    std::string uppercase_hex = encoded;
    const std::string lower = skeleton_digest.hex();
    std::string upper = lower;
    std::transform(upper.begin(), upper.end(), upper.begin(),
        [](char byte) { return static_cast<char>(std::toupper(byte)); });
    const auto at = uppercase_hex.find(lower);
    require(at != std::string::npos, "fixture digest appears in the envelope");
    uppercase_hex.replace(at, lower.size(), upper);
    require(!character::CharacterPreparedManifest::deserialize(uppercase_hex),
        "uppercase hex digests fail the exact-lowercase admission rule");

    const auto first_digest = manifest.canonical_digest();
    const auto second_digest = reopened.value().canonical_digest();
    require(first_digest && second_digest &&
            first_digest.value() == second_digest.value(),
        "manifest identity survives reopen");
    require(!character::CharacterPreparedManifest::deserialize(
                std::string(encoded) + " x"),
        "trailing data outside the JSON object fails closed");
}

void invalidation_dag_guarantees() {
    using namespace carto;
    const character::InvalidationDag dag = character::InvalidationDag::character_default();
    require(static_cast<bool>(dag.validate()),
        "default invalidation DAG validates");

    const character::DependencyKind mesh[] = {character::DependencyKind::mesh_topology_change};
    const auto mesh_result = dag.propagate(mesh);
    require(mesh_result.contains(character::ProductKind::skin_weights) &&
            mesh_result.contains(character::ProductKind::morph_target) &&
            mesh_result.contains(character::ProductKind::tangent_data) &&
            mesh_result.contains(character::ProductKind::collision_proxy) &&
            mesh_result.contains(character::ProductKind::deformation_lod) &&
            mesh_result.contains(character::ProductKind::prepared_bundle),
        "mesh topology edits invalidate weights, morphs, tangents, proxies, LODs, bundles");

    const character::DependencyKind timeline[] = {character::DependencyKind::timeline_edit};
    const auto timeline_result = dag.propagate(timeline);
    require(timeline_result.contains(character::ProductKind::sampled_animation) &&
            timeline_result.contains(character::ProductKind::runtime_frame) &&
            timeline_result.contains(character::ProductKind::prepared_bundle),
        "timeline edits invalidate sampled animation and runtime frame outputs");

    const character::DependencyKind material[] = {character::DependencyKind::material_edit};
    const auto material_result = dag.propagate(material);
    require(material_result.contains(character::ProductKind::material_shader) &&
            material_result.contains(character::ProductKind::prepared_bundle),
        "material edits invalidate material shader product state");

    const character::DependencyKind rig[] = {character::DependencyKind::rig_edit};
    const auto rig_result = dag.propagate(rig);
    require(rig_result.contains(character::ProductKind::rest_skeleton) &&
            rig_result.contains(character::ProductKind::skin_weights) &&
            rig_result.contains(character::ProductKind::prepared_bundle),
        "rig edits invalidate the rest skeleton and dependent products");

    // Chapter 04 negative guarantees.
    require(!timeline_result.contains(character::ProductKind::material_shader),
        "timeline edits do NOT invalidate materials");
    require(!timeline_result.contains(character::ProductKind::rest_skeleton),
        "timeline edits do NOT invalidate the rest skeleton");
    require(!material_result.contains(character::ProductKind::rest_skeleton),
        "material edits do NOT invalidate the rest skeleton");
    require(!material_result.contains(character::ProductKind::sampled_animation),
        "material edits do NOT invalidate sampled animation");
    require(!character::invalidates(character::ProductKind::material_shader,
                character::DependencyKind::timeline_edit),
        "encoded reachability agrees: timeline edit does not touch materials");
    require(!character::invalidates(character::ProductKind::rest_skeleton,
                character::DependencyKind::material_edit),
        "encoded reachability agrees: material edit does not touch rest skeleton");

    const character::DependencyKind both[] = {character::DependencyKind::timeline_edit,
        character::DependencyKind::material_edit};
    const auto combined = dag.propagate(both);
    require(combined.contains(character::ProductKind::sampled_animation) &&
            combined.contains(character::ProductKind::material_shader),
        "multi-kind propagation unions the individual closures");
    require(!dag.propagate({}).contains(character::ProductKind::prepared_bundle),
        "an empty change set invalidates nothing");
}

void receipts_carry_no_wall_time() {
    using namespace carto;
    character::CharacterProductReceipt receipt;
    receipt.producer_build = "cartographer-test 1.0";
    receipt.source_namespace = "example-project";
    receipt.source_revision = core::Revision{42U};
    receipt.source_digest = digest("source");
    receipt.recipe_digest = digest("recipe");
    receipt.format_schema_major = 1U;
    receipt.format_schema_minor = 0U;
    receipt.backend_identity = "cpu-reference";
    receipt.precision_identity = "fp64";
    receipt.output_digest = digest("output");
    require(static_cast<bool>(receipt.validate()),
        "product receipt validates without any wall-time field");
    const auto first = receipt.canonical_digest();
    const auto second = receipt.canonical_digest();
    require(first && second && first.value() == second.value(),
        "product receipt digest is deterministic across evaluations");
    require(receipt.serialize().find("TIME") == std::string::npos,
        "no clock field appears in the canonical receipt text");
    auto stale = receipt;
    stale.output_digest = {};
    require(!stale.validate(),
        "a receipt without an output digest fails closed");

    character::CharacterPromotionReceipt promotion;
    promotion.gate_ledger_ref = "ledger/revision-42";
    promotion.gate_statuses = {{"deformation.dqs", character::GateStatus::pass},
        {"motion.retarget", character::GateStatus::not_run},
        {"surface.uv", character::GateStatus::hold}};
    promotion.signer = "subsystem-lead";
    promotion.digest = digest("promotion");
    require(static_cast<bool>(promotion.validate()),
        "promotion receipt stub validates with explicit per-gate statuses");
    auto unsigned_receipt = promotion;
    unsigned_receipt.signer.clear();
    require(!unsigned_receipt.validate(),
        "an unsigned promotion receipt fails closed");
    require(character::gate_status_name(character::GateStatus::not_run) == "not-run",
        "gate statuses render as stable tokens");
}

void determinism_is_a_hard_gate() {
    using namespace carto;
    character::CharacterPreparedManifest first;
    first.source = {"example-project", 42U, digest("project-source")};
    first.coordinate = {character::Handedness::right,
        character::Axis::positive_y, character::Axis::positive_z, 1.0};
    first.character_id = character::CharacterId{7U};
    first.target_profile = {"vanta-character-v1", digest("profile")};
    first.required_capabilities = {"animation.clips", "skeleton", "skin.lbs"};
    first.products = {
        {"character.skeleton", "products/skeleton.bin", 1U,
            digest("skeleton-payload")},
        {"character.weights8", "products/weights.bin", 1U,
            digest("weights8-payload")},
    };
    character::CharacterPreparedManifest second = first;
    require(first.serialize() == second.serialize(),
        "equal manifests serialize to equal bytes twice");
    const auto first_digest = first.canonical_digest();
    const auto second_digest = second.canonical_digest();
    require(first_digest && second_digest &&
            first_digest.value() == second_digest.value(),
        "equal manifests digest equally twice");

    // Receipt determinism across equal constructions.
    character::CharacterProductReceipt receipt_a;
    receipt_a.producer_build = "cartographer-test 1.0";
    receipt_a.source_namespace = "example-project";
    receipt_a.source_revision = core::Revision{42U};
    receipt_a.source_digest = digest("source");
    receipt_a.recipe_digest = digest("recipe");
    receipt_a.format_schema_major = 1U;
    receipt_a.format_schema_minor = 0U;
    receipt_a.backend_identity = "cpu-reference";
    receipt_a.precision_identity = "fp64";
    receipt_a.output_digest = digest("output");
    character::CharacterProductReceipt receipt_b = receipt_a;
    require(receipt_a.serialize() == receipt_b.serialize() &&
            receipt_a.canonical_digest().value() == receipt_b.canonical_digest().value(),
        "equal receipts reproduce equal canonical bytes and digests");
}

} // namespace

void rig_symmetry_mirrors_reports_divergence_and_round_trips() {
    using namespace carto;
    const auto node = [](std::uint64_t id, std::optional<std::uint64_t> parent,
                         core::Vec3d translation, double z_angle) {
        kinematic::KinematicNode joint;
        joint.id = kinematic::JointId{id};
        if (parent.has_value()) joint.parent = kinematic::JointId{*parent};
        joint.rest.translation = translation;
        joint.rest.rotation = {0.0, 0.0, std::sin(0.5 * z_angle), std::cos(0.5 * z_angle)};
        joint.kind = kinematic::JointKind::ball;
        return joint;
    };
    kinematic::KinematicGraph graph;
    require(static_cast<bool>(graph.insert_node(node(1U, std::nullopt, {0.0, 1.0, 0.0}, 0.0))),
        "spine inserts");
    require(static_cast<bool>(graph.insert_node(node(2U, 1U, {1.0, 0.0, 0.0}, 0.3))),
        "left arm inserts");
    require(static_cast<bool>(graph.insert_node(node(3U, 1U, {-1.0, 0.0, 0.0}, 0.1))),
        "right arm inserts");
    require(static_cast<bool>(graph.insert_node(node(4U, 2U, {0.5, 0.0, 0.0}, 0.0))),
        "left hand inserts");
    require(static_cast<bool>(graph.insert_node(node(5U, 3U, {-0.4, 0.1, 0.0}, 0.0))),
        "right hand inserts");
    require(static_cast<bool>(graph.validate()), "symmetry fixture graph validates");
    const std::vector<character::SymmetryPair> pairs{
        {kinematic::JointId{2U}, kinematic::JointId{3U}},
        {kinematic::JointId{4U}, kinematic::JointId{5U}}};

    const character::SymmetryTolerance tolerance{};
    const auto before = character::analyze_rig_symmetry(
        graph, pairs, character::SymmetryAxis::x, tolerance);
    require(static_cast<bool>(before) && before.value().pair_count == 2U &&
            before.value().divergent.size() == 2U,
        "authored asymmetry is reported for both pairs");
    require(before.value().divergent[0U].rotation_error > 0.1 &&
            !before.value().divergent[0U].parent_mismatch,
        "arm divergence reports its rotation error");

    const auto mirrored = character::mirror_rig_rest(
        graph, pairs, character::SymmetryAxis::x, character::SymmetryDirection::left_to_right);
    require(static_cast<bool>(mirrored), "rig mirrors left to right");
    require(mirrored.value().receipt.changed_joint_count == 2U,
        "mirror receipt counts rewritten joints");
    const auto* right_arm = mirrored.value().graph.find(kinematic::JointId{3U});
    require(right_arm != nullptr && right_arm->rest.translation.x == -1.0 &&
            std::abs(right_arm->rest.rotation.z + std::sin(0.15)) < 1e-15,
        "right arm takes the reflected left rest");
    require(mirrored.value().graph.find(kinematic::JointId{2U})->rest.rotation.z ==
            graph.find(kinematic::JointId{2U})->rest.rotation.z,
        "the source side is untouched");
    const auto after = character::analyze_rig_symmetry(
        mirrored.value().graph, pairs, character::SymmetryAxis::x, tolerance);
    require(static_cast<bool>(after) && after.value().divergent.empty(),
        "a mirrored rig reports no divergence");

    const auto back = character::mirror_rig_rest(mirrored.value().graph, pairs,
        character::SymmetryAxis::x, character::SymmetryDirection::right_to_left);
    require(static_cast<bool>(back) && back.value().receipt.changed_joint_count == 0U &&
            back.value().receipt.result_graph_digest ==
                mirrored.value().receipt.result_graph_digest,
        "double mirror is the identity");

    const std::vector<character::SymmetryPair> crossed{
        {kinematic::JointId{2U}, kinematic::JointId{5U}}};
    const auto mismatch = character::mirror_rig_rest(
        graph, crossed, character::SymmetryAxis::x, character::SymmetryDirection::left_to_right);
    require(!mismatch, "a pair with non-mirrored parents refuses");
    const auto crossed_report = character::analyze_rig_symmetry(
        graph, crossed, character::SymmetryAxis::x, tolerance);
    require(static_cast<bool>(crossed_report) &&
            crossed_report.value().divergent.front().parent_mismatch,
        "analysis reports a parent mismatch instead of refusing");

    const std::vector<character::SymmetryPair> reused{
        {kinematic::JointId{2U}, kinematic::JointId{3U}},
        {kinematic::JointId{3U}, kinematic::JointId{5U}}};
    require(!character::mirror_rig_rest(graph, reused, character::SymmetryAxis::x,
                character::SymmetryDirection::left_to_right),
        "a joint in two pairs refuses");
    const std::vector<character::SymmetryPair> absent{
        {kinematic::JointId{2U}, kinematic::JointId{42U}}};
    require(!character::mirror_rig_rest(graph, absent, character::SymmetryAxis::x,
                character::SymmetryDirection::left_to_right),
        "a pair naming an absent joint refuses");
    require(!character::mirror_rig_rest(graph, {}, character::SymmetryAxis::x,
                character::SymmetryDirection::left_to_right),
        "an empty pairing refuses");
}

void rig_control_symmetry_copies_remap_and_round_trips() {
    using namespace carto;
    const auto node = [](std::uint64_t id, std::optional<std::uint64_t> parent,
                         core::Vec3d translation) {
        kinematic::KinematicNode joint;
        joint.id = kinematic::JointId{id};
        if (parent.has_value()) joint.parent = kinematic::JointId{*parent};
        joint.rest.translation = translation;
        joint.kind = kinematic::JointKind::ball;
        return joint;
    };
    kinematic::KinematicGraph graph;
    require(static_cast<bool>(graph.insert_node(node(1U, std::nullopt, {0.0, 1.0, 0.0}))),
        "spine inserts");
    require(static_cast<bool>(graph.insert_node(node(2U, 1U, {1.0, 0.0, 0.0}))),
        "left arm inserts");
    require(static_cast<bool>(graph.insert_node(node(3U, 1U, {-1.0, 0.0, 0.0}))),
        "right arm inserts");
    require(static_cast<bool>(graph.insert_node(node(4U, 2U, {0.5, 0.0, 0.0}))),
        "left hand inserts");
    require(static_cast<bool>(graph.insert_node(node(5U, 3U, {-0.5, 0.0, 0.0}))),
        "right hand inserts");
    require(static_cast<bool>(graph.insert_node(node(6U, 1U, {0.0, 1.5, 0.0}))),
        "unpaired head inserts");
    const std::vector<character::SymmetryPair> joint_pairs{
        {kinematic::JointId{2U}, kinematic::JointId{3U}},
        {kinematic::JointId{4U}, kinematic::JointId{5U}}};

    const auto control = [](std::uint64_t id, std::vector<std::uint64_t> joints,
                            std::optional<std::uint64_t> follow, std::string set) {
        character::ControlDefinition definition;
        definition.id = character::ControlId{id};
        definition.shape = character::ControlShape::sphere;
        for (const std::uint64_t joint : joints) {
            definition.affected_joints.push_back(kinematic::JointId{joint});
        }
        std::sort(definition.affected_joints.begin(), definition.affected_joints.end());
        definition.control_set = std::move(set);
        if (follow.has_value()) {
            definition.follow_space = character::ControlId{*follow};
        }
        return definition;
    };

    character::ControlGraph controls;
    character::ControlDefinition left = control(10U, {2U, 4U}, std::nullopt, "body.core");
    left.transform.translation = {1.0, 0.5, 0.0};
    left.allowed_axes = character::kAxisTranslateX | character::kAxisRotateZ;
    left.axis_minimum[0] = -2.0;
    left.axis_maximum[0] = 2.0;
    character::ControlDefinition right = control(11U, {3U, 5U}, std::nullopt, "");
    right.transform.translation = {-0.3, 0.0, 0.0};
    const character::ControlDefinition spine = control(12U, {1U}, std::nullopt, "");
    require(static_cast<bool>(controls.insert(left)) &&
            static_cast<bool>(controls.insert(right)) &&
            static_cast<bool>(controls.insert(spine)),
        "symmetry fixture controls insert");
    require(static_cast<bool>(controls.insert_edge(
                character::ControlEdge{character::ControlId{12U}, character::ControlId{10U}})),
        "an explicit driver edge inserts");
    require(static_cast<bool>(controls.validate()), "symmetry fixture controls validate");

    const auto digest = controls.canonical_digest();
    const character::ControlGraph twin = controls;
    const auto twin_digest = twin.canonical_digest();
    require(digest && twin_digest && digest.value() == twin_digest.value(),
        "equal control graphs digest equally");
    character::ControlGraph altered = controls;
    require(static_cast<bool>(altered.insert(control(13U, {1U}, std::nullopt, "extra"))),
        "an extra control inserts for the digest contrast");
    const auto altered_digest = altered.canonical_digest();
    require(altered_digest && digest && altered_digest.value() != digest.value(),
        "a changed control graph digests differently");
    character::ControlGraph broken;
    require(static_cast<bool>(broken.insert(control(20U, {1U}, 99U, ""))),
        "a control with a dangling follow-space inserts");
    require(!broken.canonical_digest(),
        "an invalid control graph fails closed instead of digesting");

    character::ControlSymmetryPair degenerate{character::ControlId{10U}, character::ControlId{10U}};
    require(!degenerate.validate(),
        "a control pair with two equal identities refuses");

    const std::vector<character::ControlSymmetryPair> control_pairs{
        {character::ControlId{10U}, character::ControlId{11U}}};
    const auto mirrored = character::mirror_rig_controls(graph, controls, joint_pairs,
        control_pairs, character::SymmetryAxis::x, character::SymmetryDirection::left_to_right);
    require(static_cast<bool>(mirrored), "controls mirror left to right");
    require(mirrored.value().receipt.changed_control_count == 1U,
        "the control receipt counts rewritten controls");
    require(mirrored.value().receipt.source_graph_digest ==
                mirrored.value().receipt.result_graph_digest,
        "the joint graph is carried through, not rewritten");
    require(mirrored.value().receipt.source_control_graph_digest !=
                mirrored.value().receipt.result_control_graph_digest,
        "the control graph digest changes under the copy");

    const character::ControlDefinition* copied =
        mirrored.value().control_graph.find(character::ControlId{11U});
    require(copied != nullptr && copied->transform.translation.x == 1.0 &&
            copied->transform.translation.y == 0.5,
        "the destination takes the source transform");
    require(copied->affected_joints.size() == 2U &&
            copied->affected_joints[0U] == kinematic::JointId{3U} &&
            copied->affected_joints[1U] == kinematic::JointId{5U},
        "affected joints remap through the joint mirror map");
    require(copied->control_set == "body.core",
        "the control set is carried verbatim");
    require(copied->allowed_axes ==
                (character::kAxisTranslateX | character::kAxisRotateZ) &&
            copied->axis_minimum[0U] == -2.0 && copied->axis_maximum[0U] == 2.0,
        "allowed axes and their limits copy across");
    const character::ControlDefinition* untouched =
        mirrored.value().control_graph.find(character::ControlId{10U});
    require(untouched != nullptr &&
            untouched->affected_joints[0U] == kinematic::JointId{2U} &&
            untouched->transform.translation.x == 1.0,
        "the source side is untouched");
    require(mirrored.value().control_graph.edges().size() == 1U &&
            mirrored.value().control_graph.edges().front().driver ==
                character::ControlId{12U},
        "explicit control edges are preserved");

    const auto back = character::mirror_rig_controls(mirrored.value().graph,
        mirrored.value().control_graph, joint_pairs, control_pairs,
        character::SymmetryAxis::x, character::SymmetryDirection::right_to_left);
    require(static_cast<bool>(back) && back.value().receipt.changed_control_count == 0U &&
            back.value().receipt.result_control_graph_digest ==
                mirrored.value().receipt.result_control_graph_digest,
        "double control mirror is the identity");

    const std::vector<character::ControlSymmetryPair> absent{
        {character::ControlId{10U}, character::ControlId{99U}}};
    require(!character::mirror_rig_controls(graph, controls, joint_pairs, absent,
                character::SymmetryAxis::x, character::SymmetryDirection::left_to_right),
        "a control pair naming an absent control refuses");
    const std::vector<character::ControlSymmetryPair> reused{
        {character::ControlId{10U}, character::ControlId{11U}},
        {character::ControlId{10U}, character::ControlId{12U}}};
    require(!character::mirror_rig_controls(graph, controls, joint_pairs, reused,
                character::SymmetryAxis::x, character::SymmetryDirection::left_to_right),
        "a control in two pairs refuses");
    require(!character::mirror_rig_controls(graph, controls, joint_pairs, {},
                character::SymmetryAxis::x, character::SymmetryDirection::left_to_right),
        "an empty control pairing refuses");
    require(!character::mirror_rig_controls(graph, controls, {}, control_pairs,
                character::SymmetryAxis::x, character::SymmetryDirection::left_to_right),
        "control mirroring without joint pairs refuses");

    character::ControlGraph lopsided;
    require(static_cast<bool>(lopsided.insert(control(30U, {2U, 6U}, std::nullopt, ""))) &&
            static_cast<bool>(lopsided.insert(control(31U, {3U, 6U}, std::nullopt, ""))),
        "unpaired-affecting controls insert");
    const std::vector<character::ControlSymmetryPair> lopsided_pairs{
        {character::ControlId{30U}, character::ControlId{31U}}};
    require(!character::mirror_rig_controls(graph, lopsided, joint_pairs, lopsided_pairs,
                character::SymmetryAxis::x, character::SymmetryDirection::left_to_right),
        "a source control affecting a joint outside the symmetry map refuses");

    character::ControlGraph cycling;
    require(static_cast<bool>(cycling.insert(control(40U, {2U}, 41U, ""))) &&
            static_cast<bool>(cycling.insert(control(41U, {3U}, std::nullopt, ""))) &&
            static_cast<bool>(cycling.validate()),
        "a cross-side follow chain is valid before mirroring");
    const std::vector<character::ControlSymmetryPair> cycling_pairs{
        {character::ControlId{40U}, character::ControlId{41U}}};
    require(!character::mirror_rig_controls(graph, cycling, joint_pairs, cycling_pairs,
                character::SymmetryAxis::x, character::SymmetryDirection::left_to_right),
        "a remapped follow space that would cycle refuses");
}

void rig_control_symmetry_copies_and_remaps_references_atomically() {
    using namespace carto;
    const auto node = [](std::uint64_t id, std::optional<std::uint64_t> parent) {
        kinematic::KinematicNode value;
        value.id = kinematic::JointId{id};
        if (parent.has_value()) value.parent = kinematic::JointId{*parent};
        value.kind = kinematic::JointKind::ball;
        return value;
    };
    kinematic::KinematicGraph graph;
    require(static_cast<bool>(graph.insert_node(node(1U, std::nullopt))) &&
            static_cast<bool>(graph.insert_node(node(2U, 1U))) &&
            static_cast<bool>(graph.insert_node(node(3U, 1U))) &&
            static_cast<bool>(graph.validate()),
        "control symmetry fixture joints validate");
    const std::vector<character::SymmetryPair> joint_pairs{
        {kinematic::JointId{2U}, kinematic::JointId{3U}}};

    character::ControlDefinition left;
    left.id = character::ControlId{10U};
    left.transform.translation = {0.75, 1.0, 2.0};
    left.affected_joints = {kinematic::JointId{2U}};
    left.allowed_axes = static_cast<std::uint8_t>(
        character::kAxisTranslateX | character::kAxisRotateZ);
    left.control_set = "arm.primary";
    character::ControlDefinition right;
    right.id = character::ControlId{11U};
    right.transform.translation = {-4.0, 0.0, 0.0};
    right.affected_joints = {kinematic::JointId{3U}};
    right.allowed_axes = character::kAxisTranslateY;
    right.control_set = "old.right";
    character::ControlDefinition left_child = left;
    left_child.id = character::ControlId{20U};
    left_child.transform.translation = {0.25, 0.5, 0.0};
    left_child.follow_space = character::ControlId{10U};
    character::ControlDefinition right_child = right;
    right_child.id = character::ControlId{21U};
    right_child.follow_space = character::ControlId{11U};

    character::ControlGraph controls;
    require(static_cast<bool>(controls.insert(left)) &&
            static_cast<bool>(controls.insert(right)) &&
            static_cast<bool>(controls.insert(left_child)) &&
            static_cast<bool>(controls.insert(right_child)) &&
            static_cast<bool>(controls.insert_edge(
                {character::ControlId{10U}, character::ControlId{20U}})) &&
            static_cast<bool>(controls.insert_edge(
                {character::ControlId{11U}, character::ControlId{21U}})) &&
            static_cast<bool>(controls.validate()),
        "control symmetry fixture graph validates");
    const std::vector<character::ControlSymmetryPair> control_pairs{
        {character::ControlId{10U}, character::ControlId{11U}},
        {character::ControlId{20U}, character::ControlId{21U}}};
    const auto source_graph_digest = graph.canonical_digest();
    const auto source_control_digest = controls.canonical_digest();
    require(static_cast<bool>(source_graph_digest) &&
            static_cast<bool>(source_control_digest),
        "source graph digests are available");

    const auto mirrored = character::mirror_rig_controls(
        graph, controls, joint_pairs, control_pairs,
        character::SymmetryAxis::x, character::SymmetryDirection::left_to_right);
    require(static_cast<bool>(mirrored), "paired rig controls mirror left to right");
    const auto* destination = mirrored.value().control_graph.find(character::ControlId{11U});
    require(destination != nullptr &&
            destination->transform.translation.x == left.transform.translation.x &&
            destination->transform.translation.y == left.transform.translation.y &&
            destination->transform.translation.z == left.transform.translation.z &&
            destination->allowed_axes == left.allowed_axes &&
            destination->control_set == left.control_set &&
            destination->affected_joints ==
                std::vector<kinematic::JointId>{kinematic::JointId{3U}},
        "destination copies source fields and remaps affected joints");
    const auto* child = mirrored.value().control_graph.find(character::ControlId{21U});
    require(child != nullptr &&
            child->follow_space ==
                std::optional<character::ControlId>(character::ControlId{11U}) &&
            child->affected_joints ==
                std::vector<kinematic::JointId>{kinematic::JointId{3U}},
        "destination child remaps its follow-space and joint references");
    require(controls.find(character::ControlId{10U})->affected_joints ==
                std::vector<kinematic::JointId>{kinematic::JointId{2U}} &&
            controls.find(character::ControlId{11U})->control_set == "old.right",
        "mirroring leaves the input control graph untouched");
    const auto result_graph_digest = mirrored.value().graph.canonical_digest();
    const auto result_control_digest = mirrored.value().control_graph.canonical_digest();
    require(static_cast<bool>(result_graph_digest) &&
            static_cast<bool>(result_control_digest) &&
            mirrored.value().receipt.source_graph_digest == source_graph_digest.value() &&
            mirrored.value().receipt.result_graph_digest == source_graph_digest.value() &&
            mirrored.value().receipt.source_control_graph_digest ==
                source_control_digest.value() &&
            mirrored.value().receipt.result_control_graph_digest ==
                result_control_digest.value() &&
            mirrored.value().receipt.changed_control_count == 2U,
        "receipt binds unchanged rig and changed control graphs");
    const auto repeated = character::mirror_rig_controls(
        graph, controls, joint_pairs, control_pairs,
        character::SymmetryAxis::x, character::SymmetryDirection::left_to_right);
    require(static_cast<bool>(repeated) &&
            repeated.value().receipt.result_control_graph_digest ==
                mirrored.value().receipt.result_control_graph_digest,
        "equal control mirrors reproduce the same result digest");

    const std::vector<character::ControlSymmetryPair> reused{
        {character::ControlId{10U}, character::ControlId{11U}},
        {character::ControlId{11U}, character::ControlId{20U}}};
    require(!character::mirror_rig_controls(
            graph, controls, joint_pairs, reused,
            character::SymmetryAxis::x, character::SymmetryDirection::left_to_right),
        "a control used in two symmetry pairs fails closed");

    character::ControlDefinition outside_left;
    outside_left.id = character::ControlId{30U};
    outside_left.affected_joints = {kinematic::JointId{1U}};
    character::ControlDefinition outside_right;
    outside_right.id = character::ControlId{31U};
    outside_right.affected_joints = {kinematic::JointId{1U}};
    character::ControlGraph outside_controls;
    require(static_cast<bool>(outside_controls.insert(outside_left)) &&
            static_cast<bool>(outside_controls.insert(outside_right)) &&
            static_cast<bool>(outside_controls.validate()),
        "unpaired-joint refusal fixture validates");
    const std::vector<character::ControlSymmetryPair> outside_pair{
        {character::ControlId{30U}, character::ControlId{31U}}};
    require(!character::mirror_rig_controls(
            graph, outside_controls, joint_pairs, outside_pair,
            character::SymmetryAxis::x, character::SymmetryDirection::left_to_right),
        "a source control affecting an unpaired joint fails closed");
}

int main() {
    id_allocation_and_tombstones();
    rational_time_exactness_and_conversion_receipts();
    coordinate_policy_checklist_fail_closed();
    control_graph_cycles_and_definition_failures();
    influence_admission();
    morph_target_bounds();
    constraint_cycles_and_arbitration();
    motion_document_validation();
    bindings_no_orphans();
    manifest_golden_bytes_and_closure();
    invalidation_dag_guarantees();
    receipts_carry_no_wall_time();
    determinism_is_a_hard_gate();
    rig_symmetry_mirrors_reports_divergence_and_round_trips();
    rig_control_symmetry_copies_remap_and_round_trips();
    rig_control_symmetry_copies_and_remaps_references_atomically();
    std::cout << "character foundation tests passed\n";
    return EXIT_SUCCESS;
}
