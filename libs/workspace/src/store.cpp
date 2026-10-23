#include <carto/workspace/store.hpp>

#include "codec.hpp"
#include "record_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <utility>

namespace carto::workspace {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}


// ---- scalar/array emission ----







std::string double_array_json(const std::vector<double>& values) {
    std::string out = "[";
    bool first = true;
    for (const double value : values) {
        if (!first) out.push_back(',');
        first = false;
        out += codec::number(value);
    }
    out.push_back(']');
    return out;
}

core::Result<std::vector<double>> double_array_from(
    std::string_view raw, std::size_t max_entries) {
    const auto elements = codec::split_array(raw);
    if (!elements) return core::Result<std::vector<double>>::failure(elements.error());
    if (elements.value().size() > max_entries) {
        return core::Result<std::vector<double>>::failure(codec::malformed(
            "workspace double array exceeds its bounded capacity"));
    }
    std::vector<double> values;
    values.reserve(elements.value().size());
    for (const std::string_view element : elements.value()) {
        const auto value = codec::parse_double(element);
        if (!value) return core::Result<std::vector<double>>::failure(value.error());
        values.push_back(value.value());
    }
    return core::Result<std::vector<double>>::success(std::move(values));
}





// ---- enum codecs ----

// Scans a foundation name function to parse a value back; the foundation owns
// the spelling, the workspace only maps text to the enumeration.

std::string_view handedness_name(character::Handedness value) noexcept {
    switch (value) {
    case character::Handedness::right: return "right";
    case character::Handedness::left: return "left";
    }
    return "unknown";
}

std::string_view axis_name(character::Axis value) noexcept {
    switch (value) {
    case character::Axis::positive_x: return "positive-x";
    case character::Axis::negative_x: return "negative-x";
    case character::Axis::positive_y: return "positive-y";
    case character::Axis::negative_y: return "negative-y";
    case character::Axis::positive_z: return "positive-z";
    case character::Axis::negative_z: return "negative-z";
    }
    return "unknown";
}

std::string_view winding_name(character::WindingOrder value) noexcept {
    switch (value) {
    case character::WindingOrder::counter_clockwise: return "counter-clockwise";
    case character::WindingOrder::clockwise: return "clockwise";
    }
    return "unknown";
}

std::string_view rotation_repr_name(character::RotationRepresentation value) noexcept {
    switch (value) {
    case character::RotationRepresentation::quaternion_normalized_positive_w:
        return "quaternion-normalized-positive-w";
    }
    return "unknown";
}

std::string_view inverse_bind_name(character::InverseBindSpace value) noexcept {
    switch (value) {
    case character::InverseBindSpace::world_space: return "world-space";
    case character::InverseBindSpace::joint_local: return "joint-local";
    }
    return "unknown";
}

std::string coordinate_policy_json(const character::CoordinatePolicy& policy) {
    codec::ObjectWriter writer;
    writer.add("handedness", handedness_name(policy.handedness));
    writer.add("up", axis_name(policy.up));
    writer.add("forward", axis_name(policy.forward));
    writer.add("meters_per_unit", policy.meters_per_unit);
    writer.add("winding", winding_name(policy.winding));
    writer.add("rotation", rotation_repr_name(policy.rotation));
    writer.add("inverse_bind", inverse_bind_name(policy.inverse_bind));
    return std::move(writer).finish();
}

core::Result<character::CoordinatePolicy> coordinate_policy_from(
    const std::vector<core::json::Member>& members) {
    character::CoordinatePolicy policy;
    const auto handedness = codec::string_member(members, "handedness");
    if (!handedness) {
        return core::Result<character::CoordinatePolicy>::failure(handedness.error());
    }
    const auto parsed_handedness = record::scan_enum(
        handedness.value(), character::Handedness::right,
        character::Handedness::left, handedness_name);
    if (!parsed_handedness) {
        return core::Result<character::CoordinatePolicy>::failure(
            parsed_handedness.error());
    }
    policy.handedness = parsed_handedness.value();
    const auto up = codec::string_member(members, "up");
    if (!up) return core::Result<character::CoordinatePolicy>::failure(up.error());
    const auto parsed_up = record::scan_enum(
        up.value(), character::Axis::positive_x, character::Axis::negative_z,
        axis_name);
    if (!parsed_up) {
        return core::Result<character::CoordinatePolicy>::failure(parsed_up.error());
    }
    policy.up = parsed_up.value();
    const auto forward = codec::string_member(members, "forward");
    if (!forward) {
        return core::Result<character::CoordinatePolicy>::failure(forward.error());
    }
    const auto parsed_forward = record::scan_enum(
        forward.value(), character::Axis::positive_x, character::Axis::negative_z,
        axis_name);
    if (!parsed_forward) {
        return core::Result<character::CoordinatePolicy>::failure(parsed_forward.error());
    }
    policy.forward = parsed_forward.value();
    const auto meters = codec::double_member(members, "meters_per_unit");
    if (!meters) {
        return core::Result<character::CoordinatePolicy>::failure(meters.error());
    }
    policy.meters_per_unit = meters.value();
    const auto winding = codec::string_member(members, "winding");
    if (!winding) {
        return core::Result<character::CoordinatePolicy>::failure(winding.error());
    }
    const auto parsed_winding = record::scan_enum(
        winding.value(), character::WindingOrder::counter_clockwise,
        character::WindingOrder::clockwise, winding_name);
    if (!parsed_winding) {
        return core::Result<character::CoordinatePolicy>::failure(parsed_winding.error());
    }
    policy.winding = parsed_winding.value();
    const auto rotation = codec::string_member(members, "rotation");
    if (!rotation) {
        return core::Result<character::CoordinatePolicy>::failure(rotation.error());
    }
    const auto parsed_rotation = record::scan_enum(
        rotation.value(),
        character::RotationRepresentation::quaternion_normalized_positive_w,
        character::RotationRepresentation::quaternion_normalized_positive_w,
        rotation_repr_name);
    if (!parsed_rotation) {
        return core::Result<character::CoordinatePolicy>::failure(parsed_rotation.error());
    }
    policy.rotation = parsed_rotation.value();
    const auto inverse_bind = codec::string_member(members, "inverse_bind");
    if (!inverse_bind) {
        return core::Result<character::CoordinatePolicy>::failure(inverse_bind.error());
    }
    const auto parsed_inverse_bind = record::scan_enum(
        inverse_bind.value(), character::InverseBindSpace::world_space,
        character::InverseBindSpace::joint_local, inverse_bind_name);
    if (!parsed_inverse_bind) {
        return core::Result<character::CoordinatePolicy>::failure(
            parsed_inverse_bind.error());
    }
    policy.inverse_bind = parsed_inverse_bind.value();
    return core::Result<character::CoordinatePolicy>::success(policy);
}



} // namespace

std::string_view bake_job_status_name(BakeJobStatus status) noexcept {
    switch (status) {
    case BakeJobStatus::queued: return "queued";
    case BakeJobStatus::cancelled: return "cancelled";
    case BakeJobStatus::receipted: return "receipted";
    }
    return "unknown";
}

core::Result<void> AttachmentBinding::validate() const {
    if (joint.value == 0U || mesh.content_digest.is_zero() ||
        !offset.finite() || !record::safe_text(role, 64U, true)) {
        return core::Result<void>::failure(invalid(
            "workspace attachment joint, mesh digest, offset, or role is invalid"));
    }
    if (auto result = mesh.validate(); !result) return result;
    return core::Result<void>::success();
}

core::Result<void> BakeJob::validate() const {
    if (job_id == 0U) {
        return core::Result<void>::failure(invalid(
            "workspace bake job carries the invalid zero identity"));
    }
    if (auto result = frame_start.validate(); !result) return result;
    if (auto result = frame_end.validate(); !result) return result;
    if (!character::same_timebase(frame_start, frame_end) ||
        character::compare(frame_start, frame_end) == std::strong_ordering::greater) {
        return core::Result<void>::failure(invalid(
            "workspace bake job frame range is inverted or mixes timebases"));
    }
    const auto frame_count =
        static_cast<std::uint64_t>(frame_end.tick - frame_start.tick + 1);
    if (frame_count > max_frames || max_frames == 0U || max_frames > kMaxFrames) {
        return core::Result<void>::failure(invalid(
            "workspace bake job exceeds its bounded frame budget"));
    }
    if (settings_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace bake job carries no settings digest"));
    }
    if (receipt_digest.has_value() != (status == BakeJobStatus::receipted)) {
        return core::Result<void>::failure(invalid(
            "workspace bake job receipt digest and status disagree"));
    }
    return core::Result<void>::success();
}

