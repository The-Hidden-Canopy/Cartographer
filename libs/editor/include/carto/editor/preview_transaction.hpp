#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/editor/authoring_context.hpp>

#include <optional>

namespace carto::application {
class ApplicationSession;
}

namespace carto::editor {

enum class PreviewKind {
    object_transform,
    extrude_face,
    inset_face,
    set_vertex_position,
};

struct PreviewParameters {
    std::optional<core::Transform> transform;
    std::optional<double> distance;
    std::optional<core::Vec3d> position;

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

} // namespace carto::editor
