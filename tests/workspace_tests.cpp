// Workspace controller admission tests: apply/undo command pairs per domain,
// stale revision refusal, referential validation, session binding, and journal
// verify + replay recovery reaching an identical store digest.

#include <carto/journal/journal.hpp>
#include <carto/workspace/controller.hpp>
#include <carto/workspace/proposals.hpp>

#include <chrono>
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

carto::workspace::RigAddJoint add_joint_command_payload(
    carto::core::Revision revision,
    carto::kinematic::JointId joint,
    std::optional<carto::kinematic::JointId> parent) {
    carto::workspace::RigAddJoint payload;
    payload.expected_source_revision = revision;
    payload.joint = joint;
    payload.parent = parent;
    payload.kind = carto::kinematic::JointKind::fixed;
    payload.semantic_type = "test.joint";
    return payload;
}

void workspace_applies_and_undoes_rig_commands() {
    namespace workspace = carto::workspace;
    TempDirectory temp;
    workspace::WorkspaceController controller(temp.path() / "rig.journal");
    const auto original = make_bundle(carto::character::CharacterId{7U});
    REQUIRE(controller.store().insert(original));

    auto add_joint = make_command(
        workspace::WorkspaceDomain::rig, carto::character::CharacterId{7U},
        add_joint_command_payload(carto::core::Revision{0U},
                                  carto::kinematic::JointId{2U},
                                  carto::kinematic::JointId{1U}));
    const auto applied = controller.apply(add_joint);
    REQUIRE(applied);
    REQUIRE(applied.value().applied_revision == carto::core::Revision{1U});
    REQUIRE(!applied.value().journal_entry_digest.is_zero());
    REQUIRE(controller.store().revision() == carto::core::Revision{1U});
    const auto* bundle = controller.store().find(carto::character::CharacterId{7U});
    REQUIRE(bundle != nullptr);
    REQUIRE(bundle->graph.find(carto::kinematic::JointId{2U}) != nullptr);
    const auto graph_digest = bundle->graph.canonical_digest();
    REQUIRE(graph_digest);
    REQUIRE(bundle->rig.kinematic_graph_digest == graph_digest.value());
    REQUIRE(bundle->definition.rig.graph_digest == graph_digest.value());
    REQUIRE(bundle->definition.source_digest ==
        definition_digest_for_test(bundle->definition));

    const auto undone = controller.undo();
    REQUIRE(undone);
    REQUIRE(undone.value().applied_revision == carto::core::Revision{2U});
    bundle = controller.store().find(carto::character::CharacterId{7U});
    REQUIRE(bundle != nullptr);
    REQUIRE(bundle->graph.find(carto::kinematic::JointId{2U}) == nullptr);
    REQUIRE(bundle->serialize() == original.serialize());
    REQUIRE(!controller.undo());
}

void workspace_applies_and_undoes_skin_commands() {
    namespace workspace = carto::workspace;
    namespace character = carto::character;
    namespace kinematic = carto::kinematic;
    TempDirectory temp;
    workspace::WorkspaceController controller(temp.path() / "skin.journal");
    const auto original = make_bundle(character::CharacterId{7U});
    REQUIRE(controller.store().insert(original));

    workspace::SkinPaintStamp absent_vertex;
    absent_vertex.expected_source_revision = carto::core::Revision{0U};
    absent_vertex.vertex = 20U;
    absent_vertex.influences.push_back(
        kinematic::JointInfluence{kinematic::JointId{1U}, 1.0});
    const auto rejected_absent_vertex = controller.apply(make_command(
        workspace::WorkspaceDomain::skin, character::CharacterId{7U}, absent_vertex));
    REQUIRE(!rejected_absent_vertex);
    REQUIRE(rejected_absent_vertex.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(controller.store().revision() == carto::core::Revision{0U});
    const auto unchanged_journal = controller.journal().read_all();
    REQUIRE(unchanged_journal);
    REQUIRE(unchanged_journal.value().empty());

    workspace::SkinPaintStamp stamp;
    stamp.expected_source_revision = carto::core::Revision{0U};
    stamp.vertex = 10U;
    stamp.influences.push_back(
        kinematic::JointInfluence{kinematic::JointId{1U}, 1.0});
    const auto stamped = controller.apply(make_command(
        workspace::WorkspaceDomain::skin, character::CharacterId{7U}, stamp));
    REQUIRE(stamped);
    const auto* bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->skin->influences.size() == 1U);
    REQUIRE(bundle->skin_layer->influenced_vertex_count == 1U);
    REQUIRE(bundle->skin_layer->total_influence_count == 1U);

    REQUIRE(controller.undo());
    bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->skin->influences.size() == 1U);
    REQUIRE(bundle->skin->influences.contains(10U));
    REQUIRE(bundle->serialize() == original.serialize());

    workspace::SkinSetWeightLock lock;
    lock.expected_source_revision = carto::core::Revision{2U};
    lock.vertex = 10U;
    lock.locked = true;
    const auto locked = controller.apply(make_command(
        workspace::WorkspaceDomain::skin, character::CharacterId{7U}, lock));
    REQUIRE(locked);
    bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->weight_locks.size() == 1U);
    REQUIRE(bundle->weight_locks.front() == 10U);
    REQUIRE(controller.undo());
    bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->weight_locks.empty());

    workspace::SkinRebindLayer rebind;
    rebind.expected_source_revision = carto::core::Revision{4U};
    const auto* current = controller.store().find(character::CharacterId{7U});
    const auto digest = current->graph.canonical_digest();
    REQUIRE(digest);
    rebind.joint_graph_digest = digest.value();
    const auto rebound = controller.apply(make_command(
        workspace::WorkspaceDomain::skin, character::CharacterId{7U}, rebind));
    REQUIRE(rebound);
    current = controller.store().find(character::CharacterId{7U});
    REQUIRE(current->skin_layer->joint_graph_digest == digest.value());
    REQUIRE(current->skin_layer->staleness == character::WeightStaleness::fresh);
    REQUIRE(controller.undo());
    current = controller.store().find(character::CharacterId{7U});
    REQUIRE(current->skin_layer->joint_graph_digest == tag_digest("skin-graph"));
}

