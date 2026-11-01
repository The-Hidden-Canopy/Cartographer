#include <carto/character/rig_record.hpp>

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>

namespace {
using namespace carto;
void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
assets::Sha256Digest hash(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}
character::RigDefinition fixture() {
    character::RigDefinition rig;
    rig.id = character::RigId{7};
    rig.kinematic_graph_digest = hash("graph");
    rig.joint_limits.digest = hash("limits");
    rig.rest_basis.meters_per_unit = 0.01;
    rig.semantic_tags = {"arm left", "body"};
    character::ControlDefinition a;
    a.id = character::ControlId{1};
    a.shape = character::ControlShape::box;
    a.transform.translation.x = 1.25;
    a.affected_joints = {kinematic::JointId{11}, kinematic::JointId{12}};
    a.allowed_axes = character::kAxisTranslateX;
    a.axis_minimum[0] = -2.0;
    a.axis_maximum[0] = 3.0;
    a.control_set = "primary controls";
    character::ControlDefinition b;
    b.id = character::ControlId{2};
    b.follow_space = a.id;
    require(static_cast<bool>(rig.control_graph.insert(a)), "insert first control");
    require(static_cast<bool>(rig.control_graph.insert(b)), "insert second control");
    require(static_cast<bool>(rig.control_graph.insert_edge({a.id, b.id})), "insert edge");
    rig.symmetry = {{kinematic::JointId{11}, kinematic::JointId{12}}};
    return rig;
}
void replace_once(std::string& text, std::string_view from, std::string_view to) {
    auto pos = text.find(from);
    require(pos != std::string::npos, "test mutation target exists");
    text.replace(pos, from.size(), to);
}
} // namespace

int main() {
    const auto rig = fixture();
    const auto bytes = character::serialize_rig_definition(rig);
    require(static_cast<bool>(bytes), "valid rig serializes");
    const auto decoded = character::deserialize_rig_definition(bytes.value());
    require(static_cast<bool>(decoded), "canonical rig parses");
    const auto& copy = decoded.value();
    require(copy.id == rig.id && copy.kinematic_graph_digest == rig.kinematic_graph_digest &&
        copy.joint_limits.digest == rig.joint_limits.digest && copy.rest_basis == rig.rest_basis &&
        copy.semantic_tags == rig.semantic_tags && copy.symmetry.size() == 1 &&
        copy.symmetry[0].left_joint.value == 11 && copy.symmetry[0].right_joint.value == 12,
        "identity, references, basis, tags, and symmetry round-trip");
    const auto* control = copy.control_graph.find(character::ControlId{1});
    const auto* follower = copy.control_graph.find(character::ControlId{2});
    require(control && follower && control->shape == character::ControlShape::box &&
        control->transform.translation.x == 1.25 && control->axis_minimum[0] == -2.0 &&
        control->axis_maximum[0] == 3.0 && control->affected_joints.size() == 2 &&
        control->control_set == "primary controls" && follower->follow_space == character::ControlId{1} &&
        copy.control_graph.edges().size() == 1, "complete control graph round-trips");
    require(character::serialize_rig_definition(copy).value() == bytes.value(), "encoding is stable");
    const auto digest = character::rig_definition_digest(copy);
    require(digest && digest.value() == hash(bytes.value()), "digest binds canonical bytes");
    auto altered = rig;
    altered.semantic_tags.push_back("wing");
    require(character::rig_definition_digest(altered).value() != digest.value(), "content changes digest");
    auto invalid = rig;
    invalid.kinematic_graph_digest = {};
    require(!character::serialize_rig_definition(invalid), "invalid rig cannot serialize");
    invalid = rig;
    invalid.rest_basis.winding = static_cast<character::WindingOrder>(2);
    require(!character::serialize_rig_definition(invalid), "unknown winding cannot serialize");
    std::string changed = bytes.value();
    changed += "trailing";
    require(!character::deserialize_rig_definition(changed), "trailing data refused");
    changed = bytes.value();
    replace_once(changed, "TAGS 2", "TAGS 129");
    require(!character::deserialize_rig_definition(changed), "excess count refused");
    changed = bytes.value();
    replace_once(changed, "CARTO_RIG_DEFINITION 1", "CARTO_RIG_DEFINITION 2");
    require(!character::deserialize_rig_definition(changed), "unknown version refused");
    changed = bytes.value();
    replace_once(changed, "CONTROL 1 1", "CONTROL 1 257");
    require(!character::deserialize_rig_definition(changed), "overflowing enum refused");
    changed = bytes.value();
    replace_once(changed, "EDGE 1 2", "EDGE 2 1");
    require(!character::deserialize_rig_definition(changed), "control dependency cycle refused");
    changed = bytes.value();
    replace_once(changed, "PAIR 11 12", "PAIR 11 11");
    require(!character::deserialize_rig_definition(changed), "invalid symmetry refused");
    changed = bytes.value();
    replace_once(changed, "TAGS 2", "TAGS 02");
    require(!character::deserialize_rig_definition(changed), "noncanonical numbers refused");
    changed = bytes.value().substr(0, bytes.value().size() - 5);
    require(!character::deserialize_rig_definition(changed), "truncation refused");
    require(!character::deserialize_rig_definition(std::string(character::kMaxRigDefinitionBytes + 1, 'x')),
        "oversize input refused");
    std::cout << "rig record tests passed\n";
}