namespace {

// ---- character:: definition/rig/motion/weight/morph/garment codecs ----

std::string character_definition_json(const character::CharacterDefinition& definition) {
    codec::ObjectWriter writer;
    writer.add("id", definition.id.value);
    writer.add("project_namespace", definition.project_namespace);
    writer.add("source_revision", definition.source_revision.value());
    writer.add("source_digest", definition.source_digest);
    writer.add("root", definition.root.value);
    std::string refs = "[";
    bool first = true;
    for (const character::MeshAssetRef& ref : definition.mesh_refs) {
        if (!first) refs.push_back(',');
        first = false;
        refs += record::mesh_asset_ref_json(ref);
    }
    refs.push_back(']');
    writer.add_raw("mesh_refs", refs);
    codec::ObjectWriter rig_writer;
    rig_writer.add("rig", definition.rig.rig.value);
    rig_writer.add("graph_digest", definition.rig.graph_digest);
    writer.add_raw("rig", std::move(rig_writer).finish());
    codec::ObjectWriter control_writer;
    control_writer.add("owner", definition.control_graph.owner.value);
    control_writer.add("digest", definition.control_graph.digest);
    writer.add_raw("control_graph", std::move(control_writer).finish());
    std::string labels = "{";
    first = true;
    for (const auto& [key, value] : definition.locale_neutral_labels) {
        if (!first) labels.push_back(',');
        first = false;
        labels += codec::quote(key);
        labels.push_back(':');
        labels += codec::quote(value);
    }
    labels.push_back('}');
    writer.add_raw("locale_neutral_labels", labels);
    writer.add("coordinate_policy_digest", definition.coordinate_policy_digest);
    return std::move(writer).finish();
}

core::Result<character::CharacterDefinition> character_definition_from(
    const std::vector<core::json::Member>& members) {
    character::CharacterDefinition definition;
    const auto id = codec::uint_member(members, "id");
    if (!id) return core::Result<character::CharacterDefinition>::failure(id.error());
    definition.id = character::CharacterId{id.value()};
    const auto ns = codec::string_member(members, "project_namespace");
    if (!ns) return core::Result<character::CharacterDefinition>::failure(ns.error());
    definition.project_namespace = ns.value();
    const auto revision = codec::uint_member(members, "source_revision");
    if (!revision) {
        return core::Result<character::CharacterDefinition>::failure(revision.error());
    }
    definition.source_revision = core::Revision{revision.value()};
    const auto source_digest = codec::digest_member(members, "source_digest");
    if (!source_digest) {
        return core::Result<character::CharacterDefinition>::failure(source_digest.error());
    }
    definition.source_digest = source_digest.value();
    const auto root = codec::uint_member(members, "root");
    if (!root) return core::Result<character::CharacterDefinition>::failure(root.error());
    definition.root = scene::ObjectId{root.value()};
    const auto refs = codec::array_member(members, "mesh_refs");
    if (!refs) return core::Result<character::CharacterDefinition>::failure(refs.error());
    if (refs.value().size() > character::CharacterDefinition::kMaxMeshRefs) {
        return core::Result<character::CharacterDefinition>::failure(codec::malformed(
            "workspace mesh reference table exceeds its bounded capacity"));
    }
    for (const std::string_view raw_ref : refs.value()) {
        const auto ref_members = core::json::parse_object(raw_ref);
        if (!ref_members) {
            return core::Result<character::CharacterDefinition>::failure(ref_members.error());
        }
        const auto ref = record::mesh_asset_ref_from(ref_members.value());
        if (!ref) return core::Result<character::CharacterDefinition>::failure(ref.error());
        definition.mesh_refs.push_back(ref.value());
    }
    const auto rig = codec::object_member(members, "rig");
    if (!rig) return core::Result<character::CharacterDefinition>::failure(rig.error());
    const auto rig_id = codec::uint_member(rig.value(), "rig");
    if (!rig_id) return core::Result<character::CharacterDefinition>::failure(rig_id.error());
    definition.rig.rig = character::RigId{rig_id.value()};
    const auto rig_digest = codec::digest_member(rig.value(), "graph_digest");
    if (!rig_digest) {
        return core::Result<character::CharacterDefinition>::failure(rig_digest.error());
    }
    definition.rig.graph_digest = rig_digest.value();
    const auto control_graph = codec::object_member(members, "control_graph");
    if (!control_graph) {
        return core::Result<character::CharacterDefinition>::failure(control_graph.error());
    }
    const auto owner = codec::uint_member(control_graph.value(), "owner");
    if (!owner) return core::Result<character::CharacterDefinition>::failure(owner.error());
    definition.control_graph.owner = character::RigId{owner.value()};
    const auto control_digest = codec::digest_member(control_graph.value(), "digest");
    if (!control_digest) {
        return core::Result<character::CharacterDefinition>::failure(control_digest.error());
    }
    definition.control_graph.digest = control_digest.value();
    const auto labels = codec::object_member(members, "locale_neutral_labels");
    if (!labels) {
        return core::Result<character::CharacterDefinition>::failure(labels.error());
    }
    for (const core::json::Member& member : labels.value()) {
        const auto value = core::json::decode_string(member.raw_value);
        if (!value) {
            return core::Result<character::CharacterDefinition>::failure(value.error());
        }
        definition.locale_neutral_labels.emplace(member.key, value.value());
    }
    const auto policy_digest = codec::digest_member(members, "coordinate_policy_digest");
    if (!policy_digest) {
        return core::Result<character::CharacterDefinition>::failure(policy_digest.error());
    }
    definition.coordinate_policy_digest = policy_digest.value();
    return core::Result<character::CharacterDefinition>::success(std::move(definition));
}

std::string control_definition_json(const character::ControlDefinition& control) {
    codec::ObjectWriter writer;
    writer.add("id", control.id.value);
    writer.add("shape", character::control_shape_name(control.shape));
    writer.add_raw("transform", record::transform_json(control.transform));
    std::string joints = "[";
    bool first = true;
    for (const kinematic::JointId joint : control.affected_joints) {
        if (!first) joints.push_back(',');
        first = false;
        joints += codec::u64(joint.value);
    }
    joints.push_back(']');
    writer.add_raw("affected_joints", joints);
    writer.add("allowed_axes", static_cast<std::uint64_t>(control.allowed_axes));
    writer.add_raw("axis_minimum", double_array_json(
        std::vector<double>(control.axis_minimum.begin(), control.axis_minimum.end())));
    writer.add_raw("axis_maximum", double_array_json(
        std::vector<double>(control.axis_maximum.begin(), control.axis_maximum.end())));
    if (control.follow_space.has_value()) {
        writer.add("follow_space", control.follow_space->value);
    } else {
        writer.add_raw("follow_space", "null");
    }
    writer.add("control_set", control.control_set);
    return std::move(writer).finish();
}

core::Result<character::ControlDefinition> control_definition_from(
    const std::vector<core::json::Member>& members) {
    character::ControlDefinition control;
    const auto id = codec::uint_member(members, "id");
    if (!id) return core::Result<character::ControlDefinition>::failure(id.error());
    control.id = character::ControlId{id.value()};
    const auto shape = codec::string_member(members, "shape");
    if (!shape) return core::Result<character::ControlDefinition>::failure(shape.error());
    const auto parsed_shape = record::scan_enum(
        shape.value(), character::ControlShape::sphere, character::ControlShape::locator,
        character::control_shape_name);
    if (!parsed_shape) {
        return core::Result<character::ControlDefinition>::failure(parsed_shape.error());
    }
    control.shape = parsed_shape.value();
    const auto transform = codec::object_member(members, "transform");
    if (!transform) {
        return core::Result<character::ControlDefinition>::failure(transform.error());
    }
    const auto parsed_transform = record::transform_from(transform.value());
    if (!parsed_transform) {
        return core::Result<character::ControlDefinition>::failure(parsed_transform.error());
    }
    control.transform = parsed_transform.value();
    const auto joints = codec::uint_array_member(
        members, "affected_joints",
        character::ControlGraph::kMaxAffectedJointsPerControl);
    if (!joints) {
        return core::Result<character::ControlDefinition>::failure(joints.error());
    }
    for (const std::uint64_t joint : joints.value()) {
        control.affected_joints.push_back(kinematic::JointId{joint});
    }
    const auto axes = codec::uint_member(members, "allowed_axes");
    if (!axes) return core::Result<character::ControlDefinition>::failure(axes.error());
    if (axes.value() > character::kAllowedAxesMask) {
        return core::Result<character::ControlDefinition>::failure(codec::malformed(
            "workspace control axes exceed the allowed axis mask"));
    }
    control.allowed_axes = static_cast<std::uint8_t>(axes.value());
    const auto minimum_member = codec::find(members, "axis_minimum");
    if (minimum_member == nullptr) {
        return core::Result<character::ControlDefinition>::failure(
            codec::missing("axis_minimum"));
    }
    const auto minimum = double_array_from(
        minimum_member->raw_value, character::kAxisCount);
    if (!minimum) {
        return core::Result<character::ControlDefinition>::failure(minimum.error());
    }
    const auto maximum_member = codec::find(members, "axis_maximum");
    if (maximum_member == nullptr) {
        return core::Result<character::ControlDefinition>::failure(
            codec::missing("axis_maximum"));
    }
    const auto maximum = double_array_from(
        maximum_member->raw_value, character::kAxisCount);
    if (!maximum) {
        return core::Result<character::ControlDefinition>::failure(maximum.error());
    }
    if (minimum.value().size() != character::kAxisCount ||
        maximum.value().size() != character::kAxisCount) {
        return core::Result<character::ControlDefinition>::failure(codec::malformed(
            "workspace control axis limits must cover every axis"));
    }
    std::copy(minimum.value().begin(), minimum.value().end(), control.axis_minimum.begin());
    std::copy(maximum.value().begin(), maximum.value().end(), control.axis_maximum.begin());
    const core::json::Member* follow = codec::find(members, "follow_space");
    if (follow == nullptr) {
        return core::Result<character::ControlDefinition>::failure(
            codec::missing("follow_space"));
    }
    if (follow->raw_value != "null") {
        const auto follow_id = codec::parse_uint(follow->raw_value);
        if (!follow_id) {
            return core::Result<character::ControlDefinition>::failure(follow_id.error());
        }
        control.follow_space = character::ControlId{follow_id.value()};
    }
    const auto control_set = codec::optional_string_member(members, "control_set");
    if (!control_set) {
        return core::Result<character::ControlDefinition>::failure(control_set.error());
    }
    control.control_set = control_set.value();
    return core::Result<character::ControlDefinition>::success(std::move(control));
}

std::string rig_definition_json(const character::RigDefinition& rig) {
    codec::ObjectWriter writer;
    writer.add("id", rig.id.value);
    writer.add("kinematic_graph_digest", rig.kinematic_graph_digest);
    codec::ObjectWriter limits_writer;
    limits_writer.add("digest", rig.joint_limits.digest);
    writer.add_raw("joint_limits", std::move(limits_writer).finish());
    writer.add_raw("rest_basis", coordinate_policy_json(rig.rest_basis));
    std::string tags = "[";
    bool first = true;
    for (const std::string& tag : rig.semantic_tags) {
        if (!first) tags.push_back(',');
        first = false;
        tags += codec::quote(tag);
    }
    tags.push_back(']');
    writer.add_raw("semantic_tags", tags);
    codec::ObjectWriter graph_writer;
    std::string nodes = "[";
    first = true;
    for (const auto& [id, control] : rig.control_graph.nodes()) {
        (void)id;
        if (!first) nodes.push_back(',');
        first = false;
        nodes += control_definition_json(control);
    }
    nodes.push_back(']');
    graph_writer.add_raw("nodes", nodes);
    std::string edges = "[";
    first = true;
    for (const character::ControlEdge& edge : rig.control_graph.edges()) {
        if (!first) edges.push_back(',');
        first = false;
        codec::ObjectWriter edge_writer;
        edge_writer.add("driver", edge.driver.value);
        edge_writer.add("follower", edge.follower.value);
        edges += std::move(edge_writer).finish();
    }
    edges.push_back(']');
    graph_writer.add_raw("edges", edges);
    writer.add_raw("control_graph", std::move(graph_writer).finish());
    std::string symmetry = "[";
    first = true;
    for (const character::SymmetryPair& pair : rig.symmetry) {
        if (!first) symmetry.push_back(',');
        first = false;
        codec::ObjectWriter pair_writer;
        pair_writer.add("left", pair.left_joint.value);
        pair_writer.add("right", pair.right_joint.value);
        symmetry += std::move(pair_writer).finish();
    }
    symmetry.push_back(']');
    writer.add_raw("symmetry", symmetry);
    return std::move(writer).finish();
}

core::Result<character::RigDefinition> rig_definition_from(
    const std::vector<core::json::Member>& members) {
    character::RigDefinition rig;
    const auto id = codec::uint_member(members, "id");
    if (!id) return core::Result<character::RigDefinition>::failure(id.error());
    rig.id = character::RigId{id.value()};
    const auto graph_digest = codec::digest_member(members, "kinematic_graph_digest");
    if (!graph_digest) {
        return core::Result<character::RigDefinition>::failure(graph_digest.error());
    }
    rig.kinematic_graph_digest = graph_digest.value();
    const auto limits = codec::object_member(members, "joint_limits");
    if (!limits) return core::Result<character::RigDefinition>::failure(limits.error());
    const auto limits_digest = codec::digest_member(limits.value(), "digest");
    if (!limits_digest) {
        return core::Result<character::RigDefinition>::failure(limits_digest.error());
    }
    rig.joint_limits.digest = limits_digest.value();
    const auto rest_basis = codec::object_member(members, "rest_basis");
    if (!rest_basis) {
        return core::Result<character::RigDefinition>::failure(rest_basis.error());
    }
    const auto parsed_basis = coordinate_policy_from(rest_basis.value());
    if (!parsed_basis) {
        return core::Result<character::RigDefinition>::failure(parsed_basis.error());
    }
    rig.rest_basis = parsed_basis.value();
    const auto tags = codec::string_array_member(
        members, "semantic_tags", character::RigDefinition::kMaxSemanticTags);
    if (!tags) return core::Result<character::RigDefinition>::failure(tags.error());
    rig.semantic_tags = tags.value();
    const auto control_graph = codec::object_member(members, "control_graph");
    if (!control_graph) {
        return core::Result<character::RigDefinition>::failure(control_graph.error());
    }
    const auto nodes = codec::array_member(control_graph.value(), "nodes");
    if (!nodes) return core::Result<character::RigDefinition>::failure(nodes.error());
    if (nodes.value().size() > character::ControlGraph::kMaxControls) {
        return core::Result<character::RigDefinition>::failure(codec::malformed(
            "workspace control graph exceeds its bounded capacity"));
    }
    for (const std::string_view raw_node : nodes.value()) {
        const auto node_members = core::json::parse_object(raw_node);
        if (!node_members) {
            return core::Result<character::RigDefinition>::failure(node_members.error());
        }
        const auto control = control_definition_from(node_members.value());
        if (!control) return core::Result<character::RigDefinition>::failure(control.error());
        if (auto result = rig.control_graph.insert(control.value()); !result) {
            return core::Result<character::RigDefinition>::failure(result.error());
        }
    }
    const auto edges = codec::array_member(control_graph.value(), "edges");
    if (!edges) return core::Result<character::RigDefinition>::failure(edges.error());
    if (edges.value().size() > character::ControlGraph::kMaxEdges) {
        return core::Result<character::RigDefinition>::failure(codec::malformed(
            "workspace control edge table exceeds its bounded capacity"));
    }
    for (const std::string_view raw_edge : edges.value()) {
        const auto edge_members = core::json::parse_object(raw_edge);
        if (!edge_members) {
            return core::Result<character::RigDefinition>::failure(edge_members.error());
        }
        const auto driver = codec::uint_member(edge_members.value(), "driver");
        if (!driver) return core::Result<character::RigDefinition>::failure(driver.error());
        const auto follower = codec::uint_member(edge_members.value(), "follower");
        if (!follower) return core::Result<character::RigDefinition>::failure(follower.error());
        if (auto result = rig.control_graph.insert_edge(character::ControlEdge{
                character::ControlId{driver.value()},
                character::ControlId{follower.value()}});
            !result) {
            return core::Result<character::RigDefinition>::failure(result.error());
        }
    }
    const auto symmetry = codec::array_member(members, "symmetry");
    if (!symmetry) return core::Result<character::RigDefinition>::failure(symmetry.error());
    if (symmetry.value().size() > character::RigDefinition::kMaxSymmetryPairs) {
        return core::Result<character::RigDefinition>::failure(codec::malformed(
            "workspace symmetry table exceeds its bounded capacity"));
    }
    for (const std::string_view raw_pair : symmetry.value()) {
        const auto pair_members = core::json::parse_object(raw_pair);
        if (!pair_members) {
            return core::Result<character::RigDefinition>::failure(pair_members.error());
        }
        const auto left = codec::uint_member(pair_members.value(), "left");
        if (!left) return core::Result<character::RigDefinition>::failure(left.error());
        const auto right = codec::uint_member(pair_members.value(), "right");
        if (!right) return core::Result<character::RigDefinition>::failure(right.error());
        rig.symmetry.push_back(character::SymmetryPair{
            kinematic::JointId{left.value()}, kinematic::JointId{right.value()}});
    }
    return core::Result<character::RigDefinition>::success(std::move(rig));
}

std::string skin_weight_layer_json(const character::SkinWeightLayer& layer) {
    codec::ObjectWriter writer;
    writer.add("layer_id", layer.layer_id.value);
    writer.add("mesh_topology_digest", layer.mesh_topology_digest);
    writer.add("joint_graph_digest", layer.joint_graph_digest);
    writer.add("influenced_vertex_count", layer.influenced_vertex_count);
    writer.add("total_influence_count", layer.total_influence_count);
    writer.add_raw("locks_digest", codec::nullable_digest_json(layer.locks_digest));
    writer.add_raw("masks_digest", codec::nullable_digest_json(layer.masks_digest));
    writer.add("provenance", character::weight_provenance_name(layer.provenance));
    writer.add("provenance_digest", layer.provenance_digest);
    writer.add("staleness", character::weight_staleness_name(layer.staleness));
    writer.add("stale_since", layer.stale_since.value());
    return std::move(writer).finish();
}

core::Result<character::SkinWeightLayer> skin_weight_layer_from(
    const std::vector<core::json::Member>& members) {
    character::SkinWeightLayer layer;
    const auto layer_id = codec::uint_member(members, "layer_id");
    if (!layer_id) {
        return core::Result<character::SkinWeightLayer>::failure(layer_id.error());
    }
    layer.layer_id = character::RigLayerId{layer_id.value()};
    const auto topology = codec::digest_member(members, "mesh_topology_digest");
    if (!topology) {
        return core::Result<character::SkinWeightLayer>::failure(topology.error());
    }
    layer.mesh_topology_digest = topology.value();
    const auto joint_graph = codec::digest_member(members, "joint_graph_digest");
    if (!joint_graph) {
        return core::Result<character::SkinWeightLayer>::failure(joint_graph.error());
    }
    layer.joint_graph_digest = joint_graph.value();
    const auto influenced = codec::uint_member(members, "influenced_vertex_count");
    if (!influenced) {
        return core::Result<character::SkinWeightLayer>::failure(influenced.error());
    }
    layer.influenced_vertex_count = influenced.value();
    const auto total = codec::uint_member(members, "total_influence_count");
    if (!total) return core::Result<character::SkinWeightLayer>::failure(total.error());
    layer.total_influence_count = total.value();
    const auto locks = codec::nullable_digest_from(members, "locks_digest");
    if (!locks) return core::Result<character::SkinWeightLayer>::failure(locks.error());
    layer.locks_digest = locks.value();
    const auto masks = codec::nullable_digest_from(members, "masks_digest");
    if (!masks) return core::Result<character::SkinWeightLayer>::failure(masks.error());
    layer.masks_digest = masks.value();
    const auto provenance = codec::string_member(members, "provenance");
    if (!provenance) {
        return core::Result<character::SkinWeightLayer>::failure(provenance.error());
    }
    const auto parsed_provenance = record::scan_enum(
        provenance.value(), character::WeightProvenance::authored_paint,
        character::WeightProvenance::imported, character::weight_provenance_name);
    if (!parsed_provenance) {
        return core::Result<character::SkinWeightLayer>::failure(parsed_provenance.error());
    }
    layer.provenance = parsed_provenance.value();
    const auto provenance_digest = codec::digest_member(members, "provenance_digest");
    if (!provenance_digest) {
        return core::Result<character::SkinWeightLayer>::failure(provenance_digest.error());
    }
    layer.provenance_digest = provenance_digest.value();
    const auto staleness = codec::string_member(members, "staleness");
    if (!staleness) {
        return core::Result<character::SkinWeightLayer>::failure(staleness.error());
    }
    const auto parsed_staleness = record::scan_enum(
        staleness.value(), character::WeightStaleness::fresh,
        character::WeightStaleness::invalid, character::weight_staleness_name);
    if (!parsed_staleness) {
        return core::Result<character::SkinWeightLayer>::failure(parsed_staleness.error());
    }
    layer.staleness = parsed_staleness.value();
    const auto stale_since = codec::uint_member(members, "stale_since");
    if (!stale_since) {
        return core::Result<character::SkinWeightLayer>::failure(stale_since.error());
    }
    layer.stale_since = core::Revision{stale_since.value()};
    return core::Result<character::SkinWeightLayer>::success(std::move(layer));
}

template <typename Id>
std::string temporal_ref_json(std::uint64_t id, const assets::Sha256Digest& digest) {
    codec::ObjectWriter writer;
    writer.add("id", id);
    writer.add("digest", digest);
    return std::move(writer).finish();
}

std::string motion_document_json(const character::MotionDocument& motion) {
    codec::ObjectWriter writer;
    writer.add("id", motion.id.value);
    writer.add("timebase_ticks_per_second",
        static_cast<std::uint64_t>(motion.timebase_ticks_per_second));
    std::string clips = "[";
    bool first = true;
    for (const character::ClipRef& ref : motion.clips) {
        if (!first) clips.push_back(',');
        first = false;
        clips += temporal_ref_json<temporal::ClipId>(ref.clip.value, ref.digest);
    }
    clips.push_back(']');
    writer.add_raw("clips", clips);
    std::string channels = "[";
    first = true;
    for (const character::ChannelRef& ref : motion.channels) {
        if (!first) channels.push_back(',');
        first = false;
        channels += temporal_ref_json<temporal::ChannelId>(ref.channel.value, ref.digest);
    }
    channels.push_back(']');
    writer.add_raw("channels", channels);
    std::string sequences = "[";
    first = true;
    for (const character::SequenceRef& ref : motion.sequences) {
        if (!first) sequences.push_back(',');
        first = false;
        sequences += temporal_ref_json<temporal::SequenceId>(ref.sequence.value, ref.digest);
    }
    sequences.push_back(']');
    writer.add_raw("sequences", sequences);
    std::string events = "[";
    first = true;
    for (const character::EventRef& ref : motion.events) {
        if (!first) events.push_back(',');
        first = false;
        events += temporal_ref_json<temporal::EventId>(ref.event.value, ref.digest);
    }
    events.push_back(']');
    writer.add_raw("events", events);
    std::string targets = "[";
    first = true;
    for (const character::MotionTargetRef& target : motion.targets) {
        if (!first) targets.push_back(',');
        first = false;
        codec::ObjectWriter target_writer;
        target_writer.add("joint", target.joint.value);
        target_writer.add("control", target.control.value);
        targets += std::move(target_writer).finish();
    }
    targets.push_back(']');
    writer.add_raw("targets", targets);
    writer.add("root_motion", character::root_motion_policy_name(motion.root_motion));
    writer.add("missing_target",
        character::missing_target_handling_name(motion.missing_target));
    return std::move(writer).finish();
}

template <typename Id>
core::Result<std::pair<Id, assets::Sha256Digest>> temporal_ref_from(
    std::string_view raw) {
    const auto members = core::json::parse_object(raw);
    if (!members) {
        return core::Result<std::pair<Id, assets::Sha256Digest>>::failure(
            members.error());
    }
    const auto id = codec::uint_member(members.value(), "id");
    if (!id) {
        return core::Result<std::pair<Id, assets::Sha256Digest>>::failure(id.error());
    }
    const auto digest = codec::digest_member(members.value(), "digest");
    if (!digest) {
        return core::Result<std::pair<Id, assets::Sha256Digest>>::failure(digest.error());
    }
    return core::Result<std::pair<Id, assets::Sha256Digest>>::success(
        std::pair<Id, assets::Sha256Digest>{Id{id.value()}, digest.value()});
}

template <typename Id, typename Ref, Id Ref::*field>
core::Result<std::vector<Ref>> temporal_ref_array_from(
    std::string_view raw, std::size_t max_entries) {
    const auto elements = codec::split_array(raw);
    if (!elements) return core::Result<std::vector<Ref>>::failure(elements.error());
    if (elements.value().size() > max_entries) {
        return core::Result<std::vector<Ref>>::failure(codec::malformed(
            "workspace temporal reference table exceeds its bounded capacity"));
    }
    std::vector<Ref> refs;
    refs.reserve(elements.value().size());
    for (const std::string_view element : elements.value()) {
        const auto parsed = temporal_ref_from<Id>(element);
        if (!parsed) return core::Result<std::vector<Ref>>::failure(parsed.error());
        Ref ref;
        ref.*field = parsed.value().first;
        ref.digest = parsed.value().second;
        refs.push_back(ref);
    }
    return core::Result<std::vector<Ref>>::success(std::move(refs));
}

} // namespace

