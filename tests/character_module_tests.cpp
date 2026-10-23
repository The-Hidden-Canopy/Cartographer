#include <carto/character/controls.hpp>
#include <carto/character/definition.hpp>
#include <carto/character/rig_module.hpp>

#include <carto/kinematic/graph.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <locale>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace kinematic = carto::kinematic;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class comma_decimal final : public std::numpunct<char> {
protected:
    [[nodiscard]] char do_decimal_point() const override { return ','; }
};

kinematic::KinematicNode joint(
    std::uint64_t id,
    std::optional<std::uint64_t> parent,
    std::string semantic = {}) {
    kinematic::KinematicNode node;
    node.id = kinematic::JointId{id};
    if (parent.has_value()) node.parent = kinematic::JointId{*parent};
    node.semantic_type = std::move(semantic);
    node.kind = kinematic::JointKind::ball;
    return node;
}

carto::character::RigModuleTemplate arm_module() {
    carto::character::RigModuleTemplate module;
    module.name = "arm.left";
    module.joints = {
        joint(10U, std::nullopt, "arm.root"),
        joint(11U, 10U, "arm.elbow"),
    };
    carto::character::ControlDefinition driver;
    driver.id = carto::character::ControlId{100U};
    driver.shape = carto::character::ControlShape::locator;
    driver.affected_joints = {kinematic::JointId{10U}, kinematic::JointId{11U}};
    driver.allowed_axes = carto::character::kAxisTranslateX;
    carto::character::ControlDefinition follower = driver;
    follower.id = carto::character::ControlId{101U};
    follower.follow_space = carto::character::ControlId{100U};
    module.controls = {driver, follower};
    module.control_edges = {{carto::character::ControlId{100U},
        carto::character::ControlId{101U}}};
    module.symmetry = {{kinematic::JointId{10U}, kinematic::JointId{11U}}};
    return module;
}

kinematic::KinematicGraph spine_graph() {
    kinematic::KinematicGraph graph;
    require(static_cast<bool>(graph.insert_node(joint(1U, std::nullopt, "spine"))),
        "fixture spine inserts");
    require(static_cast<bool>(graph.insert_node(joint(2U, 1U, "spine.tip"))),
        "fixture spine tip inserts");
    require(static_cast<bool>(graph.validate()), "fixture target graph validates");
    return graph;
}

void template_validation_is_bounded_and_fail_closed() {
    using namespace carto;
    character::RigModuleTemplate module = arm_module();
    require(static_cast<bool>(module.validate()),
        "well-formed arm module template validates");

    auto wrong_schema = module;
    wrong_schema.schema_version = character::RigModuleTemplate::kSchemaVersion + 1U;
    require(!wrong_schema.validate(),
        "an unknown template schema version fails closed");

    auto empty_name = module;
    empty_name.name.clear();
    require(!empty_name.validate(), "an empty module name fails closed");
    auto long_name = module;
    long_name.name.assign(character::RigModuleTemplate::kMaxNameBytes + 1U, 'n');
    require(!long_name.validate(), "an over-long module name fails closed");
    auto unsafe_name = module;
    unsafe_name.name = "arm\nleft";
    require(!unsafe_name.validate(), "an unsafe module name fails closed");

    auto no_joints = module;
    no_joints.joints.clear();
    require(!no_joints.validate(), "an empty joint fragment fails closed");
    auto two_roots = module;
    two_roots.joints.push_back(joint(12U, std::nullopt));
    require(!two_roots.validate(), "two attachment roots fail closed");
    auto duplicate_id = module;
    duplicate_id.joints[1U].id = kinematic::JointId{10U};
    require(!duplicate_id.validate(), "duplicate fragment joint ids fail closed");
    auto dangling_parent = module;
    dangling_parent.joints[1U].parent = kinematic::JointId{99U};
    require(!dangling_parent.validate(), "a parent outside the fragment fails closed");
    auto parent_cycle = module;
    parent_cycle.joints[0U].parent = kinematic::JointId{11U};
    require(!parent_cycle.validate(), "a fragment parent cycle fails closed");

    auto missing_follow = module;
    missing_follow.controls[1U].follow_space = character::ControlId{999U};
    require(!missing_follow.validate(),
        "a follow-space outside the control fragment fails closed");
    auto unsorted_edges = module;
    auto third_control = unsorted_edges.controls.front();
    third_control.id = character::ControlId{102U};
    unsorted_edges.controls.push_back(third_control);
    unsorted_edges.control_edges.push_back(
        {character::ControlId{100U}, character::ControlId{102U}});
    std::swap(unsorted_edges.control_edges[0U], unsorted_edges.control_edges[1U]);
    require(!unsorted_edges.validate(), "unsorted module control edges fail closed");
    auto edge_missing_control = module;
    edge_missing_control.control_edges[0U].follower = character::ControlId{777U};
    require(!edge_missing_control.validate(),
        "an edge naming a missing fragment control fails closed");

    auto symmetry_outside = module;
    symmetry_outside.symmetry = {{kinematic::JointId{10U}, kinematic::JointId{42U}}};
    require(!symmetry_outside.validate(),
        "a symmetry pair outside the joint fragment fails closed");
    auto symmetry_unsorted = module;
    symmetry_unsorted.symmetry = {
        {kinematic::JointId{10U}, kinematic::JointId{11U}},
        {kinematic::JointId{9U}, kinematic::JointId{12U}}};
    symmetry_unsorted.joints.push_back(joint(9U, 10U));
    symmetry_unsorted.joints.push_back(joint(12U, 11U));
    require(!symmetry_unsorted.validate(), "unsorted module symmetry fails closed");
    auto self_pair = module;
    self_pair.symmetry = {{kinematic::JointId{10U}, kinematic::JointId{10U}}};
    require(!self_pair.validate(), "a self-pairing module symmetry entry fails closed");
}

