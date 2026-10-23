#include <carto/editor/preview_transaction.hpp>

#include <algorithm>
#include <cmath>
#include <set>
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

core::Result<MaterialGraphAuthoringReview> MaterialGraphAuthoringReview::begin(
    core::Revision base_project_revision,
    std::string identity,
    std::optional<project::MaterialGraph> current_graph) {
    const bool identity_valid = !identity.empty() &&
        identity.size() <= project::MaterialGraph::kMaxIdentityBytes &&
        std::all_of(identity.begin(), identity.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        });
    if (base_project_revision.exhausted() || !identity_valid) {
        return core::Result<MaterialGraphAuthoringReview>::failure(invalid(
            "material graph review requires a bounded identity and project revision"));
    }
    MaterialGraphAuthoringReview review;
    review.base_project_revision_ = base_project_revision;
    review.identity_ = std::move(identity);
    if (current_graph.has_value()) {
        if (auto result = current_graph->validate(); !result) {
            return core::Result<MaterialGraphAuthoringReview>::failure(result.error());
        }
        if (current_graph->identity != review.identity_) {
            return core::Result<MaterialGraphAuthoringReview>::failure(invalid(
                "material graph review source identity does not match its target"));
        }
        auto digest = current_graph->canonical_digest();
        if (!digest) {
            return core::Result<MaterialGraphAuthoringReview>::failure(digest.error());
        }
        review.expected_graph_revision_ = current_graph->revision;
        review.expected_graph_digest_ = digest.value();
        review.original_graph_ = std::move(current_graph);
    }
    return core::Result<MaterialGraphAuthoringReview>::success(std::move(review));
}

core::Result<void> MaterialGraphAuthoringReview::update(
    project::MaterialGraph candidate) {
    if (!active_) return core::Result<void>::failure(invalid_state(
        "material graph review is no longer active"));
    if (auto result = candidate.validate(); !result) return result;
    if (candidate.identity != identity_) {
        return core::Result<void>::failure(invalid(
            "material graph proposal cannot change its reviewed identity"));
    }
    if (original_graph_.has_value()) {
        const auto& original = *original_graph_;
        if (original.revision.exhausted() || candidate.revision != original.revision.next()) {
            return core::Result<void>::failure(invalid(
                "material graph proposal must advance exactly one source revision"));
        }
        std::set<std::uint64_t> expected_retired(original.retired_node_ids.begin(),
            original.retired_node_ids.end());
        for (const auto& node : original.nodes) {
            const bool retained = std::any_of(candidate.nodes.begin(), candidate.nodes.end(),
                [&](const auto& candidate_node) { return candidate_node.id == node.id; });
            if (!retained) expected_retired.insert(node.id);
        }
        if (candidate.retired_node_ids != std::vector<std::uint64_t>(
                expected_retired.begin(), expected_retired.end())) {
            return core::Result<void>::failure(invalid(
                "material graph proposal must preserve node tombstones and record removed nodes"));
        }
    } else if (candidate.revision != core::Revision(1U) ||
        !candidate.retired_node_ids.empty()) {
        return core::Result<void>::failure(invalid(
            "new material graph proposal must start at revision one without invented tombstones"));
    }
    auto digest = candidate.canonical_digest();
    if (!digest) return core::Result<void>::failure(digest.error());
    candidate_ = std::move(candidate);
    candidate_digest_ = digest.value();
    return core::Result<void>::success();
}

core::Result<void> MaterialGraphAuthoringReview::validate_commit(
    core::Revision current_project_revision,
    const std::optional<project::MaterialGraph>& current_graph) const {
    if (!active_ || current_project_revision != base_project_revision_ ||
        !candidate_.has_value() || !candidate_digest_.has_value()) {
        return core::Result<void>::failure(stale(
            "material graph review has no candidate or its project revision changed"));
    }
    if (original_graph_.has_value()) {
        if (!current_graph.has_value() || current_graph->identity != identity_ ||
            !expected_graph_revision_.has_value() ||
            current_graph->revision != *expected_graph_revision_) {
            return core::Result<void>::failure(stale(
                "material graph source changed after review began"));
        }
        auto current_digest = current_graph->canonical_digest();
        if (!current_digest || !expected_graph_digest_.has_value() ||
            current_digest.value() != *expected_graph_digest_) {
            return core::Result<void>::failure(stale(
                "material graph content changed after review began"));
        }
    } else if (current_graph.has_value()) {
        return core::Result<void>::failure(stale(
            "material graph identity appeared after the create review began"));
    }
    auto candidate_digest = candidate_->canonical_digest();
    if (!candidate_digest || candidate_digest.value() != *candidate_digest_) {
        return core::Result<void>::failure(stale(
            "material graph proposal changed after review"));
    }
    return core::Result<void>::success();
}

void MaterialGraphAuthoringReview::cancel() noexcept {
    active_ = false;
    candidate_.reset();
    candidate_digest_.reset();
}

} // namespace carto::editor
