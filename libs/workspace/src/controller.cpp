#include <carto/workspace/controller.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::workspace {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic not_found(std::string message) {
    return core::Diagnostic(core::ErrorCode::not_found, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

// Every command kind belongs to exactly one workspace domain; a payload
// presented under the wrong domain is refused before anything else runs.
core::Result<WorkspaceDomain> domain_of(WorkspaceCommandKind kind) {
    switch (kind) {
    case WorkspaceCommandKind::rig_add_joint:
    case WorkspaceCommandKind::rig_remove_joint:
    case WorkspaceCommandKind::rig_set_rest:
    case WorkspaceCommandKind::rig_add_control:
    case WorkspaceCommandKind::rig_remove_control:
    case WorkspaceCommandKind::rig_set_control_axes:
    case WorkspaceCommandKind::rig_bind_attachment:
    case WorkspaceCommandKind::rig_unbind_attachment:
        return core::Result<WorkspaceDomain>::success(WorkspaceDomain::rig);
    case WorkspaceCommandKind::skin_paint_stamp:
    case WorkspaceCommandKind::skin_set_weight_lock:
    case WorkspaceCommandKind::skin_rebind_layer:
        return core::Result<WorkspaceDomain>::success(WorkspaceDomain::skin);
    case WorkspaceCommandKind::animate_set_key:
    case WorkspaceCommandKind::animate_remove_key:
    case WorkspaceCommandKind::animate_set_root_motion:
        return core::Result<WorkspaceDomain>::success(WorkspaceDomain::animate);
    case WorkspaceCommandKind::face_add_morph_target:
    case WorkspaceCommandKind::face_remove_morph_target:
        return core::Result<WorkspaceDomain>::success(WorkspaceDomain::face);
    case WorkspaceCommandKind::sim_bind_garment:
    case WorkspaceCommandKind::sim_unbind_garment:
    case WorkspaceCommandKind::sim_enqueue_bake:
    case WorkspaceCommandKind::sim_cancel_bake:
    case WorkspaceCommandKind::sim_discard_bake:
    case WorkspaceCommandKind::sim_attach_bake_receipt:
    case WorkspaceCommandKind::sim_clear_bake_receipt:
        return core::Result<WorkspaceDomain>::success(WorkspaceDomain::sim);
    case WorkspaceCommandKind::review_record_gate:
        return core::Result<WorkspaceDomain>::success(WorkspaceDomain::review);
    }
    return core::Result<WorkspaceDomain>::failure(invalid(
        "workspace command kind is unknown"));
}

void rewrite_expected_revision(WorkspaceCommand& command, core::Revision revision) {
    std::visit(
        [revision](auto& payload) { payload.expected_source_revision = revision; },
        command.payload);
}

// ---- canonical digest emission -------------------------------------------
//
// The controller refreshes digest-bound references after every mutation.
// The kinematic graph and the control graph own their canonical_digest()
// contracts; the character definition carries no serializer of its own, so
// its source digest is computed here over a canonical spelling of the
// durable definition content. Provenance fields (source revision and the
// digest itself) are excluded: the digest pins content, the fields pin
// provenance.

std::string digest_hex(const assets::Sha256Digest& digest) {
    return digest.hex();
}

std::string definition_content_digest_input(const character::CharacterDefinition& definition) {
    std::string out = "{";
    auto append = [&out](std::string_view key, std::string_view value) {
        if (out.size() > 1U) out.push_back(',');
        out += key;
        out.push_back('=');
        out += value;
    };
    append("id", std::to_string(definition.id.value));
    append("namespace", definition.project_namespace);
    append("root", std::to_string(definition.root.value));
    std::string refs;
    for (const character::MeshAssetRef& ref : definition.mesh_refs) {
        if (!refs.empty()) refs.push_back(';');
        refs += std::to_string(ref.asset_id);
        refs.push_back(':');
        refs += digest_hex(ref.content_digest);
    }
    append("mesh_refs", refs);
    append("rig", std::to_string(definition.rig.rig.value) + ":" +
        digest_hex(definition.rig.graph_digest));
    append("control_graph", std::to_string(definition.control_graph.owner.value) + ":" +
        digest_hex(definition.control_graph.digest));
    std::string labels;
    for (const auto& [key, value] : definition.locale_neutral_labels) {
        if (!labels.empty()) labels.push_back(';');
        labels += key;
        labels.push_back('=');
        labels += value;
    }
    append("labels", labels);
    append("coordinate_policy_digest", digest_hex(definition.coordinate_policy_digest));
    out.push_back('}');
    return out;
}

core::Result<assets::Sha256Digest> definition_source_digest(
    const character::CharacterDefinition& definition) {
    const std::string canonical = definition_content_digest_input(definition);
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size())));
}