void module_template_serialization_is_canonical_and_fail_closed() {
    using namespace carto;
    auto module = arm_module();
    module.joints[1U].rest.translation = {0.125, -0.25, 0.5};
    module.controls[0U].transform.translation = {1.0, 2.0, 3.0};
    module.controls[0U].axis_minimum[0U] = -2.0;
    module.controls[0U].axis_maximum[0U] = 2.0;
    module.controls[0U].control_set = "body core";

    const auto serialized = module.serialize();
    require(static_cast<bool>(serialized),
        "a valid module template should serialize within its bound");
    auto reordered = module;
    std::reverse(reordered.joints.begin(), reordered.joints.end());
    std::reverse(reordered.controls.begin(), reordered.controls.end());
    const auto reordered_serialization = reordered.serialize();
    require(reordered_serialization && reordered_serialization.value() == serialized.value(),
        "module serialization should ignore input vector ordering where identity is stable");

    const auto previous_locale = std::locale::global(
        std::locale(std::locale::classic(), new comma_decimal()));
    const auto locale_serialization = module.serialize();
    (void)std::locale::global(previous_locale);
    require(locale_serialization && locale_serialization.value() == serialized.value(),
        "module serialization should remain deterministic under a comma-decimal locale");

    const auto decoded = character::RigModuleTemplate::deserialize(serialized.value());
    require(decoded && decoded.value().serialize() &&
            decoded.value().serialize().value() == serialized.value(),
        "module template serialization should round-trip to canonical bytes");
    require(decoded.value().joints[1U].rest.translation.x == 0.125 &&
            decoded.value().controls[0U].transform.translation.z == 3.0 &&
            decoded.value().controls[0U].axis_minimum[0U] == -2.0 &&
            decoded.value().controls[0U].axis_maximum[0U] == 2.0 &&
            decoded.value().controls[0U].control_set == "body core" &&
            decoded.value().control_edges == module.control_edges &&
            decoded.value().symmetry.size() == module.symmetry.size(),
        "serialization should preserve every authored module field");

    auto unknown_version = serialized.value();
    const auto version_position = unknown_version.find(" 1\n");
    require(version_position != std::string::npos,
        "serialized template should include a schema version");
    unknown_version.replace(version_position, 3U, " 2\n");
    require(!character::RigModuleTemplate::deserialize(unknown_version),
        "unknown template schema versions must fail closed");

    auto unknown_record = serialized.value();
    const auto end_position = unknown_record.rfind("END\n");
    require(end_position != std::string::npos,
        "serialized template should have a terminal record");
    unknown_record.insert(end_position, "UNRECOGNIZED 7\n");
    require(!character::RigModuleTemplate::deserialize(unknown_record),
        "unknown template records must fail closed");

    auto over_budget = serialized.value();
    const auto joints_position = over_budget.find("JOINTS 2\n");
    require(joints_position != std::string::npos,
        "serialized template should include a joint count");
    over_budget.replace(joints_position, 9U, "JOINTS 1025\n");
    require(!character::RigModuleTemplate::deserialize(over_budget),
        "over-budget template records must be rejected before allocation");
}