void workspace_applies_and_undoes_animate_commands() {
    namespace workspace = carto::workspace;
    namespace character = carto::character;
    TempDirectory temp;
    workspace::WorkspaceController controller(temp.path() / "animate.journal");
    const auto original = make_bundle(character::CharacterId{7U});
    REQUIRE(controller.store().insert(original));

    workspace::AnimateSetKey set_key;
    set_key.expected_source_revision = carto::core::Revision{0U};
    set_key.channel = carto::temporal::ChannelId{1U};
    set_key.time = character::RationalTime{12, 24U};
    set_key.value = 3.0;
    const auto keyed = controller.apply(make_command(
        workspace::WorkspaceDomain::animate, character::CharacterId{7U}, set_key));
    REQUIRE(keyed);
    const auto* bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->channel->scalar_keyframes.size() == 2U);
    REQUIRE(bundle->channel->scalar_keyframes[1U].time == 0.5);
    REQUIRE(bundle->channel->scalar_keyframes[1U].value == 3.0);
    REQUIRE(controller.undo());
    bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->channel->scalar_keyframes.size() == 1U);
    REQUIRE(bundle->serialize() == original.serialize());

    // Set again so the removal below never empties the channel: a temporal
    // channel must always carry at least one keyframe.
    set_key.expected_source_revision = carto::core::Revision{2U};
    REQUIRE(controller.apply(make_command(
        workspace::WorkspaceDomain::animate, character::CharacterId{7U}, set_key)));
    workspace::AnimateRemoveKey remove_key;
    remove_key.expected_source_revision = carto::core::Revision{3U};
    remove_key.channel = carto::temporal::ChannelId{1U};
    remove_key.time = character::RationalTime{0, 24U};
    const auto removed = controller.apply(make_command(
        workspace::WorkspaceDomain::animate, character::CharacterId{7U}, remove_key));
    REQUIRE(removed);
    bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->channel->scalar_keyframes.size() == 1U);
    REQUIRE(bundle->channel->scalar_keyframes.front().time == 0.5);
    REQUIRE(controller.undo());
    bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->channel->scalar_keyframes.size() == 2U);
    REQUIRE(bundle->channel->scalar_keyframes.front().time == 0.0);
    REQUIRE(controller.undo());
    bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->channel->scalar_keyframes.size() == 1U);
    REQUIRE(bundle->channel->scalar_keyframes.front().time == 0.0);

    workspace::AnimateSetRootMotion root_motion;
    root_motion.expected_source_revision = carto::core::Revision{6U};
    root_motion.policy = character::RootMotionPolicy::authored;
    const auto motion = controller.apply(make_command(
        workspace::WorkspaceDomain::animate, character::CharacterId{7U}, root_motion));
    REQUIRE(motion);
    bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->motion->root_motion == character::RootMotionPolicy::authored);
    REQUIRE(controller.undo());
    bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->motion->root_motion == character::RootMotionPolicy::none);
}

void workspace_refuses_stale_revisions_without_mutation() {
    namespace workspace = carto::workspace;
    TempDirectory temp;
    workspace::WorkspaceController controller(temp.path() / "stale.journal");
    const auto original = make_bundle(carto::character::CharacterId{7U});
    REQUIRE(controller.store().insert(original));
    REQUIRE(controller.apply(make_command(
        workspace::WorkspaceDomain::rig, carto::character::CharacterId{7U},
        add_joint_command_payload(carto::core::Revision{0U},
                                  carto::kinematic::JointId{2U},
                                  carto::kinematic::JointId{1U}))));

    // The same command is now stale: the store moved to revision 1.
    const auto stale_again = controller.apply(make_command(
        workspace::WorkspaceDomain::rig, carto::character::CharacterId{7U},
        add_joint_command_payload(carto::core::Revision{0U},
                                  carto::kinematic::JointId{3U},
                                  carto::kinematic::JointId{1U})));
    REQUIRE(!stale_again);
    REQUIRE(stale_again.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(controller.store().revision() == carto::core::Revision{1U});

    auto far_future = make_command(
        workspace::WorkspaceDomain::rig, carto::character::CharacterId{7U},
        add_joint_command_payload(carto::core::Revision{9U},
                                  carto::kinematic::JointId{3U},
                                  carto::kinematic::JointId{1U}));
    const auto stale_future = controller.apply(far_future);
    REQUIRE(!stale_future);
    REQUIRE(stale_future.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(controller.store().revision() == carto::core::Revision{1U});

    const auto entries = controller.journal().read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 1U);
    const auto* bundle = controller.store().find(carto::character::CharacterId{7U});
    REQUIRE(bundle->graph.find(carto::kinematic::JointId{3U}) == nullptr);
}

void workspace_refuses_referential_violations() {
    namespace workspace = carto::workspace;
    namespace character = carto::character;
    namespace kinematic = carto::kinematic;
    TempDirectory temp;
    workspace::WorkspaceController controller(temp.path() / "referential.journal");
    REQUIRE(controller.store().insert(make_bundle(character::CharacterId{7U})));

    // Unknown parent joint.
    const auto missing_parent = controller.apply(make_command(
        workspace::WorkspaceDomain::rig, character::CharacterId{7U},
        add_joint_command_payload(carto::core::Revision{0U},
                                  kinematic::JointId{2U}, kinematic::JointId{99U})));
    REQUIRE(!missing_parent);
    REQUIRE(missing_parent.error().code == carto::core::ErrorCode::not_found);

    // Removing a joint that still carries children.
    REQUIRE(controller.apply(make_command(
        workspace::WorkspaceDomain::rig, character::CharacterId{7U},
        add_joint_command_payload(carto::core::Revision{0U},
                                  kinematic::JointId{2U}, kinematic::JointId{1U}))));
    REQUIRE(controller.apply(make_command(
        workspace::WorkspaceDomain::rig, character::CharacterId{7U},
        add_joint_command_payload(carto::core::Revision{1U},
                                  kinematic::JointId{3U}, kinematic::JointId{2U}))));
    workspace::RigRemoveJoint remove_childed;
    remove_childed.expected_source_revision = carto::core::Revision{2U};
    remove_childed.joint = kinematic::JointId{2U};
    const auto childed = controller.apply(make_command(
        workspace::WorkspaceDomain::rig, character::CharacterId{7U}, remove_childed));
    REQUIRE(!childed);
    REQUIRE(childed.error().code == carto::core::ErrorCode::validation_failed);

    // A control may not affect a joint outside the bundle graph.
    workspace::RigAddControl control;
    control.expected_source_revision = carto::core::Revision{2U};
    control.control = character::ControlId{5U};
    control.affected_joints.push_back(kinematic::JointId{77U});
    const auto bad_control = controller.apply(make_command(
        workspace::WorkspaceDomain::rig, character::CharacterId{7U}, control));
    REQUIRE(!bad_control);
    REQUIRE(bad_control.error().code == carto::core::ErrorCode::not_found);

    // The root joint is still referenced by a skin influence and refuses
    // structural removal.
    workspace::RigRemoveJoint remove_referenced;
    remove_referenced.expected_source_revision = carto::core::Revision{2U};
    remove_referenced.joint = kinematic::JointId{1U};
    const auto referenced = controller.apply(make_command(
        workspace::WorkspaceDomain::rig, character::CharacterId{7U}, remove_referenced));
    REQUIRE(!referenced);
    REQUIRE(referenced.error().code == carto::core::ErrorCode::validation_failed);

    // Paint stamps must bind to joints that exist in the graph.
    workspace::SkinPaintStamp bad_stamp;
    bad_stamp.expected_source_revision = carto::core::Revision{2U};
    bad_stamp.vertex = 30U;
    bad_stamp.influences.push_back(kinematic::JointInfluence{kinematic::JointId{55U}, 1.0});
    const auto bad_paint = controller.apply(make_command(
        workspace::WorkspaceDomain::skin, character::CharacterId{7U}, bad_stamp));
    REQUIRE(!bad_paint);
    REQUIRE(bad_paint.error().code == carto::core::ErrorCode::not_found);

    // Keys address the bundle channel, inside its duration.
    workspace::AnimateSetKey wrong_channel;
    wrong_channel.expected_source_revision = carto::core::Revision{2U};
    wrong_channel.channel = carto::temporal::ChannelId{999U};
    wrong_channel.time = character::RationalTime{1, 24U};
    const auto missing_channel = controller.apply(make_command(
        workspace::WorkspaceDomain::animate, character::CharacterId{7U}, wrong_channel));
    REQUIRE(!missing_channel);
    REQUIRE(missing_channel.error().code == carto::core::ErrorCode::not_found);

    workspace::AnimateSetKey outside_duration;
    outside_duration.expected_source_revision = carto::core::Revision{2U};
    outside_duration.channel = carto::temporal::ChannelId{1U};
    outside_duration.time = character::RationalTime{24 * 60, 24U};
    const auto outside = controller.apply(make_command(
        workspace::WorkspaceDomain::animate, character::CharacterId{7U}, outside_duration));
    REQUIRE(!outside);
    REQUIRE(outside.error().code == carto::core::ErrorCode::validation_failed);

    // A skin rebind must present the current joint graph digest.
    workspace::SkinRebindLayer stale_rebind;
    stale_rebind.expected_source_revision = carto::core::Revision{2U};
    stale_rebind.joint_graph_digest = tag_digest("stale-graph");
    const auto stale_layer = controller.apply(make_command(
        workspace::WorkspaceDomain::skin, character::CharacterId{7U}, stale_rebind));
    REQUIRE(!stale_layer);
    REQUIRE(stale_layer.error().code == carto::core::ErrorCode::stale_data);

    // None of the refusals touched source or the journal.
    REQUIRE(controller.store().revision() == carto::core::Revision{2U});
    const auto entries = controller.journal().read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 2U);
}

