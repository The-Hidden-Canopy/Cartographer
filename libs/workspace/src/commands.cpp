#include <carto/workspace/commands.hpp>

#include "codec.hpp"
#include "record_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::workspace {
namespace {

constexpr std::uint32_t kCommandSchemaVersion = 1U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic not_found(std::string message) {
    return core::Diagnostic(core::ErrorCode::not_found, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

// ---- digest codecs -------------------------------------------------------
//
// Command documents quote every digest. The shared ObjectWriter digest
// overload emits bare hex tokens, which the strict carto::core::json grammar
// boundary rejects on read; commands must round-trip byte-identically, so the
// quoted form below is the canonical command spelling.

std::string quoted_digest(const assets::Sha256Digest& digest) {
    return codec::quote(digest.hex());
}

core::Result<assets::Sha256Digest> digest_from(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = codec::find(members, key);
    if (member == nullptr) {
        return core::Result<assets::Sha256Digest>::failure(codec::missing(key));
    }
    const auto decoded = core::json::decode_string(member->raw_value);
    if (!decoded) {
        return core::Result<assets::Sha256Digest>::failure(decoded.error());
    }
    return assets::Sha256Digest::from_hex(decoded.value());
}

core::Result<std::optional<assets::Sha256Digest>> nullable_digest_from(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = codec::find(members, key);
    if (member == nullptr) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(
            codec::missing(key));
    }
    if (member->raw_value == "null") {
        return core::Result<std::optional<assets::Sha256Digest>>::success(std::nullopt);
    }
    const auto decoded = core::json::decode_string(member->raw_value);
    if (!decoded) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(decoded.error());
    }
    const auto digest = assets::Sha256Digest::from_hex(decoded.value());
    if (!digest) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(digest.error());
    }
    return core::Result<std::optional<assets::Sha256Digest>>::success(digest.value());
}

std::string nullable_digest_json(const std::optional<assets::Sha256Digest>& digest) {
    if (!digest.has_value()) return "null";
    return quoted_digest(digest.value());
}

// ---- scalar members ------------------------------------------------------

core::Result<core::Revision> expected_revision_member(
    const std::vector<core::json::Member>& members) {
    const auto revision = codec::uint_member(members, "expected_source_revision");
    if (!revision) {
        return core::Result<core::Revision>::failure(revision.error());
    }
    return core::Result<core::Revision>::success(core::Revision{revision.value()});
}

core::Result<std::optional<std::uint64_t>> nullable_uint_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = codec::find(members, key);
    if (member == nullptr) {
        return core::Result<std::optional<std::uint64_t>>::failure(codec::missing(key));
    }
    if (member->raw_value == "null") {
        return core::Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
    const auto value = core::json::parse_uint(member->raw_value);
    if (!value) {
        return core::Result<std::optional<std::uint64_t>>::failure(value.error());
    }
    return core::Result<std::optional<std::uint64_t>>::success(value.value());
}

core::Result<core::Transform> transform_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const auto object = codec::object_member(members, key);
    if (!object) return core::Result<core::Transform>::failure(object.error());
    return record::transform_from(object.value());
}

core::Result<character::RationalTime> time_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const auto text = codec::string_member(members, key);
    if (!text) return core::Result<character::RationalTime>::failure(text.error());
    return character::deserialize_rational_time(text.value());
}

template <typename Enum>
core::Result<Enum> enum_member(
    const std::vector<core::json::Member>& members, std::string_view key,
    Enum first, Enum last, std::string_view (*name_of)(Enum)) {
    const auto text = codec::string_member(members, key);
    if (!text) return core::Result<Enum>::failure(text.error());
    return record::scan_enum(text.value(), first, last, name_of);
}

template <typename Enum>
core::Result<std::optional<Enum>> nullable_enum_member(
    const std::vector<core::json::Member>& members, std::string_view key,
    Enum first, Enum last, std::string_view (*name_of)(Enum)) {
    const core::json::Member* member = codec::find(members, key);
    if (member == nullptr) {
        return core::Result<std::optional<Enum>>::failure(codec::missing(key));
    }
    if (member->raw_value == "null") {
        return core::Result<std::optional<Enum>>::success(std::nullopt);
    }
    const auto decoded = core::json::decode_string(member->raw_value);
    if (!decoded) return core::Result<std::optional<Enum>>::failure(decoded.error());
    const auto value = record::scan_enum(decoded.value(), first, last, name_of);
    if (!value) return core::Result<std::optional<Enum>>::failure(value.error());
    return core::Result<std::optional<Enum>>::success(value.value());
}

core::Result<WorkspaceDomain> domain_from(std::string_view name) {
    for (std::uint8_t raw = 0U;
         raw <= static_cast<std::uint8_t>(WorkspaceDomain::review); ++raw) {
        const auto domain_value = static_cast<WorkspaceDomain>(raw);
        if (workspace_domain_name(domain_value) == name) {
            return core::Result<WorkspaceDomain>::success(domain_value);
        }
    }
    return core::Result<WorkspaceDomain>::failure(
        codec::malformed("workspace domain is unknown"));
}

core::Result<WorkspaceCommandKind> kind_from(std::string_view name) {
    for (std::uint8_t raw = 0U;
         raw <= static_cast<std::uint8_t>(WorkspaceCommandKind::review_record_gate);
         ++raw) {
        const auto kind = static_cast<WorkspaceCommandKind>(raw);
        if (workspace_command_kind_name(kind) == name) {
            return core::Result<WorkspaceCommandKind>::success(kind);
        }
    }
    return core::Result<WorkspaceCommandKind>::failure(
        codec::malformed("workspace command kind is unknown"));
}

WorkspaceDomain kind_domain(WorkspaceCommandKind kind) noexcept {
    switch (kind) {
    case WorkspaceCommandKind::rig_add_joint:
    case WorkspaceCommandKind::rig_remove_joint:
    case WorkspaceCommandKind::rig_set_rest:
    case WorkspaceCommandKind::rig_add_control:
    case WorkspaceCommandKind::rig_remove_control:
    case WorkspaceCommandKind::rig_set_control_axes:
    case WorkspaceCommandKind::rig_bind_attachment:
    case WorkspaceCommandKind::rig_unbind_attachment:
        return WorkspaceDomain::rig;
    case WorkspaceCommandKind::skin_paint_stamp:
    case WorkspaceCommandKind::skin_set_weight_lock:
    case WorkspaceCommandKind::skin_rebind_layer:
        return WorkspaceDomain::skin;
    case WorkspaceCommandKind::animate_set_key:
    case WorkspaceCommandKind::animate_remove_key:
    case WorkspaceCommandKind::animate_set_root_motion:
        return WorkspaceDomain::animate;
    case WorkspaceCommandKind::face_add_morph_target:
    case WorkspaceCommandKind::face_remove_morph_target:
        return WorkspaceDomain::face;
    case WorkspaceCommandKind::sim_bind_garment:
    case WorkspaceCommandKind::sim_unbind_garment:
    case WorkspaceCommandKind::sim_enqueue_bake:
    case WorkspaceCommandKind::sim_cancel_bake:
    case WorkspaceCommandKind::sim_discard_bake:
    case WorkspaceCommandKind::sim_attach_bake_receipt:
    case WorkspaceCommandKind::sim_clear_bake_receipt:
        return WorkspaceDomain::sim;
    case WorkspaceCommandKind::review_record_gate:
        return WorkspaceDomain::review;
    }
    return WorkspaceDomain::rig;
}

// ---- record fragments ----------------------------------------------------

std::string joint_id_array_json(const std::vector<kinematic::JointId>& joints) {
    std::string out = "[";
    bool first = true;
    for (const kinematic::JointId joint : joints) {
        if (!first) out.push_back(',');
        first = false;
        out += codec::u64(joint.value);
    }
    out.push_back(']');
    return out;
}

