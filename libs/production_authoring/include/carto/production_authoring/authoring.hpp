#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/biological/authoring.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production_authoring {

template <typename Tag>
struct Id {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value != 0U;
    }
    [[nodiscard]] constexpr auto operator<=>(const Id&) const noexcept = default;
};

struct SculptLayerTag;
struct SculptStrokeTag;
struct FabricIntentTag;
struct PanelTag;
struct SeamTag;
struct PinTag;
struct ClothConstraintTag;

using SculptLayerId = Id<SculptLayerTag>;
using SculptStrokeId = Id<SculptStrokeTag>;
using FabricIntentId = Id<FabricIntentTag>;
using PanelId = Id<PanelTag>;
using SeamId = Id<SeamTag>;
using PinId = Id<PinTag>;
using ClothConstraintId = Id<ClothConstraintTag>;

enum class SculptLayerKind : std::uint8_t {
    form,
    secondary_form,
    damage,
    weathering,
    microdetail,
};

enum class BrushKind : std::uint8_t {
    grab,
    clay,
    smooth,
    crease,
    flatten,
    polish,
};

enum class SymmetryAxis : std::uint8_t {
    none,
    x,
    y,
    z,
};

enum class ClothRepresentationKind : std::uint8_t {
    baked,
    skinned,
    live,
};

enum class ClothConstraintKind : std::uint8_t {
    stretch,
    shear,
    bend,
    pin,
    body_collision,
    self_collision,
};

enum class DrapeEvidenceSource : std::uint8_t {
    cpu_reference,
    native_runtime,
};

[[nodiscard]] std::string_view sculpt_layer_kind_name(SculptLayerKind value) noexcept;
[[nodiscard]] core::Result<SculptLayerKind> sculpt_layer_kind_from_name(
    std::string_view value);
[[nodiscard]] std::string_view brush_kind_name(BrushKind value) noexcept;
[[nodiscard]] core::Result<BrushKind> brush_kind_from_name(std::string_view value);
[[nodiscard]] std::string_view symmetry_axis_name(SymmetryAxis value) noexcept;
[[nodiscard]] core::Result<SymmetryAxis> symmetry_axis_from_name(std::string_view value);
[[nodiscard]] std::string_view cloth_representation_kind_name(
    ClothRepresentationKind value) noexcept;
[[nodiscard]] core::Result<ClothRepresentationKind>
cloth_representation_kind_from_name(std::string_view value);
[[nodiscard]] std::string_view cloth_constraint_kind_name(
    ClothConstraintKind value) noexcept;
[[nodiscard]] core::Result<ClothConstraintKind> cloth_constraint_kind_from_name(
    std::string_view value);
[[nodiscard]] std::string_view drape_evidence_source_name(
    DrapeEvidenceSource value) noexcept;
[[nodiscard]] core::Result<DrapeEvidenceSource> drape_evidence_source_from_name(
    std::string_view value);

struct SculptSample {
    static constexpr std::size_t kMaxSemanticBytes = 128U;