void workspace_refuses_session_domain_mismatch() {
    namespace workspace = carto::workspace;
    namespace character = carto::character;
    namespace kinematic = carto::kinematic;
    TempDirectory temp;
    workspace::WorkspaceController controller(temp.path() / "session.journal");
    REQUIRE(controller.store().insert(make_bundle(character::CharacterId{7U})));
    REQUIRE(controller.store().insert(make_bundle(character::CharacterId{8U})));

    // The rig session binds to character 7 on first contact.
    REQUIRE(controller.apply(make_command(
        workspace::WorkspaceDomain::rig, character::CharacterId{7U},
        add_joint_command_payload(carto::core::Revision{0U},
                                  kinematic::JointId{2U}, kinematic::JointId{1U}))));
    REQUIRE(controller.session(workspace::WorkspaceDomain::rig).target_character ==
            character::CharacterId{7U});

    // A rig command against another character refuses: the session is a
    // projection of one target, never a wildcard over the store.
    const auto other_target = controller.apply(make_command(
        workspace::WorkspaceDomain::rig, character::CharacterId{8U},
        add_joint_command_payload(carto::core::Revision{1U},
                                  kinematic::JointId{2U}, kinematic::JointId{1U})));
    REQUIRE(!other_target);
    REQUIRE(other_target.error().code == carto::core::ErrorCode::invalid_argument);

    // A rig payload presented under the skin domain refuses before the
    // revision gate or any store access.
    auto wrong_domain = make_command(
        workspace::WorkspaceDomain::skin, character::CharacterId{7U},
        add_joint_command_payload(carto::core::Revision{1U},
                                  kinematic::JointId{4U}, kinematic::JointId{1U}));
    const auto mismatched = controller.apply(wrong_domain);
    REQUIRE(!mismatched);
    REQUIRE(mismatched.error().code == carto::core::ErrorCode::invalid_argument);
}

void workspace_journals_commands_and_replays_to_identical_digest() {
    namespace workspace = carto::workspace;
    namespace character = carto::character;
    namespace kinematic = carto::kinematic;
    TempDirectory temp;
    workspace::WorkspaceController controller(temp.path() / "history.journal");
    REQUIRE(controller.store().insert(make_bundle(character::CharacterId{7U})));

    REQUIRE(controller.apply(make_command(
        workspace::WorkspaceDomain::rig, character::CharacterId{7U},
        add_joint_command_payload(carto::core::Revision{0U},
                                  kinematic::JointId{2U}, kinematic::JointId{1U}))));
    workspace::SkinPaintStamp stamp;
    stamp.expected_source_revision = carto::core::Revision{1U};
    stamp.vertex = 10U;
    stamp.influences.push_back(kinematic::JointInfluence{kinematic::JointId{1U}, 1.0});
    REQUIRE(controller.apply(make_command(
        workspace::WorkspaceDomain::skin, character::CharacterId{7U}, stamp)));
    workspace::AnimateSetKey set_key;
    set_key.expected_source_revision = carto::core::Revision{2U};
    set_key.channel = carto::temporal::ChannelId{1U};
    set_key.time = character::RationalTime{12, 24U};
    set_key.value = 3.0;
    REQUIRE(controller.apply(make_command(
        workspace::WorkspaceDomain::animate, character::CharacterId{7U}, set_key)));
    // Undo is itself a forward journal entry.
    REQUIRE(controller.undo());
    workspace::AnimateSetRootMotion root_motion;
    root_motion.expected_source_revision = carto::core::Revision{4U};
    root_motion.policy = character::RootMotionPolicy::authored;
    REQUIRE(controller.apply(make_command(
        workspace::WorkspaceDomain::animate, character::CharacterId{7U}, root_motion)));

    REQUIRE(controller.journal().verify());
    const auto entries = controller.journal().read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 5U);

    // Recovery: replay every journaled command from the empty revision into
    // a fresh controller holding the same initial snapshot.
    workspace::WorkspaceController recovery(temp.path() / "recovery.journal");
    REQUIRE(recovery.store().insert(make_bundle(character::CharacterId{7U})));
    const auto replayed = controller.journal().replay_from(
        carto::core::Revision{}, [&](const carto::journal::JournalEntry& entry) {
            const std::string payload(
                reinterpret_cast<const char*>(entry.payload.data()), entry.payload.size());
            const auto command = workspace::WorkspaceCommand::deserialize(payload);
            if (!command) return carto::core::Result<void>::failure(command.error());
            return recovery.apply(command.value())
                .has_value()
                ? carto::core::Result<void>::success()
                : carto::core::Result<void>::failure(
                      carto::core::Diagnostic(carto::core::ErrorCode::invalid_state,
                                              "workspace replay command was refused"));
        });
    REQUIRE(replayed);

    const auto live_digest = controller.store().canonical_digest();
    const auto recovered_digest = recovery.store().canonical_digest();
    REQUIRE(live_digest);
    REQUIRE(recovered_digest);
    REQUIRE(live_digest.value() == recovered_digest.value());
    REQUIRE(recovery.store().revision() == controller.store().revision());
}