void instantiation_remaps_every_reference_under_fresh_ids() {
    using namespace carto;
    const character::RigModuleTemplate module = arm_module();
    const kinematic::KinematicGraph target = spine_graph();
    character::ControlGraph target_controls;
    require(static_cast<bool>(target_controls.insert(character::ControlDefinition{
                .id = character::ControlId{1U}})),
        "fixture target control inserts");
    require(static_cast<bool>(target_controls.validate()),
        "fixture target control graph validates");

    const auto instance = character::instantiate_rig_module(
        module, target, target_controls, character::ModuleIdPolicy::allocate_fresh_ids);
    require(static_cast<bool>(instance), "arm module instantiates onto the spine");
    const character::RigModuleInstance& value = instance.value();

    require(value.kinematic_graph.nodes().size() == 4U,
        "merged graph carries target and fragment joints");
    require(value.kinematic_graph.find(kinematic::JointId{1U}) != nullptr &&
            value.kinematic_graph.find(kinematic::JointId{10U}) == nullptr,
        "target joints keep their identities and fragment ids are not reused");
    const kinematic::KinematicNode* root = value.kinematic_graph.find(kinematic::JointId{12U});
    require(root != nullptr && root->semantic_type == "arm.root",
        "attachment root takes the first fresh identity");
    require(root->parent.has_value() && root->parent.value() == kinematic::JointId{1U},
        "attachment root grafts under the target root");
    const kinematic::KinematicNode* elbow = value.kinematic_graph.find(kinematic::JointId{13U});
    require(elbow != nullptr && elbow->parent.has_value() &&
            elbow->parent.value() == kinematic::JointId{12U},
        "fragment parentage remaps to the fresh root");
    require(static_cast<bool>(value.kinematic_graph.validate()),
        "merged kinematic graph validates as one tree");
    require(target.nodes().size() == 2U,
        "the target kinematic graph is not mutated");

    require(value.control_graph.nodes().size() == 3U,
        "merged control graph carries target and fragment controls");
    const character::ControlDefinition* remapped_driver =
        value.control_graph.find(character::ControlId{102U});
    require(remapped_driver != nullptr &&
            remapped_driver->affected_joints ==
                std::vector<kinematic::JointId>{kinematic::JointId{12U},
                    kinematic::JointId{13U}},
        "affected joints remap to fresh identities in sorted order");
    const character::ControlDefinition* remapped_follower =
        value.control_graph.find(character::ControlId{103U});
    require(remapped_follower != nullptr &&
            remapped_follower->follow_space.has_value() &&
            remapped_follower->follow_space.value() == character::ControlId{102U},
        "follow-space remaps to the fresh driver identity");
    require(value.control_graph.edges().size() == 1U &&
            value.control_graph.edges().front().driver == character::ControlId{102U} &&
            value.control_graph.edges().front().follower == character::ControlId{103U},
        "control edges remap with their endpoints");
    require(static_cast<bool>(value.control_graph.validate()),
        "merged control graph stays acyclic");
    require(target_controls.nodes().size() == 1U,
        "the target control graph is not mutated");

    require(value.symmetry.size() == 1U &&
            value.symmetry.front().left_joint == kinematic::JointId{12U} &&
            value.symmetry.front().right_joint == kinematic::JointId{13U},
        "internal symmetry sides remap to the fresh identities");

    require(value.receipt.module_name == "arm.left",
        "receipt names the module");
    require(!value.receipt.source_graph_digest.is_zero() &&
            !value.receipt.result_graph_digest.is_zero() &&
            value.receipt.source_graph_digest != value.receipt.result_graph_digest,
        "receipt binds source and result digests");
    require(value.receipt.added_joint_count == 2U &&
            value.receipt.added_control_count == 2U &&
            value.receipt.added_edge_count == 1U &&
            value.receipt.added_symmetry_pair_count == 1U,
        "receipt counts every admitted fragment member");
}

