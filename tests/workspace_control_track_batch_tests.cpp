// Workspace controller admission tests: apply/undo command pairs per domain,
// stale revision refusal, referential validation, session binding, and journal
// verify + replay recovery reaching an identical store digest.

#include <carto/journal/journal.hpp>
#include <carto/workspace/controller.hpp>
#include <carto/workspace/proposals.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            throw TestFailure(std::string("requirement failed: ") + #condition);         \
        }                                                                                \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
            ("cartographer-workspace-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

carto::assets::Sha256Digest tag_digest(const std::string& tag) {
    return carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(tag.data()), tag.size()));
}

carto::assets::Sha256Digest definition_digest_for_test(
    const carto::character::CharacterDefinition& definition) {
    std::string canonical = "{";
    const auto append = [&canonical](std::string_view key, std::string_view value) {
        if (canonical.size() > 1U) canonical.push_back(',');
        canonical.append(key);
        canonical.push_back('=');
        canonical.append(value);
    };
    append("id", std::to_string(definition.id.value));
    append("namespace", definition.project_namespace);
    append("root", std::to_string(definition.root.value));
    std::string refs;
    for (const auto& ref : definition.mesh_refs) {
        if (!refs.empty()) refs.push_back(';');
        refs += std::to_string(ref.asset_id);
        refs.push_back(':');
        refs += ref.content_digest.hex();
    }
    append("mesh_refs", refs);
    append("rig", std::to_string(definition.rig.rig.value) + ":" +
        definition.rig.graph_digest.hex());
    append("control_graph", std::to_string(definition.control_graph.owner.value) + ":" +
        definition.control_graph.digest.hex());
    std::string labels;
    for (const auto& [key, value] : definition.locale_neutral_labels) {
        if (!labels.empty()) labels.push_back(';');
        labels += key;
        labels.push_back('=');
        labels += value;
    }
    append("labels", labels);
    append("coordinate_policy_digest", definition.coordinate_policy_digest.hex());
    canonical.push_back('}');
    return tag_digest(canonical);
}

carto::workspace::CharacterSourceBundle make_bundle(carto::character::CharacterId id) {
    namespace character = carto::character;
    namespace kinematic = carto::kinematic;
    namespace temporal = carto::temporal;

    carto::workspace::CharacterSourceBundle bundle;

    bundle.definition.id = id;
    bundle.definition.project_namespace = "workspace.tests";
    bundle.definition.source_revision = carto::core::Revision{1U};
    bundle.definition.source_digest = tag_digest("definition");
    bundle.definition.root = carto::scene::ObjectId{1U};
    bundle.definition.rig.rig = character::RigId{1U};
    bundle.definition.rig.graph_digest = tag_digest("rig");
    bundle.definition.control_graph.owner = character::RigId{1U};
    bundle.definition.control_graph.digest = tag_digest("control-graph");
    bundle.definition.coordinate_policy_digest = tag_digest("policy");

    kinematic::KinematicNode root;
    root.id = kinematic::JointId{1U};
    root.kind = kinematic::JointKind::fixed;
    root.semantic_type = "root";
    REQUIRE(bundle.graph.insert_node(root));

    bundle.rig.id = character::RigId{1U};
    bundle.rig.kinematic_graph_digest = tag_digest("rig");
    bundle.rig.joint_limits.digest = tag_digest("limits");

    bundle.skin = kinematic::SkinBinding{};
    bundle.skin->mesh_asset = 1U;
    bundle.skin->mesh_digest = tag_digest("mesh");
    bundle.skin->graph_digest = tag_digest("skin-graph");
    bundle.skin->influences.emplace(
        10U, std::vector<kinematic::JointInfluence>{
            kinematic::JointInfluence{kinematic::JointId{1U}, 1.0}});

    bundle.skin_layer = character::SkinWeightLayer{};
    bundle.skin_layer->layer_id = character::RigLayerId{1U};
    bundle.skin_layer->mesh_topology_digest = tag_digest("topology");
    bundle.skin_layer->joint_graph_digest = tag_digest("skin-graph");
    bundle.skin_layer->influenced_vertex_count = 1U;
    bundle.skin_layer->total_influence_count = 1U;
    bundle.skin_layer->provenance_digest = tag_digest("recipe");

    bundle.motion = character::MotionDocument{};
    bundle.motion->id = character::MotionDocumentId{1U};
    bundle.motion->timebase_ticks_per_second = 24U;

    bundle.channel = temporal::TemporalChannel{};
    bundle.channel->id = temporal::ChannelId{1U};
    bundle.channel->channel_name = "workspace.tests.channel";
    bundle.channel->revision = carto::core::Revision{1U};
    bundle.channel->value_kind = temporal::ChannelValueKind::scalar;
    bundle.channel->duration = 10.0;
    bundle.channel->scalar_keyframes.push_back(temporal::ScalarKeyframe{0.0, 0.0});

    const auto graph_digest = bundle.graph.canonical_digest();
    REQUIRE(graph_digest);
    bundle.rig.kinematic_graph_digest = graph_digest.value();
    bundle.definition.rig.graph_digest = graph_digest.value();
    const auto control_digest = bundle.rig.control_graph.canonical_digest();
    REQUIRE(control_digest);
    bundle.definition.control_graph.owner = bundle.rig.id;
    bundle.definition.control_graph.digest = control_digest.value();
    bundle.definition.source_digest = definition_digest_for_test(bundle.definition);

    REQUIRE(bundle.validate());
    return bundle;
}