// ---- proposal layer tests (ch20) -------------------------------------------

carto::workspace::UncertaintyDiagnostic uncertainty_for_test(
    std::string aspect, double confidence, std::string note) {
    carto::workspace::UncertaintyDiagnostic entry;
    entry.aspect = std::move(aspect);
    entry.confidence = confidence;
    entry.note = std::move(note);
    return entry;
}

// Accept flow bookkeeping: every materialized operation is applied through
// the controller, so its expected revision is restamped to the revision
// current at apply time, exactly like a manual author.
void restamp_expected(carto::workspace::WorkspaceCommand& command,
                      carto::core::Revision revision) {
    std::visit(
        [revision](auto& payload) { payload.expected_source_revision = revision; },
        command.payload);
}

carto::workspace::ProposalReview rejected_review_for_test(
    const carto::workspace::AuthoringProposal& envelope,
    const carto::assets::Sha256Digest& store_digest) {
    carto::workspace::ProposalReview review;
    review.proposal_id = envelope.proposal_id;
    const auto digest = envelope.canonical_digest();
    REQUIRE(digest);
    review.proposal_digest = digest.value();
    review.decision = carto::workspace::ProposalDecision::rejected;
    review.reviewer = "artist";
    review.reason = "not now";
    review.review_revision = envelope.source_revision;
    review.store_digest_before = store_digest;
    review.store_digest_after = store_digest;
    return review;
}

void proposal_envelope_round_trips_through_canonical_codec() {
    namespace workspace = carto::workspace;
    TempDirectory temp;
    workspace::WorkspaceController controller(temp.path() / "proposal.journal");
    REQUIRE(controller.store().insert(make_bundle(carto::character::CharacterId{7U})));

    workspace::local::RigChainOptions options;
    options.joint_count = 3U;
    const auto typed = workspace::local::propose_rig_chain(
        controller.store(), carto::character::CharacterId{7U}, options);
    REQUIRE(typed);
    const auto envelope = workspace::build_rig_proposal(
        controller.store(), carto::character::CharacterId{7U}, typed.value(),
        tag_digest("objective"), "carto-local-tests");
    REQUIRE(envelope);
    REQUIRE(envelope.value().validate());
    REQUIRE(envelope.value().operation_sequence.size() == 3U);
    REQUIRE(envelope.value().uncertainty_by_operation.size() == 3U);
    REQUIRE(envelope.value().explicit_target_ids.size() == 3U);
    REQUIRE(envelope.value().permitted_effect_kinds.size() == 1U);
    REQUIRE(envelope.value().permitted_effect_kinds.front() ==
            workspace::ProposalEffect::rig_structure);

    const std::string text = envelope.value().serialize();
    const auto decoded = workspace::AuthoringProposal::deserialize(text);
    REQUIRE(decoded);
    REQUIRE(decoded.value().serialize() == text);
    const auto digest_a = envelope.value().canonical_digest();
    const auto digest_b = decoded.value().canonical_digest();
    REQUIRE(digest_a);
    REQUIRE(digest_b);
    REQUIRE(digest_a.value() == digest_b.value());

    // Re-serialization is byte-identical: the codec is canonical.
    const auto decoded_twice = workspace::AuthoringProposal::deserialize(
        decoded.value().serialize());
    REQUIRE(decoded_twice);
    REQUIRE(decoded_twice.value().serialize() == text);

    // The review record round-trips through its own canonical codec.
    const auto store_digest = controller.store().canonical_digest();
    REQUIRE(store_digest);
    workspace::ProposalReview review = rejected_review_for_test(
        envelope.value(), store_digest.value());
    REQUIRE(review.validate());
    const std::string review_text = review.serialize();
    const auto decoded_review = workspace::ProposalReview::deserialize(review_text);
    REQUIRE(decoded_review);
    REQUIRE(decoded_review.value().serialize() == review_text);
}