void instantiation_is_deterministic_and_repeated() {
    using namespace carto;
    const character::RigModuleTemplate module = arm_module();
    const kinematic::KinematicGraph target = spine_graph();
    character::ControlGraph empty_controls;
    require(static_cast<bool>(empty_controls.insert(character::ControlDefinition{
                .id = character::ControlId{5U}})),
        "fixture control inserts");

    const auto first = character::instantiate_rig_module(
        module, target, empty_controls, character::ModuleIdPolicy::allocate_fresh_ids);
    const auto second = character::instantiate_rig_module(
        module, target, empty_controls, character::ModuleIdPolicy::allocate_fresh_ids);
    require(static_cast<bool>(first) && static_cast<bool>(second) &&
            first.value().receipt.result_graph_digest ==
                second.value().receipt.result_graph_digest,
        "equal instantiations reproduce the identical result digest");

    auto right_arm = module;
    right_arm.name = "arm.right";
    right_arm.joints[0U].semantic_type = "arm.right.root";
    right_arm.joints[1U].semantic_type = "arm.right.elbow";
    const auto twin = character::instantiate_rig_module(
        right_arm, first.value().kinematic_graph, first.value().control_graph,
        character::ModuleIdPolicy::allocate_fresh_ids);
    require(static_cast<bool>(twin) && twin.value().kinematic_graph.nodes().size() == 6U &&
            twin.value().receipt.added_joint_count == 2U,
        "a module instantiates twice into one rig under fresh identities");
    require(twin.value().kinematic_graph.find(kinematic::JointId{12U}) != nullptr &&
            twin.value().kinematic_graph.find(kinematic::JointId{14U}) != nullptr,
        "the second copy receives identities past the first");
}

void preserve_policy_and_collisions_fail_closed() {
    using namespace carto;
    const kinematic::KinematicGraph target = spine_graph();
    character::ControlGraph target_controls;
    require(static_cast<bool>(target_controls.insert(character::ControlDefinition{
                .id = character::ControlId{1U}})),
        "fixture target control inserts");

    const auto preserved = character::instantiate_rig_module(arm_module(), target,
        target_controls, character::ModuleIdPolicy::preserve_fragment_ids);
    require(static_cast<bool>(preserved) &&
            preserved.value().kinematic_graph.find(kinematic::JointId{10U}) != nullptr &&
            preserved.value().kinematic_graph.find(kinematic::JointId{10U})
                ->parent.value() == kinematic::JointId{1U},
        "preserve_fragment_ids keeps authored identities when free");

    character::RigModuleTemplate colliding = arm_module();
    colliding.joints[0U].id = kinematic::JointId{1U};
    require(!character::instantiate_rig_module(colliding, target, target_controls,
                character::ModuleIdPolicy::preserve_fragment_ids),
        "preserving a fragment identity the target owns fails closed");
    require(!character::instantiate_rig_module(arm_module(), target, target_controls,
                static_cast<character::ModuleIdPolicy>(99U)),
        "an unknown id policy fails closed");

    character::RigModuleTemplate name_collision = arm_module();
    name_collision.joints[0U].semantic_type = "spine";
    const auto rejected = character::instantiate_rig_module(name_collision, target,
        target_controls, character::ModuleIdPolicy::allocate_fresh_ids);
    require(!rejected, "a fragment semantic-type collision fails closed");
    require(static_cast<bool>(character::instantiate_rig_module(arm_module(), target,
                target_controls, character::ModuleIdPolicy::allocate_fresh_ids)),
        "the same instantiation without the collision still succeeds");

    character::ControlGraph full_controls;
    for (std::uint64_t identity = 1U; identity <= character::ControlGraph::kMaxControls;
            ++identity) {
        require(static_cast<bool>(full_controls.insert(character::ControlDefinition{
                    .id = character::ControlId{identity}})),
            "capacity fixture control inserts");
    }
    require(!character::instantiate_rig_module(arm_module(), target, full_controls,
                character::ModuleIdPolicy::allocate_fresh_ids),
        "a module exceeding the TARGET control bound fails closed");
}

} // namespace

int main() {
    template_validation_is_bounded_and_fail_closed();
    module_template_serialization_is_canonical_and_fail_closed();
    instantiation_remaps_every_reference_under_fresh_ids();
    instantiation_is_deterministic_and_repeated();
    preserve_policy_and_collisions_fail_closed();
    std::cout << "character module tests passed\n";
    return EXIT_SUCCESS;
}