namespace {

core::Result<character::MotionDocument> motion_document_from(
    const std::vector<core::json::Member>& members) {
    using Doc = character::MotionDocument;
    character::MotionDocument motion;
    const auto id = codec::uint_member(members, "id");
    if (!id) return core::Result<Doc>::failure(id.error());
    motion.id = character::MotionDocumentId{id.value()};
    const auto timebase = codec::u32_member(members, "timebase_ticks_per_second");
    if (!timebase) return core::Result<Doc>::failure(timebase.error());
    motion.timebase_ticks_per_second = timebase.value();
    const core::json::Member* clips = codec::find(members, "clips");
    if (clips == nullptr) return core::Result<Doc>::failure(codec::missing("clips"));
    const auto parsed_clips = temporal_ref_array_from<
        temporal::ClipId, character::ClipRef, &character::ClipRef::clip>(
        clips->raw_value, Doc::kMaxTemporalRefs);
    if (!parsed_clips) return core::Result<Doc>::failure(parsed_clips.error());
    motion.clips = parsed_clips.value();
    const core::json::Member* channels = codec::find(members, "channels");
    if (channels == nullptr) return core::Result<Doc>::failure(codec::missing("channels"));
    const auto parsed_channels = temporal_ref_array_from<
        temporal::ChannelId, character::ChannelRef, &character::ChannelRef::channel>(
        channels->raw_value, Doc::kMaxTemporalRefs);
    if (!parsed_channels) return core::Result<Doc>::failure(parsed_channels.error());
    motion.channels = parsed_channels.value();
    const core::json::Member* sequences = codec::find(members, "sequences");
    if (sequences == nullptr) {
        return core::Result<Doc>::failure(codec::missing("sequences"));
    }
    const auto parsed_sequences = temporal_ref_array_from<
        temporal::SequenceId, character::SequenceRef,
        &character::SequenceRef::sequence>(
        sequences->raw_value, Doc::kMaxTemporalRefs);
    if (!parsed_sequences) return core::Result<Doc>::failure(parsed_sequences.error());
    motion.sequences = parsed_sequences.value();
    const core::json::Member* events = codec::find(members, "events");
    if (events == nullptr) return core::Result<Doc>::failure(codec::missing("events"));
    const auto parsed_events = temporal_ref_array_from<
        temporal::EventId, character::EventRef, &character::EventRef::event>(
        events->raw_value, Doc::kMaxTemporalRefs);
    if (!parsed_events) return core::Result<Doc>::failure(parsed_events.error());
    motion.events = parsed_events.value();
    const auto targets = codec::array_member(members, "targets");
    if (!targets) return core::Result<Doc>::failure(targets.error());
    if (targets.value().size() > Doc::kMaxTargets) {
        return core::Result<Doc>::failure(codec::malformed(
            "workspace motion target table exceeds its bounded capacity"));
    }
    for (const std::string_view raw_target : targets.value()) {
        const auto target_members = core::json::parse_object(raw_target);
        if (!target_members) return core::Result<Doc>::failure(target_members.error());
        const auto joint = codec::uint_member(target_members.value(), "joint");
        if (!joint) return core::Result<Doc>::failure(joint.error());
        const auto control = codec::uint_member(target_members.value(), "control");
        if (!control) return core::Result<Doc>::failure(control.error());
        motion.targets.push_back(character::MotionTargetRef{
            kinematic::JointId{joint.value()}, character::ControlId{control.value()}});
    }
    const auto root_motion = codec::string_member(members, "root_motion");
    if (!root_motion) return core::Result<Doc>::failure(root_motion.error());
    const auto parsed_root = record::scan_enum(
        root_motion.value(), character::RootMotionPolicy::authored,
        character::RootMotionPolicy::none, character::root_motion_policy_name);
    if (!parsed_root) return core::Result<Doc>::failure(parsed_root.error());
    motion.root_motion = parsed_root.value();
    const auto missing_target = codec::string_member(members, "missing_target");
    if (!missing_target) return core::Result<Doc>::failure(missing_target.error());
    const auto parsed_missing = record::scan_enum(
        missing_target.value(), character::MissingTargetHandling::fail,
        character::MissingTargetHandling::skip_with_receipt,
        character::missing_target_handling_name);
    if (!parsed_missing) return core::Result<Doc>::failure(parsed_missing.error());
    motion.missing_target = parsed_missing.value();
    return core::Result<Doc>::success(std::move(motion));
}















std::string attachment_binding_json(const AttachmentBinding& binding) {
    codec::ObjectWriter writer;
    writer.add("joint", binding.joint.value);
    writer.add_raw("mesh", record::mesh_asset_ref_json(binding.mesh));
    writer.add_raw("offset", record::transform_json(binding.offset));
    writer.add("role", binding.role);
    return std::move(writer).finish();
}

core::Result<AttachmentBinding> attachment_binding_from(
    const std::vector<core::json::Member>& members) {
    AttachmentBinding binding;
    const auto joint = codec::uint_member(members, "joint");
    if (!joint) return core::Result<AttachmentBinding>::failure(joint.error());
    binding.joint = kinematic::JointId{joint.value()};
    const auto mesh = codec::object_member(members, "mesh");
    if (!mesh) return core::Result<AttachmentBinding>::failure(mesh.error());
    const auto parsed_mesh = record::mesh_asset_ref_from(mesh.value());
    if (!parsed_mesh) return core::Result<AttachmentBinding>::failure(parsed_mesh.error());
    binding.mesh = parsed_mesh.value();
    const auto offset = codec::object_member(members, "offset");
    if (!offset) return core::Result<AttachmentBinding>::failure(offset.error());
    const auto parsed_offset = record::transform_from(offset.value());
    if (!parsed_offset) {
        return core::Result<AttachmentBinding>::failure(parsed_offset.error());
    }
    binding.offset = parsed_offset.value();
    const auto role = codec::optional_string_member(members, "role");
    if (!role) return core::Result<AttachmentBinding>::failure(role.error());
    binding.role = role.value();
    return core::Result<AttachmentBinding>::success(std::move(binding));
}

std::string bake_job_json(const BakeJob& job) {
    codec::ObjectWriter writer;
    writer.add("job_id", job.job_id);
    writer.add("frame_start", character::serialize(job.frame_start));
    writer.add("frame_end", character::serialize(job.frame_end));
    writer.add("max_frames", static_cast<std::uint64_t>(job.max_frames));
    writer.add("settings_digest", job.settings_digest);
    writer.add("status", bake_job_status_name(job.status));
    writer.add_raw("receipt_digest", codec::nullable_digest_json(job.receipt_digest));
    return std::move(writer).finish();
}

core::Result<BakeJob> bake_job_from(const std::vector<core::json::Member>& members) {
    BakeJob job;
    const auto job_id = codec::uint_member(members, "job_id");
    if (!job_id) return core::Result<BakeJob>::failure(job_id.error());
    job.job_id = job_id.value();
    const auto start = codec::string_member(members, "frame_start");
    if (!start) return core::Result<BakeJob>::failure(start.error());
    const auto parsed_start = character::deserialize_rational_time(start.value());
    if (!parsed_start) return core::Result<BakeJob>::failure(parsed_start.error());
    job.frame_start = parsed_start.value();
    const auto end = codec::string_member(members, "frame_end");
    if (!end) return core::Result<BakeJob>::failure(end.error());
    const auto parsed_end = character::deserialize_rational_time(end.value());
    if (!parsed_end) return core::Result<BakeJob>::failure(parsed_end.error());
    job.frame_end = parsed_end.value();
    const auto max_frames = codec::u32_member(members, "max_frames");
    if (!max_frames) return core::Result<BakeJob>::failure(max_frames.error());
    job.max_frames = max_frames.value();
    const auto settings = codec::digest_member(members, "settings_digest");
    if (!settings) return core::Result<BakeJob>::failure(settings.error());
    job.settings_digest = settings.value();
    const auto status = codec::string_member(members, "status");
    if (!status) return core::Result<BakeJob>::failure(status.error());
    const auto parsed_status = record::scan_enum(
        status.value(), BakeJobStatus::queued, BakeJobStatus::receipted,
        bake_job_status_name);
    if (!parsed_status) return core::Result<BakeJob>::failure(parsed_status.error());
    job.status = parsed_status.value();
    const auto receipt = codec::nullable_digest_from(members, "receipt_digest");
    if (!receipt) return core::Result<BakeJob>::failure(receipt.error());
    job.receipt_digest = receipt.value();
    if (job.receipt_digest.has_value() != (job.status == BakeJobStatus::receipted)) {
        return core::Result<BakeJob>::failure(codec::malformed(
            "workspace bake job receipt digest and status disagree"));
    }
    return core::Result<BakeJob>::success(std::move(job));
}

template <typename T>
bool sorted_and_unique(const std::vector<T>& values) {
    return std::is_sorted(values.begin(), values.end()) &&
        std::adjacent_find(values.begin(), values.end()) == values.end();
}

} // namespace

