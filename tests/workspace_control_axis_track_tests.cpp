#include <carto/workspace/store.hpp>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace carto;
namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
assets::Sha256Digest hash(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}
workspace::CharacterSourceBundle fixture() {
    workspace::CharacterSourceBundle bundle;
    bundle.definition.id = character::CharacterId{7};
    bundle.definition.project_namespace = "workspace.tests";
    bundle.definition.source_revision = core::Revision{1};
    bundle.definition.source_digest = hash("definition");
    bundle.definition.root = scene::ObjectId{1};
    bundle.definition.rig.rig = character::RigId{1};
    bundle.definition.rig.graph_digest = hash("graph");
    bundle.definition.control_graph.owner = character::RigId{1};
    bundle.definition.control_graph.digest = hash("control");
    bundle.definition.coordinate_policy_digest = hash("policy");
    kinematic::KinematicNode root;
    root.id = kinematic::JointId{1}; root.kind = kinematic::JointKind::fixed;
    root.semantic_type = "root";
    require(static_cast<bool>(bundle.graph.insert_node(root)), "insert root");
    bundle.rig.id = character::RigId{1};
    bundle.rig.kinematic_graph_digest = hash("graph");
    bundle.rig.joint_limits.digest = hash("limits");
    character::ControlDefinition control;
    control.id = character::ControlId{9};
    control.allowed_axes = character::kAxisTranslateX | character::kAxisTranslateY;
    require(static_cast<bool>(bundle.rig.control_graph.insert(control)), "insert control");
    require(static_cast<bool>(bundle.validate()), "baseline bundle");
    return bundle;
}
workspace::ControlAxisTrack track() {
    workspace::ControlAxisTrack value;
    value.control = character::ControlId{9};
    value.axis = 0;
    value.role = workspace::ControlTrackRole::ik_fk_mode;
    value.space = workspace::ControlTrackSpace::raw_control_value;
    value.source_namespace = "caller.source";
    value.source_revision = core::Revision{4};
    value.reference_frame = "caller-control-frame";
    value.rig_digest = hash("rig pin"); value.graph_digest = hash("graph pin");
    value.curve.id = motion::MotionCurveId{3};
    value.curve.timebase_ticks_per_second = 30000;
    value.curve.interpolation = motion::InterpolationKind::step;
    value.curve.keys = {{{0, 30000}, 0, 0, 0}, {{1001, 30000}, 1, 0, 0}};
    value.curve_digest = value.curve.canonical_digest().value();
    return value;
}
}