void proposal_deserialize_rejects_hostile_input() {
    namespace workspace = carto::workspace;
    TempDirectory temp;
    workspace::WorkspaceController controller(temp.path() / "hostile.journal");
    REQUIRE(controller.store().insert(make_bundle(carto::character::CharacterId{7U})));

    workspace::local::RigChainOptions options;
    options.joint_count = 2U;
    const auto typed = workspace::local::propose_rig_chain(
        controller.store(), carto::character::CharacterId{7U}, options);
    REQUIRE(typed);
    const auto envelope = workspace::build_rig_proposal(
        controller.store(), carto::character::CharacterId{7U}, typed.value(),
        tag_digest("objective"), "carto-local-tests");
    REQUIRE(envelope);
    const std::string text = envelope.value().serialize();

    REQUIRE(!workspace::AuthoringProposal::deserialize(""));
    REQUIRE(!workspace::AuthoringProposal::deserialize("{}"));
    REQUIRE(!workspace::AuthoringProposal::deserialize("not json at all"));
    // Oversized input is refused at the size gate before any parsing.
    const std::string oversized(workspace::AuthoringProposal::kMaxSerializedBytes + 1U, ' ');
    REQUIRE(!workspace::AuthoringProposal::deserialize(oversized));

    // Wrong schema version.
    std::string tampered = text;
    const auto schema_pos = tampered.find("\"schema\":1");
    REQUIRE(schema_pos != std::string::npos);
    tampered.replace(schema_pos, std::string("\"schema\":1").size(), "\"schema\":9");
    REQUIRE(!workspace::AuthoringProposal::deserialize(tampered));

    // Unknown proposal kind.
    tampered = text;
    const auto kind_pos = tampered.find("\"kind\":\"rig\"");
    REQUIRE(kind_pos != std::string::npos);
    tampered.replace(kind_pos, std::string("\"kind\":\"rig\"").size(),
                     "\"kind\":\"warp\"");
    REQUIRE(!workspace::AuthoringProposal::deserialize(tampered));

    // Truncated document.
    REQUIRE(!workspace::AuthoringProposal::deserialize(
        std::string_view(text.data(), text.size() / 2U)));

    // A valid envelope that admits no matching effect fails validation.
    workspace::AuthoringProposal wrong_effects = envelope.value();
    wrong_effects.permitted_effect_kinds.clear();
    wrong_effects.permitted_effect_kinds.push_back(workspace::ProposalEffect::rig_rest);
    REQUIRE(!wrong_effects.validate());
    REQUIRE(!workspace::AuthoringProposal::deserialize(wrong_effects.serialize()));

    // Uncertainty must mirror the operation sequence exactly.
    workspace::AuthoringProposal dropped_uncertainty = envelope.value();
    dropped_uncertainty.uncertainty_by_operation.pop_back();
    REQUIRE(!dropped_uncertainty.validate());

    // Referenced assets must be sorted and unique.
    workspace::AuthoringProposal unsorted_assets = envelope.value();
    carto::assets::Sha256Digest higher{};
    higher.bytes.back() = 0x02U;
    carto::assets::Sha256Digest lower{};
    lower.bytes.back() = 0x01U;
    unsorted_assets.referenced_assets.push_back(higher);
    unsorted_assets.referenced_assets.push_back(lower);
    REQUIRE(!unsorted_assets.validate());

    // Review codec refuses a rejection that records a source change.
    const auto store_digest = controller.store().canonical_digest();
    REQUIRE(store_digest);
    workspace::ProposalReview bad_review = rejected_review_for_test(
        envelope.value(), store_digest.value());
    bad_review.store_digest_after = tag_digest("changed");
    REQUIRE(!bad_review.validate());
    workspace::ProposalReview wrong_order = bad_review;
    wrong_order.decision = workspace::ProposalDecision::accepted;
    wrong_order.store_digest_after = store_digest.value();
    workspace::ProposalApplication application;
    application.operation_index = 5U;
    application.kind = workspace::WorkspaceCommandKind::rig_add_joint;
    application.revision_before = carto::core::Revision{0U};
    application.revision_after = carto::core::Revision{1U};
    application.command_digest = tag_digest("command");
    wrong_order.applications.push_back(application);
    application.operation_index = 2U;
    wrong_order.applications.push_back(application);
    REQUIRE(!wrong_order.validate());
}

void proposal_validators_enforce_bounds_and_recipe_binding() {
    namespace workspace = carto::workspace;
    namespace kinematic = carto::kinematic;

    // Uncertainty bounds: aspect must be bounded safe text, confidence a
    // finite value in [0, 1], note bounded.
    workspace::UncertaintyDiagnostic uncertainty =
        uncertainty_for_test("aspect", 0.5, "note");
    REQUIRE(uncertainty.validate());
    uncertainty.aspect.clear();
    REQUIRE(!uncertainty.validate());
    uncertainty = uncertainty_for_test(std::string(129U, 'a'), 0.5, "note");
    REQUIRE(!uncertainty.validate());
    uncertainty = uncertainty_for_test("aspect", 1.5, "note");
    REQUIRE(!uncertainty.validate());
    uncertainty = uncertainty_for_test(
        "aspect", std::numeric_limits<double>::quiet_NaN(), "note");
    REQUIRE(!uncertainty.validate());
    uncertainty = uncertainty_for_test("aspect", -0.1, "note");
    REQUIRE(!uncertainty.validate());
    uncertainty = uncertainty_for_test("aspect", 0.5, std::string(257U, 'n'));
    REQUIRE(!uncertainty.validate());

    workspace::ExpectedQualityMetric metric;
    metric.metric = "added_joint_count";
    metric.value = 1.0;
    metric.tolerance = 0.0;
    REQUIRE(metric.validate());
    metric.tolerance = -1.0;
    REQUIRE(!metric.validate());

    // Typed rig proposal validation.
    workspace::RigProposal rig;
    rig.candidate_joints.push_back(workspace::RigCandidateJoint{});
    rig.uncertainty.push_back(uncertainty_for_test("chain.link", 0.9, "note"));
    rig.recipe_digest = tag_digest("recipe");
    REQUIRE(!rig.validate()); // zero joint identity
    rig.candidate_joints.front().joint = kinematic::JointId{2U};
    REQUIRE(rig.validate());
    rig.uncertainty.clear();
    REQUIRE(!rig.validate()); // one uncertainty per candidate
    rig.uncertainty.push_back(uncertainty_for_test("chain.link", 0.9, "note"));
    rig.uncertainty.push_back(uncertainty_for_test("chain.link", 0.8, "note"));
    REQUIRE(!rig.validate());
    rig.uncertainty.pop_back();
    rig.candidate_joints.push_back(rig.candidate_joints.front());
    REQUIRE(!rig.validate()); // duplicate joint identity
    rig.candidate_joints.pop_back();
    rig.recipe_digest = carto::assets::Sha256Digest{};
    REQUIRE(!rig.validate()); // recipe digest binds the proposal

    // Typed skin proposal validation.
    workspace::SkinProposal skin;
    skin.recipe_digest = tag_digest("recipe");
    workspace::SkinCandidateStamp stamp_a;
    stamp_a.vertex = 30U;
    stamp_a.influences.push_back(kinematic::JointInfluence{kinematic::JointId{1U}, 1.0});
    workspace::SkinCandidateStamp stamp_b = stamp_a;
    stamp_b.vertex = 10U;
    skin.stamps.push_back(stamp_a);
    skin.stamps.push_back(stamp_b);
    skin.uncertainty.push_back(uncertainty_for_test("uniform.prior", 0.5, "note"));
    skin.uncertainty.push_back(uncertainty_for_test("uniform.prior", 0.5, "note"));
    REQUIRE(!skin.validate()); // stamps must be sorted by vertex
    std::swap(skin.stamps[0U], skin.stamps[1U]);
    REQUIRE(skin.validate());
    skin.stamps.front().influences.clear();
    REQUIRE(!skin.validate()); // empty influence lists are not proposals

    // Typed motion proposal validation.
    workspace::MotionProposal motion;
    motion.recipe_digest = tag_digest("recipe");
    workspace::MotionCandidateKey key_a;
    key_a.channel = carto::temporal::ChannelId{1U};
    key_a.time = carto::character::RationalTime{0, 24U};
    key_a.value = 1.0;
    motion.keys.push_back(key_a);
    motion.keys.push_back(key_a); // exact duplicate channel and time
    motion.uncertainty.push_back(uncertainty_for_test("static.pose", 0.5, "note"));
    motion.uncertainty.push_back(uncertainty_for_test("static.pose", 0.5, "note"));
    REQUIRE(!motion.validate()); // duplicate channel and time
    motion.keys.back().time = carto::character::RationalTime{12, 24U};
    motion.keys.front().time = carto::character::RationalTime{24, 24U};
    REQUIRE(!motion.validate()); // keys must be sorted by time
    motion.keys.front().time = carto::character::RationalTime{6, 24U};
    REQUIRE(motion.validate());
    motion.keys.back().time = carto::character::RationalTime{12, 0U};
    REQUIRE(!motion.validate()); // invalid rational timebase

    // Recipe digest binding: identical envelopes with different recipes are
    // different proposals.
    TempDirectory temp;
    workspace::WorkspaceController controller(temp.path() / "recipe.journal");
    REQUIRE(controller.store().insert(make_bundle(carto::character::CharacterId{7U})));
    workspace::local::RigChainOptions options;
    options.joint_count = 2U;
    const auto typed = workspace::local::propose_rig_chain(
        controller.store(), carto::character::CharacterId{7U}, options);
    REQUIRE(typed);
    workspace::RigProposal re_reciped = typed.value();
    re_reciped.recipe_digest = tag_digest("other-recipe");
    const auto envelope_a = workspace::build_rig_proposal(
        controller.store(), carto::character::CharacterId{7U}, typed.value(),
        tag_digest("objective"), "carto-local-tests");
    const auto envelope_b = workspace::build_rig_proposal(
        controller.store(), carto::character::CharacterId{7U}, re_reciped,
        tag_digest("objective"), "carto-local-tests");
    REQUIRE(envelope_a);
    REQUIRE(envelope_b);
    const auto digest_a = envelope_a.value().canonical_digest();
    const auto digest_b = envelope_b.value().canonical_digest();
    REQUIRE(digest_a);
    REQUIRE(digest_b);
    REQUIRE(digest_a.value() != digest_b.value());
    REQUIRE(envelope_a.value().proposal_id != envelope_b.value().proposal_id);
}

