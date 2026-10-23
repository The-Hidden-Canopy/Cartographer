#include <carto/character/rig_module.hpp>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

constexpr std::string_view kModuleMagic = "CARTO_RIG_MODULE_TEMPLATE";

void write_transform(std::ostream& output, const core::Transform& transform) {
    output << transform.translation.x << ' ' << transform.translation.y << ' '
           << transform.translation.z << ' ' << transform.rotation.x << ' '
           << transform.rotation.y << ' ' << transform.rotation.z << ' '
           << transform.rotation.w << ' ' << transform.scale.x << ' '
           << transform.scale.y << ' ' << transform.scale.z;
}

bool read_transform(std::istream& input, core::Transform& transform) {
    return static_cast<bool>(input >> transform.translation.x >> transform.translation.y >>
        transform.translation.z >> transform.rotation.x >> transform.rotation.y >>
        transform.rotation.z >> transform.rotation.w >> transform.scale.x >>
        transform.scale.y >> transform.scale.z);
}

bool expect_token(std::istream& input, std::string_view expected) {
    std::string actual;
    return static_cast<bool>(input >> actual) && actual == expected;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& count) {
    std::uint64_t raw = 0U;
    if (!(input >> raw) || raw > maximum) return false;
    count = static_cast<std::size_t>(raw);
    return true;
}