core::Result<void> CharacterSourceBundle::validate() const {
    if (auto result = definition.validate(); !result) return result;
    if (auto result = graph.validate(); !result) return result;
    if (auto result = rig.validate(); !result) return result;
    if (rig.id != definition.rig.rig) {
        return core::Result<void>::failure(validation(
            "workspace bundle rig identity disagrees with the character definition"));
    }
    if (skin.has_value() != skin_layer.has_value()) {
        return core::Result<void>::failure(validation(
            "workspace bundle must carry a skin binding and its layer record together"));
    }
    if (skin.has_value()) {
        if (auto result = skin->validate(); !result) return result;
        if (auto result = skin_layer->validate(); !result) return result;
        if (skin_layer->influenced_vertex_count != skin->influences.size()) {
            return core::Result<void>::failure(validation(
                "workspace skin layer vertex count disagrees with the binding"));
        }
        std::uint64_t total_influences = 0U;
        for (const auto& [vertex, influences] : skin->influences) {
            (void)vertex;
            total_influences += influences.size();
        }
        if (skin_layer->total_influence_count != total_influences) {
            return core::Result<void>::failure(validation(
                "workspace skin layer influence count disagrees with the binding"));
        }
    }
    if (motion.has_value()) {
        if (auto result = motion->validate(); !result) return result;
        for (const character::MotionTargetRef& target : motion->targets) {
            const bool joint_known =
                target.joint.value != 0U && graph.find(target.joint) != nullptr;
            const bool control_known = target.control.value != 0U &&
                rig.control_graph.find(target.control) != nullptr;
            if (!joint_known && !control_known) {
                return core::Result<void>::failure(validation(
                    "workspace motion target does not resolve in the bundle"));
            }
        }
    }
    if (channel.has_value()) {
        if (auto result = channel->validate(); !result) return result;
    }
    if (morphs.size() > kMaxMorphTargets ||
        !std::is_sorted(morphs.begin(), morphs.end(),
            [](const character::MorphTarget& left, const character::MorphTarget& right) {
                return left.id.value < right.id.value;
            }) ||
        std::adjacent_find(morphs.begin(), morphs.end(),
            [](const character::MorphTarget& left, const character::MorphTarget& right) {
                return left.id.value == right.id.value;
            }) != morphs.end()) {
        return core::Result<void>::failure(validation(
            "workspace morph targets are not sorted and unique within bounds"));
    }
    for (const character::MorphTarget& morph : morphs) {
        if (auto result = morph.validate(); !result) return result;
    }
    if (attachments.size() > kMaxAttachments || !sorted_and_unique(attachments)) {
        return core::Result<void>::failure(validation(
            "workspace attachments are not sorted and unique within bounds"));
    }
    for (const AttachmentBinding& attachment : attachments) {
        if (auto result = attachment.validate(); !result) return result;
        if (graph.find(attachment.joint) == nullptr) {
            return core::Result<void>::failure(validation(
                "workspace attachment references a joint outside the bundle graph"));
        }
    }
    if (garments.size() > kMaxGarments) {
        return core::Result<void>::failure(validation(
            "workspace garment binding table exceeds its bounded capacity"));
    }
    for (const character::GarmentBinding& garment : garments) {
        if (auto result = garment.validate(); !result) return result;
    }
    if (grooms.size() > kMaxGrooms) {
        return core::Result<void>::failure(validation(
            "workspace groom binding table exceeds its bounded capacity"));
    }
    for (const character::GroomBinding& groom : grooms) {
        if (auto result = groom.validate(); !result) return result;
    }
    if (bake_jobs.size() > kMaxBakeJobs || !sorted_and_unique(bake_jobs)) {
        return core::Result<void>::failure(validation(
            "workspace bake jobs are not sorted and unique within bounds"));
    }
    for (const BakeJob& job : bake_jobs) {
        if (auto result = job.validate(); !result) return result;
    }
    if (weight_locks.size() > character::SkinWeightLayer::kMaxInfluencedVertices ||
        !sorted_and_unique(weight_locks)) {
        return core::Result<void>::failure(validation(
            "workspace weight locks are not sorted and unique within bounds"));
    }
    if (review_gates.size() > kMaxReviewGates) {
        return core::Result<void>::failure(validation(
            "workspace review gate ledger exceeds its bounded capacity"));
    }
    for (const auto& [gate, status] : review_gates) {
        if (!record::safe_text(gate, kMaxGateIdBytes)) {
            return core::Result<void>::failure(invalid(
                "workspace review gate identity is out of bounds"));
        }
        if (status > character::GateStatus::not_run) {
            return core::Result<void>::failure(invalid(
                "workspace review gate status is unknown"));
        }
    }
    return core::Result<void>::success();
}