core::Result<std::vector<kinematic::JointInfluence>> influences_member(
    const std::vector<core::json::Member>& members) {
    const auto elements = codec::array_member(members, "influences");
    if (!elements) {
        return core::Result<std::vector<kinematic::JointInfluence>>::failure(
            elements.error());
    }
    if (elements.value().size() > kinematic::SkinBinding::kMaxInfluencesPerVertex) {
        return core::Result<std::vector<kinematic::JointInfluence>>::failure(
            codec::malformed("workspace influence list exceeds its bounded capacity"));
    }
    std::vector<kinematic::JointInfluence> influences;
    influences.reserve(elements.value().size());
    for (const std::string_view element : elements.value()) {
        const auto element_members = core::json::parse_object(element);
        if (!element_members) {
            return core::Result<std::vector<kinematic::JointInfluence>>::failure(
                element_members.error());
        }
        const auto joint = codec::uint_member(element_members.value(), "joint");
        if (!joint) {
            return core::Result<std::vector<kinematic::JointInfluence>>::failure(
                joint.error());
        }
        const auto weight = codec::double_member(element_members.value(), "weight");
        if (!weight) {
            return core::Result<std::vector<kinematic::JointInfluence>>::failure(
                weight.error());
        }
        influences.push_back(kinematic::JointInfluence{
            kinematic::JointId{joint.value()}, weight.value()});
    }
    return core::Result<std::vector<kinematic::JointInfluence>>::success(
        std::move(influences));
}

std::string influences_json(const std::vector<kinematic::JointInfluence>& influences) {
    std::string out = "[";
    bool first = true;
    for (const kinematic::JointInfluence& influence : influences) {
        if (!first) out.push_back(',');
        first = false;
        codec::ObjectWriter item;
        item.add("joint", influence.joint.value);
        item.add("weight", influence.weight);
        out += std::move(item).finish();
    }
    out.push_back(']');
    return out;
}

std::string mesh_asset_ref_json(const character::MeshAssetRef& ref) {
    codec::ObjectWriter writer;
    writer.add("asset_id", ref.asset_id);
    writer.add_raw("content_digest", quoted_digest(ref.content_digest));
    return std::move(writer).finish();
}

core::Result<character::MeshAssetRef> mesh_asset_ref_from(
    const std::vector<core::json::Member>& members) {
    character::MeshAssetRef ref;
    const auto asset = codec::uint_member(members, "asset_id");
    if (!asset) return core::Result<character::MeshAssetRef>::failure(asset.error());
    ref.asset_id = asset.value();
    const auto digest = digest_from(members, "content_digest");
    if (!digest) return core::Result<character::MeshAssetRef>::failure(digest.error());
    ref.content_digest = digest.value();
    return core::Result<character::MeshAssetRef>::success(ref);
}

core::Result<character::MeshAssetRef> mesh_ref_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const auto object = codec::object_member(members, key);
    if (!object) return core::Result<character::MeshAssetRef>::failure(object.error());
    return mesh_asset_ref_from(object.value());
}

std::string morph_target_json(const character::MorphTarget& morph) {
    codec::ObjectWriter writer;
    writer.add("id", morph.id.value);
    writer.add("source_mesh_asset", morph.source_mesh_asset);
    writer.add_raw("source_mesh_digest", quoted_digest(morph.source_mesh_digest));
    writer.add("displaced_point_count", morph.displaced_point_count);
    writer.add_raw("delta_blob_digest", quoted_digest(morph.delta_blob_digest));
    writer.add("delta_blob_bytes", morph.delta_blob_bytes);
    writer.add_raw("topology_correspondence_digest",
                   quoted_digest(morph.topology_correspondence_digest));
    writer.add("tangent_in", character::tangent_policy_name(morph.tangent_in));
    writer.add("tangent_out", character::tangent_policy_name(morph.tangent_out));
    writer.add("precision_bound", morph.precision_bound);
    return std::move(writer).finish();
}

core::Result<character::MorphTarget> morph_target_from(
    const std::vector<core::json::Member>& members) {
    character::MorphTarget morph;
    const auto id = codec::uint_member(members, "id");
    if (!id) return core::Result<character::MorphTarget>::failure(id.error());
    morph.id = character::MorphTargetId{id.value()};
    const auto mesh = codec::uint_member(members, "source_mesh_asset");
    if (!mesh) return core::Result<character::MorphTarget>::failure(mesh.error());
    morph.source_mesh_asset = mesh.value();
    const auto mesh_digest = digest_from(members, "source_mesh_digest");
    if (!mesh_digest) {
        return core::Result<character::MorphTarget>::failure(mesh_digest.error());
    }
    morph.source_mesh_digest = mesh_digest.value();
    const auto displaced = codec::uint_member(members, "displaced_point_count");
    if (!displaced) {
        return core::Result<character::MorphTarget>::failure(displaced.error());
    }
    morph.displaced_point_count = displaced.value();
    const auto delta_digest = digest_from(members, "delta_blob_digest");
    if (!delta_digest) {
        return core::Result<character::MorphTarget>::failure(delta_digest.error());
    }
    morph.delta_blob_digest = delta_digest.value();
    const auto delta_bytes = codec::uint_member(members, "delta_blob_bytes");
    if (!delta_bytes) {
        return core::Result<character::MorphTarget>::failure(delta_bytes.error());
    }
    morph.delta_blob_bytes = delta_bytes.value();
    const auto correspondence = digest_from(members, "topology_correspondence_digest");
    if (!correspondence) {
        return core::Result<character::MorphTarget>::failure(correspondence.error());
    }
    morph.topology_correspondence_digest = correspondence.value();
    const auto tangent_in = enum_member(
        members, "tangent_in", character::TangentPolicy::none,
        character::TangentPolicy::recompute, character::tangent_policy_name);
    if (!tangent_in) return core::Result<character::MorphTarget>::failure(tangent_in.error());
    morph.tangent_in = tangent_in.value();
    const auto tangent_out = enum_member(
        members, "tangent_out", character::TangentPolicy::none,
        character::TangentPolicy::recompute, character::tangent_policy_name);
    if (!tangent_out) {
        return core::Result<character::MorphTarget>::failure(tangent_out.error());
    }
    morph.tangent_out = tangent_out.value();
    const auto precision = codec::double_member(members, "precision_bound");
    if (!precision) {
        return core::Result<character::MorphTarget>::failure(precision.error());
    }
    morph.precision_bound = precision.value();
    return core::Result<character::MorphTarget>::success(std::move(morph));
}

// Parses a digest array in the quoted command spelling (the shared
// codec::digest_array_from expects the bare-hex form and cannot be reused for
// grammar-valid command documents).
core::Result<std::vector<assets::Sha256Digest>> digest_array_from(
    std::string_view raw, std::size_t max_entries) {
    const auto elements = codec::split_array(raw);
    if (!elements) {
        return core::Result<std::vector<assets::Sha256Digest>>::failure(elements.error());
    }
    if (elements.value().size() > max_entries) {
        return core::Result<std::vector<assets::Sha256Digest>>::failure(codec::malformed(
            "workspace digest array exceeds its bounded capacity"));
    }
    std::vector<assets::Sha256Digest> digests;
    digests.reserve(elements.value().size());
    for (const std::string_view element : elements.value()) {
        const auto decoded = core::json::decode_string(element);
        if (!decoded) {
            return core::Result<std::vector<assets::Sha256Digest>>::failure(decoded.error());
        }
        const auto digest = assets::Sha256Digest::from_hex(decoded.value());
        if (!digest) {
            return core::Result<std::vector<assets::Sha256Digest>>::failure(digest.error());
        }
        digests.push_back(digest.value());
    }
    return core::Result<std::vector<assets::Sha256Digest>>::success(std::move(digests));
}