void proposal_generators_are_deterministic() {
    namespace workspace = carto::workspace;
    TempDirectory temp;
    // Two identical stores: every generator must produce byte-identical
    // proposals for identical source.
    workspace::WorkspaceController first(temp.path() / "first.journal");
    workspace::WorkspaceController second(temp.path() / "second.journal");
    REQUIRE(first.store().insert(make_bundle(carto::character::CharacterId{7U})));
    REQUIRE(second.store().insert(make_bundle(carto::character::CharacterId{7U})));

    workspace::local::RigChainOptions rig_options;
    rig_options.joint_count = 4U;
    const auto rig_a = workspace::local::propose_rig_chain(
        first.store(), carto::character::CharacterId{7U}, rig_options);
    const auto rig_b = workspace::local::propose_rig_chain(
        second.store(), carto::character::CharacterId{7U}, rig_options);
    REQUIRE(rig_a);
    REQUIRE(rig_b);
    REQUIRE(rig_a.value().recipe_digest == rig_b.value().recipe_digest);
    REQUIRE(rig_a.value().candidate_joints.size() ==
            rig_b.value().candidate_joints.size());
    for (std::size_t index = 0U; index < rig_a.value().candidate_joints.size();
         ++index) {
        const auto& left = rig_a.value().candidate_joints[index];
        const auto& right = rig_b.value().candidate_joints[index];
        REQUIRE(left.joint == right.joint);
        REQUIRE(left.parent == right.parent);
        REQUIRE(left.rest.translation.x == right.rest.translation.x);
        REQUIRE(left.rest.translation.y == right.rest.translation.y);
        REQUIRE(left.rest.translation.z == right.rest.translation.z);
        REQUIRE(left.kind == right.kind);
        REQUIRE(left.semantic_type == right.semantic_type);
    }
    REQUIRE(rig_a.value().uncertainty == rig_b.value().uncertainty);
    // Confidence decays along the chain.
    REQUIRE(rig_a.value().uncertainty.front().confidence >
            rig_a.value().uncertainty.back().confidence);

    workspace::local::UniformSkinOptions skin_options;
    skin_options.default_joint = carto::kinematic::JointId{1U};
    const auto skin_a = workspace::local::propose_uniform_skin(
        first.store(), carto::character::CharacterId{7U}, skin_options);
    const auto skin_b = workspace::local::propose_uniform_skin(
        second.store(), carto::character::CharacterId{7U}, skin_options);
    REQUIRE(skin_a);
    REQUIRE(skin_b);
    REQUIRE(skin_a.value().recipe_digest == skin_b.value().recipe_digest);
    REQUIRE(skin_a.value().stamps.size() == skin_b.value().stamps.size());
    for (std::size_t index = 0U; index < skin_a.value().stamps.size(); ++index) {
        const auto& left = skin_a.value().stamps[index];
        const auto& right = skin_b.value().stamps[index];
        REQUIRE(left.vertex == right.vertex);
        REQUIRE(left.influences.size() == right.influences.size());
        for (std::size_t influence = 0U; influence < left.influences.size();
             ++influence) {
            REQUIRE(left.influences[influence].joint ==
                    right.influences[influence].joint);
            REQUIRE(left.influences[influence].weight ==
                    right.influences[influence].weight);
        }
    }

    workspace::local::StaticKeysOptions motion_options;
    motion_options.time = carto::character::RationalTime{0, 24U};
    motion_options.value = 2.5;
    const auto motion_a = workspace::local::propose_static_keys(
        first.store(), carto::character::CharacterId{7U}, motion_options);
    const auto motion_b = workspace::local::propose_static_keys(
        second.store(), carto::character::CharacterId{7U}, motion_options);
    REQUIRE(motion_a);
    REQUIRE(motion_b);
    REQUIRE(motion_a.value().recipe_digest == motion_b.value().recipe_digest);
    REQUIRE(motion_a.value().keys.size() == motion_b.value().keys.size());
    for (std::size_t index = 0U; index < motion_a.value().keys.size(); ++index) {
        const auto& left = motion_a.value().keys[index];
        const auto& right = motion_b.value().keys[index];
        REQUIRE(left.channel == right.channel);
        REQUIRE(left.time.tick == right.time.tick);
        REQUIRE(left.time.ticks_per_second == right.time.ticks_per_second);
        REQUIRE(left.value == right.value);
    }

    // Envelopes built twice from the same source serialize identically.
    const auto envelope_a = workspace::build_rig_proposal(
        first.store(), carto::character::CharacterId{7U}, rig_a.value(),
        tag_digest("objective"), "carto-local-tests");
    const auto envelope_b = workspace::build_rig_proposal(
        first.store(), carto::character::CharacterId{7U}, rig_a.value(),
        tag_digest("objective"), "carto-local-tests");
    REQUIRE(envelope_a);
    REQUIRE(envelope_b);
    REQUIRE(envelope_a.value().serialize() == envelope_b.value().serialize());
    const auto skin_envelope = workspace::build_skin_proposal(
        first.store(), carto::character::CharacterId{7U}, skin_a.value(),
        tag_digest("objective"), "carto-local-tests");
    const auto motion_envelope = workspace::build_motion_proposal(
        first.store(), carto::character::CharacterId{7U}, motion_a.value(),
        tag_digest("objective"), "carto-local-tests");
    REQUIRE(skin_envelope);
    REQUIRE(motion_envelope);
    REQUIRE(skin_envelope.value().kind == workspace::ProposalKind::skin);
    REQUIRE(motion_envelope.value().kind == workspace::ProposalKind::motion);
    REQUIRE(motion_envelope.value().operation_sequence.front().domain ==
            workspace::WorkspaceDomain::animate);
}