std::string CharacterSourceBundle::serialize() const {
    codec::ObjectWriter writer;
    writer.add_raw("definition", character_definition_json(definition));
    writer.add("graph", graph.serialize());
    writer.add_raw("rig", rig_definition_json(rig));
    if (skin.has_value()) {
        writer.add("skin", skin->serialize());
    } else {
        writer.add_raw("skin", "null");
    }
    if (skin_layer.has_value()) {
        writer.add_raw("skin_layer", skin_weight_layer_json(skin_layer.value()));
    } else {
        writer.add_raw("skin_layer", "null");
    }
    if (motion.has_value()) {
        writer.add_raw("motion", motion_document_json(motion.value()));
    } else {
        writer.add_raw("motion", "null");
    }
    if (channel.has_value()) {
        writer.add("channel", channel->serialize());
    } else {
        writer.add_raw("channel", "null");
    }
    std::string morphs_json = "[";
    bool first = true;
    for (const character::MorphTarget& morph : morphs) {
        if (!first) morphs_json.push_back(',');
        first = false;
        morphs_json += record::morph_target_json(morph);
    }
    morphs_json.push_back(']');
    writer.add_raw("morphs", morphs_json);
    std::string attachments_json = "[";
    first = true;
    for (const AttachmentBinding& attachment : attachments) {
        if (!first) attachments_json.push_back(',');
        first = false;
        attachments_json += attachment_binding_json(attachment);
    }
    attachments_json.push_back(']');
    writer.add_raw("attachments", attachments_json);
    std::string garments_json = "[";
    first = true;
    for (const character::GarmentBinding& garment : garments) {
        if (!first) garments_json.push_back(',');
        first = false;
        garments_json += record::garment_binding_json(garment);
    }
    garments_json.push_back(']');
    writer.add_raw("garments", garments_json);
    std::string grooms_json = "[";
    first = true;
    for (const character::GroomBinding& groom : grooms) {
        if (!first) grooms_json.push_back(',');
        first = false;
        grooms_json += record::groom_binding_json(groom);
    }
    grooms_json.push_back(']');
    writer.add_raw("grooms", grooms_json);
    std::string jobs_json = "[";
    first = true;
    for (const BakeJob& job : bake_jobs) {
        if (!first) jobs_json.push_back(',');
        first = false;
        jobs_json += bake_job_json(job);
    }
    jobs_json.push_back(']');
    writer.add_raw("bake_jobs", jobs_json);
    std::string locks = "[";
    first = true;
    for (const std::uint32_t vertex : weight_locks) {
        if (!first) locks.push_back(',');
        first = false;
        locks += codec::u64(vertex);
    }
    locks.push_back(']');
    writer.add_raw("weight_locks", locks);
    std::string gates = "{";
    first = true;
    for (const auto& [gate, status] : review_gates) {
        if (!first) gates.push_back(',');
        first = false;
        gates += codec::quote(gate);
        gates.push_back(':');
        gates += codec::quote(character::gate_status_name(status));
    }
    gates.push_back('}');
    writer.add_raw("review_gates", gates);
    return std::move(writer).finish();
}