// Parses an LOD mapping in the quoted command spelling (record::lod_mapping_from
// expects the bare-hex form).
core::Result<std::map<std::uint32_t, assets::Sha256Digest>> lod_mapping_from(
    const std::vector<core::json::Member>& members, std::string_view key,
    std::size_t max_entries) {
    const auto raw_members = codec::object_member(members, key);
    if (!raw_members) {
        return core::Result<std::map<std::uint32_t, assets::Sha256Digest>>::failure(
            raw_members.error());
    }
    if (raw_members.value().size() > max_entries) {
        return core::Result<std::map<std::uint32_t, assets::Sha256Digest>>::failure(
            codec::malformed("workspace LOD mapping exceeds its bounded capacity"));
    }
    std::map<std::uint32_t, assets::Sha256Digest> mapping;
    for (const core::json::Member& member : raw_members.value()) {
        const auto lod = codec::parse_uint(member.key);
        if (!lod || lod.value() > std::numeric_limits<std::uint32_t>::max() ||
            lod.value() > character::GarmentBinding::kMaxLod) {
            return core::Result<std::map<std::uint32_t, assets::Sha256Digest>>::failure(
                codec::malformed("workspace LOD mapping key is invalid"));
        }
        const auto decoded = core::json::decode_string(member.raw_value);
        if (!decoded) {
            return core::Result<std::map<std::uint32_t, assets::Sha256Digest>>::failure(
                decoded.error());
        }
        const auto digest = assets::Sha256Digest::from_hex(decoded.value());
        if (!digest) {
            return core::Result<std::map<std::uint32_t, assets::Sha256Digest>>::failure(
                digest.error());
        }
        mapping.emplace(static_cast<std::uint32_t>(lod.value()), digest.value());
    }
    return core::Result<std::map<std::uint32_t, assets::Sha256Digest>>::success(
        std::move(mapping));
}

std::string pinned_source_json(const character::PinnedSourceRef& source) {
    codec::ObjectWriter writer;
    writer.add("source_id", source.source_id);
    writer.add("pinned_revision", source.pinned_revision.value());
    writer.add_raw("digest", quoted_digest(source.digest));
    return std::move(writer).finish();
}

core::Result<character::PinnedSourceRef> pinned_source_from(
    const std::vector<core::json::Member>& members) {
    character::PinnedSourceRef source;
    const auto source_id = codec::uint_member(members, "source_id");
    if (!source_id) {
        return core::Result<character::PinnedSourceRef>::failure(source_id.error());
    }
    source.source_id = source_id.value();
    const auto revision = codec::uint_member(members, "pinned_revision");
    if (!revision) {
        return core::Result<character::PinnedSourceRef>::failure(revision.error());
    }
    source.pinned_revision = core::Revision{revision.value()};
    const auto digest = digest_from(members, "digest");
    if (!digest) return core::Result<character::PinnedSourceRef>::failure(digest.error());
    source.digest = digest.value();
    return core::Result<character::PinnedSourceRef>::success(std::move(source));
}

std::string garment_binding_json(const character::GarmentBinding& binding) {
    codec::ObjectWriter writer;
    writer.add("id", binding.id.value);
    writer.add_raw("source_mesh", pinned_source_json(binding.source_mesh));
    writer.add_raw("attachments",
                   record::binding_attachment_array_json(binding.attachments));
    writer.add_raw("seams", record::seam_array_json(binding.seams));
    writer.add_raw("collision_filter_refs",
                   codec::digest_array_json(binding.collision_filter_refs));
    writer.add_raw("material_intent_ref", quoted_digest(binding.material_intent_ref));
    writer.add_raw("lod_mapping", record::lod_mapping_json(binding.lod_mapping));
    return std::move(writer).finish();
}

core::Result<character::GarmentBinding> garment_binding_from(
    const std::vector<core::json::Member>& members) {
    character::GarmentBinding binding;
    const auto id = codec::uint_member(members, "id");
    if (!id) return core::Result<character::GarmentBinding>::failure(id.error());
    binding.id = character::GarmentBindingId{id.value()};
    const auto source = codec::object_member(members, "source_mesh");
    if (!source) return core::Result<character::GarmentBinding>::failure(source.error());
    const auto parsed_source = pinned_source_from(source.value());
    if (!parsed_source) {
        return core::Result<character::GarmentBinding>::failure(parsed_source.error());
    }
    binding.source_mesh = parsed_source.value();
    const core::json::Member* attachments = codec::find(members, "attachments");
    if (attachments == nullptr) {
        return core::Result<character::GarmentBinding>::failure(
            codec::missing("attachments"));
    }
    const auto parsed_attachments = record::binding_attachment_array_from(
        attachments->raw_value, character::GarmentBinding::kMaxAttachments);
    if (!parsed_attachments) {
        return core::Result<character::GarmentBinding>::failure(
            parsed_attachments.error());
    }
    binding.attachments = parsed_attachments.value();
    const core::json::Member* seams = codec::find(members, "seams");
    if (seams == nullptr) {
        return core::Result<character::GarmentBinding>::failure(codec::missing("seams"));
    }
    const auto elements = codec::split_array(seams->raw_value);
    if (!elements) return core::Result<character::GarmentBinding>::failure(elements.error());
    if (elements.value().size() > character::GarmentBinding::kMaxSeams) {
        return core::Result<character::GarmentBinding>::failure(codec::malformed(
            "workspace seam table exceeds its bounded capacity"));
    }
    for (const std::string_view element : elements.value()) {
        const auto seam_members = core::json::parse_object(element);
        if (!seam_members) {
            return core::Result<character::GarmentBinding>::failure(seam_members.error());
        }
        const auto seam_id = codec::uint_member(seam_members.value(), "seam_id");
        if (!seam_id) {
            return core::Result<character::GarmentBinding>::failure(seam_id.error());
        }
        const auto index = codec::u32_member(seam_members.value(), "attachment_index");
        if (!index) {
            return core::Result<character::GarmentBinding>::failure(index.error());
        }
        binding.seams.push_back(character::SeamRef{seam_id.value(), index.value()});
    }
    const core::json::Member* filters = codec::find(members, "collision_filter_refs");
    if (filters == nullptr) {
        return core::Result<character::GarmentBinding>::failure(
            codec::missing("collision_filter_refs"));
    }
    const auto parsed_filters = digest_array_from(
        filters->raw_value, character::GarmentBinding::kMaxCollisionFilters);
    if (!parsed_filters) {
        return core::Result<character::GarmentBinding>::failure(parsed_filters.error());
    }
    binding.collision_filter_refs = parsed_filters.value();
    const auto material = digest_from(members, "material_intent_ref");
    if (!material) {
        return core::Result<character::GarmentBinding>::failure(material.error());
    }
    binding.material_intent_ref = material.value();
    const auto lod = lod_mapping_from(
        members, "lod_mapping", character::GarmentBinding::kMaxLod);
    if (!lod) return core::Result<character::GarmentBinding>::failure(lod.error());
    binding.lod_mapping = lod.value();
    return core::Result<character::GarmentBinding>::success(std::move(binding));
}

// ---- payload validation --------------------------------------------------
//
// Payload-level validation is pure: no store access. Referential checks
// against the session and character definitions belong to the controller.

core::Result<void> validate_payload(const RigAddJoint& payload) {
    kinematic::KinematicNode node;
    node.id = payload.joint;
    node.parent = payload.parent;
    node.rest = payload.rest;
    node.kind = payload.kind;
    node.semantic_type = payload.semantic_type;
    return node.validate();
}