int main() {
    workspace::CharacterSourceStore store;
    auto bundle = fixture();
    bundle.control_axis_tracks.push_back(track());
    require(static_cast<bool>(store.insert(bundle)), "insert track bundle");
    const auto encoded = store.serialize();
    auto reopened = workspace::CharacterSourceStore::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded, "v2 populated round trip");
    const auto& saved = reopened.value().find(character::CharacterId{7})->control_axis_tracks.front();
    require(saved.curve.keys[1].time.tick == 1001 && saved.curve.keys[1].time.ticks_per_second == 30000 &&
        saved.curve.sample({1000, 30000}).value() == 0 && saved.curve.sample({1001, 30000}).value() == 1 &&
        saved.source_namespace == "caller.source" && saved.reference_frame == "caller-control-frame" &&
        saved.rig_digest == hash("rig pin"), "exact ticks, role and asserted source survive");

    auto duplicate = bundle;
    duplicate.control_axis_tracks.push_back(track());
    require(!duplicate.validate(), "duplicate control axis refused");
    auto duplicate_curve = bundle;
    auto second = track(); second.axis = 1;
    duplicate_curve.control_axis_tracks.push_back(second);
    require(!duplicate_curve.validate(), "duplicate curve identity refused");
    auto bad_axis = bundle;
    bad_axis.control_axis_tracks.front().axis = 5;
    require(!bad_axis.validate(), "disabled axis refused");
    auto bad_role = bundle;
    bad_role.control_axis_tracks.front().role = static_cast<workspace::ControlTrackRole>(255);
    require(!bad_role.validate(), "unknown role refused");
    auto bad_pin = bundle;
    bad_pin.control_axis_tracks.front().graph_digest = {};
    require(!bad_pin.validate(), "missing graph pin refused");
    auto bad_digest = bundle;
    bad_digest.control_axis_tracks.front().curve.keys[1].value = 0.5;
    require(!bad_digest.validate(), "changed curve cannot retain old digest");
    auto non_step = bundle;
    non_step.control_axis_tracks.front().curve.interpolation = motion::InterpolationKind::linear;
    non_step.control_axis_tracks.front().curve_digest = non_step.control_axis_tracks.front().curve.canonical_digest().value();
    require(!non_step.validate() && non_step.validate().error().code == core::ErrorCode::unsupported,
        "non-step tracks are explicitly unsupported");
    auto no_control = bundle;
    no_control.control_axis_tracks.front().control = character::ControlId{99};
    require(!no_control.validate(), "missing control refused");

    workspace::CharacterSourceStore legacy_store;
    require(static_cast<bool>(legacy_store.insert(fixture())), "legacy fixture");
    auto old = legacy_store.serialize();
    const auto table = old.find(",\"control_axis_tracks\":[]");
    require(table != std::string::npos, "empty v2 table present");
    auto missing_table = old;
    missing_table.erase(table, std::string(",\"control_axis_tracks\":[]").size());
    require(!workspace::CharacterSourceStore::deserialize(missing_table), "v2 requires explicit track table");
    old.erase(table, std::string(",\"control_axis_tracks\":[]").size());
    const auto version = old.find("\"schema\":2");
    require(version != std::string::npos, "v2 schema present");
    old.replace(version, std::string("\"schema\":2").size(), "\"schema\":1");
    const auto legacy_digest = hash(old);
    auto migrated = workspace::CharacterSourceStore::deserialize(old);
    require(migrated && migrated.value().find(character::CharacterId{7})->control_axis_tracks.empty() &&
        static_cast<bool>(workspace::CharacterSourceStore::deserialize(migrated.value().serialize())) &&
        migrated.value().canonical_digest().value() != legacy_digest,
        "v1 migration leaves tracks empty and changes the store digest without rebinding old pins");
    auto disguised = encoded;
    const auto v2 = disguised.find("\"schema\":2");
    require(v2 != std::string::npos, "populated v2 schema present");
    disguised.replace(v2, std::string("\"schema\":2").size(), "\"schema\":1");
    require(!workspace::CharacterSourceStore::deserialize(disguised), "v1 refuses v2 track table");

    // Individually bounded bundles can exceed the whole-store readable limit
    // once quoted curve bytes are combined. Refuse before insertion.
    auto large = fixture();
    for (std::uint64_t i = 0; i < 100; ++i) {
        character::ControlDefinition control;
        control.id = character::ControlId{100 + i};
        control.allowed_axes = character::kAxisTranslateX;
        require(static_cast<bool>(large.rig.control_graph.insert(control)), "large fixture control");
        auto item = track();
        item.control = control.id; item.curve.id = motion::MotionCurveId{100 + i};
        item.reference_frame = std::string(256, '"'); // each byte expands under JSON escaping
        item.curve.keys.clear();
        for (std::int64_t tick = 0; tick < 4096; ++tick)
            item.curve.keys.push_back({{tick, 30000}, 0.12345678901234567, 0, 0});
        item.curve_digest = item.curve.canonical_digest().value();
        large.control_axis_tracks.push_back(std::move(item));
    }
    require(static_cast<bool>(large.validate()), "large bundle is individually bounded");
    workspace::CharacterSourceStore bounded;
    bool refused = false;
    for (std::uint64_t id = 1; id <= 12; ++id) {
        large.definition.id = character::CharacterId{id};
        const auto before = bounded.size();
        const auto before_digest = bounded.canonical_digest().value();
        if (!bounded.insert(large)) {
            require(bounded.size() == before && bounded.canonical_digest().value() == before_digest,
                "oversize insert preserves the prior store bytes and identity");
            refused = true;
            break;
        }
    }
    require(refused && bounded.serialize().size() <= workspace::CharacterSourceStore::kMaxSerializedBytes &&
        static_cast<bool>(workspace::CharacterSourceStore::deserialize(bounded.serialize())),
        "combined escaped store size is bounded and remains readable");
}
