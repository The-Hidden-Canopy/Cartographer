#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/editor/authoring_context.hpp>
#include <carto/project/material_graph.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace carto::application {
class ApplicationSession;
}

namespace carto::editor {

enum class PreviewKind {
    object_transform,
    object_translation_delta,
    extrude_face,
    inset_face,
    set_vertex_position,
    poke_face,
    set_material_region_assignment,
    generate_unique_lightmap_uv,
};

struct PreviewParameters {
    PreviewParameters() = default;
    PreviewParameters(
        std::optional<core::Transform> transform_value,
        std::optional<double> distance_value,
        std::optional<core::Vec3d> position_value,
        std::optional<std::uint32_t> region_id_value = std::nullopt,
        std::optional<std::uint64_t> material_id_value = std::nullopt,
        std::optional<std::uint64_t> material_variant_id_value = std::nullopt,
        std::optional<std::uint64_t> lightmap_uv_set_id_value = std::nullopt,
        std::optional<std::uint32_t> lightmap_resolution_value = std::nullopt,
        std::optional<std::uint32_t> lightmap_padding_texels_value = std::nullopt,
        std::optional<std::uint32_t> lightmap_maximum_udim_tiles_value = std::nullopt)
        : transform(std::move(transform_value)),
          distance(distance_value),
          position(std::move(position_value)),
          material_region_id(region_id_value),
          material_id(material_id_value),
          material_variant_id(material_variant_id_value),
          lightmap_uv_set_id(lightmap_uv_set_id_value),
          lightmap_resolution(lightmap_resolution_value),
          lightmap_padding_texels(lightmap_padding_texels_value),
          lightmap_maximum_udim_tiles(lightmap_maximum_udim_tiles_value) {}

    std::optional<core::Transform> transform;
    std::optional<double> distance;
    std::optional<core::Vec3d> position;
    std::optional<std::uint32_t> material_region_id;
    std::optional<std::uint64_t> material_id;
    std::optional<std::uint64_t> material_variant_id;
    std::optional<std::uint64_t> lightmap_uv_set_id;
    std::optional<std::uint32_t> lightmap_resolution;
    std::optional<std::uint32_t> lightmap_padding_texels;
    std::optional<std::uint32_t> lightmap_maximum_udim_tiles;

    [[nodiscard]] core::Result<void> validate(PreviewKind kind) const;
};

struct PreviewSnapshot {
    core::Revision base_revision;
    AuthoringContext context;
    PreviewKind kind = PreviewKind::object_transform;
    PreviewParameters parameters;
    bool active = true;
};

// A preview is an ephemeral, revision-bound authoring proposal. Updating it
// never mutates ProjectDocument or advances a project revision. Only the
// application/session admission path may commit it.
class AuthoringPreview final {
public:
    [[nodiscard]] static core::Result<AuthoringPreview> begin(
        core::Revision base_revision,
        AuthoringContext context,
        PreviewKind kind);

    [[nodiscard]] core::Result<PreviewSnapshot> update(
        const PreviewParameters& parameters);
    [[nodiscard]] core::Result<void> validate_commit(
        core::Revision current_revision) const;
    void cancel() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] bool committed() const noexcept { return committed_; }
    [[nodiscard]] core::Revision base_revision() const noexcept { return base_revision_; }
    [[nodiscard]] PreviewKind kind() const noexcept { return kind_; }
    [[nodiscard]] const AuthoringContext& context() const noexcept { return context_; }
    [[nodiscard]] const PreviewParameters& parameters() const noexcept { return parameters_; }

private:
    friend class ::carto::application::ApplicationSession;

    void mark_committed() noexcept {
        active_ = false;
        committed_ = true;
    }

    core::Revision base_revision_;
    AuthoringContext context_;
    PreviewKind kind_ = PreviewKind::object_transform;
    PreviewParameters parameters_;
    bool active_ = false;
    bool committed_ = false;
};

using PreviewTransaction = AuthoringPreview;

// Read-only, project-revision-bound proposal for creating or replacing one
// material graph. It never mutates ProjectDocument; the UI commits its
// candidate only through the human application action boundary.
class MaterialGraphAuthoringReview final {
public:
    [[nodiscard]] static core::Result<MaterialGraphAuthoringReview> begin(
        core::Revision base_project_revision,
        std::string identity,
        std::optional<project::MaterialGraph> current_graph);

    [[nodiscard]] core::Result<void> update(project::MaterialGraph candidate);
    [[nodiscard]] core::Result<void> validate_commit(
        core::Revision current_project_revision,
        const std::optional<project::MaterialGraph>& current_graph) const;
    void cancel() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] core::Revision base_project_revision() const noexcept {
        return base_project_revision_;
    }
    [[nodiscard]] const std::string& identity() const noexcept { return identity_; }
    [[nodiscard]] const std::optional<core::Revision>& expected_graph_revision() const noexcept {
        return expected_graph_revision_;
    }
    [[nodiscard]] const std::optional<assets::Sha256Digest>& expected_graph_digest() const noexcept {
        return expected_graph_digest_;
    }
    [[nodiscard]] const std::optional<project::MaterialGraph>& candidate() const noexcept {
        return candidate_;
    }

private:
    core::Revision base_project_revision_;
    std::string identity_;
    std::optional<project::MaterialGraph> original_graph_;
    std::optional<core::Revision> expected_graph_revision_;
    std::optional<assets::Sha256Digest> expected_graph_digest_;
    std::optional<project::MaterialGraph> candidate_;
    std::optional<assets::Sha256Digest> candidate_digest_;
    bool active_ = true;
};

} // namespace carto::editor