// Rebinds every digest-bound reference in the bundle to the content it now
// carries. Runs after the mutation and before the validating admission, so a
// bundle can never persist with a dangling digest.
core::Result<void> refresh_digest_refs(CharacterSourceBundle& bundle) {
    const auto graph_digest = bundle.graph.canonical_digest();
    if (!graph_digest) return core::Result<void>::failure(graph_digest.error());
    const auto control_digest = bundle.rig.control_graph.canonical_digest();
    if (!control_digest) return core::Result<void>::failure(control_digest.error());
    bundle.rig.kinematic_graph_digest = graph_digest.value();
    bundle.definition.rig.graph_digest = graph_digest.value();
    bundle.definition.control_graph.owner = bundle.rig.id;
    bundle.definition.control_graph.digest = control_digest.value();
    const auto source_digest = definition_source_digest(bundle.definition);
    if (!source_digest) return core::Result<void>::failure(source_digest.error());
    bundle.definition.source_digest = source_digest.value();
    return core::Result<void>::success();
}

// ---- graph rebuild helpers ------------------------------------------------
//
// KinematicGraph and ControlGraph admit mutation only through validating
// insertion, so edits are committed by rebuilding from the working copy in
// deterministic topological order (parents before children), which keeps
// every intermediate insert admissible.

core::Result<void> commit_graph(
    CharacterSourceBundle& bundle, std::vector<kinematic::KinematicNode> nodes) {
    std::map<kinematic::JointId, kinematic::JointId> parents;
    for (const kinematic::KinematicNode& node : nodes) {
        parents.emplace(node.id, node.parent.value_or(kinematic::JointId{}));
    }
    const auto depth = [&parents](kinematic::JointId joint) {
        std::uint64_t level = 0U;
        kinematic::JointId current = joint;
        while (parents.contains(current) && parents.at(current)) {
            current = parents.at(current);
            ++level;
        }
        return level;
    };
    std::sort(nodes.begin(), nodes.end(), [&](const auto& left, const auto& right) {
        const std::uint64_t left_depth = depth(left.id);
        const std::uint64_t right_depth = depth(right.id);
        if (left_depth != right_depth) return left_depth < right_depth;
        return left.id < right.id;
    });
    kinematic::KinematicGraph graph;
    for (const kinematic::KinematicNode& node : nodes) {
        if (auto result = graph.insert_node(node); !result) return result;
    }
    bundle.graph = std::move(graph);
    return core::Result<void>::success();
}

core::Result<void> commit_control_graph(
    CharacterSourceBundle& bundle,
    std::vector<character::ControlDefinition> controls,
    std::vector<character::ControlEdge> edges) {
    character::ControlGraph graph;
    for (const character::ControlDefinition& control : controls) {
        if (auto result = graph.insert(control); !result) return result;
    }
    for (const character::ControlEdge& edge : edges) {
        if (auto result = graph.insert_edge(edge); !result) return result;
    }
    bundle.rig.control_graph = std::move(graph);
    return core::Result<void>::success();
}

std::vector<kinematic::KinematicNode> graph_nodes(const kinematic::KinematicGraph& graph) {
    std::vector<kinematic::KinematicNode> nodes;
    nodes.reserve(graph.nodes().size());
    for (const auto& [identity, node] : graph.nodes()) {
        static_cast<void>(identity);
        nodes.push_back(node);
    }
    return nodes;
}

// ---- referential validation ------------------------------------------------
//
// Pure payload validation cannot see the store; these checks bind every
// command to the target bundle before anything is allowed to mutate: joints,
// parents, controls and skin must exist or not exist exactly as the command
// kind requires, and structural removals must leave no dangling referent.

bool joint_referenced(const CharacterSourceBundle& bundle, kinematic::JointId joint) {
    for (const auto& [identity, control] : bundle.rig.control_graph.nodes()) {
        static_cast<void>(identity);
        if (std::find(control.affected_joints.begin(), control.affected_joints.end(),
                      joint) != control.affected_joints.end()) {
            return true;
        }
    }
    for (const AttachmentBinding& attachment : bundle.attachments) {
        if (attachment.joint == joint) return true;
    }
    if (bundle.motion.has_value()) {
        for (const character::MotionTargetRef& target : bundle.motion->targets) {
            if (target.joint == joint) return true;
        }
    }
    if (bundle.skin.has_value()) {
        for (const auto& [vertex, influences] : bundle.skin->influences) {
            static_cast<void>(vertex);
            for (const kinematic::JointInfluence& influence : influences) {
                if (influence.joint == joint) return true;
            }
        }
    }
    return false;
}

