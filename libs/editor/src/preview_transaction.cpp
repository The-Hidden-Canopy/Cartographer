#include <carto/editor/preview_transaction.hpp>

#include <cmath>
#include <string>
#include <utility>

namespace carto::editor {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic invalid_state(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool valid_kind(PreviewKind kind) noexcept {
    return kind == PreviewKind::object_transform ||
        kind == PreviewKind::object_translation_delta ||
        kind == PreviewKind::extrude_face ||
        kind == PreviewKind::inset_face || kind == PreviewKind::set_vertex_position ||
        kind == PreviewKind::poke_face ||
        kind == PreviewKind::set_material_region_assignment ||
        kind == PreviewKind::generate_unique_lightmap_uv;
}

} // namespace

core::Result<void> PreviewParameters::validate(PreviewKind kind) const {
    if (!valid_kind(kind)) {
        return core::Result<void>::failure(invalid("preview kind is invalid"));
    }
    const bool has_material_parameters = material_region_id.has_value() ||
        material_id.has_value() || material_variant_id.has_value();
    const bool has_lightmap_uv_parameters = lightmap_uv_set_id.has_value() ||
        lightmap_resolution.has_value() || lightmap_padding_texels.has_value() ||
        lightmap_maximum_udim_tiles.has_value();
    if (kind != PreviewKind::set_material_region_assignment && has_material_parameters) {
        return core::Result<void>::failure(invalid(
            "non-material preview contains material assignment parameters"));
    }
    if (kind != PreviewKind::generate_unique_lightmap_uv && has_lightmap_uv_parameters) {
        return core::Result<void>::failure(invalid(
            "non-lightmap preview contains lightmap UV parameters"));
    }
    if (kind == PreviewKind::object_transform) {
        if (!transform.has_value() || distance.has_value() || position.has_value()) {
            return core::Result<void>::failure(
                invalid("object transform preview requires only a transform"));
        }
        if (!transform->finite()) {
            return core::Result<void>::failure(
                invalid("object transform preview requires a finite transform"));
        }
        return core::Result<void>::success();
    }
    if (kind == PreviewKind::object_translation_delta) {
        if (!position.has_value() || transform.has_value() || distance.has_value()) {
            return core::Result<void>::failure(invalid(
                "object translation preview requires only a displacement"));
        }
        if (!position->finite()) {
            return core::Result<void>::failure(invalid(
                "object translation preview requires a finite displacement"));
        }
        return core::Result<void>::success();
    }
    if (kind == PreviewKind::extrude_face || kind == PreviewKind::inset_face) {
        if (!distance.has_value() || transform.has_value() || position.has_value()) {
            return core::Result<void>::failure(invalid(
                "face operation preview requires only a distance"));
        }
        if (!std::isfinite(*distance) || *distance <= 0.0) {
            return core::Result<void>::failure(
                invalid("face operation preview distance must be finite and strictly positive"));
        }
        return core::Result<void>::success();
    }
    if (kind == PreviewKind::poke_face) {
        if (transform.has_value() || distance.has_value() || position.has_value()) {
            return core::Result<void>::failure(
                invalid("face poke preview does not accept parameters"));
        }
        return core::Result<void>::success();
    }
    if (kind == PreviewKind::set_material_region_assignment) {
        if (transform.has_value() || distance.has_value() || position.has_value() ||
            !material_region_id.has_value() || material_region_id.value() == 0U ||
            !material_id.has_value() || material_id.value() == 0U ||
            (material_variant_id.has_value() && material_variant_id.value() == 0U)) {
            return core::Result<void>::failure(invalid(
                "material assignment preview requires non-zero region and material IDs, "
                "an optional non-zero variant ID, and no unrelated parameters"));
        }
        return core::Result<void>::success();
    }
    if (kind == PreviewKind::generate_unique_lightmap_uv) {
        if (transform.has_value() || distance.has_value() || position.has_value() ||
            !lightmap_uv_set_id.has_value() || lightmap_uv_set_id.value() == 0U ||
            !lightmap_resolution.has_value() || lightmap_resolution.value() < 16U ||
            lightmap_resolution.value() > 8192U ||
            !lightmap_padding_texels.has_value() ||
            lightmap_padding_texels.value() == 0U ||
            lightmap_padding_texels.value() > lightmap_resolution.value() / 8U ||
            !lightmap_maximum_udim_tiles.has_value() ||
            lightmap_maximum_udim_tiles.value() == 0U ||
            lightmap_maximum_udim_tiles.value() > 100U) {
            return core::Result<void>::failure(invalid(
                "unique lightmap UV preview requires a non-zero set ID, bounded resolution, "
                "positive padding, and a bounded UDIM tile count"));
        }
        return core::Result<void>::success();
    }
    if (!position.has_value() || transform.has_value() || distance.has_value()) {
        return core::Result<void>::failure(
            invalid("vertex position preview requires only a position"));
    }
    if (!position->finite()) {
        return core::Result<void>::failure(
            invalid("vertex position preview requires a finite position"));
    }
    return core::Result<void>::success();
}

core::Result<AuthoringPreview> AuthoringPreview::begin(
    core::Revision base_revision,
    AuthoringContext context,
    PreviewKind kind) {
    if (auto result = context.validate(); !result) {
        return core::Result<AuthoringPreview>::failure(result.error().with_context(
            "authoring preview context"));
    }
    if (!valid_kind(kind)) {
        return core::Result<AuthoringPreview>::failure(invalid("preview kind is invalid"));
    }
    if ((kind == PreviewKind::object_transform ||
         kind == PreviewKind::object_translation_delta ||
         kind == PreviewKind::set_material_region_assignment ||
         kind == PreviewKind::generate_unique_lightmap_uv) &&
        (context.selection_mode != SelectionMode::object || context.objects.size() != 1U)) {
        return core::Result<AuthoringPreview>::failure(invalid(
            "object operation preview requires exactly one selected object"));
    }
    if ((kind == PreviewKind::extrude_face || kind == PreviewKind::inset_face ||
         kind == PreviewKind::poke_face) &&
        (context.selection_mode != SelectionMode::face || context.faces.size() != 1U ||
         !context.component_object.has_value())) {
            return core::Result<AuthoringPreview>::failure(invalid(
            "face operation preview requires exactly one object-bound selected face"));
    }
    if (kind == PreviewKind::set_vertex_position &&
        (context.selection_mode != SelectionMode::vertex || context.vertices.size() != 1U ||
         !context.component_object.has_value())) {
        return core::Result<AuthoringPreview>::failure(invalid(
            "vertex position preview requires exactly one object-bound selected vertex"));
    }

    AuthoringPreview preview;
    preview.base_revision_ = base_revision;
    preview.context_ = std::move(context);
    preview.kind_ = kind;
    preview.active_ = true;
    return core::Result<AuthoringPreview>::success(std::move(preview));
}

core::Result<PreviewSnapshot> AuthoringPreview::update(
    const PreviewParameters& parameters) {
    if (!active_) {
        return core::Result<PreviewSnapshot>::failure(
            invalid_state("authoring preview is no longer active"));
    }
    if (auto result = parameters.validate(kind_); !result) {
        return core::Result<PreviewSnapshot>::failure(result.error().with_context(
            "authoring preview update"));
    }
    parameters_ = parameters;
    return core::Result<PreviewSnapshot>::success(
        PreviewSnapshot{base_revision_, context_, kind_, parameters_, true});
}

core::Result<void> AuthoringPreview::validate_commit(core::Revision current_revision) const {
    if (!active_) {
        return core::Result<void>::failure(
            invalid_state("authoring preview is no longer active"));
    }
    if (current_revision != base_revision_) {
        return core::Result<void>::failure(stale(
            "authoring preview was created against an older project revision"));
    }
    if (auto result = context_.validate(); !result) {
        return result;
    }
    return parameters_.validate(kind_);
}

void AuthoringPreview::cancel() noexcept {
    active_ = false;
    committed_ = false;
}

} // namespace carto::editor
