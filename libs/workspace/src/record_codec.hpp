// Private record codecs shared by the workspace sources: the small set of
// character/kinematic record fragments that commands, proposals and the store
// all emit and parse. Internal to carto_workspace; never installed.

#pragma once

#include "codec.hpp"

#include <carto/character/bindings.hpp>
#include <carto/character/definition.hpp>
#include <carto/character/morph.hpp>
#include <carto/core/math.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace carto::workspace::record {

inline bool safe_text(std::string_view value, std::size_t maximum,
                      bool allow_empty = false) {
    if ((value.empty() && !allow_empty) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

// Scans a foundation name function to parse a value back; the foundation owns
// the spelling, the workspace codec only maps text to the enumeration.
template <typename Enum>
core::Result<Enum> scan_enum(std::string_view name, Enum first, Enum last,
                             std::string_view (*name_of)(Enum)) {
    using Raw = std::underlying_type_t<Enum>;
    for (Raw raw = static_cast<Raw>(first); raw <= static_cast<Raw>(last); ++raw) {
        const Enum value = static_cast<Enum>(raw);
        if (name_of(value) == name) {
            return core::Result<Enum>::success(value);
        }
    }
    return core::Result<Enum>::failure(codec::malformed(
        "workspace enum value is unknown"));
}

inline std::string vec3_json(const core::Vec3d& value) {
    std::string out = "[";
    out += codec::number(value.x);
    out.push_back(',');
    out += codec::number(value.y);
    out.push_back(',');
    out += codec::number(value.z);
    out.push_back(']');
    return out;
}

inline core::Result<core::Vec3d> vec3_from(std::string_view raw) {
    const auto elements = codec::split_array(raw);
    if (!elements || elements.value().size() != 3U) {
        return core::Result<core::Vec3d>::failure(codec::malformed(
            "workspace vector must have exactly three components"));
    }
    core::Vec3d value;
    const auto x = codec::parse_double(elements.value()[0]);
    if (!x) return core::Result<core::Vec3d>::failure(x.error());
    const auto y = codec::parse_double(elements.value()[1]);
    if (!y) return core::Result<core::Vec3d>::failure(y.error());
    const auto z = codec::parse_double(elements.value()[2]);
    if (!z) return core::Result<core::Vec3d>::failure(z.error());
    value.x = x.value();
    value.y = y.value();
    value.z = z.value();
    if (!value.finite()) {
        return core::Result<core::Vec3d>::failure(codec::malformed(
            "workspace vector is not finite"));
    }
    return core::Result<core::Vec3d>::success(value);
}

inline std::string quat_json(const core::Quaternion& value) {
    std::string out = "[";
    out += codec::number(value.x);
    out.push_back(',');
    out += codec::number(value.y);
    out.push_back(',');
    out += codec::number(value.z);
    out.push_back(',');
    out += codec::number(value.w);
    out.push_back(']');
    return out;
}

inline core::Result<core::Quaternion> quat_from(std::string_view raw) {
    const auto elements = codec::split_array(raw);
    if (!elements || elements.value().size() != 4U) {
        return core::Result<core::Quaternion>::failure(codec::malformed(
            "workspace quaternion must have exactly four components"));
    }
    core::Quaternion value;
    const auto x = codec::parse_double(elements.value()[0]);
    if (!x) return core::Result<core::Quaternion>::failure(x.error());
    const auto y = codec::parse_double(elements.value()[1]);
    if (!y) return core::Result<core::Quaternion>::failure(y.error());
    const auto z = codec::parse_double(elements.value()[2]);
    if (!z) return core::Result<core::Quaternion>::failure(z.error());
    const auto w = codec::parse_double(elements.value()[3]);
    if (!w) return core::Result<core::Quaternion>::failure(w.error());
    value.x = x.value();
    value.y = y.value();
    value.z = z.value();
    value.w = w.value();
    if (!value.finite() || !value.normalizable()) {
        return core::Result<core::Quaternion>::failure(codec::malformed(
            "workspace quaternion is not finite or not normalizable"));
    }
    return core::Result<core::Quaternion>::success(value);
}

inline std::string transform_json(const core::Transform& value) {
    codec::ObjectWriter writer;
    writer.add_raw("translation", vec3_json(value.translation));
    writer.add_raw("rotation", quat_json(value.rotation));
    writer.add_raw("scale", vec3_json(value.scale));
    return std::move(writer).finish();
}

inline core::Result<core::Transform> transform_from(
    const std::vector<core::json::Member>& members) {
    const auto translation = codec::find(members, "translation");
    if (translation == nullptr) {
        return core::Result<core::Transform>::failure(codec::missing("translation"));
    }
    const auto parsed_translation = vec3_from(translation->raw_value);
    if (!parsed_translation) {
        return core::Result<core::Transform>::failure(parsed_translation.error());
    }
    const auto rotation = codec::find(members, "rotation");
    if (rotation == nullptr) {
        return core::Result<core::Transform>::failure(codec::missing("rotation"));
    }
    const auto parsed_rotation = quat_from(rotation->raw_value);
    if (!parsed_rotation) {
        return core::Result<core::Transform>::failure(parsed_rotation.error());
    }
    const auto scale = codec::find(members, "scale");
    if (scale == nullptr) {
        return core::Result<core::Transform>::failure(codec::missing("scale"));
    }
    const auto parsed_scale = vec3_from(scale->raw_value);
    if (!parsed_scale) {
        return core::Result<core::Transform>::failure(parsed_scale.error());
    }
    core::Transform value;
    value.translation = parsed_translation.value();
    value.rotation = parsed_rotation.value();
    value.scale = parsed_scale.value();
    return core::Result<core::Transform>::success(value);
}

inline std::string mesh_asset_ref_json(const character::MeshAssetRef& ref) {
    codec::ObjectWriter writer;
    writer.add("asset_id", ref.asset_id);
    writer.add("content_digest", ref.content_digest);
    return std::move(writer).finish();
}

inline core::Result<character::MeshAssetRef> mesh_asset_ref_from(
    const std::vector<core::json::Member>& members) {
    character::MeshAssetRef ref;
    const auto asset = codec::uint_member(members, "asset_id");
    if (!asset) return core::Result<character::MeshAssetRef>::failure(asset.error());
    ref.asset_id = asset.value();
    const auto digest = codec::digest_member(members, "content_digest");
    if (!digest) return core::Result<character::MeshAssetRef>::failure(digest.error());
    ref.content_digest = digest.value();
    return core::Result<character::MeshAssetRef>::success(ref);
}

inline std::string morph_target_json(const character::MorphTarget& morph) {
    codec::ObjectWriter writer;
    writer.add("id", morph.id.value);
    writer.add("source_mesh_asset", morph.source_mesh_asset);
    writer.add("source_mesh_digest", morph.source_mesh_digest);
    writer.add("displaced_point_count", morph.displaced_point_count);
    writer.add("delta_blob_digest", morph.delta_blob_digest);
    writer.add("delta_blob_bytes", morph.delta_blob_bytes);
    writer.add("topology_correspondence_digest", morph.topology_correspondence_digest);
    writer.add("tangent_in", character::tangent_policy_name(morph.tangent_in));
    writer.add("tangent_out", character::tangent_policy_name(morph.tangent_out));
    writer.add("precision_bound", morph.precision_bound);
    return std::move(writer).finish();
}

inline core::Result<character::MorphTarget> morph_target_from(
    const std::vector<core::json::Member>& members) {
    character::MorphTarget morph;
    const auto id = codec::uint_member(members, "id");
    if (!id) return core::Result<character::MorphTarget>::failure(id.error());
    morph.id = character::MorphTargetId{id.value()};
    const auto mesh = codec::uint_member(members, "source_mesh_asset");
    if (!mesh) return core::Result<character::MorphTarget>::failure(mesh.error());
    morph.source_mesh_asset = mesh.value();
    const auto mesh_digest = codec::digest_member(members, "source_mesh_digest");
    if (!mesh_digest) {
        return core::Result<character::MorphTarget>::failure(mesh_digest.error());
    }
    morph.source_mesh_digest = mesh_digest.value();
    const auto displaced = codec::uint_member(members, "displaced_point_count");
    if (!displaced) {
        return core::Result<character::MorphTarget>::failure(displaced.error());
    }
    morph.displaced_point_count = displaced.value();
    const auto delta_digest = codec::digest_member(members, "delta_blob_digest");
    if (!delta_digest) {
        return core::Result<character::MorphTarget>::failure(delta_digest.error());
    }
    morph.delta_blob_digest = delta_digest.value();
    const auto delta_bytes = codec::uint_member(members, "delta_blob_bytes");
    if (!delta_bytes) {
        return core::Result<character::MorphTarget>::failure(delta_bytes.error());
    }
    morph.delta_blob_bytes = delta_bytes.value();
    const auto correspondence = codec::digest_member(
        members, "topology_correspondence_digest");
    if (!correspondence) {
        return core::Result<character::MorphTarget>::failure(correspondence.error());
    }
    morph.topology_correspondence_digest = correspondence.value();
    const auto tangent_in = codec::string_member(members, "tangent_in");
    if (!tangent_in) {
        return core::Result<character::MorphTarget>::failure(tangent_in.error());
    }
    const auto parsed_in = scan_enum(
        tangent_in.value(), character::TangentPolicy::none,
        character::TangentPolicy::recompute, character::tangent_policy_name);
    if (!parsed_in) return core::Result<character::MorphTarget>::failure(parsed_in.error());
    morph.tangent_in = parsed_in.value();
    const auto tangent_out = codec::string_member(members, "tangent_out");
    if (!tangent_out) {
        return core::Result<character::MorphTarget>::failure(tangent_out.error());
    }
    const auto parsed_out = scan_enum(
        tangent_out.value(), character::TangentPolicy::none,
        character::TangentPolicy::recompute, character::tangent_policy_name);
    if (!parsed_out) {
        return core::Result<character::MorphTarget>::failure(parsed_out.error());
    }
    morph.tangent_out = parsed_out.value();
    const auto precision = codec::double_member(members, "precision_bound");
    if (!precision) {
        return core::Result<character::MorphTarget>::failure(precision.error());
    }
    morph.precision_bound = precision.value();
    return core::Result<character::MorphTarget>::success(std::move(morph));
}


inline std::string binding_attachment_array_json(
    const std::vector<character::BindingAttachment>& attachments) {
    std::string out = "[";
    bool first = true;
    for (const character::BindingAttachment& attachment : attachments) {
        if (!first) out.push_back(',');
        first = false;
        codec::ObjectWriter writer;
        writer.add("joint", attachment.joint.value);
        writer.add("control", attachment.control.value);
        out += std::move(writer).finish();
    }
    out.push_back(']');
    return out;
}

inline core::Result<std::vector<character::BindingAttachment>> binding_attachment_array_from(
    std::string_view raw, std::size_t max_entries) {
    const auto elements = codec::split_array(raw);
    if (!elements) {
        return core::Result<std::vector<character::BindingAttachment>>::failure(
            elements.error());
    }
    if (elements.value().size() > max_entries) {
        return core::Result<std::vector<character::BindingAttachment>>::failure(
            codec::malformed("workspace binding attachment table exceeds its bounds"));
    }
    std::vector<character::BindingAttachment> attachments;
    attachments.reserve(elements.value().size());
    for (const std::string_view element : elements.value()) {
        const auto members = core::json::parse_object(element);
        if (!members) {
            return core::Result<std::vector<character::BindingAttachment>>::failure(
                members.error());
        }
        const auto joint = codec::uint_member(members.value(), "joint");
        if (!joint) {
            return core::Result<std::vector<character::BindingAttachment>>::failure(
                joint.error());
        }
        const auto control = codec::uint_member(members.value(), "control");
        if (!control) {
            return core::Result<std::vector<character::BindingAttachment>>::failure(
                control.error());
        }
        attachments.push_back(character::BindingAttachment{
            kinematic::JointId{joint.value()}, character::ControlId{control.value()}});
    }
    return core::Result<std::vector<character::BindingAttachment>>::success(
        std::move(attachments));
}

inline std::string pinned_source_json(const character::PinnedSourceRef& source) {
    codec::ObjectWriter writer;
    writer.add("source_id", source.source_id);
    writer.add("pinned_revision", source.pinned_revision.value());
    writer.add("digest", source.digest);
    return std::move(writer).finish();
}

inline core::Result<character::PinnedSourceRef> pinned_source_from(
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
    const auto digest = codec::digest_member(members, "digest");
    if (!digest) return core::Result<character::PinnedSourceRef>::failure(digest.error());
    source.digest = digest.value();
    return core::Result<character::PinnedSourceRef>::success(source);
}

inline std::string lod_mapping_json(const std::map<std::uint32_t, assets::Sha256Digest>& mapping) {
    std::string out = "{";
    bool first = true;
    for (const auto& [lod, digest] : mapping) {
        if (!first) out.push_back(',');
        first = false;
        out += codec::quote(codec::u64(lod));
        out.push_back(':');
        out += codec::quote(digest.hex());
    }
    out.push_back('}');
    return out;
}

inline core::Result<std::map<std::uint32_t, assets::Sha256Digest>> lod_mapping_from(
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

inline std::string seam_array_json(const std::vector<character::SeamRef>& seams) {
    std::string out = "[";
    bool first = true;
    for (const character::SeamRef& seam : seams) {
        if (!first) out.push_back(',');
        first = false;
        codec::ObjectWriter writer;
        writer.add("seam_id", seam.seam_id);
        writer.add("attachment_index", static_cast<std::uint64_t>(seam.attachment_index));
        out += std::move(writer).finish();
    }
    out.push_back(']');
    return out;
}

inline std::string guide_array_json(const std::vector<character::GuideRef>& guides) {
    std::string out = "[";
    bool first = true;
    for (const character::GuideRef& guide : guides) {
        if (!first) out.push_back(',');
        first = false;
        codec::ObjectWriter writer;
        writer.add("guide_id", guide.guide_id);
        writer.add("attachment_index", static_cast<std::uint64_t>(guide.attachment_index));
        out += std::move(writer).finish();
    }
    out.push_back(']');
    return out;
}

inline std::string garment_binding_json(const character::GarmentBinding& binding) {
    codec::ObjectWriter writer;
    writer.add("id", binding.id.value);
    writer.add_raw("source_mesh", pinned_source_json(binding.source_mesh));
    writer.add_raw("attachments", binding_attachment_array_json(binding.attachments));
    writer.add_raw("seams", seam_array_json(binding.seams));
    writer.add_raw("collision_filter_refs",
        codec::digest_array_json(binding.collision_filter_refs));
    writer.add("material_intent_ref", binding.material_intent_ref);
    writer.add_raw("lod_mapping", lod_mapping_json(binding.lod_mapping));
    return std::move(writer).finish();
}

inline core::Result<character::GarmentBinding> garment_binding_from(
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
    const auto parsed_attachments = binding_attachment_array_from(
        attachments->raw_value, character::GarmentBinding::kMaxAttachments);
    if (!parsed_attachments) {
        return core::Result<character::GarmentBinding>::failure(parsed_attachments.error());
    }
    binding.attachments = parsed_attachments.value();
    const core::json::Member* seams = codec::find(members, "seams");
    if (seams == nullptr) {
        return core::Result<character::GarmentBinding>::failure(codec::missing("seams"));
    }
    const auto elements = codec::split_array(seams->raw_value);
    if (!elements) {
        return core::Result<character::GarmentBinding>::failure(elements.error());
    }
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
        if (!index) return core::Result<character::GarmentBinding>::failure(index.error());
        binding.seams.push_back(
            character::SeamRef{seam_id.value(), index.value()});
    }
    const core::json::Member* filters = codec::find(members, "collision_filter_refs");
    if (filters == nullptr) {
        return core::Result<character::GarmentBinding>::failure(
            codec::missing("collision_filter_refs"));
    }
    const auto parsed_filters = codec::digest_array_from(
        filters->raw_value, character::GarmentBinding::kMaxCollisionFilters);
    if (!parsed_filters) {
        return core::Result<character::GarmentBinding>::failure(parsed_filters.error());
    }
    binding.collision_filter_refs = parsed_filters.value();
    const auto material = codec::digest_member(members, "material_intent_ref");
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

inline std::string groom_binding_json(const character::GroomBinding& binding) {
    codec::ObjectWriter writer;
    writer.add("id", binding.id.value);
    writer.add_raw("source_guides", pinned_source_json(binding.source_guides));
    writer.add_raw("attachments", binding_attachment_array_json(binding.attachments));
    writer.add_raw("guides", guide_array_json(binding.guides));
    writer.add_raw("collision_filter_refs",
        codec::digest_array_json(binding.collision_filter_refs));
    writer.add("material_intent_ref", binding.material_intent_ref);
    writer.add_raw("lod_mapping", lod_mapping_json(binding.lod_mapping));
    return std::move(writer).finish();
}

inline core::Result<character::GroomBinding> groom_binding_from(
    const std::vector<core::json::Member>& members) {
    character::GroomBinding binding;
    const auto id = codec::uint_member(members, "id");
    if (!id) return core::Result<character::GroomBinding>::failure(id.error());
    binding.id = character::GroomBindingId{id.value()};
    const auto source = codec::object_member(members, "source_guides");
    if (!source) return core::Result<character::GroomBinding>::failure(source.error());
    const auto parsed_source = pinned_source_from(source.value());
    if (!parsed_source) {
        return core::Result<character::GroomBinding>::failure(parsed_source.error());
    }
    binding.source_guides = parsed_source.value();
    const core::json::Member* attachments = codec::find(members, "attachments");
    if (attachments == nullptr) {
        return core::Result<character::GroomBinding>::failure(
            codec::missing("attachments"));
    }
    const auto parsed_attachments = binding_attachment_array_from(
        attachments->raw_value, character::GroomBinding::kMaxAttachments);
    if (!parsed_attachments) {
        return core::Result<character::GroomBinding>::failure(parsed_attachments.error());
    }
    binding.attachments = parsed_attachments.value();
    const core::json::Member* guides = codec::find(members, "guides");
    if (guides == nullptr) {
        return core::Result<character::GroomBinding>::failure(codec::missing("guides"));
    }
    const auto elements = codec::split_array(guides->raw_value);
    if (!elements) {
        return core::Result<character::GroomBinding>::failure(elements.error());
    }
    if (elements.value().size() > character::GroomBinding::kMaxGuides) {
        return core::Result<character::GroomBinding>::failure(codec::malformed(
            "workspace guide table exceeds its bounded capacity"));
    }
    for (const std::string_view element : elements.value()) {
        const auto guide_members = core::json::parse_object(element);
        if (!guide_members) {
            return core::Result<character::GroomBinding>::failure(guide_members.error());
        }
        const auto guide_id = codec::uint_member(guide_members.value(), "guide_id");
        if (!guide_id) {
            return core::Result<character::GroomBinding>::failure(guide_id.error());
        }
        const auto index = codec::u32_member(guide_members.value(), "attachment_index");
        if (!index) return core::Result<character::GroomBinding>::failure(index.error());
        binding.guides.push_back(
            character::GuideRef{guide_id.value(), index.value()});
    }
    const core::json::Member* filters = codec::find(members, "collision_filter_refs");
    if (filters == nullptr) {
        return core::Result<character::GroomBinding>::failure(
            codec::missing("collision_filter_refs"));
    }
    const auto parsed_filters = codec::digest_array_from(
        filters->raw_value, character::GroomBinding::kMaxCollisionFilters);
    if (!parsed_filters) {
        return core::Result<character::GroomBinding>::failure(parsed_filters.error());
    }
    binding.collision_filter_refs = parsed_filters.value();
    const auto material = codec::digest_member(members, "material_intent_ref");
    if (!material) {
        return core::Result<character::GroomBinding>::failure(material.error());
    }
    binding.material_intent_ref = material.value();
    const auto lod = lod_mapping_from(
        members, "lod_mapping", character::GroomBinding::kMaxLod);
    if (!lod) return core::Result<character::GroomBinding>::failure(lod.error());
    binding.lod_mapping = lod.value();
    return core::Result<character::GroomBinding>::success(std::move(binding));
}

} // namespace carto::workspace::record