core::Result<void> validate_referential(
    const WorkspaceCommand& command, const CharacterSourceBundle& bundle) {
    switch (command.kind()) {
    case WorkspaceCommandKind::rig_add_joint: {
        const auto& payload = std::get<RigAddJoint>(command.payload);
        if (bundle.graph.find(payload.joint) != nullptr) {
            return core::Result<void>::failure(validation(
                "workspace joint is already present in the character graph"));
        }
        if (payload.parent.has_value()) {
            if (bundle.graph.find(payload.parent.value()) == nullptr) {
                return core::Result<void>::failure(not_found(
                    "workspace joint parent is not present in the character graph"));
            }
        } else if (!bundle.graph.nodes().empty()) {
            return core::Result<void>::failure(validation(
                "workspace root joint would introduce a second graph root"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::rig_remove_joint: {
        const auto& payload = std::get<RigRemoveJoint>(command.payload);
        if (bundle.graph.find(payload.joint) == nullptr) {
            return core::Result<void>::failure(not_found(
                "workspace joint is not present in the character graph"));
        }
        for (const auto& [identity, node] : bundle.graph.nodes()) {
            static_cast<void>(identity);
            if (node.parent.has_value() && node.parent.value() == payload.joint) {
                return core::Result<void>::failure(validation(
                    "workspace joint still carries children in the character graph"));
            }
        }
        if (joint_referenced(bundle, payload.joint)) {
            return core::Result<void>::failure(validation(
                "workspace joint is still referenced by controls, attachments, "
                "motion targets, or skin influences"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::rig_set_rest: {
        const auto& payload = std::get<RigSetRest>(command.payload);
        if (bundle.graph.find(payload.joint) == nullptr) {
            return core::Result<void>::failure(not_found(
                "workspace joint is not present in the character graph"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::rig_add_control: {
        const auto& payload = std::get<RigAddControl>(command.payload);
        if (bundle.rig.control_graph.find(payload.control) != nullptr) {
            return core::Result<void>::failure(validation(
                "workspace control is already present in the character rig"));
        }
        for (const kinematic::JointId joint : payload.affected_joints) {
            if (bundle.graph.find(joint) == nullptr) {
                return core::Result<void>::failure(not_found(
                    "workspace control affects a joint outside the bundle graph"));
            }
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::rig_remove_control: {
        const auto& payload = std::get<RigRemoveControl>(command.payload);
        if (bundle.rig.control_graph.find(payload.control) == nullptr) {
            return core::Result<void>::failure(not_found(
                "workspace control is not present in the character rig"));
        }
        for (const auto& [identity, control] : bundle.rig.control_graph.nodes()) {
            static_cast<void>(identity);
            if (control.follow_space.has_value() &&
                control.follow_space.value() == payload.control) {
                return core::Result<void>::failure(validation(
                    "workspace control is still a follow-space referent"));
            }
        }
        for (const character::ControlEdge& edge : bundle.rig.control_graph.edges()) {
            if (edge.driver == payload.control || edge.follower == payload.control) {
                return core::Result<void>::failure(validation(
                    "workspace control is still referenced by a control edge"));
            }
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::rig_set_control_axes: {
        const auto& payload = std::get<RigSetControlAxes>(command.payload);
        if (bundle.rig.control_graph.find(payload.control) == nullptr) {
            return core::Result<void>::failure(not_found(
                "workspace control is not present in the character rig"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::rig_bind_attachment: {
        const auto& payload = std::get<RigBindAttachment>(command.payload);
        if (bundle.graph.find(payload.joint) == nullptr) {
            return core::Result<void>::failure(not_found(
                "workspace attachment joint is not present in the character graph"));
        }
        const AttachmentBinding key{payload.joint, payload.mesh};
        if (std::binary_search(bundle.attachments.begin(), bundle.attachments.end(), key)) {
            return core::Result<void>::failure(validation(
                "workspace attachment is already recorded on the character"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::rig_unbind_attachment: {
        const auto& payload = std::get<RigUnbindAttachment>(command.payload);
        const auto matches = [&payload](const AttachmentBinding& attachment) {
            return attachment.joint == payload.joint &&
                attachment.mesh.asset_id == payload.mesh_asset;
        };
        if (std::find_if(bundle.attachments.begin(), bundle.attachments.end(), matches) ==
            bundle.attachments.end()) {
            return core::Result<void>::failure(not_found(
                "workspace attachment is not recorded on the character"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::skin_paint_stamp: {
        const auto& payload = std::get<SkinPaintStamp>(command.payload);
        if (!bundle.skin.has_value()) {
            return core::Result<void>::failure(not_found(
                "workspace character carries no skin binding"));
        }
        for (const kinematic::JointInfluence& influence : payload.influences) {
            if (bundle.graph.find(influence.joint) == nullptr) {
                return core::Result<void>::failure(not_found(
                    "workspace paint stamp references a joint outside the bundle graph"));
            }
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::skin_set_weight_lock:
        return core::Result<void>::success();
    case WorkspaceCommandKind::skin_rebind_layer: {
        const auto& payload = std::get<SkinRebindLayer>(command.payload);
        if (!bundle.skin_layer.has_value()) {
            return core::Result<void>::failure(not_found(
                "workspace character carries no skin layer"));
        }
        if (payload.previous_staleness.has_value()) {
            // Inverse bookkeeping restores recorded state; the recorded
            // digest was already checked against this graph when recorded.
            return core::Result<void>::success();
        }
        const auto graph_digest = bundle.graph.canonical_digest();
        if (!graph_digest) return core::Result<void>::failure(graph_digest.error());
        if (payload.joint_graph_digest != graph_digest.value()) {
            return core::Result<void>::failure(stale(
                "workspace skin rebind presents a stale joint graph digest"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::animate_set_key: {
        const auto& payload = std::get<AnimateSetKey>(command.payload);
        if (!bundle.channel.has_value() || bundle.channel->id != payload.channel) {
            return core::Result<void>::failure(not_found(
                "workspace channel is not present in the character bundle"));
        }
        if (bundle.channel->value_kind != temporal::ChannelValueKind::scalar) {
            return core::Result<void>::failure(validation(
                "workspace channel does not carry scalar keys"));
        }
        const double seconds = character::to_seconds(payload.time).seconds;
        if (seconds < 0.0 || seconds > bundle.channel->duration) {
            return core::Result<void>::failure(validation(
                "workspace key time lies outside the channel duration"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::animate_remove_key: {
        const auto& payload = std::get<AnimateRemoveKey>(command.payload);
        if (!bundle.channel.has_value() || bundle.channel->id != payload.channel) {
            return core::Result<void>::failure(not_found(
                "workspace channel is not present in the character bundle"));
        }
        if (bundle.channel->value_kind != temporal::ChannelValueKind::scalar) {
            return core::Result<void>::failure(validation(
                "workspace channel does not carry scalar keys"));
        }
        const double seconds = character::to_seconds(payload.time).seconds;
        const auto matches = [seconds](const temporal::ScalarKeyframe& key) {
            return key.time == seconds;
        };
        if (std::find_if(bundle.channel->scalar_keyframes.begin(),
                         bundle.channel->scalar_keyframes.end(), matches) ==
            bundle.channel->scalar_keyframes.end()) {
            return core::Result<void>::failure(not_found(
                "workspace channel carries no key at the removed time"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::animate_set_root_motion: {
        if (!bundle.motion.has_value()) {
            return core::Result<void>::failure(not_found(
                "workspace character carries no motion document"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::face_add_morph_target: {
        const auto& payload = std::get<FaceAddMorphTarget>(command.payload);
        const auto matches = [&payload](const character::MorphTarget& morph) {
            return morph.id == payload.morph.id;
        };
        if (std::find_if(bundle.morphs.begin(), bundle.morphs.end(), matches) !=
            bundle.morphs.end()) {
            return core::Result<void>::failure(validation(
                "workspace morph target is already present in the character bundle"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::face_remove_morph_target: {
        const auto& payload = std::get<FaceRemoveMorphTarget>(command.payload);
        const auto matches = [&payload](const character::MorphTarget& morph) {
            return morph.id == payload.morph;
        };
        if (std::find_if(bundle.morphs.begin(), bundle.morphs.end(), matches) ==
            bundle.morphs.end()) {
            return core::Result<void>::failure(not_found(
                "workspace morph target is not present in the character bundle"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_bind_garment: {
        const auto& payload = std::get<SimBindGarment>(command.payload);
        const auto matches = [&payload](const character::GarmentBinding& binding) {
            return binding.id == payload.binding.id;
        };
        if (std::find_if(bundle.garments.begin(), bundle.garments.end(), matches) !=
            bundle.garments.end()) {
            return core::Result<void>::failure(validation(
                "workspace garment binding is already present in the character bundle"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_unbind_garment: {
        const auto& payload = std::get<SimUnbindGarment>(command.payload);
        const auto matches = [&payload](const character::GarmentBinding& binding) {
            return binding.id == payload.binding;
        };
        if (std::find_if(bundle.garments.begin(), bundle.garments.end(), matches) ==
            bundle.garments.end()) {
            return core::Result<void>::failure(not_found(
                "workspace garment binding is not present in the character bundle"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_enqueue_bake: {
        const auto& payload = std::get<SimEnqueueBake>(command.payload);
        const auto matches = [&payload](const BakeJob& job) {
            return job.job_id == payload.job_id;
        };
        if (std::find_if(bundle.bake_jobs.begin(), bundle.bake_jobs.end(), matches) !=
            bundle.bake_jobs.end()) {
            return core::Result<void>::failure(validation(
                "workspace bake job is already queued on the character"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_cancel_bake: {
        const auto& payload = std::get<SimCancelBake>(command.payload);
        const BakeJob* job = nullptr;
        for (const BakeJob& candidate : bundle.bake_jobs) {
            if (candidate.job_id == payload.job_id) job = &candidate;
        }
        if (job == nullptr) {
            return core::Result<void>::failure(not_found(
                "workspace bake job is not registered"));
        }
        if (job->status != BakeJobStatus::queued) {
            return core::Result<void>::failure(validation(
                "workspace bake job is not queued"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_discard_bake: {
        const auto& payload = std::get<SimDiscardBake>(command.payload);
        const BakeJob* job = nullptr;
        for (const BakeJob& candidate : bundle.bake_jobs) {
            if (candidate.job_id == payload.job_id) job = &candidate;
        }
        if (job == nullptr) {
            return core::Result<void>::failure(not_found(
                "workspace bake job is not registered"));
        }
        if (job->status == BakeJobStatus::receipted) {
            return core::Result<void>::failure(validation(
                "workspace bake job carries a receipt and may not be discarded"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_attach_bake_receipt: {
        const auto& payload = std::get<SimAttachBakeReceipt>(command.payload);
        const BakeJob* job = nullptr;
        for (const BakeJob& candidate : bundle.bake_jobs) {
            if (candidate.job_id == payload.job_id) job = &candidate;
        }
        if (job == nullptr) {
            return core::Result<void>::failure(not_found(
                "workspace bake job is not registered"));
        }
        if (job->status != BakeJobStatus::queued) {
            return core::Result<void>::failure(validation(
                "workspace bake job is not queued"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_clear_bake_receipt: {
        const auto& payload = std::get<SimClearBakeReceipt>(command.payload);
        const BakeJob* job = nullptr;
        for (const BakeJob& candidate : bundle.bake_jobs) {
            if (candidate.job_id == payload.job_id) job = &candidate;
        }
        if (job == nullptr) {
            return core::Result<void>::failure(not_found(
                "workspace bake job is not registered"));
        }
        if (job->status != BakeJobStatus::receipted) {
            return core::Result<void>::failure(validation(
                "workspace bake job carries no attached receipt"));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::review_record_gate:
        return core::Result<void>::success();
    }
    return core::Result<void>::failure(validation(
        "workspace command kind is unknown"));
}

// ---- mutation ---------------------------------------------------------------
//
// Every mutation runs on a working copy of the target bundle; the store is
// touched only after the copy validates and the journal accepts the entry.

core::Result<void> mutate_bundle(
    CharacterSourceBundle& bundle, const WorkspaceCommand& command) {
    switch (command.kind()) {
    case WorkspaceCommandKind::rig_add_joint: {
        const auto& payload = std::get<RigAddJoint>(command.payload);
        kinematic::KinematicNode node;
        node.id = payload.joint;
        node.parent = payload.parent;
        node.rest = payload.rest;
        node.kind = payload.kind;
        node.semantic_type = payload.semantic_type;
        return bundle.graph.insert_node(std::move(node));
    }
    case WorkspaceCommandKind::rig_remove_joint: {
        const auto& payload = std::get<RigRemoveJoint>(command.payload);
        std::vector<kinematic::KinematicNode> nodes = graph_nodes(bundle.graph);
        const auto removed = std::remove_if(
            nodes.begin(), nodes.end(),
            [&payload](const kinematic::KinematicNode& node) {
                return node.id == payload.joint;
            });
        nodes.erase(removed, nodes.end());
        return commit_graph(bundle, std::move(nodes));
    }
    case WorkspaceCommandKind::rig_set_rest: {
        const auto& payload = std::get<RigSetRest>(command.payload);
        std::vector<kinematic::KinematicNode> nodes = graph_nodes(bundle.graph);
        for (kinematic::KinematicNode& node : nodes) {
            if (node.id == payload.joint) node.rest = payload.rest;
        }
        return commit_graph(bundle, std::move(nodes));
    }
    case WorkspaceCommandKind::rig_add_control: {
        const auto& payload = std::get<RigAddControl>(command.payload);
        character::ControlDefinition control;
        control.id = payload.control;
        control.shape = payload.shape;
        control.transform = payload.rest;
        control.affected_joints = payload.affected_joints;
        control.allowed_axes = payload.allowed_axes;
        control.control_set = payload.control_set;
        return bundle.rig.control_graph.insert(std::move(control));
    }
    case WorkspaceCommandKind::rig_remove_control: {
        const auto& payload = std::get<RigRemoveControl>(command.payload);
        std::vector<character::ControlDefinition> controls;
        for (const auto& [identity, control] : bundle.rig.control_graph.nodes()) {
            static_cast<void>(identity);
            if (control.id != payload.control) controls.push_back(control);
        }
        return commit_control_graph(
            bundle, std::move(controls), bundle.rig.control_graph.edges());
    }
    case WorkspaceCommandKind::rig_set_control_axes: {
        const auto& payload = std::get<RigSetControlAxes>(command.payload);
        std::vector<character::ControlDefinition> controls;
        for (const auto& [identity, control] : bundle.rig.control_graph.nodes()) {
            static_cast<void>(identity);
            controls.push_back(control);
            if (control.id == payload.control) controls.back().allowed_axes = payload.allowed_axes;
        }
        return commit_control_graph(
            bundle, std::move(controls), bundle.rig.control_graph.edges());
    }
    case WorkspaceCommandKind::rig_bind_attachment: {
        const auto& payload = std::get<RigBindAttachment>(command.payload);
        AttachmentBinding binding;
        binding.joint = payload.joint;
        binding.mesh = payload.mesh;
        binding.offset = payload.offset;
        binding.role = payload.role;
        const auto position = std::lower_bound(
            bundle.attachments.begin(), bundle.attachments.end(), binding);
        bundle.attachments.insert(position, std::move(binding));
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::rig_unbind_attachment: {
        const auto& payload = std::get<RigUnbindAttachment>(command.payload);
        const auto matches = [&payload](const AttachmentBinding& attachment) {
            return attachment.joint == payload.joint &&
                attachment.mesh.asset_id == payload.mesh_asset;
        };
        const auto removed = std::remove_if(
            bundle.attachments.begin(), bundle.attachments.end(), matches);
        bundle.attachments.erase(removed, bundle.attachments.end());
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::skin_paint_stamp: {
        const auto& payload = std::get<SkinPaintStamp>(command.payload);
        kinematic::SkinBinding& skin = bundle.skin.value();
        character::SkinWeightLayer& layer = bundle.skin_layer.value();
        const auto found = skin.influences.find(payload.vertex);
        const std::uint64_t previous_count =
            found == skin.influences.end() ? 0U
                : static_cast<std::uint64_t>(found->second.size());
        skin.influences[payload.vertex] = payload.influences;
        if (found == skin.influences.end()) {
            ++layer.influenced_vertex_count;
        }
        layer.total_influence_count +=
            static_cast<std::uint64_t>(payload.influences.size()) - previous_count;
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::skin_set_weight_lock: {
        const auto& payload = std::get<SkinSetWeightLock>(command.payload);
        const auto position = std::lower_bound(
            bundle.weight_locks.begin(), bundle.weight_locks.end(), payload.vertex);
        const bool locked = position != bundle.weight_locks.end() &&
            *position == payload.vertex;
        if (payload.locked && !locked) {
            bundle.weight_locks.insert(position, payload.vertex);
        } else if (!payload.locked && locked) {
            bundle.weight_locks.erase(position);
        }
        if (bundle.skin_layer.has_value() && bundle.skin_layer->locks_digest.has_value()) {
            // Re-pin the locks digest to the lock population the bundle now
            // carries, in the same canonical array spelling the store writes.
            std::string canonical = "[";
            bool first = true;
            for (const std::uint32_t vertex : bundle.weight_locks) {
                if (!first) canonical.push_back(',');
                first = false;
                canonical += std::to_string(vertex);
            }
            canonical.push_back(']');
            bundle.skin_layer->locks_digest = assets::sha256(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(canonical.data()),
                    canonical.size()));
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::skin_rebind_layer: {
        const auto& payload = std::get<SkinRebindLayer>(command.payload);
        character::SkinWeightLayer& layer = bundle.skin_layer.value();
        if (payload.previous_staleness.has_value()) {
            layer.staleness = payload.previous_staleness.value();
            layer.stale_since = payload.previous_stale_since.value_or(core::Revision{});
            layer.joint_graph_digest =
                payload.previous_joint_graph_digest.value_or(payload.joint_graph_digest);
        } else {
            layer.staleness = character::WeightStaleness::fresh;
            layer.stale_since = core::Revision{};
            layer.joint_graph_digest = payload.joint_graph_digest;
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::animate_set_key: {
        const auto& payload = std::get<AnimateSetKey>(command.payload);
        temporal::TemporalChannel& channel = bundle.channel.value();
        const double seconds = character::to_seconds(payload.time).seconds;
        auto& keys = channel.scalar_keyframes;
        const auto displaced = std::remove_if(
            keys.begin(), keys.end(),
            [seconds](const temporal::ScalarKeyframe& key) {
                return key.time == seconds;
            });
        keys.erase(displaced, keys.end());
        const temporal::ScalarKeyframe key{seconds, payload.value};
        const auto position = std::lower_bound(
            keys.begin(), keys.end(), key,
            [](const temporal::ScalarKeyframe& left,
               const temporal::ScalarKeyframe& right) {
                return left.time < right.time;
            });
        keys.insert(position, key);
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::animate_remove_key: {
        const auto& payload = std::get<AnimateRemoveKey>(command.payload);
        temporal::TemporalChannel& channel = bundle.channel.value();
        const double seconds = character::to_seconds(payload.time).seconds;
        auto& keys = channel.scalar_keyframes;
        const auto removed = std::remove_if(
            keys.begin(), keys.end(),
            [seconds](const temporal::ScalarKeyframe& key) {
                return key.time == seconds;
            });
        keys.erase(removed, keys.end());
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::animate_set_root_motion: {
        const auto& payload = std::get<AnimateSetRootMotion>(command.payload);
        bundle.motion.value().root_motion = payload.policy;
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::face_add_morph_target: {
        const auto& payload = std::get<FaceAddMorphTarget>(command.payload);
        const auto order = [](const character::MorphTarget& left,
                              const character::MorphTarget& right) {
            return left.id.value < right.id.value;
        };
        const auto position = std::lower_bound(
            bundle.morphs.begin(), bundle.morphs.end(), payload.morph, order);
        bundle.morphs.insert(position, payload.morph);
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::face_remove_morph_target: {
        const auto& payload = std::get<FaceRemoveMorphTarget>(command.payload);
        const auto removed = std::remove_if(
            bundle.morphs.begin(), bundle.morphs.end(),
            [&payload](const character::MorphTarget& morph) {
                return morph.id == payload.morph;
            });
        bundle.morphs.erase(removed, bundle.morphs.end());
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_bind_garment: {
        const auto& payload = std::get<SimBindGarment>(command.payload);
        bundle.garments.push_back(payload.binding);
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_unbind_garment: {
        const auto& payload = std::get<SimUnbindGarment>(command.payload);
        const auto removed = std::remove_if(
            bundle.garments.begin(), bundle.garments.end(),
            [&payload](const character::GarmentBinding& binding) {
                return binding.id == payload.binding;
            });
        bundle.garments.erase(removed, bundle.garments.end());
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_enqueue_bake: {
        const auto& payload = std::get<SimEnqueueBake>(command.payload);
        BakeJob job;
        job.job_id = payload.job_id;
        job.frame_start = payload.frame_start;
        job.frame_end = payload.frame_end;
        job.max_frames = payload.max_frames;
        job.settings_digest = payload.settings_digest;
        const auto position = std::lower_bound(
            bundle.bake_jobs.begin(), bundle.bake_jobs.end(), job);
        bundle.bake_jobs.insert(position, std::move(job));
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_cancel_bake: {
        const auto& payload = std::get<SimCancelBake>(command.payload);
        for (BakeJob& job : bundle.bake_jobs) {
            if (job.job_id == payload.job_id) job.status = BakeJobStatus::cancelled;
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_discard_bake: {
        const auto& payload = std::get<SimDiscardBake>(command.payload);
        const auto removed = std::remove_if(
            bundle.bake_jobs.begin(), bundle.bake_jobs.end(),
            [&payload](const BakeJob& job) { return job.job_id == payload.job_id; });
        bundle.bake_jobs.erase(removed, bundle.bake_jobs.end());
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_attach_bake_receipt: {
        const auto& payload = std::get<SimAttachBakeReceipt>(command.payload);
        for (BakeJob& job : bundle.bake_jobs) {
            if (job.job_id == payload.job_id) {
                job.status = BakeJobStatus::receipted;
                job.receipt_digest = payload.receipt_digest;
            }
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::sim_clear_bake_receipt: {
        const auto& payload = std::get<SimClearBakeReceipt>(command.payload);
        for (BakeJob& job : bundle.bake_jobs) {
            if (job.job_id == payload.job_id) {
                job.status = BakeJobStatus::queued;
                job.receipt_digest = std::nullopt;
            }
        }
        return core::Result<void>::success();
    }
    case WorkspaceCommandKind::review_record_gate: {
        const auto& payload = std::get<ReviewRecordGate>(command.payload);
        const auto found = bundle.review_gates.find(payload.gate_id);
        if (payload.previous_status.has_value()) {
            // Inverse bookkeeping: restore the recorded prior status.
            bundle.review_gates[payload.gate_id] = payload.status;
        } else if (found != bundle.review_gates.end()) {
            // An inverse for a gate that did not exist before the original
            // command erases the entry again.
            bundle.review_gates.erase(found);
        } else {
            bundle.review_gates.emplace(payload.gate_id, payload.status);
        }
        return core::Result<void>::success();
    }
    }
    return core::Result<void>::failure(validation(
        "workspace command kind is unknown"));
}

} // namespace

WorkspaceController::WorkspaceController(std::filesystem::path journal_path)
    : journal_(std::move(journal_path)) {
    for (std::size_t index = 0U; index < sessions_.size(); ++index) {
        sessions_[index].domain = static_cast<WorkspaceDomain>(index);
    }
}

WorkspaceSession& WorkspaceController::session(WorkspaceDomain domain) noexcept {
    return sessions_[static_cast<std::size_t>(domain)];
}

const WorkspaceSession& WorkspaceController::session(WorkspaceDomain domain) const noexcept {
    return sessions_[static_cast<std::size_t>(domain)];
}

core::Result<WorkspaceApplyReceipt> WorkspaceController::apply(
    const WorkspaceCommand& command) {
    return apply_impl(command, true);
}

core::Result<WorkspaceApplyReceipt> WorkspaceController::apply_impl(
    const WorkspaceCommand& command, bool record_undo) {
    if (auto result = command.validate(); !result) {
        return core::Result<WorkspaceApplyReceipt>::failure(result.error());
    }
    const auto kind_domain = domain_of(command.kind());
    if (!kind_domain) {
        return core::Result<WorkspaceApplyReceipt>::failure(kind_domain.error());
    }
    if (kind_domain.value() != command.domain) {
        return core::Result<WorkspaceApplyReceipt>::failure(invalid(
            "workspace command payload does not belong to its declared domain"));
    }
    WorkspaceSession& target_session = session(command.domain);
    if (target_session.target_character &&
        target_session.target_character != command.target_character) {
        return core::Result<WorkspaceApplyReceipt>::failure(invalid(
            "workspace session is bound to a different target character"));
    }
    const core::Revision revision_before = store_.revision();
    if (command.expected_source_revision() != revision_before) {
        return core::Result<WorkspaceApplyReceipt>::failure(stale(
            "workspace command was authored against a stale source revision"));
    }
    CharacterSourceBundle* bundle = store_.find(command.target_character);
    if (bundle == nullptr) {
        return core::Result<WorkspaceApplyReceipt>::failure(not_found(
            "workspace character is not registered"));
    }
    if (auto result = validate_referential(command, *bundle); !result) {
        return core::Result<WorkspaceApplyReceipt>::failure(result.error());
    }
    // The exact inverse is computed against the pre-mutation state; a
    // command without an exact inverse is not admissible, so undo stays a
    // forward journal entry for every committed command.
    const auto inverse = invert_command(command, store_);
    if (!inverse) {
        return core::Result<WorkspaceApplyReceipt>::failure(inverse.error());
    }

    CharacterSourceBundle working = *bundle;
    if (auto result = mutate_bundle(working, command); !result) {
        return core::Result<WorkspaceApplyReceipt>::failure(result.error());
    }
    if (auto result = refresh_digest_refs(working); !result) {
        return core::Result<WorkspaceApplyReceipt>::failure(result.error());
    }
    if (auto result = working.validate(); !result) {
        return core::Result<WorkspaceApplyReceipt>::failure(result.error());
    }

    const std::string serialized = command.serialize();
    const std::uint8_t* payload_bytes =
        reinterpret_cast<const std::uint8_t*>(serialized.data());
    const auto entry = journal_.append(journal::JournalAppend{
        revision_before,
        revision_before.next(),
        std::string(workspace_command_kind_name(command.kind())),
        std::span<const std::uint8_t>(payload_bytes, serialized.size()),
        std::nullopt,
    });
    if (!entry) {
        // The journal refused the entry: source is left exactly as it was,
        // so the store and the journal can never disagree.
        return core::Result<WorkspaceApplyReceipt>::failure(entry.error());
    }

    *bundle = std::move(working);
    store_.advance_revision();
    if (!target_session.target_character) {
        target_session.target_character = command.target_character;
    }
    if (record_undo) {
        undo_stack_.push_back(inverse.value());
    }
    return core::Result<WorkspaceApplyReceipt>::success(WorkspaceApplyReceipt{
        entry.value().revision_after, entry.value().entry_hash});
}

core::Result<WorkspaceApplyReceipt> WorkspaceController::undo() {
    if (undo_stack_.empty()) {
        return core::Result<WorkspaceApplyReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "workspace has nothing to undo"));
    }
    WorkspaceCommand inverse = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    rewrite_expected_revision(inverse, store_.revision());
    return apply_impl(inverse, false);
}

} // namespace carto::workspace