    core::Vec3d position;
    double radius = 0.0;
    double strength = 0.0;
    std::uint64_t surface_element = 0U;
    std::string surface_domain;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SculptStroke {
    static constexpr std::size_t kMaxSamples = 65'536U;

    SculptStrokeId id;
    BrushKind brush = BrushKind::smooth;
    SymmetryAxis symmetry = SymmetryAxis::none;
    double falloff = 0.5;
    std::optional<assets::Sha256Digest> mask_digest;
    std::vector<SculptSample> samples;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SculptLayer {
    static constexpr std::size_t kMaxStrokes = 16'384U;

    SculptLayerId id;
    SculptLayerKind kind = SculptLayerKind::form;
    std::optional<SculptLayerId> parent;
    std::uint32_t order = 0U;
    assets::Sha256Digest settings_digest;
    std::optional<assets::Sha256Digest> displacement_digest;
    std::vector<SculptStroke> strokes;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SculptPublication {
    biological::AuthoringSourceIdentity source;
    std::uint64_t mesh_asset = 0U;
    assets::Sha256Digest stack_digest;
    assets::Sha256Digest product_digest;
    assets::Sha256Digest topology_lineage_digest;
    std::string target_profile;
    assets::Sha256Digest receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_receipt() const;
    [[nodiscard]] core::Result<void> refresh_receipt_digest();
};

class SculptStack final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U * 1024U;
    static constexpr std::size_t kMaxLayers = 256U;
    static constexpr std::size_t kMaxStrokes = 65'536U;

    biological::AuthoringSourceIdentity source;
    std::uint64_t mesh_asset = 0U;
    assets::Sha256Digest base_mesh_digest;
    SymmetryAxis default_symmetry = SymmetryAxis::none;

    [[nodiscard]] core::Result<void> insert_layer(SculptLayer value);
    [[nodiscard]] const std::map<SculptLayerId, SculptLayer>& layers() const noexcept {
        return layers_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<SculptStack> deserialize(std::string_view text);

private:
    std::map<SculptLayerId, SculptLayer> layers_;
};

struct FabricIntent {
    static constexpr std::size_t kMaxFamilyBytes = 128U;

    FabricIntentId id;
    std::string family;
    assets::Sha256Digest weave_field_digest;
    double areal_density = 0.0;
    double thickness = 0.0;
    double stretch_stiffness = 0.0;
    double shear_stiffness = 0.0;
    double bend_stiffness = 0.0;
    double friction = 0.0;
    double damping = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct GarmentPanel {
    PanelId id;
    std::string semantic_type;
    assets::Sha256Digest outline_digest;
    assets::Sha256Digest rest_shape_digest;
    FabricIntentId fabric;
    std::uint32_t boundary_edges = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct GarmentSeam {
    SeamId id;
    PanelId first_panel;
    PanelId second_panel;
    std::uint32_t first_edge = 0U;
    std::uint32_t second_edge = 0U;
    double rest_length = 0.0;
    assets::Sha256Digest construction_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct GarmentPin {
    PinId id;
    PanelId panel;
    biological::AuthoringSourceIdentity body_source;
    biological::StructureId body_structure;
    std::uint32_t panel_vertex = 0U;
    double weight = 0.0;
    assets::Sha256Digest attachment_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ClothConstraint {
    ClothConstraintId id;
    ClothConstraintKind kind = ClothConstraintKind::stretch;
    double compliance = 0.0;
    double limit = 0.0;
    double thickness = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ClothRepresentation {
    ClothRepresentationKind kind = ClothRepresentationKind::baked;
    assets::Sha256Digest product_digest;
    assets::Sha256Digest rest_pose_digest;
    assets::Sha256Digest lineage_digest;
    std::uint64_t resident_bytes = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct DrapeRequest {
    biological::AuthoringSourceIdentity source;
    assets::Sha256Digest garment_digest;
    std::uint32_t iterations = 0U;
    double time_step = 0.0;
    double convergence_tolerance = 0.0;
    std::uint64_t stable_seed = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct DrapeReceipt {
    biological::AuthoringSourceIdentity source;
    assets::Sha256Digest request_digest;
    assets::Sha256Digest output_digest;
    DrapeEvidenceSource evidence_source = DrapeEvidenceSource::cpu_reference;
    std::uint32_t iterations = 0U;
    double maximum_strain = 0.0;
    double maximum_shear = 0.0;
    double maximum_bend = 0.0;
    bool converged = false;

    [[nodiscard]] core::Result<void> validate() const;
};

class GarmentAsset final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U * 1024U;
    static constexpr std::size_t kMaxFabrics = 4'096U;
    static constexpr std::size_t kMaxPanels = 4'096U;
    static constexpr std::size_t kMaxSeams = 16'384U;
    static constexpr std::size_t kMaxPins = 16'384U;
    static constexpr std::size_t kMaxConstraints = 64U;
    static constexpr std::size_t kMaxRepresentations = 3U;

    biological::AuthoringSourceIdentity source;
    std::string semantic_name;

    [[nodiscard]] core::Result<void> insert_fabric(FabricIntent value);
    [[nodiscard]] core::Result<void> insert_panel(GarmentPanel value);
    [[nodiscard]] core::Result<void> insert_seam(GarmentSeam value);
    [[nodiscard]] core::Result<void> insert_pin(GarmentPin value);
    [[nodiscard]] core::Result<void> insert_constraint(ClothConstraint value);
    [[nodiscard]] core::Result<void> add_representation(ClothRepresentation value);

    [[nodiscard]] const std::map<FabricIntentId, FabricIntent>& fabrics() const noexcept {
        return fabrics_;
    }
    [[nodiscard]] const std::map<PanelId, GarmentPanel>& panels() const noexcept {
        return panels_;
    }
    [[nodiscard]] const std::map<SeamId, GarmentSeam>& seams() const noexcept {
        return seams_;
    }
    [[nodiscard]] const std::map<PinId, GarmentPin>& pins() const noexcept {
        return pins_;
    }
    [[nodiscard]] const std::map<ClothConstraintId, ClothConstraint>& constraints() const noexcept {
        return constraints_;
    }
    [[nodiscard]] const std::vector<ClothRepresentation>& representations() const noexcept {
        return representations_;
    }

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<GarmentAsset> deserialize(std::string_view text);

private:
    std::map<FabricIntentId, FabricIntent> fabrics_;
    std::map<PanelId, GarmentPanel> panels_;
    std::map<SeamId, GarmentSeam> seams_;
    std::map<PinId, GarmentPin> pins_;
    std::map<ClothConstraintId, ClothConstraint> constraints_;
    std::vector<ClothRepresentation> representations_;
};

[[nodiscard]] core::Result<void> validate_sculpt_publication(
    const SculptStack& stack,
    const SculptPublication& publication);
[[nodiscard]] core::Result<void> validate_drape_receipt(
    const GarmentAsset& garment,
    const DrapeRequest& request,
    const DrapeReceipt& receipt);

} // namespace carto::production_authoring