core::Result<CharacterSourceBundle> CharacterSourceBundle::deserialize(
    std::string_view text) {
    const auto members = core::json::parse_object(text);
    if (!members) {
        return core::Result<CharacterSourceBundle>::failure(members.error());
    }
    CharacterSourceBundle bundle;
    const auto definition = codec::object_member(members.value(), "definition");
    if (!definition) {
        return core::Result<CharacterSourceBundle>::failure(definition.error());
    }
    const auto parsed_definition = character_definition_from(definition.value());
    if (!parsed_definition) {
        return core::Result<CharacterSourceBundle>::failure(parsed_definition.error());
    }
    bundle.definition = parsed_definition.value();
    const auto graph = codec::string_member(members.value(), "graph");
    if (!graph) return core::Result<CharacterSourceBundle>::failure(graph.error());
    const auto parsed_graph = kinematic::KinematicGraph::deserialize(graph.value());
    if (!parsed_graph) {
        return core::Result<CharacterSourceBundle>::failure(parsed_graph.error());
    }
    bundle.graph = parsed_graph.value();
    const auto rig = codec::object_member(members.value(), "rig");
    if (!rig) return core::Result<CharacterSourceBundle>::failure(rig.error());
    const auto parsed_rig = rig_definition_from(rig.value());
    if (!parsed_rig) return core::Result<CharacterSourceBundle>::failure(parsed_rig.error());
    bundle.rig = parsed_rig.value();
    const core::json::Member* skin = codec::find(members.value(), "skin");
    if (skin == nullptr) {
        return core::Result<CharacterSourceBundle>::failure(codec::missing("skin"));
    }
    if (skin->raw_value != "null") {
        const auto decoded = core::json::decode_string(skin->raw_value);
        if (!decoded) {
            return core::Result<CharacterSourceBundle>::failure(decoded.error());
        }
        const auto parsed_skin = kinematic::SkinBinding::deserialize(decoded.value());
        if (!parsed_skin) {
            return core::Result<CharacterSourceBundle>::failure(parsed_skin.error());
        }
        bundle.skin = parsed_skin.value();
    }
    const core::json::Member* layer = codec::find(members.value(), "skin_layer");
    if (layer == nullptr) {
        return core::Result<CharacterSourceBundle>::failure(codec::missing("skin_layer"));
    }
    if (layer->raw_value != "null") {
        const auto layer_members = core::json::parse_object(layer->raw_value);
        if (!layer_members) {
            return core::Result<CharacterSourceBundle>::failure(layer_members.error());
        }
        const auto parsed_layer = skin_weight_layer_from(layer_members.value());
        if (!parsed_layer) {
            return core::Result<CharacterSourceBundle>::failure(parsed_layer.error());
        }
        bundle.skin_layer = parsed_layer.value();
    }
    const core::json::Member* motion = codec::find(members.value(), "motion");
    if (motion == nullptr) {
        return core::Result<CharacterSourceBundle>::failure(codec::missing("motion"));
    }
    if (motion->raw_value != "null") {
        const auto motion_members = core::json::parse_object(motion->raw_value);
        if (!motion_members) {
            return core::Result<CharacterSourceBundle>::failure(motion_members.error());
        }
        const auto parsed_motion = motion_document_from(motion_members.value());
        if (!parsed_motion) {
            return core::Result<CharacterSourceBundle>::failure(parsed_motion.error());
        }
        bundle.motion = parsed_motion.value();
    }
    const core::json::Member* channel = codec::find(members.value(), "channel");
    if (channel == nullptr) {
        return core::Result<CharacterSourceBundle>::failure(codec::missing("channel"));
    }
    if (channel->raw_value != "null") {
        const auto decoded = core::json::decode_string(channel->raw_value);
        if (!decoded) {
            return core::Result<CharacterSourceBundle>::failure(decoded.error());
        }
        const auto parsed_channel = temporal::TemporalChannel::deserialize(decoded.value());
        if (!parsed_channel) {
            return core::Result<CharacterSourceBundle>::failure(parsed_channel.error());
        }
        bundle.channel = parsed_channel.value();
    }
    const auto morphs = codec::array_member(members.value(), "morphs");
    if (!morphs) return core::Result<CharacterSourceBundle>::failure(morphs.error());
    if (morphs.value().size() > kMaxMorphTargets) {
        return core::Result<CharacterSourceBundle>::failure(codec::malformed(
            "workspace morph target table exceeds its bounded capacity"));
    }
    for (const std::string_view raw_morph : morphs.value()) {
        const auto morph_members = core::json::parse_object(raw_morph);
        if (!morph_members) {
            return core::Result<CharacterSourceBundle>::failure(morph_members.error());
        }
        const auto morph = record::morph_target_from(morph_members.value());
        if (!morph) return core::Result<CharacterSourceBundle>::failure(morph.error());
        bundle.morphs.push_back(morph.value());
    }
    const auto attachments = codec::array_member(members.value(), "attachments");
    if (!attachments) {
        return core::Result<CharacterSourceBundle>::failure(attachments.error());
    }
    if (attachments.value().size() > kMaxAttachments) {
        return core::Result<CharacterSourceBundle>::failure(codec::malformed(
            "workspace attachment table exceeds its bounded capacity"));
    }
    for (const std::string_view raw_attachment : attachments.value()) {
        const auto attachment_members = core::json::parse_object(raw_attachment);
        if (!attachment_members) {
            return core::Result<CharacterSourceBundle>::failure(attachment_members.error());
        }
        const auto attachment = attachment_binding_from(attachment_members.value());
        if (!attachment) {
            return core::Result<CharacterSourceBundle>::failure(attachment.error());
        }
        bundle.attachments.push_back(attachment.value());
    }
    const auto garments = codec::array_member(members.value(), "garments");
    if (!garments) return core::Result<CharacterSourceBundle>::failure(garments.error());
    if (garments.value().size() > kMaxGarments) {
        return core::Result<CharacterSourceBundle>::failure(codec::malformed(
            "workspace garment table exceeds its bounded capacity"));
    }
    for (const std::string_view raw_garment : garments.value()) {
        const auto garment_members = core::json::parse_object(raw_garment);
        if (!garment_members) {
            return core::Result<CharacterSourceBundle>::failure(garment_members.error());
        }
        const auto garment = record::garment_binding_from(garment_members.value());
        if (!garment) return core::Result<CharacterSourceBundle>::failure(garment.error());
        bundle.garments.push_back(garment.value());
    }
    const auto grooms = codec::array_member(members.value(), "grooms");
    if (!grooms) return core::Result<CharacterSourceBundle>::failure(grooms.error());
    if (grooms.value().size() > kMaxGrooms) {
        return core::Result<CharacterSourceBundle>::failure(codec::malformed(
            "workspace groom table exceeds its bounded capacity"));
    }
    for (const std::string_view raw_groom : grooms.value()) {
        const auto groom_members = core::json::parse_object(raw_groom);
        if (!groom_members) {
            return core::Result<CharacterSourceBundle>::failure(groom_members.error());
        }
        const auto groom = record::groom_binding_from(groom_members.value());
        if (!groom) return core::Result<CharacterSourceBundle>::failure(groom.error());
        bundle.grooms.push_back(groom.value());
    }
    const auto jobs = codec::array_member(members.value(), "bake_jobs");
    if (!jobs) return core::Result<CharacterSourceBundle>::failure(jobs.error());
    if (jobs.value().size() > kMaxBakeJobs) {
        return core::Result<CharacterSourceBundle>::failure(codec::malformed(
            "workspace bake job table exceeds its bounded capacity"));
    }
    for (const std::string_view raw_job : jobs.value()) {
        const auto job_members = core::json::parse_object(raw_job);
        if (!job_members) {
            return core::Result<CharacterSourceBundle>::failure(job_members.error());
        }
        const auto job = bake_job_from(job_members.value());
        if (!job) return core::Result<CharacterSourceBundle>::failure(job.error());
        bundle.bake_jobs.push_back(job.value());
    }
    const auto locks = codec::uint_array_member(
        members.value(), "weight_locks",
        character::SkinWeightLayer::kMaxInfluencedVertices);
    if (!locks) return core::Result<CharacterSourceBundle>::failure(locks.error());
    for (const std::uint64_t vertex : locks.value()) {
        if (vertex > kinematic::SkinBinding::kMaxVertexIndex) {
            return core::Result<CharacterSourceBundle>::failure(codec::malformed(
                "workspace weight lock vertex index is out of range"));
        }
        bundle.weight_locks.push_back(static_cast<std::uint32_t>(vertex));
    }
    const auto gates = codec::object_member(members.value(), "review_gates");
    if (!gates) return core::Result<CharacterSourceBundle>::failure(gates.error());
    if (gates.value().size() > kMaxReviewGates) {
        return core::Result<CharacterSourceBundle>::failure(codec::malformed(
            "workspace review gate ledger exceeds its bounded capacity"));
    }
    for (const core::json::Member& member : gates.value()) {
        const auto decoded = core::json::decode_string(member.raw_value);
        if (!decoded) {
            return core::Result<CharacterSourceBundle>::failure(decoded.error());
        }
        const auto parsed_status = record::scan_enum(
            decoded.value(), character::GateStatus::pass,
            character::GateStatus::not_run, character::gate_status_name);
        if (!parsed_status) {
            return core::Result<CharacterSourceBundle>::failure(parsed_status.error());
        }
        bundle.review_gates.emplace(member.key, parsed_status.value());
    }
    if (auto result = bundle.validate(); !result) {
        return core::Result<CharacterSourceBundle>::failure(result.error());
    }
    return core::Result<CharacterSourceBundle>::success(std::move(bundle));
}