template <typename Payload>
carto::workspace::WorkspaceCommand make_command(
    carto::workspace::WorkspaceDomain domain,
    carto::character::CharacterId target,
    Payload payload) {
    carto::workspace::WorkspaceCommand command;
    command.domain = domain;
    command.target_character = target;
    command.payload = carto::workspace::WorkspaceCommandPayload{std::move(payload)};
    return command;
}

carto::workspace::CharacterSourceBundle track_bundle() {
    namespace workspace = carto::workspace;
    namespace character = carto::character;
    auto bundle = make_bundle(character::CharacterId{7U});
    character::ControlDefinition control;
    control.id = {9U};
    control.allowed_axes = character::kAxisTranslateX | character::kAxisTranslateY;
    control.axis_minimum[0] = control.axis_minimum[1] = -10.0;
    control.axis_maximum[0] = control.axis_maximum[1] = 10.0;
    REQUIRE(bundle.rig.control_graph.insert(control));
    bundle.definition.control_graph.digest = bundle.rig.control_graph.canonical_digest().value();
    bundle.definition.source_digest = definition_digest_for_test(bundle.definition);
    for (std::uint8_t axis = 0U; axis != 2U; ++axis) {
        workspace::ControlAxisTrack track;
        track.control = {9U}; track.axis = axis;
        track.role = workspace::ControlTrackRole::raw_axis;
        track.space = workspace::ControlTrackSpace::raw_control_value;
        track.source_namespace = "caller.source";
        track.source_revision = carto::core::Revision{4U};
        track.reference_frame = "caller-frame";
        track.rig_digest = tag_digest("asserted-rig"); track.graph_digest = tag_digest("asserted-graph");
        track.curve.id = carto::motion::MotionCurveId{static_cast<std::uint64_t>(axis) + 3U};
        track.curve.timebase_ticks_per_second = 30000U;
        track.curve.interpolation = carto::motion::InterpolationKind::step;
        track.curve.keys = {{{0, 30000U}, 0, 0, 0}, {{2002, 30000U}, 1, 0, 0}};
        track.curve_digest = track.curve.canonical_digest().value();
        bundle.control_axis_tracks.push_back(track);
    }
    REQUIRE(bundle.validate());
    return bundle;
}

carto::workspace::WorkspaceCommand make_batch(const carto::workspace::CharacterSourceStore& store) {
    namespace workspace = carto::workspace;
    const auto* bundle = store.find(carto::character::CharacterId{7U});
    REQUIRE(bundle && bundle->control_axis_tracks.size() == 2U);
    workspace::AnimateEditControlTracks payload;
    payload.expected_source_revision = store.revision();
    payload.expected_store_digest = store.canonical_digest().value();
    payload.edits.push_back({{9U}, 0U, bundle->control_axis_tracks[0].curve_digest,
        {{1001, 30000U}, 0.25, 0, 0}, true});
    payload.edits.push_back({{9U}, 1U, bundle->control_axis_tracks[1].curve_digest,
        {{1001, 30000U}, -0.0, 0, 0}, true});
    return make_command(workspace::WorkspaceDomain::animate, carto::character::CharacterId{7U}, payload);
}

