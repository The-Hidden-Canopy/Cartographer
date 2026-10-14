#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::spatial_field {

enum class FieldSemantic : std::uint8_t {
    generic,
    moisture,
    sunlight,
    shade,
    disturbance,
    wind_u,
    wind_v,
    drainage_flow,
    acoustic_dampening,
    fog_tendency,
    vegetation_suitability,
    terrain_elevation,
    ridge,
    basin,
    drainage,
    low_ground,
    elevation_band,
};

enum class BrushMode : std::uint8_t { replace, add, erase, smooth };

struct FieldBrush {
    core::Revision expected_revision;
    core::Vec2d center_uv;
    double radius = 1.0;
    double value = 0.0;
    double strength = 1.0;
    BrushMode mode = BrushMode::replace;

    [[nodiscard]] core::Result<void> validate() const;
};

struct DirtyRegion {
    std::uint32_t min_u = 0U;
    std::uint32_t min_v = 0U;
    std::uint32_t max_u = 0U;
    std::uint32_t max_v = 0U;
    // Exact provenance of the source immediately before the edit. For a no-op
    // brush this equals the resulting revision/digest.
    core::Revision previous_revision;
    assets::Sha256Digest previous_digest;
    core::Revision resulting_revision;
    bool changed = false;
    assets::Sha256Digest source_digest;
};

// Authoritative, project-local scalar source. The U/V plane is caller-defined
// (for example terrain X/Z); consumers must not infer axes from a renderer.
// Samples are stored row-major and are never a simulation/runtime state.
class ScalarField2D final {
public:
    static constexpr std::uint32_t kSchemaVersion = 2U;
    static constexpr std::uint32_t kMaxAxisSamples = 256U;
    static constexpr std::size_t kMaxSamples = 65'536U;
    static constexpr std::size_t kMaxSerializedBytes = 2U * 1024U * 1024U;

    [[nodiscard]] static core::Result<ScalarField2D> create(
        std::string identity,
        FieldSemantic semantic,
        core::Vec2d origin_uv,
        double cell_size,
        std::uint32_t width,
        std::uint32_t height,
        double minimum_value,
        double maximum_value,
        double initial_value = 0.0);

    [[nodiscard]] const std::string& identity() const noexcept { return identity_; }
    [[nodiscard]] FieldSemantic semantic() const noexcept { return semantic_; }
    [[nodiscard]] core::Vec2d origin_uv() const noexcept { return origin_uv_; }
    [[nodiscard]] double cell_size() const noexcept { return cell_size_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] double minimum_value() const noexcept { return minimum_value_; }
    [[nodiscard]] double maximum_value() const noexcept { return maximum_value_; }
    [[nodiscard]] core::Revision source_revision() const noexcept { return source_revision_; }
    [[nodiscard]] const std::vector<double>& values() const noexcept { return values_; }
    [[nodiscard]] bool same_content(const ScalarField2D& other) const noexcept;

    // Re-expresses these exact source values as the next revision after an
    // existing field. Used by governed undo/redo so source revisions never
    // move backwards when authored values are restored.
    [[nodiscard]] core::Result<ScalarField2D> at_next_revision(
        core::Revision current_revision) const;

    [[nodiscard]] core::Result<double> sample_bilinear(core::Vec2d uv) const;
    [[nodiscard]] core::Result<DirtyRegion> paint(const FieldBrush& brush);
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<ScalarField2D> deserialize(std::string_view text);

private:
    [[nodiscard]] core::Result<void> validate_header() const;

    std::string identity_;
    FieldSemantic semantic_ = FieldSemantic::generic;
    core::Vec2d origin_uv_{};
    double cell_size_ = 1.0;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    double minimum_value_ = 0.0;
    double maximum_value_ = 1.0;
    core::Revision source_revision_{1U};
    std::vector<double> values_;
};

struct VectorFieldSourceReceipt {
    std::string identity;
    FieldSemantic semantic = FieldSemantic::generic;
    core::Revision source_revision;
    assets::Sha256Digest digest;
    core::Vec2d origin_uv{};
    double cell_size = 1.0;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

struct VectorFieldSample {
    core::Vec2d position_uv{};
    core::Vec2d vector_uv{};
};

// Ephemeral, source-bound visualization product. It deliberately has no
// project persistence or mutation API.
struct VectorFieldPreview {
    static constexpr std::uint32_t kMaxPreviewAxis = 16U;
    static constexpr std::size_t kMaxSamples =
        static_cast<std::size_t>(kMaxPreviewAxis) * kMaxPreviewAxis;

    VectorFieldSourceReceipt u_component;
    VectorFieldSourceReceipt v_component;
    std::uint32_t preview_width = 0U;
    std::uint32_t preview_height = 0U;
    std::vector<VectorFieldSample> samples;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool sources_match(
        std::string_view u_identity,
        core::Revision u_revision,
        const assets::Sha256Digest& u_digest,
        std::string_view v_identity,
        core::Revision v_revision,
        const assets::Sha256Digest& v_digest) const noexcept;
};

[[nodiscard]] core::Result<VectorFieldPreview> build_vector_field_preview(
    const ScalarField2D& u_component,
    const ScalarField2D& v_component,
    std::uint32_t max_preview_axis = VectorFieldPreview::kMaxPreviewAxis);

[[nodiscard]] std::string_view field_semantic_name(FieldSemantic semantic) noexcept;

} // namespace carto::spatial_field