core::Result<void> CharacterSourceStore::insert(CharacterSourceBundle bundle) {
    if (auto result = bundle.validate(); !result) return result;
    if (bundles_.size() >= kMaxCharacters) {
        return core::Result<void>::failure(validation(
            "workspace character store has reached its bounded capacity"));
    }
    if (!bundle.definition.id) {
        return core::Result<void>::failure(invalid(
            "workspace character bundle carries the invalid zero identity"));
    }
    if (bundles_.find(bundle.definition.id) != bundles_.end()) {
        return core::Result<void>::failure(validation(
            "workspace character identity is already registered"));
    }
    bundles_.emplace(bundle.definition.id, std::move(bundle));
    return core::Result<void>::success();
}

core::Result<void> CharacterSourceStore::remove(character::CharacterId id) {
    const auto found = bundles_.find(id);
    if (found == bundles_.end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "workspace character is not registered"));
    }
    bundles_.erase(found);
    return core::Result<void>::success();
}

CharacterSourceBundle* CharacterSourceStore::find(character::CharacterId id) noexcept {
    const auto found = bundles_.find(id);
    return found == bundles_.end() ? nullptr : &found->second;
}

const CharacterSourceBundle* CharacterSourceStore::find(
    character::CharacterId id) const noexcept {
    const auto found = bundles_.find(id);
    return found == bundles_.end() ? nullptr : &found->second;
}

