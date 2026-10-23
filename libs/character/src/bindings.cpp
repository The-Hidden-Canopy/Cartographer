#include <carto/character/bindings.hpp>

#include <algorithm>
#include <tuple>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Result<void> validate_common(
    const PinnedSourceRef& source,
    const std::vector<BindingAttachment>& attachments,
    const std::vector<assets::Sha256Digest>& collision_filters,
    const assets::Sha256Digest& material_intent,
    const std::map<std::uint32_t, assets::Sha256Digest>& lod_mapping,
    std::size_t max_attachments,
    std::size_t max_filters,
    std::uint32_t max_lod) {
    if (auto result = source.validate(); !result) return result;
    if (attachments.empty() || attachments.size() > max_attachments) {
        return core::Result<void>::failure(validation(
            "binding requires a bounded non-empty attachment set"));
    }
    for (const BindingAttachment& attachment : attachments) {
        if (auto result = attachment.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < attachments.size(); ++index) {
        if (!(attachments[index - 1U] < attachments[index])) {
            return core::Result<void>::failure(validation(
                "binding attachments must be sorted and unique"));
        }
    }
    if (collision_filters.size() > max_filters) {
        return core::Result<void>::failure(validation(
            "binding collision filters exceed their bounded capacity"));
    }
    for (std::size_t index = 0U; index < collision_filters.size(); ++index) {
        if (collision_filters[index].is_zero()) {
            return core::Result<void>::failure(invalid(
                "binding collision filter references must be content-addressed"));
        }
        if (index > 0U && !(collision_filters[index - 1U] < collision_filters[index])) {
            return core::Result<void>::failure(validation(
                "binding collision filters must be sorted and unique"));
        }
    }
    if (material_intent.is_zero()) {
        return core::Result<void>::failure(invalid(
            "binding material intent must be content-addressed"));
    }
    if (lod_mapping.empty() || lod_mapping.size() > max_lod) {
        return core::Result<void>::failure(validation(
            "binding LOD mapping must be bounded and non-empty"));
    }
    for (const auto& [lod, digest] : lod_mapping) {
        if (digest.is_zero()) {
            return core::Result<void>::failure(invalid(
                "binding LOD products must be content-addressed"));
        }
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> PinnedSourceRef::validate() const {
    if (source_id == 0U || pinned_revision.value() == 0U || digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "pinned source reference requires an identity, revision, and digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> BindingAttachment::validate() const {
    if ((joint.value != 0U) == (control.value != 0U)) {
        return core::Result<void>::failure(invalid(
            "binding attachment must set exactly one joint or control"));
    }
    return core::Result<void>::success();
}

bool BindingAttachment::operator<(const BindingAttachment& other) const noexcept {
    return std::tuple{joint.value, control.value} <
        std::tuple{other.joint.value, other.control.value};
}

core::Result<void> GarmentBinding::validate() const {
    if (!id) {
        return core::Result<void>::failure(invalid(
            "garment binding identity is invalid"));
    }
    if (auto result = validate_common(source_mesh, attachments, collision_filter_refs,
            material_intent_ref, lod_mapping, kMaxAttachments, kMaxCollisionFilters,
            kMaxLod);
        !result) {
        return result;
    }
    if (seams.size() > kMaxSeams) {
        return core::Result<void>::failure(validation(
            "garment seams exceed their bounded capacity"));
    }
    for (const SeamRef& seam : seams) {
        if (seam.seam_id == 0U) {
            return core::Result<void>::failure(invalid(
                "garment seam identity must be non-zero"));
        }
        if (seam.attachment_index >= attachments.size()) {
            return core::Result<void>::failure(validation(
                "garment seam references an orphan attachment index"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> GroomBinding::validate() const {
    if (!id) {
        return core::Result<void>::failure(invalid(
            "groom binding identity is invalid"));
    }
    if (auto result = validate_common(source_guides, attachments, collision_filter_refs,
            material_intent_ref, lod_mapping, kMaxAttachments, kMaxCollisionFilters,
            kMaxLod);
        !result) {
        return result;
    }
    if (guides.size() > kMaxGuides) {
        return core::Result<void>::failure(validation(
            "groom guides exceed their bounded capacity"));
    }
    for (const GuideRef& guide : guides) {
        if (guide.guide_id == 0U) {
            return core::Result<void>::failure(invalid(
                "groom guide identity must be non-zero"));
        }
        if (guide.attachment_index >= attachments.size()) {
            return core::Result<void>::failure(validation(
                "groom guide references an orphan attachment index"));
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::character