core::Result<void> validate_payload(const RigRemoveJoint& payload) {
    if (!payload.joint) {
        return core::Result<void>::failure(invalid(
            "workspace rig command carries the invalid zero joint identity"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const RigSetRest& payload) {
    if (!payload.joint) {
        return core::Result<void>::failure(invalid(
            "workspace rig command carries the invalid zero joint identity"));
    }
    if (!payload.rest.finite()) {
        return core::Result<void>::failure(invalid(
            "workspace rig rest transform is not finite"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const RigAddControl& payload) {
    character::ControlDefinition control;
    control.id = payload.control;
    control.shape = payload.shape;
    control.transform = payload.rest;
    control.affected_joints = payload.affected_joints;
    control.allowed_axes = payload.allowed_axes;
    control.control_set = payload.control_set;
    return control.validate();
}

core::Result<void> validate_payload(const RigRemoveControl& payload) {
    if (!payload.control) {
        return core::Result<void>::failure(invalid(
            "workspace rig command carries the invalid zero control identity"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const RigSetControlAxes& payload) {
    if (!payload.control) {
        return core::Result<void>::failure(invalid(
            "workspace rig command carries the invalid zero control identity"));
    }
    if ((payload.allowed_axes & ~character::kAllowedAxesMask) != 0U) {
        return core::Result<void>::failure(invalid(
            "workspace control axes exceed the allowed axis mask"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const RigBindAttachment& payload) {
    AttachmentBinding binding;
    binding.joint = payload.joint;
    binding.mesh = payload.mesh;
    binding.offset = payload.offset;
    binding.role = payload.role;
    return binding.validate();
}

core::Result<void> validate_payload(const RigUnbindAttachment& payload) {
    if (!payload.joint || payload.mesh_asset == 0U) {
        return core::Result<void>::failure(invalid(
            "workspace unbind command carries an invalid joint or mesh asset"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const SkinPaintStamp& payload) {
    if (payload.vertex > kinematic::SkinBinding::kMaxVertexIndex) {
        return core::Result<void>::failure(invalid(
            "workspace paint stamp vertex index is out of range"));
    }
    // An empty influence list is the explicit clear-stamp: it removes every
    // influence from the vertex (the vertex becomes rigid). Non-empty lists
    // are admitted by the shared influence contract exactly like every other
    // skin weight producer.
    if (payload.influences.empty()) {
        return core::Result<void>::success();
    }
    return character::validate_influences(payload.influences);
}

core::Result<void> validate_payload(const SkinSetWeightLock& payload) {
    if (payload.vertex > kinematic::SkinBinding::kMaxVertexIndex) {
        return core::Result<void>::failure(invalid(
            "workspace weight lock vertex index is out of range"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const SkinRebindLayer& payload) {
    if (payload.joint_graph_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace skin rebind carries no joint graph digest"));
    }
    if (payload.previous_staleness.has_value() &&
        static_cast<std::uint8_t>(payload.previous_staleness.value()) >
            static_cast<std::uint8_t>(character::WeightStaleness::invalid)) {
        return core::Result<void>::failure(invalid(
            "workspace skin rebind carries an unknown previous staleness"));
    }
    if (payload.previous_joint_graph_digest.has_value() &&
        payload.previous_joint_graph_digest.value().is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace skin rebind carries a zero previous graph digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const AnimateSetKey& payload) {
    if (!payload.channel) {
        return core::Result<void>::failure(invalid(
            "workspace animate command carries the invalid zero channel identity"));
    }
    if (auto result = payload.time.validate(); !result) return result;
    if (!std::isfinite(payload.value)) {
        return core::Result<void>::failure(invalid(
            "workspace key value is not finite"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const AnimateRemoveKey& payload) {
    if (!payload.channel) {
        return core::Result<void>::failure(invalid(
            "workspace animate command carries the invalid zero channel identity"));
    }
    return payload.time.validate();
}

core::Result<void> validate_payload(const AnimateSetRootMotion& payload) {
    if (static_cast<std::uint8_t>(payload.policy) >
        static_cast<std::uint8_t>(character::RootMotionPolicy::none)) {
        return core::Result<void>::failure(invalid(
            "workspace root motion policy is unknown"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const FaceAddMorphTarget& payload) {
    return payload.morph.validate();
}

core::Result<void> validate_payload(const FaceRemoveMorphTarget& payload) {
    if (!payload.morph) {
        return core::Result<void>::failure(invalid(
            "workspace face command carries the invalid zero morph identity"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const SimBindGarment& payload) {
    return payload.binding.validate();
}

core::Result<void> validate_payload(const SimUnbindGarment& payload) {
    if (!payload.binding) {
        return core::Result<void>::failure(invalid(
            "workspace sim command carries the invalid zero binding identity"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const SimEnqueueBake& payload) {
    // The bake payload is exactly the journaled BakeJob record; reuse its
    // admission contract (identity, frame range, budget, settings digest).
    BakeJob job;
    job.job_id = payload.job_id;
    job.frame_start = payload.frame_start;
    job.frame_end = payload.frame_end;
    job.max_frames = payload.max_frames;
    job.settings_digest = payload.settings_digest;
    return job.validate();
}

core::Result<void> validate_payload(const SimCancelBake& payload) {
    if (payload.job_id == 0U) {
        return core::Result<void>::failure(invalid(
            "workspace bake job carries the invalid zero identity"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const SimDiscardBake& payload) {
    if (payload.job_id == 0U) {
        return core::Result<void>::failure(invalid(
            "workspace bake job carries the invalid zero identity"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const SimAttachBakeReceipt& payload) {
    if (payload.job_id == 0U || payload.receipt_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace receipt attach carries an invalid job or receipt digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const SimClearBakeReceipt& payload) {
    if (payload.job_id == 0U) {
        return core::Result<void>::failure(invalid(
            "workspace bake job carries the invalid zero identity"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_payload(const ReviewRecordGate& payload) {
    if (!record::safe_text(payload.gate_id, WorkspaceCommand::kMaxGateIdBytes)) {
        return core::Result<void>::failure(invalid(
            "workspace review gate identity is out of bounds"));
    }
    if (static_cast<std::uint8_t>(payload.status) >
        static_cast<std::uint8_t>(character::GateStatus::not_run)) {
        return core::Result<void>::failure(invalid(
            "workspace review gate status is unknown"));
    }
    if (payload.previous_status.has_value() &&
        static_cast<std::uint8_t>(payload.previous_status.value()) >
            static_cast<std::uint8_t>(character::GateStatus::not_run)) {
        return core::Result<void>::failure(invalid(
            "workspace review gate previous status is unknown"));
    }
    return core::Result<void>::success();
}

// ---- payload writers -----------------------------------------------------

void add_expected(codec::ObjectWriter& writer, core::Revision revision) {
    writer.add("expected_source_revision", revision.value());
}

std::string payload_json(const WorkspaceCommandPayload& payload) {
    switch (static_cast<WorkspaceCommandKind>(payload.index())) {
    case WorkspaceCommandKind::rig_add_joint: {
        const auto& typed = std::get<RigAddJoint>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("joint", typed.joint.value);
        if (typed.parent.has_value()) {
            writer.add("parent", typed.parent.value().value);
        } else {
            writer.add_raw("parent", "null");
        }
        writer.add_raw("rest", record::transform_json(typed.rest));
        writer.add("kind", kinematic::joint_kind_name(typed.kind));
        writer.add("semantic_type", typed.semantic_type);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::rig_remove_joint: {
        const auto& typed = std::get<RigRemoveJoint>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("joint", typed.joint.value);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::rig_set_rest: {
        const auto& typed = std::get<RigSetRest>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("joint", typed.joint.value);
        writer.add_raw("rest", record::transform_json(typed.rest));
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::rig_add_control: {
        const auto& typed = std::get<RigAddControl>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("control", typed.control.value);
        writer.add("shape", character::control_shape_name(typed.shape));
        writer.add_raw("rest", record::transform_json(typed.rest));
        writer.add_raw("affected_joints", joint_id_array_json(typed.affected_joints));
        writer.add("allowed_axes", static_cast<std::uint64_t>(typed.allowed_axes));
        writer.add("control_set", typed.control_set);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::rig_remove_control: {
        const auto& typed = std::get<RigRemoveControl>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("control", typed.control.value);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::rig_set_control_axes: {
        const auto& typed = std::get<RigSetControlAxes>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("control", typed.control.value);
        writer.add("allowed_axes", static_cast<std::uint64_t>(typed.allowed_axes));
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::rig_bind_attachment: {
        const auto& typed = std::get<RigBindAttachment>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("joint", typed.joint.value);
        writer.add_raw("mesh", mesh_asset_ref_json(typed.mesh));
        writer.add_raw("offset", record::transform_json(typed.offset));
        writer.add("role", typed.role);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::rig_unbind_attachment: {
        const auto& typed = std::get<RigUnbindAttachment>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("joint", typed.joint.value);
        writer.add("mesh_asset", typed.mesh_asset);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::skin_paint_stamp: {
        const auto& typed = std::get<SkinPaintStamp>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("vertex", static_cast<std::uint64_t>(typed.vertex));
        writer.add_raw("influences", influences_json(typed.influences));
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::skin_set_weight_lock: {
        const auto& typed = std::get<SkinSetWeightLock>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("vertex", static_cast<std::uint64_t>(typed.vertex));
        writer.add("locked", typed.locked);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::skin_rebind_layer: {
        const auto& typed = std::get<SkinRebindLayer>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add_raw("joint_graph_digest", quoted_digest(typed.joint_graph_digest));
        if (typed.previous_staleness.has_value()) {
            writer.add("previous_staleness",
                       character::weight_staleness_name(typed.previous_staleness.value()));
        } else {
            writer.add_raw("previous_staleness", "null");
        }
        if (typed.previous_stale_since.has_value()) {
            writer.add("previous_stale_since", typed.previous_stale_since.value().value());
        } else {
            writer.add_raw("previous_stale_since", "null");
        }
        writer.add_raw("previous_joint_graph_digest",
                       nullable_digest_json(typed.previous_joint_graph_digest));
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::animate_set_key: {
        const auto& typed = std::get<AnimateSetKey>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("channel", typed.channel.value);
        writer.add("time", character::serialize(typed.time));
        writer.add("value", typed.value);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::animate_remove_key: {
        const auto& typed = std::get<AnimateRemoveKey>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("channel", typed.channel.value);
        writer.add("time", character::serialize(typed.time));
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::animate_set_root_motion: {
        const auto& typed = std::get<AnimateSetRootMotion>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("policy", character::root_motion_policy_name(typed.policy));
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::face_add_morph_target: {
        const auto& typed = std::get<FaceAddMorphTarget>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add_raw("morph", morph_target_json(typed.morph));
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::face_remove_morph_target: {
        const auto& typed = std::get<FaceRemoveMorphTarget>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("morph", typed.morph.value);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::sim_bind_garment: {
        const auto& typed = std::get<SimBindGarment>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add_raw("binding", garment_binding_json(typed.binding));
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::sim_unbind_garment: {
        const auto& typed = std::get<SimUnbindGarment>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("binding", typed.binding.value);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::sim_enqueue_bake: {
        const auto& typed = std::get<SimEnqueueBake>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("job_id", typed.job_id);
        writer.add("frame_start", character::serialize(typed.frame_start));
        writer.add("frame_end", character::serialize(typed.frame_end));
        writer.add("max_frames", static_cast<std::uint64_t>(typed.max_frames));
        writer.add_raw("settings_digest", quoted_digest(typed.settings_digest));
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::sim_cancel_bake: {
        const auto& typed = std::get<SimCancelBake>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("job_id", typed.job_id);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::sim_discard_bake: {
        const auto& typed = std::get<SimDiscardBake>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("job_id", typed.job_id);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::sim_attach_bake_receipt: {
        const auto& typed = std::get<SimAttachBakeReceipt>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("job_id", typed.job_id);
        writer.add_raw("receipt_digest", quoted_digest(typed.receipt_digest));
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::sim_clear_bake_receipt: {
        const auto& typed = std::get<SimClearBakeReceipt>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("job_id", typed.job_id);
        return std::move(writer).finish();
    }
    case WorkspaceCommandKind::review_record_gate: {
        const auto& typed = std::get<ReviewRecordGate>(payload);
        codec::ObjectWriter writer;
        add_expected(writer, typed.expected_source_revision);
        writer.add("gate_id", typed.gate_id);
        writer.add("status", character::gate_status_name(typed.status));
        if (typed.previous_status.has_value()) {
            writer.add("previous_status",
                       character::gate_status_name(typed.previous_status.value()));
        } else {
            writer.add_raw("previous_status", "null");
        }
        return std::move(writer).finish();
    }
    }
    return "{}";
}

// ---- payload parsers -----------------------------------------------------

core::Result<WorkspaceCommandPayload> payload_from(
    WorkspaceCommandKind kind, const std::vector<core::json::Member>& members) {
    switch (kind) {
    case WorkspaceCommandKind::rig_add_joint: {
        RigAddJoint payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto joint = codec::uint_member(members, "joint");
        if (!joint) return core::Result<WorkspaceCommandPayload>::failure(joint.error());
        payload.joint = kinematic::JointId{joint.value()};
        const auto parent = nullable_uint_member(members, "parent");
        if (!parent) return core::Result<WorkspaceCommandPayload>::failure(parent.error());
        if (parent.value().has_value()) {
            payload.parent = kinematic::JointId{parent.value().value()};
        }
        const auto rest = transform_member(members, "rest");
        if (!rest) return core::Result<WorkspaceCommandPayload>::failure(rest.error());
        payload.rest = rest.value();
        const auto joint_kind = enum_member(
            members, "kind", kinematic::JointKind::fixed, kinematic::JointKind::ball,
            kinematic::joint_kind_name);
        if (!joint_kind) {
            return core::Result<WorkspaceCommandPayload>::failure(joint_kind.error());
        }
        payload.kind = joint_kind.value();
        const auto semantic = codec::string_member(members, "semantic_type");
        if (!semantic) {
            return core::Result<WorkspaceCommandPayload>::failure(semantic.error());
        }
        payload.semantic_type = semantic.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::rig_remove_joint: {
        RigRemoveJoint payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto joint = codec::uint_member(members, "joint");
        if (!joint) return core::Result<WorkspaceCommandPayload>::failure(joint.error());
        payload.joint = kinematic::JointId{joint.value()};
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::rig_set_rest: {
        RigSetRest payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto joint = codec::uint_member(members, "joint");
        if (!joint) return core::Result<WorkspaceCommandPayload>::failure(joint.error());
        payload.joint = kinematic::JointId{joint.value()};
        const auto rest = transform_member(members, "rest");
        if (!rest) return core::Result<WorkspaceCommandPayload>::failure(rest.error());
        payload.rest = rest.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::rig_add_control: {
        RigAddControl payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto control = codec::uint_member(members, "control");
        if (!control) return core::Result<WorkspaceCommandPayload>::failure(control.error());
        payload.control = character::ControlId{control.value()};
        const auto shape = enum_member(
            members, "shape", character::ControlShape::sphere,
            character::ControlShape::locator, character::control_shape_name);
        if (!shape) return core::Result<WorkspaceCommandPayload>::failure(shape.error());
        payload.shape = shape.value();
        const auto rest = transform_member(members, "rest");
        if (!rest) return core::Result<WorkspaceCommandPayload>::failure(rest.error());
        payload.rest = rest.value();
        const auto joints = codec::uint_array_member(
            members, "affected_joints",
            character::ControlGraph::kMaxAffectedJointsPerControl);
        if (!joints) return core::Result<WorkspaceCommandPayload>::failure(joints.error());
        for (const std::uint64_t joint : joints.value()) {
            payload.affected_joints.push_back(kinematic::JointId{joint});
        }
        const auto axes = codec::uint_member(members, "allowed_axes");
        if (!axes) return core::Result<WorkspaceCommandPayload>::failure(axes.error());
        if (axes.value() > character::kAllowedAxesMask) {
            return core::Result<WorkspaceCommandPayload>::failure(codec::malformed(
                "workspace control axes exceed the allowed axis mask"));
        }
        payload.allowed_axes = static_cast<std::uint8_t>(axes.value());
        const auto control_set = codec::optional_string_member(members, "control_set");
        if (!control_set) {
            return core::Result<WorkspaceCommandPayload>::failure(control_set.error());
        }
        payload.control_set = control_set.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::rig_remove_control: {
        RigRemoveControl payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto control = codec::uint_member(members, "control");
        if (!control) return core::Result<WorkspaceCommandPayload>::failure(control.error());
        payload.control = character::ControlId{control.value()};
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::rig_set_control_axes: {
        RigSetControlAxes payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto control = codec::uint_member(members, "control");
        if (!control) return core::Result<WorkspaceCommandPayload>::failure(control.error());
        payload.control = character::ControlId{control.value()};
        const auto axes = codec::uint_member(members, "allowed_axes");
        if (!axes) return core::Result<WorkspaceCommandPayload>::failure(axes.error());
        if (axes.value() > character::kAllowedAxesMask) {
            return core::Result<WorkspaceCommandPayload>::failure(codec::malformed(
                "workspace control axes exceed the allowed axis mask"));
        }
        payload.allowed_axes = static_cast<std::uint8_t>(axes.value());
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::rig_bind_attachment: {
        RigBindAttachment payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto joint = codec::uint_member(members, "joint");
        if (!joint) return core::Result<WorkspaceCommandPayload>::failure(joint.error());
        payload.joint = kinematic::JointId{joint.value()};
        const auto mesh = mesh_ref_member(members, "mesh");
        if (!mesh) return core::Result<WorkspaceCommandPayload>::failure(mesh.error());
        payload.mesh = mesh.value();
        const auto offset = transform_member(members, "offset");
        if (!offset) return core::Result<WorkspaceCommandPayload>::failure(offset.error());
        payload.offset = offset.value();
        const auto role = codec::optional_string_member(members, "role");
        if (!role) return core::Result<WorkspaceCommandPayload>::failure(role.error());
        payload.role = role.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::rig_unbind_attachment: {
        RigUnbindAttachment payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto joint = codec::uint_member(members, "joint");
        if (!joint) return core::Result<WorkspaceCommandPayload>::failure(joint.error());
        payload.joint = kinematic::JointId{joint.value()};
        const auto mesh = codec::uint_member(members, "mesh_asset");
        if (!mesh) return core::Result<WorkspaceCommandPayload>::failure(mesh.error());
        payload.mesh_asset = mesh.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::skin_paint_stamp: {
        SkinPaintStamp payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto vertex = codec::u32_member(members, "vertex");
        if (!vertex) return core::Result<WorkspaceCommandPayload>::failure(vertex.error());
        payload.vertex = vertex.value();
        const auto influences = influences_member(members);
        if (!influences) {
            return core::Result<WorkspaceCommandPayload>::failure(influences.error());
        }
        payload.influences = influences.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::skin_set_weight_lock: {
        SkinSetWeightLock payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto vertex = codec::u32_member(members, "vertex");
        if (!vertex) return core::Result<WorkspaceCommandPayload>::failure(vertex.error());
        payload.vertex = vertex.value();
        const auto locked = codec::bool_member(members, "locked");
        if (!locked) return core::Result<WorkspaceCommandPayload>::failure(locked.error());
        payload.locked = locked.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::skin_rebind_layer: {
        SkinRebindLayer payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto digest = digest_from(members, "joint_graph_digest");
        if (!digest) return core::Result<WorkspaceCommandPayload>::failure(digest.error());
        payload.joint_graph_digest = digest.value();
        const auto staleness = nullable_enum_member(
            members, "previous_staleness", character::WeightStaleness::fresh,
            character::WeightStaleness::invalid, character::weight_staleness_name);
        if (!staleness) {
            return core::Result<WorkspaceCommandPayload>::failure(staleness.error());
        }
        payload.previous_staleness = staleness.value();
        const auto stale_since = nullable_uint_member(members, "previous_stale_since");
        if (!stale_since) {
            return core::Result<WorkspaceCommandPayload>::failure(stale_since.error());
        }
        if (stale_since.value().has_value()) {
            payload.previous_stale_since =
                core::Revision{stale_since.value().value()};
        }
        const auto previous_digest = nullable_digest_from(
            members, "previous_joint_graph_digest");
        if (!previous_digest) {
            return core::Result<WorkspaceCommandPayload>::failure(previous_digest.error());
        }
        payload.previous_joint_graph_digest = previous_digest.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::animate_set_key: {
        AnimateSetKey payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto channel = codec::uint_member(members, "channel");
        if (!channel) return core::Result<WorkspaceCommandPayload>::failure(channel.error());
        payload.channel = temporal::ChannelId{channel.value()};
        const auto time = time_member(members, "time");
        if (!time) return core::Result<WorkspaceCommandPayload>::failure(time.error());
        payload.time = time.value();
        const auto value = codec::double_member(members, "value");
        if (!value) return core::Result<WorkspaceCommandPayload>::failure(value.error());
        payload.value = value.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::animate_remove_key: {
        AnimateRemoveKey payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto channel = codec::uint_member(members, "channel");
        if (!channel) return core::Result<WorkspaceCommandPayload>::failure(channel.error());
        payload.channel = temporal::ChannelId{channel.value()};
        const auto time = time_member(members, "time");
        if (!time) return core::Result<WorkspaceCommandPayload>::failure(time.error());
        payload.time = time.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::animate_set_root_motion: {
        AnimateSetRootMotion payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto policy = enum_member(
            members, "policy", character::RootMotionPolicy::authored,
            character::RootMotionPolicy::none, character::root_motion_policy_name);
        if (!policy) return core::Result<WorkspaceCommandPayload>::failure(policy.error());
        payload.policy = policy.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::face_add_morph_target: {
        FaceAddMorphTarget payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto object = codec::object_member(members, "morph");
        if (!object) return core::Result<WorkspaceCommandPayload>::failure(object.error());
        const auto morph = morph_target_from(object.value());
        if (!morph) return core::Result<WorkspaceCommandPayload>::failure(morph.error());
        payload.morph = morph.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::face_remove_morph_target: {
        FaceRemoveMorphTarget payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto morph = codec::uint_member(members, "morph");
        if (!morph) return core::Result<WorkspaceCommandPayload>::failure(morph.error());
        payload.morph = character::MorphTargetId{morph.value()};
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::sim_bind_garment: {
        SimBindGarment payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto object = codec::object_member(members, "binding");
        if (!object) return core::Result<WorkspaceCommandPayload>::failure(object.error());
        const auto binding = garment_binding_from(object.value());
        if (!binding) return core::Result<WorkspaceCommandPayload>::failure(binding.error());
        payload.binding = binding.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::sim_unbind_garment: {
        SimUnbindGarment payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto binding = codec::uint_member(members, "binding");
        if (!binding) return core::Result<WorkspaceCommandPayload>::failure(binding.error());
        payload.binding = character::GarmentBindingId{binding.value()};
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::sim_enqueue_bake: {
        SimEnqueueBake payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto job_id = codec::uint_member(members, "job_id");
        if (!job_id) return core::Result<WorkspaceCommandPayload>::failure(job_id.error());
        payload.job_id = job_id.value();
        const auto start = time_member(members, "frame_start");
        if (!start) return core::Result<WorkspaceCommandPayload>::failure(start.error());
        payload.frame_start = start.value();
        const auto end = time_member(members, "frame_end");
        if (!end) return core::Result<WorkspaceCommandPayload>::failure(end.error());
        payload.frame_end = end.value();
        const auto max_frames = codec::u32_member(members, "max_frames");
        if (!max_frames) {
            return core::Result<WorkspaceCommandPayload>::failure(max_frames.error());
        }
        payload.max_frames = max_frames.value();
        const auto settings = digest_from(members, "settings_digest");
        if (!settings) return core::Result<WorkspaceCommandPayload>::failure(settings.error());
        payload.settings_digest = settings.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::sim_cancel_bake: {
        SimCancelBake payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto job_id = codec::uint_member(members, "job_id");
        if (!job_id) return core::Result<WorkspaceCommandPayload>::failure(job_id.error());
        payload.job_id = job_id.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::sim_discard_bake: {
        SimDiscardBake payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto job_id = codec::uint_member(members, "job_id");
        if (!job_id) return core::Result<WorkspaceCommandPayload>::failure(job_id.error());
        payload.job_id = job_id.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::sim_attach_bake_receipt: {
        SimAttachBakeReceipt payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto job_id = codec::uint_member(members, "job_id");
        if (!job_id) return core::Result<WorkspaceCommandPayload>::failure(job_id.error());
        payload.job_id = job_id.value();
        const auto receipt = digest_from(members, "receipt_digest");
        if (!receipt) return core::Result<WorkspaceCommandPayload>::failure(receipt.error());
        payload.receipt_digest = receipt.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::sim_clear_bake_receipt: {
        SimClearBakeReceipt payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto job_id = codec::uint_member(members, "job_id");
        if (!job_id) return core::Result<WorkspaceCommandPayload>::failure(job_id.error());
        payload.job_id = job_id.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    case WorkspaceCommandKind::review_record_gate: {
        ReviewRecordGate payload;
        const auto revision = expected_revision_member(members);
        if (!revision) return core::Result<WorkspaceCommandPayload>::failure(revision.error());
        payload.expected_source_revision = revision.value();
        const auto gate_id = codec::string_member(members, "gate_id");
        if (!gate_id) return core::Result<WorkspaceCommandPayload>::failure(gate_id.error());
        payload.gate_id = gate_id.value();
        const auto status = enum_member(
            members, "status", character::GateStatus::pass,
            character::GateStatus::not_run, character::gate_status_name);
        if (!status) return core::Result<WorkspaceCommandPayload>::failure(status.error());
        payload.status = status.value();
        const auto previous = nullable_enum_member(
            members, "previous_status", character::GateStatus::pass,
            character::GateStatus::not_run, character::gate_status_name);
        if (!previous) return core::Result<WorkspaceCommandPayload>::failure(previous.error());
        payload.previous_status = previous.value();
        return core::Result<WorkspaceCommandPayload>::success(
            WorkspaceCommandPayload{std::move(payload)});
    }
    }
    return core::Result<WorkspaceCommandPayload>::failure(
        codec::malformed("workspace command kind is unknown"));
}

} // namespace

std::string_view workspace_command_kind_name(WorkspaceCommandKind kind) noexcept {
    switch (kind) {
    case WorkspaceCommandKind::rig_add_joint: return "rig-add-joint";
    case WorkspaceCommandKind::rig_remove_joint: return "rig-remove-joint";
    case WorkspaceCommandKind::rig_set_rest: return "rig-set-rest";
    case WorkspaceCommandKind::rig_add_control: return "rig-add-control";
    case WorkspaceCommandKind::rig_remove_control: return "rig-remove-control";
    case WorkspaceCommandKind::rig_set_control_axes: return "rig-set-control-axes";
    case WorkspaceCommandKind::rig_bind_attachment: return "rig-bind-attachment";
    case WorkspaceCommandKind::rig_unbind_attachment: return "rig-unbind-attachment";
    case WorkspaceCommandKind::skin_paint_stamp: return "skin-paint-stamp";
    case WorkspaceCommandKind::skin_set_weight_lock: return "skin-set-weight-lock";
    case WorkspaceCommandKind::skin_rebind_layer: return "skin-rebind-layer";
    case WorkspaceCommandKind::animate_set_key: return "animate-set-key";
    case WorkspaceCommandKind::animate_remove_key: return "animate-remove-key";
    case WorkspaceCommandKind::animate_set_root_motion: return "animate-set-root-motion";
    case WorkspaceCommandKind::face_add_morph_target: return "face-add-morph-target";
    case WorkspaceCommandKind::face_remove_morph_target: return "face-remove-morph-target";
    case WorkspaceCommandKind::sim_bind_garment: return "sim-bind-garment";
    case WorkspaceCommandKind::sim_unbind_garment: return "sim-unbind-garment";
    case WorkspaceCommandKind::sim_enqueue_bake: return "sim-enqueue-bake";
    case WorkspaceCommandKind::sim_cancel_bake: return "sim-cancel-bake";
    case WorkspaceCommandKind::sim_discard_bake: return "sim-discard-bake";
    case WorkspaceCommandKind::sim_attach_bake_receipt: return "sim-attach-bake-receipt";
    case WorkspaceCommandKind::sim_clear_bake_receipt: return "sim-clear-bake-receipt";
    case WorkspaceCommandKind::review_record_gate: return "review-record-gate";
    }
    return "unknown";
}

core::Revision WorkspaceCommand::expected_source_revision() const noexcept {
    return std::visit(
        [](const auto& payload) { return payload.expected_source_revision; },
        payload);
}

core::Result<void> WorkspaceCommand::validate() const {
    if (static_cast<std::uint8_t>(domain) >
        static_cast<std::uint8_t>(WorkspaceDomain::review)) {
        return core::Result<void>::failure(invalid(
            "workspace command domain is unknown"));
    }
    if (!target_character) {
        return core::Result<void>::failure(invalid(
            "workspace command carries the invalid zero character identity"));
    }
    if (domain != kind_domain(kind())) {
        return core::Result<void>::failure(invalid(
            "workspace command payload kind does not belong to its domain"));
    }
    return std::visit(
        [](const auto& payload) { return validate_payload(payload); }, payload);
}

std::string WorkspaceCommand::serialize() const {
    codec::ObjectWriter writer;
    writer.add("schema", static_cast<std::uint64_t>(kCommandSchemaVersion));
    writer.add("domain", workspace_domain_name(domain));
    writer.add("target_character", target_character.value);
    writer.add("kind", workspace_command_kind_name(kind()));
    writer.add_raw("payload", payload_json(payload));
    return std::move(writer).finish();
}

core::Result<WorkspaceCommand> WorkspaceCommand::deserialize(std::string_view text) {
    const auto members = core::json::parse_object(text);
    if (!members) {
        return core::Result<WorkspaceCommand>::failure(members.error());
    }
    const auto schema = codec::uint_member(members.value(), "schema");
    if (!schema) return core::Result<WorkspaceCommand>::failure(schema.error());
    if (schema.value() != kCommandSchemaVersion) {
        return core::Result<WorkspaceCommand>::failure(codec::malformed(
            "workspace command schema version is not readable"));
    }
    const auto domain_raw = codec::string_member(members.value(), "domain");
    if (!domain_raw) {
        return core::Result<WorkspaceCommand>::failure(domain_raw.error());
    }
    const auto parsed_domain = domain_from(domain_raw.value());
    if (!parsed_domain) {
        return core::Result<WorkspaceCommand>::failure(parsed_domain.error());
    }
    const auto target = codec::uint_member(members.value(), "target_character");
    if (!target) return core::Result<WorkspaceCommand>::failure(target.error());
    const auto kind_raw = codec::string_member(members.value(), "kind");
    if (!kind_raw) return core::Result<WorkspaceCommand>::failure(kind_raw.error());
    const auto parsed_kind = kind_from(kind_raw.value());
    if (!parsed_kind) {
        return core::Result<WorkspaceCommand>::failure(parsed_kind.error());
    }
    const auto payload = codec::object_member(members.value(), "payload");
    if (!payload) return core::Result<WorkspaceCommand>::failure(payload.error());
    const auto parsed_payload = payload_from(parsed_kind.value(), payload.value());
    if (!parsed_payload) {
        return core::Result<WorkspaceCommand>::failure(parsed_payload.error());
    }
    WorkspaceCommand command;
    command.domain = parsed_domain.value();
    command.target_character = character::CharacterId{target.value()};
    command.payload = std::move(parsed_payload.value());
    if (auto result = command.validate(); !result) {
        return core::Result<WorkspaceCommand>::failure(result.error());
    }
    return core::Result<WorkspaceCommand>::success(std::move(command));
}

core::Result<WorkspaceCommand> invert_command(
    const WorkspaceCommand& command, const CharacterSourceStore& store) {
    const CharacterSourceBundle* bundle = store.find(command.target_character);
    if (bundle == nullptr) {
        return core::Result<WorkspaceCommand>::failure(not_found(
            "workspace character is not registered"));
    }
    const core::Revision revision = command.expected_source_revision();
    const auto inverse = [&command](WorkspaceCommandPayload payload) {
        return core::Result<WorkspaceCommand>::success(WorkspaceCommand{
            command.domain, command.target_character, std::move(payload)});
    };
    switch (command.kind()) {
    case WorkspaceCommandKind::rig_add_joint: {
        const auto& applied = std::get<RigAddJoint>(command.payload);
        return inverse(RigRemoveJoint{revision, applied.joint});
    }
    case WorkspaceCommandKind::rig_remove_joint: {
        const auto& applied = std::get<RigRemoveJoint>(command.payload);
        const kinematic::KinematicNode* node = bundle->graph.find(applied.joint);
        if (node == nullptr) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace joint is not present in the character graph"));
        }
        return inverse(RigAddJoint{revision, applied.joint, node->parent,
                                   node->rest, node->kind, node->semantic_type});
    }
    case WorkspaceCommandKind::rig_set_rest: {
        const auto& applied = std::get<RigSetRest>(command.payload);
        const kinematic::KinematicNode* node = bundle->graph.find(applied.joint);
        if (node == nullptr) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace joint is not present in the character graph"));
        }
        return inverse(
            RigSetRest{revision, applied.joint, node->rest});
    }
    case WorkspaceCommandKind::rig_add_control: {
        const auto& applied = std::get<RigAddControl>(command.payload);
        return inverse(RigRemoveControl{revision, applied.control});
    }
    case WorkspaceCommandKind::rig_remove_control: {
        const auto& applied = std::get<RigRemoveControl>(command.payload);
        const character::ControlDefinition* control =
            bundle->rig.control_graph.find(applied.control);
        if (control == nullptr) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace control is not present in the character rig"));
        }
        return inverse(RigAddControl{revision, applied.control, control->shape,
                                     control->transform, control->affected_joints,
                                     control->allowed_axes, control->control_set});
    }
    case WorkspaceCommandKind::rig_set_control_axes: {
        const auto& applied = std::get<RigSetControlAxes>(command.payload);
        const character::ControlDefinition* control =
            bundle->rig.control_graph.find(applied.control);
        if (control == nullptr) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace control is not present in the character rig"));
        }
        return inverse(RigSetControlAxes{
            revision, applied.control, control->allowed_axes});
    }
    case WorkspaceCommandKind::rig_bind_attachment: {
        const auto& applied = std::get<RigBindAttachment>(command.payload);
        return inverse(
            RigUnbindAttachment{revision, applied.joint, applied.mesh.asset_id});
    }
    case WorkspaceCommandKind::rig_unbind_attachment: {
        const auto& applied = std::get<RigUnbindAttachment>(command.payload);
        const AttachmentBinding* stored = nullptr;
        for (const AttachmentBinding& attachment : bundle->attachments) {
            if (attachment.joint == applied.joint &&
                attachment.mesh.asset_id == applied.mesh_asset) {
                stored = &attachment;
                break;
            }
        }
        if (stored == nullptr) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace attachment is not recorded on the character"));
        }
        return inverse(RigBindAttachment{revision, applied.joint, stored->mesh,
                                         stored->offset, stored->role});
    }
    case WorkspaceCommandKind::skin_paint_stamp: {
        const auto& applied = std::get<SkinPaintStamp>(command.payload);
        if (!bundle->skin.has_value()) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace character carries no skin binding to restore"));
        }
        const auto found = bundle->skin->influences.find(applied.vertex);
        if (found == bundle->skin->influences.end()) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace skin vertex carries no recorded influences to restore"));
        }
        return inverse(SkinPaintStamp{
            revision, applied.vertex, found->second});
    }
    case WorkspaceCommandKind::skin_set_weight_lock: {
        const auto& applied = std::get<SkinSetWeightLock>(command.payload);
        const bool locked = std::binary_search(bundle->weight_locks.begin(),
                                               bundle->weight_locks.end(),
                                               applied.vertex);
        return inverse(SkinSetWeightLock{revision, applied.vertex, locked});
    }
    case WorkspaceCommandKind::skin_rebind_layer: {
        const auto& applied = std::get<SkinRebindLayer>(command.payload);
        if (!bundle->skin_layer.has_value()) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace character carries no skin layer to restore"));
        }
        const character::SkinWeightLayer& layer = bundle->skin_layer.value();
        return inverse(SkinRebindLayer{revision, applied.joint_graph_digest,
                                       layer.staleness, layer.stale_since,
                                       layer.joint_graph_digest});
    }
    case WorkspaceCommandKind::animate_set_key: {
        const auto& applied = std::get<AnimateSetKey>(command.payload);
        return inverse(
            AnimateRemoveKey{revision, applied.channel, applied.time});
    }
    case WorkspaceCommandKind::animate_remove_key: {
        const auto& applied = std::get<AnimateRemoveKey>(command.payload);
        if (!bundle->channel.has_value() ||
            bundle->channel->id != applied.channel) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace channel is not present in the character bundle"));
        }
        const double seconds = character::to_seconds(applied.time).seconds;
        const temporal::ScalarKeyframe* stored = nullptr;
        for (const temporal::ScalarKeyframe& key :
             bundle->channel->scalar_keyframes) {
            if (key.time == seconds) {
                stored = &key;
                break;
            }
        }
        if (stored == nullptr) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace channel carries no stored key at the removed time"));
        }
        return inverse(AnimateSetKey{
            revision, applied.channel, applied.time, stored->value});
    }
    case WorkspaceCommandKind::animate_set_root_motion: {
        if (!bundle->motion.has_value()) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace character carries no motion document to restore"));
        }
        return inverse(AnimateSetRootMotion{
            revision, bundle->motion->root_motion});
    }
    case WorkspaceCommandKind::face_add_morph_target: {
        const auto& applied = std::get<FaceAddMorphTarget>(command.payload);
        return inverse(FaceRemoveMorphTarget{revision, applied.morph.id});
    }
    case WorkspaceCommandKind::face_remove_morph_target: {
        const auto& applied = std::get<FaceRemoveMorphTarget>(command.payload);
        const character::MorphTarget* stored = nullptr;
        for (const character::MorphTarget& morph : bundle->morphs) {
            if (morph.id == applied.morph) {
                stored = &morph;
                break;
            }
        }
        if (stored == nullptr) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace morph target is not present in the character bundle"));
        }
        return inverse(FaceAddMorphTarget{revision, *stored});
    }
    case WorkspaceCommandKind::sim_bind_garment: {
        const auto& applied = std::get<SimBindGarment>(command.payload);
        return inverse(SimUnbindGarment{revision, applied.binding.id});
    }
    case WorkspaceCommandKind::sim_unbind_garment: {
        const auto& applied = std::get<SimUnbindGarment>(command.payload);
        const character::GarmentBinding* stored = nullptr;
        for (const character::GarmentBinding& binding : bundle->garments) {
            if (binding.id == applied.binding) {
                stored = &binding;
                break;
            }
        }
        if (stored == nullptr) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace garment binding is not present in the character bundle"));
        }
        return inverse(SimBindGarment{revision, *stored});
    }
    case WorkspaceCommandKind::sim_enqueue_bake: {
        const auto& applied = std::get<SimEnqueueBake>(command.payload);
        return inverse(SimDiscardBake{revision, applied.job_id});
    }
    case WorkspaceCommandKind::sim_cancel_bake: {
        return core::Result<WorkspaceCommand>::failure(unsupported(
            "workspace bake cancellation has no exact inverse without "
            "previous-state bookkeeping"));
    }
    case WorkspaceCommandKind::sim_discard_bake: {
        return core::Result<WorkspaceCommand>::failure(unsupported(
            "workspace discarded bake record cannot be restored"));
    }
    case WorkspaceCommandKind::sim_attach_bake_receipt: {
        const auto& applied = std::get<SimAttachBakeReceipt>(command.payload);
        return inverse(SimClearBakeReceipt{revision, applied.job_id});
    }
    case WorkspaceCommandKind::sim_clear_bake_receipt: {
        const auto& applied = std::get<SimClearBakeReceipt>(command.payload);
        const BakeJob* stored = nullptr;
        for (const BakeJob& job : bundle->bake_jobs) {
            if (job.job_id == applied.job_id) {
                stored = &job;
                break;
            }
        }
        if (stored == nullptr) {
            return core::Result<WorkspaceCommand>::failure(not_found(
                "workspace bake job is not registered"));
        }
        if (stored->status != BakeJobStatus::receipted ||
            !stored->receipt_digest.has_value()) {
            return core::Result<WorkspaceCommand>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "workspace bake job carries no attached receipt to restore"));
        }
        return inverse(SimAttachBakeReceipt{
            revision, applied.job_id, stored->receipt_digest.value()});
    }
    case WorkspaceCommandKind::review_record_gate: {
        const auto& applied = std::get<ReviewRecordGate>(command.payload);
        const auto found = bundle->review_gates.find(applied.gate_id);
        std::optional<character::GateStatus> previous;
        if (found != bundle->review_gates.end()) {
            previous = found->second;
        }
        const character::GateStatus restore =
            applied.previous_status.value_or(
                previous.value_or(character::GateStatus::not_run));
        return inverse(ReviewRecordGate{
            revision, applied.gate_id, restore, previous});
    }
    }
    return core::Result<WorkspaceCommand>::failure(unsupported(
        "workspace command kind has no inverse"));
}

} // namespace carto::workspace