bool CharacterSourceStore::contains(character::CharacterId id) const noexcept {
    return bundles_.find(id) != bundles_.end();
}

core::Result<void> CharacterSourceStore::validate() const {
    if (bundles_.size() > kMaxCharacters) {
        return core::Result<void>::failure(validation(
            "workspace character store exceeds its bounded capacity"));
    }
    for (const auto& [id, bundle] : bundles_) {
        if (id != bundle.definition.id) {
            return core::Result<void>::failure(validation(
                "workspace store key disagrees with the bundle definition identity"));
        }
        if (auto result = bundle.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

std::string CharacterSourceStore::serialize() const {
    codec::ObjectWriter writer;
    writer.add("schema", static_cast<std::uint64_t>(kSchemaVersion));
    writer.add("revision", revision_.value());
    std::string characters = "[";
    bool first = true;
    for (const auto& [id, bundle] : bundles_) {
        if (!first) characters.push_back(',');
        first = false;
        codec::ObjectWriter entry_writer;
        entry_writer.add("id", id.value);
        entry_writer.add_raw("bundle", bundle.serialize());
        characters += std::move(entry_writer).finish();
    }
    characters.push_back(']');
    writer.add_raw("characters", characters);
    return std::move(writer).finish();
}

core::Result<assets::Sha256Digest> CharacterSourceStore::canonical_digest() const {
    const std::string serialized = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(serialized.data()),
            serialized.size())));
}

core::Result<CharacterSourceStore> CharacterSourceStore::deserialize(
    std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<CharacterSourceStore>::failure(codec::malformed(
            "workspace store document exceeds its bounded size"));
    }
    const auto members = core::json::parse_object(text);
    if (!members) {
        return core::Result<CharacterSourceStore>::failure(members.error());
    }
    const auto schema = codec::uint_member(members.value(), "schema");
    if (!schema) return core::Result<CharacterSourceStore>::failure(schema.error());
    if (schema.value() != kSchemaVersion) {
        return core::Result<CharacterSourceStore>::failure(codec::malformed(
            "workspace store schema version is not readable"));
    }
    CharacterSourceStore store;
    const auto revision = codec::uint_member(members.value(), "revision");
    if (!revision) return core::Result<CharacterSourceStore>::failure(revision.error());
    store.revision_ = core::Revision{revision.value()};
    const auto characters = codec::array_member(members.value(), "characters");
    if (!characters) {
        return core::Result<CharacterSourceStore>::failure(characters.error());
    }
    if (characters.value().size() > kMaxCharacters) {
        return core::Result<CharacterSourceStore>::failure(codec::malformed(
            "workspace character table exceeds its bounded capacity"));
    }
    for (const std::string_view raw_entry : characters.value()) {
        const auto entry_members = core::json::parse_object(raw_entry);
        if (!entry_members) {
            return core::Result<CharacterSourceStore>::failure(entry_members.error());
        }
        const auto id = codec::uint_member(entry_members.value(), "id");
        if (!id) return core::Result<CharacterSourceStore>::failure(id.error());
        const core::json::Member* bundle_member = codec::find(entry_members.value(), "bundle");
        if (bundle_member == nullptr) {
            return core::Result<CharacterSourceStore>::failure(codec::missing("bundle"));
        }
        const auto bundle = CharacterSourceBundle::deserialize(bundle_member->raw_value);
        if (!bundle) return core::Result<CharacterSourceStore>::failure(bundle.error());
        if (bundle.value().definition.id.value != id.value()) {
            return core::Result<CharacterSourceStore>::failure(codec::malformed(
                "workspace store entry identity disagrees with its bundle"));
        }
        if (!store.bundles_.emplace(character::CharacterId{id.value()}, bundle.value()).second) {
            return core::Result<CharacterSourceStore>::failure(codec::malformed(
                "workspace store carries a duplicate character identity"));
        }
    }
    return core::Result<CharacterSourceStore>::success(std::move(store));
}

} // namespace carto::workspace