void proposal_materializes_only_as_journaled_commands() {
    namespace workspace = carto::workspace;
    namespace character = carto::character;
    TempDirectory temp;
    workspace::WorkspaceController controller(temp.path() / "materialize.journal");
    REQUIRE(controller.store().insert(make_bundle(character::CharacterId{7U})));
    const auto digest_before = controller.store().canonical_digest();
    REQUIRE(digest_before);

    // Generating and materializing a proposal never touches source: the store
    // digest and the journal are untouched until the controller admits a
    // command.
    workspace::local::RigChainOptions options;
    options.joint_count = 3U;
    const auto typed = workspace::local::propose_rig_chain(
        controller.store(), character::CharacterId{7U}, options);
    REQUIRE(typed);
    const auto envelope = workspace::build_rig_proposal(
        controller.store(), character::CharacterId{7U}, typed.value(),
        tag_digest("objective"), "carto-local-tests");
    REQUIRE(envelope);
    const auto digest_after_build = controller.store().canonical_digest();
    REQUIRE(digest_after_build);
    REQUIRE(digest_after_build.value() == digest_before.value());
    const auto journal_before = controller.journal().read_all();
    REQUIRE(journal_before);
    REQUIRE(journal_before.value().empty());

    // Accept: apply every materialized operation through the controller with
    // the ordinary command admission chain, recording the review.
    workspace::ProposalReview review;
    review.proposal_id = envelope.value().proposal_id;
    const auto proposal_digest = envelope.value().canonical_digest();
    REQUIRE(proposal_digest);
    review.proposal_digest = proposal_digest.value();
    review.decision = workspace::ProposalDecision::accepted;
    review.reviewer = "artist";
    review.reason = "";
    review.review_revision = controller.store().revision();
    review.store_digest_before = digest_before.value();
    for (std::size_t index = 0U;
         index < envelope.value().operation_sequence.size(); ++index) {
        workspace::WorkspaceCommand command =
            envelope.value().operation_sequence[index];
        REQUIRE(command.validate());
        REQUIRE(command.domain == workspace::WorkspaceDomain::rig);
        restamp_expected(command, controller.store().revision());
        const auto receipt = controller.apply(command);
        REQUIRE(receipt);
        workspace::ProposalApplication application;
        application.operation_index = static_cast<std::uint32_t>(index);
        application.kind = command.kind();
        application.revision_before = carto::core::Revision{
            receipt.value().applied_revision.value() - 1U};
        application.revision_after = receipt.value().applied_revision;
        application.command_digest = receipt.value().journal_entry_digest;
        review.applications.push_back(application);
    }
    const auto digest_after = controller.store().canonical_digest();
    REQUIRE(digest_after);
    review.store_digest_after = digest_after.value();
    REQUIRE(review.validate());

    const auto* bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle != nullptr);
    REQUIRE(bundle->graph.find(carto::kinematic::JointId{2U}) != nullptr);
    REQUIRE(bundle->graph.find(carto::kinematic::JointId{3U}) != nullptr);
    REQUIRE(bundle->graph.find(carto::kinematic::JointId{4U}) != nullptr);

    // Every admitted command is a journaled entry; the journal verifies.
    REQUIRE(controller.journal().verify());
    const auto entries = controller.journal().read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == envelope.value().operation_sequence.size());

    // The durable review record round-trips.
    const std::string review_text = review.serialize();
    const auto decoded_review = workspace::ProposalReview::deserialize(review_text);
    REQUIRE(decoded_review);
    REQUIRE(decoded_review.value().serialize() == review_text);

    // A motion proposal materializes as animate commands that mutate only
    // through the same surface.
    workspace::local::StaticKeysOptions motion_options;
    motion_options.time = character::RationalTime{0, 24U};
    motion_options.value = 2.5;
    const auto motion_typed = workspace::local::propose_static_keys(
        controller.store(), character::CharacterId{7U}, motion_options);
    REQUIRE(motion_typed);
    const auto motion_envelope = workspace::build_motion_proposal(
        controller.store(), character::CharacterId{7U}, motion_typed.value(),
        tag_digest("objective"), "carto-local-tests");
    REQUIRE(motion_envelope);
    REQUIRE(motion_envelope.value().operation_sequence.size() == 1U);
    workspace::WorkspaceCommand motion_command =
        motion_envelope.value().operation_sequence.front();
    restamp_expected(motion_command, controller.store().revision());
    const auto motion_receipt = controller.apply(motion_command);
    REQUIRE(motion_receipt);
    bundle = controller.store().find(character::CharacterId{7U});
    REQUIRE(bundle->channel->scalar_keyframes.size() == 1U);
    REQUIRE(bundle->channel->scalar_keyframes.front().value == 2.5);
}