bool safe_token(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

// Fragment joint ids in ascending order: the deterministic instantiation
// order for id allocation and node insertion.
std::vector<kinematic::JointId> sorted_joint_ids(
    const RigModuleTemplate& module) {
    std::vector<kinematic::JointId> ids;
    ids.reserve(module.joints.size());
    for (const kinematic::KinematicNode& node : module.joints) {
        ids.push_back(node.id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

const kinematic::KinematicNode* find_joint(
    const RigModuleTemplate& module, kinematic::JointId id) {
    for (const kinematic::KinematicNode& node : module.joints) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

// Joint id mapping for one instantiation. preserve_fragment_ids is the
// identity mapping and refuses an identity the target already owns;
// allocate_fresh_ids issues ids past both graphs, in ascending fragment id
// order, so allocation is deterministic for one (template, target) pair.
core::Result<std::map<kinematic::JointId, kinematic::JointId>> joint_remap(
    const RigModuleTemplate& module,
    const kinematic::KinematicGraph& target,
    ModuleIdPolicy policy) {
    std::map<kinematic::JointId, kinematic::JointId> remap;
    if (policy == ModuleIdPolicy::preserve_fragment_ids) {
        for (kinematic::JointId id : sorted_joint_ids(module)) {
            if (target.find(id) != nullptr) {
                return core::Result<std::map<kinematic::JointId, kinematic::JointId>>::
                    failure(invalid(
                        "module fragment joint identity collides with the target")
                        .with_context("joint=" + std::to_string(id.value)));
            }
            remap.emplace(id, id);
        }
        return core::Result<std::map<kinematic::JointId, kinematic::JointId>>::
            success(std::move(remap));
    }
    if (policy != ModuleIdPolicy::allocate_fresh_ids) {
        return core::Result<std::map<kinematic::JointId, kinematic::JointId>>::
            failure(invalid("module id remapping policy is not supported"));
    }
    std::uint64_t base = 0U;
    for (const auto& [identity, node] : target.nodes()) {
        static_cast<void>(node);
        base = std::max(base, identity.value);
    }
    for (const kinematic::KinematicNode& node : module.joints) {
        base = std::max(base, node.id.value);
    }
    for (kinematic::JointId id : sorted_joint_ids(module)) {
        remap.emplace(id, kinematic::JointId{++base});
    }
    return core::Result<std::map<kinematic::JointId, kinematic::JointId>>::
        success(std::move(remap));
}

core::Result<std::map<ControlId, ControlId>> control_remap(
    const RigModuleTemplate& module,
    const ControlGraph& target,
    ModuleIdPolicy policy) {
    std::map<ControlId, ControlId> remap;
    std::vector<ControlId> ids;
    ids.reserve(module.controls.size());
    for (const ControlDefinition& control : module.controls) {
        ids.push_back(control.id);
    }
    std::sort(ids.begin(), ids.end());
    if (policy == ModuleIdPolicy::preserve_fragment_ids) {
        for (ControlId id : ids) {
            if (target.find(id) != nullptr) {
                return core::Result<std::map<ControlId, ControlId>>::failure(
                    invalid("module fragment control identity collides with the target")
                        .with_context("control=" + std::to_string(id.value)));
            }
            remap.emplace(id, id);
        }
        return core::Result<std::map<ControlId, ControlId>>::success(std::move(remap));
    }
    if (policy != ModuleIdPolicy::allocate_fresh_ids) {
        return core::Result<std::map<ControlId, ControlId>>::failure(
            invalid("module id remapping policy is not supported"));
    }
    std::uint64_t base = 0U;
    for (const auto& [identity, control] : target.nodes()) {
        static_cast<void>(control);
        base = std::max(base, identity.value);
    }
    for (const ControlDefinition& control : module.controls) {
        base = std::max(base, control.id.value);
    }
    for (ControlId id : ids) {
        remap.emplace(id, ControlId{++base});
    }
    return core::Result<std::map<ControlId, ControlId>>::success(std::move(remap));
}

} // namespace

core::Result<void> RigModuleTemplate::validate() const {
    if (schema_version != kSchemaVersion) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "rig module template schema version is unsupported"));
    }
    if (!safe_token(name, kMaxNameBytes)) {
        return core::Result<void>::failure(invalid(
            "rig module template name must be a bounded safe token"));
    }
    if (joints.empty() || joints.size() > kMaxJoints) {
        return core::Result<void>::failure(invalid(
            "rig module joint fragment must be non-empty and bounded"));
    }
    if (controls.size() > kMaxControls || control_edges.size() > kMaxEdges ||
            symmetry.size() > kMaxSymmetryPairs) {
        return core::Result<void>::failure(invalid(
            "rig module control, edge, or symmetry fragment exceeds its bound"));
    }

    // Fragment identities: non-zero, unique, every node valid, and exactly
    // one attachment root whose parent is std::nullopt.
    std::set<kinematic::JointId> joint_ids;
    kinematic::JointId attachment_root{0U};
    for (const kinematic::KinematicNode& node : joints) {
        if (auto result = node.validate(); !result) return result;
        if (!joint_ids.insert(node.id).second) {
            return core::Result<void>::failure(validation(
                "rig module joint fragment identities must be unique"));
        }
        if (!node.parent.has_value()) {
            if (attachment_root) {
                return core::Result<void>::failure(validation(
                    "rig module fragment must name exactly one attachment root"));
            }
            attachment_root = node.id;
        }
    }
    if (!attachment_root) {
        return core::Result<void>::failure(validation(
            "rig module fragment must carry an explicit attachment root"));
    }
    // Parents resolve inside the fragment and every chain terminates at the
    // attachment root, so the fragment is a single rooted tree.
    for (const kinematic::KinematicNode& node : joints) {
        std::set<kinematic::JointId> chain;
        kinematic::JointId current = node.id;
        while (current != attachment_root) {
            if (!chain.insert(current).second) {
                return core::Result<void>::failure(validation(
                    "rig module joint fragment contains a parent cycle"));
            }
            const kinematic::KinematicNode* parent_node = find_joint(*this, current);
            if (parent_node == nullptr || !parent_node->parent.has_value()) {
                return core::Result<void>::failure(validation(
                    "rig module joint parent does not resolve inside the fragment"));
            }
            current = parent_node->parent.value();
        }
    }

    // Control fragment: identities unique, records valid, follow-space
    // resolves inside the fragment.
    std::set<ControlId> control_ids;
    for (const ControlDefinition& control : controls) {
        if (auto result = control.validate(); !result) return result;
        if (!control_ids.insert(control.id).second) {
            return core::Result<void>::failure(validation(
                "rig module control fragment identities must be unique"));
        }
    }
    for (const ControlDefinition& control : controls) {
        if (control.follow_space.has_value() &&
                !control_ids.contains(control.follow_space.value())) {
            return core::Result<void>::failure(validation(
                "rig module control follow-space referent is missing from the fragment"));
        }
    }
    if (!std::is_sorted(control_edges.begin(), control_edges.end()) ||
            std::adjacent_find(control_edges.begin(), control_edges.end()) !=
                control_edges.end()) {
        return core::Result<void>::failure(validation(
            "rig module control edges must be sorted and unique"));
    }
    for (const ControlEdge& edge : control_edges) {
        if (!control_ids.contains(edge.driver) || !control_ids.contains(edge.follower)) {
            return core::Result<void>::failure(validation(
                "rig module control edge references a missing fragment control"));
        }
    }

    for (const SymmetryPair& pair : symmetry) {
        if (auto result = pair.validate(); !result) return result;
        if (!joint_ids.contains(pair.left_joint) || !joint_ids.contains(pair.right_joint)) {
            return core::Result<void>::failure(validation(
                "rig module symmetry pair names a joint outside the fragment"));
        }
    }
    if (!std::is_sorted(symmetry.begin(), symmetry.end(),
            [](const SymmetryPair& first, const SymmetryPair& second) {
                return first.left_joint < second.left_joint;
            })) {
        return core::Result<void>::failure(validation(
            "rig module symmetry pairs must be sorted by left joint"));
    }
    for (std::size_t left = 0U; left < symmetry.size(); ++left) {
        for (std::size_t right = left + 1U; right < symmetry.size(); ++right) {
            if (symmetry[left].left_joint == symmetry[right].left_joint ||
                    symmetry[left].right_joint == symmetry[right].right_joint) {
                return core::Result<void>::failure(validation(
                    "rig module symmetry pairs must be unique on both sides"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<std::string> RigModuleTemplate::serialize() const {
    if (auto checked = validate(); !checked)
        return core::Result<std::string>::failure(checked.error());

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(17);
    output << kModuleMagic << ' ' << kSchemaVersion << '\n'
           << "NAME " << std::quoted(name) << '\n'
           << "JOINTS " << joints.size() << '\n';
    std::vector<const kinematic::KinematicNode*> ordered_joints;
    ordered_joints.reserve(joints.size());
    for (const auto& joint : joints) ordered_joints.push_back(&joint);
    std::sort(ordered_joints.begin(), ordered_joints.end(),
        [](const auto* left, const auto* right) { return left->id < right->id; });
    for (const auto* joint : ordered_joints) {
        output << "JOINT " << joint->id.value << ' '
               << (joint->parent ? joint->parent->value : 0U) << ' '
               << static_cast<unsigned>(joint->kind) << ' '
               << static_cast<unsigned>(joint->node_class) << ' '
               << std::quoted(joint->semantic_type) << ' ';
        write_transform(output, joint->rest);
        output << '\n';
    }

    output << "CONTROLS " << controls.size() << '\n';
    std::vector<const ControlDefinition*> ordered_controls;
    ordered_controls.reserve(controls.size());
    for (const auto& control : controls) ordered_controls.push_back(&control);
    std::sort(ordered_controls.begin(), ordered_controls.end(),
        [](const auto* left, const auto* right) { return left->id < right->id; });
    for (const auto* control : ordered_controls) {
        output << "CONTROL " << control->id.value << ' '
               << static_cast<unsigned>(control->shape) << ' ';
        write_transform(output, control->transform);
        output << ' ' << static_cast<unsigned>(control->allowed_axes);
        for (std::size_t axis = 0U; axis < kAxisCount; ++axis)
            output << ' ' << control->axis_minimum[axis] << ' '
                   << control->axis_maximum[axis];
        output << ' ' << control->affected_joints.size();
        for (const auto joint : control->affected_joints)
            output << ' ' << joint.value;
        output << ' ' << (control->follow_space ? control->follow_space->value : 0U)
               << ' ' << std::quoted(control->control_set) << '\n';
    }

    output << "EDGES " << control_edges.size() << '\n';
    for (const auto& edge : control_edges)
        output << "EDGE " << edge.driver.value << ' ' << edge.follower.value << '\n';
    output << "SYMMETRY " << symmetry.size() << '\n';
    for (const auto& pair : symmetry)
        output << "PAIR " << pair.left_joint.value << ' '
               << pair.right_joint.value << '\n';
    output << "END\n";

    std::string serialized = output.str();
    if (serialized.size() > kMaxSerializedBytes) {
        return core::Result<std::string>::failure(invalid(
            "rig module template serialization exceeds its byte bound"));
    }
    return core::Result<std::string>::success(std::move(serialized));
}

core::Result<RigModuleTemplate> RigModuleTemplate::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<RigModuleTemplate>::failure(parse_error(
            "rig module template text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic >> version) || magic != kModuleMagic) {
        return core::Result<RigModuleTemplate>::failure(parse_error(
            "rig module template framing is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<RigModuleTemplate>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "rig module template schema version is unsupported"));
    }

    RigModuleTemplate module;
    module.schema_version = version;
    if (!expect_token(input, "NAME") || !(input >> std::quoted(module.name))) {
        return core::Result<RigModuleTemplate>::failure(parse_error(
            "rig module template name record is malformed"));
    }
    std::size_t count = 0U;
    if (!expect_token(input, "JOINTS") || !read_count(input, kMaxJoints, count)) {
        return core::Result<RigModuleTemplate>::failure(parse_error(
            "rig module template joint count is malformed or over budget"));
    }
    module.joints.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        kinematic::KinematicNode joint;
        std::uint64_t parent = 0U;
        unsigned kind = 0U;
        unsigned node_class = 0U;
        if (!expect_token(input, "JOINT") || !(input >> joint.id.value >> parent >> kind >>
                node_class >> std::quoted(joint.semantic_type)) ||
            !read_transform(input, joint.rest)) {
            return core::Result<RigModuleTemplate>::failure(parse_error(
                "rig module template joint record is malformed"));
        }
        joint.parent = parent == 0U ? std::nullopt :
            std::optional<kinematic::JointId>{kinematic::JointId{parent}};
        joint.kind = static_cast<kinematic::JointKind>(kind);
        joint.node_class = static_cast<kinematic::NodeClass>(node_class);
        module.joints.push_back(std::move(joint));
    }

    if (!expect_token(input, "CONTROLS") || !read_count(input, kMaxControls, count)) {
        return core::Result<RigModuleTemplate>::failure(parse_error(
            "rig module template control count is malformed or over budget"));
    }
    module.controls.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        ControlDefinition control;
        unsigned shape = 0U;
        unsigned axes = 0U;
        if (!expect_token(input, "CONTROL") || !(input >> control.id.value >> shape) ||
            !read_transform(input, control.transform) || !(input >> axes) || axes > 0xffU) {
            return core::Result<RigModuleTemplate>::failure(parse_error(
                "rig module template control record is malformed"));
        }
        control.shape = static_cast<ControlShape>(shape);
        control.allowed_axes = static_cast<std::uint8_t>(axes);
        for (std::size_t axis = 0U; axis < kAxisCount; ++axis) {
            if (!(input >> control.axis_minimum[axis] >> control.axis_maximum[axis])) {
                return core::Result<RigModuleTemplate>::failure(parse_error(
                    "rig module template control limits are malformed"));
            }
        }
        std::size_t affected_count = 0U;
        if (!read_count(input, ControlGraph::kMaxAffectedJointsPerControl, affected_count)) {
            return core::Result<RigModuleTemplate>::failure(parse_error(
                "rig module template affected-joint count is malformed or over budget"));
        }
        control.affected_joints.reserve(affected_count);
        for (std::size_t affected = 0U; affected < affected_count; ++affected) {
            kinematic::JointId joint;
            if (!(input >> joint.value)) {
                return core::Result<RigModuleTemplate>::failure(parse_error(
                    "rig module template affected-joint identity is malformed"));
            }
            control.affected_joints.push_back(joint);
        }
        std::uint64_t follow = 0U;
        if (!(input >> follow >> std::quoted(control.control_set))) {
            return core::Result<RigModuleTemplate>::failure(parse_error(
                "rig module template control tail is malformed"));
        }
        if (follow != 0U) control.follow_space = ControlId{follow};
        module.controls.push_back(std::move(control));
    }

    if (!expect_token(input, "EDGES") || !read_count(input, kMaxEdges, count)) {
        return core::Result<RigModuleTemplate>::failure(parse_error(
            "rig module template edge count is malformed or over budget"));
    }
    module.control_edges.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        ControlEdge edge;
        if (!expect_token(input, "EDGE") || !(input >> edge.driver.value >> edge.follower.value)) {
            return core::Result<RigModuleTemplate>::failure(parse_error(
                "rig module template edge record is malformed"));
        }
        module.control_edges.push_back(edge);
    }

    if (!expect_token(input, "SYMMETRY") || !read_count(input, kMaxSymmetryPairs, count)) {
        return core::Result<RigModuleTemplate>::failure(parse_error(
            "rig module template symmetry count is malformed or over budget"));
    }
    module.symmetry.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        SymmetryPair pair;
        if (!expect_token(input, "PAIR") ||
            !(input >> pair.left_joint.value >> pair.right_joint.value)) {
            return core::Result<RigModuleTemplate>::failure(parse_error(
                "rig module template symmetry record is malformed"));
        }
        module.symmetry.push_back(pair);
    }
    if (!expect_token(input, "END")) {
        return core::Result<RigModuleTemplate>::failure(parse_error(
            "rig module template is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<RigModuleTemplate>::failure(parse_error(
            "rig module template contains trailing unbound data"));
    }
    if (auto checked = module.validate(); !checked)
        return core::Result<RigModuleTemplate>::failure(checked.error());
    const auto canonical = module.serialize();
    if (!canonical || canonical.value() != text) {
        return core::Result<RigModuleTemplate>::failure(parse_error(
            "rig module template encoding is not canonical"));
    }
    return core::Result<RigModuleTemplate>::success(std::move(module));
}

core::Result<RigModuleInstance> instantiate_rig_module(
    const RigModuleTemplate& module,
    const kinematic::KinematicGraph& target_kinematic,
    const ControlGraph& target_controls,
    ModuleIdPolicy policy) {
    if (auto result = module.validate(); !result) {
        return core::Result<RigModuleInstance>::failure(result.error());
    }
    if (auto result = target_kinematic.validate(); !result) {
        return core::Result<RigModuleInstance>::failure(result.error());
    }
    if (auto result = target_controls.validate(); !result) {
        return core::Result<RigModuleInstance>::failure(result.error());
    }

    // Capacity is checked against the TARGET bounds before anything is
    // built: the merged result must fit the target graphs' own limits, and
    // the fragment's symmetry pairs must fit the pair budget the target rig
    // (RigDefinition) admits.
    if (target_kinematic.nodes().size() + module.joints.size() >
            kinematic::KinematicGraph::kMaxNodes) {
        return core::Result<RigModuleInstance>::failure(validation(
            "module instantiation would exceed the target kinematic node bound"));
    }
    if (target_controls.nodes().size() + module.controls.size() >
            ControlGraph::kMaxControls) {
        return core::Result<RigModuleInstance>::failure(validation(
            "module instantiation would exceed the target control bound"));
    }
    if (target_controls.edges().size() + module.control_edges.size() >
            ControlGraph::kMaxEdges) {
        return core::Result<RigModuleInstance>::failure(validation(
            "module instantiation would exceed the target control edge bound"));
    }
    if (module.symmetry.size() > RigDefinition::kMaxSymmetryPairs) {
        return core::Result<RigModuleInstance>::failure(validation(
            "module symmetry pairs exceed the target rig pair bound"));
    }

    // Name collisions fail closed: a fragment semantic-type token may not
    // shadow one the target kinematic graph already carries.
    std::set<std::string_view> target_semantics;
    for (const auto& [identity, node] : target_kinematic.nodes()) {
        static_cast<void>(identity);
        if (!node.semantic_type.empty()) {
            target_semantics.insert(node.semantic_type);
        }
    }
    for (const kinematic::KinematicNode& node : module.joints) {
        if (!node.semantic_type.empty() && target_semantics.contains(node.semantic_type)) {
            return core::Result<RigModuleInstance>::failure(invalid(
                "module fragment semantic type collides with the target")
                    .with_context("semantic=" + node.semantic_type));
        }
    }

    // The whole remap is computed before any output graph is touched, so a
    // failure can never leave a half-remapped result behind.
    const auto joints = joint_remap(module, target_kinematic, policy);
    if (!joints) return core::Result<RigModuleInstance>::failure(joints.error());
    const auto controls = control_remap(module, target_controls, policy);
    if (!controls) return core::Result<RigModuleInstance>::failure(controls.error());
    const auto& joint_map = joints.value();
    const auto& control_map = controls.value();

    kinematic::JointId target_root{0U};
    for (const auto& [identity, node] : target_kinematic.nodes()) {
        static_cast<void>(identity);
        if (!node.parent.has_value()) target_root = node.id;
    }
    kinematic::KinematicGraph merged_kinematic;
    for (const auto& [identity, node] : target_kinematic.nodes()) {
        static_cast<void>(identity);
        if (auto inserted = merged_kinematic.insert_node(node); !inserted) {
            return core::Result<RigModuleInstance>::failure(inserted.error());
        }
    }
    for (kinematic::JointId fragment_id : sorted_joint_ids(module)) {
        kinematic::KinematicNode node = *find_joint(module, fragment_id);
        node.id = joint_map.at(fragment_id);
        if (node.parent.has_value()) {
            node.parent = joint_map.at(node.parent.value());
        } else if (target_root) {
            node.parent = target_root;
        }
        if (auto inserted = merged_kinematic.insert_node(std::move(node)); !inserted) {
            return core::Result<RigModuleInstance>::failure(inserted.error());
        }
    }
    if (auto valid = merged_kinematic.validate(); !valid) {
        return core::Result<RigModuleInstance>::failure(valid.error());
    }

    ControlGraph merged_controls;
    for (const auto& [identity, control] : target_controls.nodes()) {
        static_cast<void>(identity);
        if (auto inserted = merged_controls.insert(control); !inserted) {
            return core::Result<RigModuleInstance>::failure(inserted.error());
        }
    }
    for (const ControlEdge& edge : target_controls.edges()) {
        if (auto inserted = merged_controls.insert_edge(edge); !inserted) {
            return core::Result<RigModuleInstance>::failure(inserted.error());
        }
    }
    for (const ControlDefinition& fragment_control : module.controls) {
        ControlDefinition control = fragment_control;
        control.id = control_map.at(control.id);
        for (kinematic::JointId& joint : control.affected_joints) {
            joint = joint_map.at(joint);
        }
        if (control.follow_space.has_value()) {
            control.follow_space = control_map.at(control.follow_space.value());
        }
        if (auto inserted = merged_controls.insert(std::move(control)); !inserted) {
            return core::Result<RigModuleInstance>::failure(inserted.error());
        }
    }
    for (const ControlEdge& edge : module.control_edges) {
        const ControlEdge remapped{control_map.at(edge.driver), control_map.at(edge.follower)};
        if (auto inserted = merged_controls.insert_edge(remapped); !inserted) {
            return core::Result<RigModuleInstance>::failure(inserted.error());
        }
    }
    if (auto valid = merged_controls.validate(); !valid) {
        return core::Result<RigModuleInstance>::failure(valid.error());
    }

    std::vector<SymmetryPair> remapped_symmetry;
    remapped_symmetry.reserve(module.symmetry.size());
    for (const SymmetryPair& pair : module.symmetry) {
        remapped_symmetry.push_back(SymmetryPair{
            joint_map.at(pair.left_joint), joint_map.at(pair.right_joint)});
    }

    const auto source_digest = target_kinematic.canonical_digest();
    if (!source_digest) {
        return core::Result<RigModuleInstance>::failure(source_digest.error());
    }
    const auto result_digest = merged_kinematic.canonical_digest();
    if (!result_digest) {
        return core::Result<RigModuleInstance>::failure(result_digest.error());
    }

    RigModuleInstance instance;
    instance.receipt.module_name = module.name;
    instance.receipt.source_graph_digest = source_digest.value();
    instance.receipt.result_graph_digest = result_digest.value();
    instance.receipt.added_joint_count = module.joints.size();
    instance.receipt.added_control_count = module.controls.size();
    instance.receipt.added_edge_count = module.control_edges.size();
    instance.receipt.added_symmetry_pair_count = module.symmetry.size();
    instance.kinematic_graph = std::move(merged_kinematic);
    instance.control_graph = std::move(merged_controls);
    instance.symmetry = std::move(remapped_symmetry);
    return core::Result<RigModuleInstance>::success(std::move(instance));
}

} // namespace carto::character