void batch_key_edits_are_atomic_and_replayable() {
    namespace workspace = carto::workspace;
    namespace character = carto::character;
    namespace journal = carto::journal;
    TempDirectory temp;
    const auto path = temp.path() / "track-batch.journal";
    workspace::WorkspaceController controller(path);
    const auto original = track_bundle();
    const character::CharacterId id{7U};
    REQUIRE(controller.store().insert(original));
    const auto command = make_batch(controller.store());
    REQUIRE(command.validate());
    const auto text = command.serialize();
    REQUIRE(text.find("\"schema\":2") != std::string::npos);
    const auto decoded = workspace::WorkspaceCommand::deserialize(text);
    REQUIRE(decoded && decoded.value().serialize() == text);
    const auto& decoded_batch = std::get<workspace::AnimateEditControlTracks>(decoded.value().payload);
    REQUIRE(std::signbit(decoded_batch.edits[1].key.value));
    auto mislabeled = text;
    mislabeled.replace(mislabeled.find("\"schema\":2"), std::string("\"schema\":2").size(), "\"schema\":1");
    REQUIRE(!workspace::WorkspaceCommand::deserialize(mislabeled));
    auto malformed = text;
    const auto insert_at = malformed.find("\"insert\":true");
    REQUIRE(insert_at != std::string::npos);
    malformed.replace(insert_at, std::string("\"insert\":true").size(), "\"insert\":null");
    REQUIRE(!workspace::WorkspaceCommand::deserialize(malformed));
    malformed = text;
    malformed.insert(malformed.size() - 1U, ",\"unknown\":0");
    REQUIRE(!workspace::WorkspaceCommand::deserialize(malformed));
    workspace::AnimateSetRootMotion legacy;
    legacy.expected_source_revision = carto::core::Revision{0U};
    legacy.policy = character::RootMotionPolicy::authored;
    const auto legacy_command = make_command(workspace::WorkspaceDomain::animate, id, legacy);
    const auto legacy_text = legacy_command.serialize();
    REQUIRE(legacy_text.find("\"schema\":1") != std::string::npos);
    const auto decoded_legacy = workspace::WorkspaceCommand::deserialize(legacy_text);
    REQUIRE(decoded_legacy && decoded_legacy.value().serialize() == legacy_text);
    auto mislabeled_legacy = legacy_text;
    const auto legacy_schema = mislabeled_legacy.find("\"schema\":1");
    REQUIRE(legacy_schema != std::string::npos);
    mislabeled_legacy.replace(legacy_schema, std::string("\"schema\":1").size(), "\"schema\":2");
    REQUIRE(!workspace::WorkspaceCommand::deserialize(mislabeled_legacy));

    const auto initial_digest = controller.store().canonical_digest();
    REQUIRE(initial_digest);
    const auto refuse_unchanged = [&](const workspace::WorkspaceCommand& bad) {
        const auto before = controller.journal().read_all();
        REQUIRE(before && before.value().empty());
        REQUIRE(!controller.apply(bad));
        REQUIRE(controller.store().revision() == carto::core::Revision{0U} &&
            controller.store().find(id)->serialize() == original.serialize() &&
            controller.store().canonical_digest().value() == initial_digest.value());
        const auto after = controller.journal().read_all();
        REQUIRE(after && after.value().empty());
    };
    auto bad = command;
    std::get<workspace::AnimateEditControlTracks>(bad.payload).expected_source_revision = carto::core::Revision{1U};
    refuse_unchanged(bad);
    bad = command;
    std::get<workspace::AnimateEditControlTracks>(bad.payload).expected_store_digest = tag_digest("stale-store");
    refuse_unchanged(bad);
    bad = command;
    std::get<workspace::AnimateEditControlTracks>(bad.payload).edits[1].expected_curve_digest = tag_digest("stale-second-curve");
    refuse_unchanged(bad);
    bad = command;
    std::get<workspace::AnimateEditControlTracks>(bad.payload).edits[1].key.time = {1001, 24000U};
    refuse_unchanged(bad);
    bad = command;
    std::get<workspace::AnimateEditControlTracks>(bad.payload).edits[1].key.time = {2002, 30000U};
    refuse_unchanged(bad);
    bad = command;
    std::get<workspace::AnimateEditControlTracks>(bad.payload).edits[1].axis = 0U;
    refuse_unchanged(bad);
    bad = command;
    auto& erase_mismatch = std::get<workspace::AnimateEditControlTracks>(bad.payload).edits[1];
    erase_mismatch.insert = false;
    erase_mismatch.key = {{0, 30000U}, -0.0, 0, 0};
    refuse_unchanged(bad); // Stored value is +0.0: exact erase must not equate sign bits.

    // A same-revision out-of-band edit must be caught by the whole-store pin.
    auto* externally_mutated = controller.store().find(id);
    externally_mutated->control_axis_tracks[1].curve.keys[1].value = 0.75;
    externally_mutated->control_axis_tracks[1].curve_digest =
        externally_mutated->control_axis_tracks[1].curve.canonical_digest().value();
    const auto external_bytes = externally_mutated->serialize();
    REQUIRE(controller.store().revision() == carto::core::Revision{0U});
    REQUIRE(!controller.apply(command));
    REQUIRE(controller.store().find(id)->serialize() == external_bytes &&
        controller.store().revision() == carto::core::Revision{0U} && controller.journal().read_all().value().empty());
    *controller.store().find(id) = original;
    REQUIRE(controller.store().canonical_digest().value() == initial_digest.value());

    REQUIRE(controller.apply(command));
    REQUIRE(controller.store().revision() == carto::core::Revision{1U});
    auto* applied = controller.store().find(id);
    REQUIRE(applied && applied->control_axis_tracks[0].curve.keys.size() == 3U &&
        applied->control_axis_tracks[1].curve.keys.size() == 3U &&
        applied->control_axis_tracks[0].curve.sample({1000, 30000U}).value() == 0 &&
        applied->control_axis_tracks[0].curve.sample({1001, 30000U}).value() == 0.25 &&
        std::signbit(applied->control_axis_tracks[1].curve.keys[1].value));
    const auto after_apply = applied->serialize();
    const auto after_apply_bundle = *applied;
    REQUIRE(controller.journal().read_all().value().size() == 1U);
    // A direct mutation at the same store revision must not be silently
    // overwritten by a previously prepared inverse.
    applied->control_axis_tracks[0].curve.keys[0].value = 0.5;
    applied->control_axis_tracks[0].curve_digest =
        applied->control_axis_tracks[0].curve.canonical_digest().value();
    const auto drifted_bytes = applied->serialize();
    const auto drifted_undo = controller.undo();
    REQUIRE(!drifted_undo && drifted_undo.error().code == carto::core::ErrorCode::stale_data &&
        controller.store().revision() == carto::core::Revision{1U} &&
        controller.store().find(id)->serialize() == drifted_bytes &&
        controller.journal().read_all().value().size() == 1U);
    *controller.store().find(id) = after_apply_bundle;
    journal::JournalLimits limits; limits.max_payload_bytes = 1U;
    controller.journal() = journal::Journal(path, limits);
    const auto failed_undo = controller.undo();
    REQUIRE(!failed_undo && failed_undo.error().message.find("journal payload exceeds") != std::string::npos &&
        controller.store().revision() == carto::core::Revision{1U} && controller.store().find(id)->serialize() == after_apply);
    controller.journal() = journal::Journal(path);
    REQUIRE(controller.journal().read_all().value().size() == 1U);

    // Another command and its undo leave content unchanged but advance revision;
    // the older batch inverse must remain valid under LIFO history semantics.
    legacy.expected_source_revision = carto::core::Revision{1U};
    REQUIRE(controller.apply(make_command(workspace::WorkspaceDomain::animate, id, legacy)));
    REQUIRE(controller.undo());
    REQUIRE(controller.store().find(id)->serialize() == after_apply);
    REQUIRE(controller.undo());
    REQUIRE(controller.store().find(id)->serialize() == original.serialize());
    REQUIRE(controller.store().revision() == carto::core::Revision{4U});
    const auto entries = controller.journal().read_all();
    REQUIRE(entries && entries.value().size() == 4U && controller.journal().verify());
    workspace::WorkspaceController recovery(temp.path() / "batch-replay.journal");
    REQUIRE(recovery.store().insert(original));
    const auto replay = controller.journal().replay_from(carto::core::Revision{},
        [&](const journal::JournalEntry& entry) {
            const std::string payload(reinterpret_cast<const char*>(entry.payload.data()), entry.payload.size());
            const auto decoded_entry = workspace::WorkspaceCommand::deserialize(payload);
            if (!decoded_entry) return carto::core::Result<void>::failure(decoded_entry.error());
            const auto applied_entry = recovery.apply(decoded_entry.value());
            if (!applied_entry) return carto::core::Result<void>::failure(applied_entry.error());
            return carto::core::Result<void>::success();
        });
    const auto final_digest = controller.store().canonical_digest();
    const auto replay_digest = recovery.store().canonical_digest();
    REQUIRE(replay && final_digest && replay_digest && final_digest.value() == replay_digest.value() &&
        recovery.store().revision() == controller.store().revision());
}
void batch_erase_restores_signed_zero_key_bits() {
    namespace workspace = carto::workspace;
    namespace character = carto::character;
    namespace journal = carto::journal;
    TempDirectory temp;
    const character::CharacterId id{7U};
    auto original = track_bundle();
    auto& original_track = original.control_axis_tracks[0];
    auto& existing = original_track.curve.keys.front();
    existing.value = -0.0;
    existing.tangent_in = -0.0;
    existing.tangent_out = -0.0;
    original_track.curve_digest = original_track.curve.canonical_digest().value();
    REQUIRE(original.validate());
    const auto key_bits = [](const carto::motion::CurveKey& key) {
        return std::array<std::uint64_t, 3>{std::bit_cast<std::uint64_t>(key.value),
            std::bit_cast<std::uint64_t>(key.tangent_in), std::bit_cast<std::uint64_t>(key.tangent_out)};
    };
    const auto original_bits = key_bits(existing);
    REQUIRE(std::signbit(existing.value) && std::signbit(existing.tangent_in) &&
        std::signbit(existing.tangent_out));
    workspace::WorkspaceController controller(temp.path() / "erase-signed-zero.journal");
    REQUIRE(controller.store().insert(original));
    workspace::AnimateEditControlTracks payload;
    payload.expected_source_revision = controller.store().revision();
    payload.expected_store_digest = controller.store().canonical_digest().value();
    payload.edits.push_back({original_track.control, original_track.axis,
        original_track.curve_digest, existing, false});
    const auto command = make_command(workspace::WorkspaceDomain::animate, id, payload);
    REQUIRE(controller.apply(command));
    REQUIRE(controller.store().find(id)->control_axis_tracks[0].curve.keys.size() == 1U);
    REQUIRE(controller.undo());
    const auto* restored = controller.store().find(id);
    REQUIRE(restored && restored->control_axis_tracks[0].curve.keys.size() == 2U &&
        key_bits(restored->control_axis_tracks[0].curve.keys.front()) == original_bits &&
        restored->serialize() == original.serialize());
    const auto entries = controller.journal().read_all();
    REQUIRE(entries && entries.value().size() == 2U && controller.journal().verify());
    workspace::WorkspaceController recovery(temp.path() / "erase-signed-zero-replay.journal");
    REQUIRE(recovery.store().insert(original));
    const auto replay = controller.journal().replay_from(carto::core::Revision{},
        [&](const journal::JournalEntry& entry) {
            const std::string raw(reinterpret_cast<const char*>(entry.payload.data()), entry.payload.size());
            const auto decoded = workspace::WorkspaceCommand::deserialize(raw);
            if (!decoded) return carto::core::Result<void>::failure(decoded.error());
            const auto applied = recovery.apply(decoded.value());
            if (!applied) return carto::core::Result<void>::failure(applied.error());
            return carto::core::Result<void>::success();
        });
    const auto* replayed = recovery.store().find(id);
    REQUIRE(replay && replayed && key_bits(replayed->control_axis_tracks[0].curve.keys.front()) == original_bits &&
        replayed->serialize() == original.serialize() && recovery.store().revision() == controller.store().revision());
}

} // namespace

int main() {
    try { batch_key_edits_are_atomic_and_replayable(); batch_erase_restores_signed_zero_key_bits(); }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
    return 0;
}