void proposal_stale_operations_fail_closed_and_skin_respects_locks() {
    namespace workspace = carto::workspace;
    namespace character = carto::character;
    namespace kinematic = carto::kinematic;
    TempDirectory temp;

    // Stale proposal: operations built against revision 0 refuse once the
    // store moved on, exactly like manually authored stale commands.
    workspace::WorkspaceController controller(temp.path() / "stale-proposal.journal");
    REQUIRE(controller.store().insert(make_bundle(character::CharacterId{7U})));
    workspace::local::RigChainOptions options;
    options.joint_count = 2U;
    const auto typed = workspace::local::propose_rig_chain(
        controller.store(), character::CharacterId{7U}, options);
    REQUIRE(typed);
    const auto envelope = workspace::build_rig_proposal(
        controller.store(), character::CharacterId{7U}, typed.value(),
        tag_digest("objective"), "carto-local-tests");
    REQUIRE(envelope);
    workspace::WorkspaceCommand first =
        envelope.value().operation_sequence.front();
    restamp_expected(first, controller.store().revision());
    REQUIRE(controller.apply(first));
    // The second operation still pins revision 0: stale, refused, no bypass.
    const workspace::WorkspaceCommand stale =
        envelope.value().operation_sequence.back();
    REQUIRE(stale.expected_source_revision() == carto::core::Revision{0U});
    const auto refused = controller.apply(stale);
    REQUIRE(!refused);
    REQUIRE(refused.error().code == carto::core::ErrorCode::stale_data);
    const auto entries = controller.journal().read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 1U);

    // Skin proposals exclude artist-locked vertices entirely.
    workspace::WorkspaceController skin_controller(temp.path() / "locks.journal");
    workspace::CharacterSourceBundle bundle = make_bundle(character::CharacterId{7U});
    bundle.skin->influences.emplace(
        20U, std::vector<kinematic::JointInfluence>{
            kinematic::JointInfluence{kinematic::JointId{1U}, 1.0}});
    bundle.weight_locks.push_back(10U);
    bundle.skin_layer->influenced_vertex_count = 2U;
    bundle.skin_layer->total_influence_count = 2U;
    REQUIRE(bundle.validate());
    REQUIRE(skin_controller.store().insert(bundle));

    workspace::local::UniformSkinOptions skin_options;
    skin_options.default_joint = kinematic::JointId{1U};
    const auto skin_typed = workspace::local::propose_uniform_skin(
        skin_controller.store(), character::CharacterId{7U}, skin_options);
    REQUIRE(skin_typed);
    REQUIRE(skin_typed.value().stamps.size() == 1U);
    REQUIRE(skin_typed.value().stamps.front().vertex == 20U);

    const auto skin_envelope = workspace::build_skin_proposal(
        skin_controller.store(), character::CharacterId{7U}, skin_typed.value(),
        tag_digest("objective"), "carto-local-tests");
    REQUIRE(skin_envelope);
    REQUIRE(skin_envelope.value().operation_sequence.size() == 1U);
    workspace::WorkspaceCommand stamp_command =
        skin_envelope.value().operation_sequence.front();
    restamp_expected(stamp_command, skin_controller.store().revision());
    const auto stamped = skin_controller.apply(stamp_command);
    REQUIRE(stamped);
    const auto* stored =
        skin_controller.store().find(character::CharacterId{7U});
    REQUIRE(stored != nullptr);
    const auto found = stored->skin->influences.find(20U);
    REQUIRE(found != stored->skin->influences.end());
    REQUIRE(found->second.size() == 1U);
    REQUIRE(found->second.front().joint == kinematic::JointId{1U});

    // A character whose influenced vertices are all locked yields no proposal.
    workspace::WorkspaceController locked_controller(temp.path() / "locked.journal");
    workspace::CharacterSourceBundle locked_bundle =
        make_bundle(character::CharacterId{7U});
    locked_bundle.weight_locks.push_back(10U);
    REQUIRE(locked_bundle.validate());
    REQUIRE(locked_controller.store().insert(locked_bundle));
    const auto empty_skin = workspace::local::propose_uniform_skin(
        locked_controller.store(), character::CharacterId{7U}, skin_options);
    REQUIRE(!empty_skin);

    // Generators fail closed on missing targets and missing channels.
    const auto missing_character = workspace::local::propose_rig_chain(
        skin_controller.store(), character::CharacterId{99U}, options);
    REQUIRE(!missing_character);
    workspace::WorkspaceController bare_controller(temp.path() / "bare.journal");
    REQUIRE(bare_controller.store().insert(make_bundle(character::CharacterId{8U})));
    const auto* bare_bundle = bare_controller.store().find(character::CharacterId{8U});
    REQUIRE(bare_bundle != nullptr);
    workspace::CharacterSourceBundle no_channel = *bare_bundle;
    no_channel.channel.reset();
    REQUIRE(no_channel.validate());
    REQUIRE(bare_controller.store().remove(character::CharacterId{8U}));
    REQUIRE(bare_controller.store().insert(no_channel));
    workspace::local::StaticKeysOptions motion_options;
    motion_options.time = character::RationalTime{0, 24U};
    const auto no_channel_motion = workspace::local::propose_static_keys(
        bare_controller.store(), character::CharacterId{8U}, motion_options);
    REQUIRE(!no_channel_motion);
}

} // namespace

int main() {
    try {
        workspace_applies_and_undoes_rig_commands();
        workspace_applies_and_undoes_skin_commands();
        workspace_applies_and_undoes_animate_commands();
        workspace_refuses_stale_revisions_without_mutation();
        workspace_refuses_referential_violations();
        workspace_refuses_session_domain_mismatch();
        workspace_journals_commands_and_replays_to_identical_digest();
        proposal_envelope_round_trips_through_canonical_codec();
        proposal_deserialize_rejects_hostile_input();
        proposal_validators_enforce_bounds_and_recipe_binding();
        proposal_generators_are_deterministic();
        proposal_materializes_only_as_journaled_commands();
        proposal_stale_operations_fail_closed_and_skin_respects_locks();
    } catch (const std::exception& error) {
        return (void(std::cerr << "FAIL " << error.what() << '\n'), 1);
    }
    return 0;
}
