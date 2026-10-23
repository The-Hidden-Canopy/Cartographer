#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstddef>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::biological {

// Opaque authored continuity label; preservation and lowering policy remain private.
struct RepresentationContinuityId {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const RepresentationContinuityId&) const noexcept = default;
};

template <typename Tag>
struct Id {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value != 0U;
    }
    [[nodiscard]] constexpr auto operator<=>(const Id&) const noexcept = default;
};

struct StructureTag;
struct FieldTag;
struct AttachmentTag;
struct LayerTag;
struct StageTag;
struct RepresentationTag;

using StructureId = Id<StructureTag>;
using FieldId = Id<FieldTag>;
using AttachmentId = Id<AttachmentTag>;
using LayerId = Id<LayerTag>;
using StageId = Id<StageTag>;
using RepresentationId = Id<RepresentationTag>;

// Explicit, low-detail source parameters for a generated ellipsoid proxy.
// This is editable geometry intent, not a claim of anatomical fidelity.
struct EllipsoidProxyProfile {
    static constexpr std::uint32_t kMinLongitudeSegments = 3U;
    static constexpr std::uint32_t kMaxLongitudeSegments = 128U;
    static constexpr std::uint32_t kMinLatitudeSegments = 2U;
    static constexpr std::uint32_t kMaxLatitudeSegments = 128U;
    static constexpr std::uint64_t kMaxTriangles = 16'384U;

    core::Vec3d center{};
    core::Vec3d radii{1.0, 1.0, 1.0};
    std::uint32_t longitude_segments = 16U;
    std::uint32_t latitude_segments = 8U;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class FieldKind : std::uint8_t {
    orientation,
    density,
    radius,
    length,
    thickness,
    coverage,
    variation,
    pigmentation,
    moisture,
    compaction,
    deformation,
    compliance,
    activation,
    damage,
    growth,
    mass,
    inertia,
    contact,
    custom,
};

enum class FieldValueKind : std::uint8_t {
    scalar,
    vector,
    direction,
    categorical,
};

enum class FieldRepresentation : std::uint8_t {
    geometry,
    displacement,
    normal,
    statistical_shading,
    constraint,
};

enum class StructureKind : std::uint8_t {
    organism,
    bone,
    cartilage,
    joint,
    tendon,
    ligament,
    fascia,
    muscle,
    soft_tissue,
    adipose,
    skin,
    organ,
    vascular,
    neural,
    fluid,
    appendage,
    garment,
    surface,
    landmark,
};

enum class RelationKind : std::uint8_t {
    attached_to,
    origin_insertion,
    contains,
    wraps,
    contacts,
    drives,
    overlays,
    continues,
};

enum class LayerKind : std::uint8_t {
    microstructure,
    strand,
    weave,
    feather,
    granular,
    anatomy,
    connective_tissue,
    muscle,
    soft_tissue,
    adipose,
    skin,
    face,
    organ,
    vascular,
    neural,
    fluid,
    injury_overlay,
    growth,
    species_morphology,
    biomechanics,
};

// Explicit production family identity for biological layers. Generic layers
// remain readable with no family; new family records also bind uncertainty
// evidence and the source fields required by their family contract.
enum class LayerFamily : std::uint8_t {
    hair,
    fur,
    feather,
    weave,
    granular,
    connective_tissue,
    muscle,
    skin,
    face,
    injury,
    species_morphology,
    biomechanics,
    anatomy,
    soft_tissue,
    adipose,
    organ,
    vascular,
    neural,
    fluid,
    growth,
};

enum class RepresentationKind : std::uint8_t {
    hero,
    close,
    gameplay,
    crowd,
    distant,
};

[[nodiscard]] std::string_view field_kind_name(FieldKind value) noexcept;
[[nodiscard]] core::Result<FieldKind> field_kind_from_name(std::string_view value);
[[nodiscard]] std::string_view field_value_kind_name(FieldValueKind value) noexcept;
[[nodiscard]] core::Result<FieldValueKind> field_value_kind_from_name(
    std::string_view value);
[[nodiscard]] std::string_view field_representation_name(FieldRepresentation value) noexcept;
[[nodiscard]] core::Result<FieldRepresentation> field_representation_from_name(
    std::string_view value);
[[nodiscard]] std::string_view structure_kind_name(StructureKind value) noexcept;
[[nodiscard]] core::Result<StructureKind> structure_kind_from_name(std::string_view value);
[[nodiscard]] std::string_view relation_kind_name(RelationKind value) noexcept;
[[nodiscard]] core::Result<RelationKind> relation_kind_from_name(std::string_view value);
[[nodiscard]] std::string_view layer_kind_name(LayerKind value) noexcept;
[[nodiscard]] core::Result<LayerKind> layer_kind_from_name(std::string_view value);
[[nodiscard]] std::string_view layer_family_name(LayerFamily value) noexcept;
[[nodiscard]] core::Result<LayerFamily> layer_family_from_name(std::string_view value);
[[nodiscard]] std::string_view representation_kind_name(RepresentationKind value) noexcept;
[[nodiscard]] core::Result<RepresentationKind> representation_kind_from_name(
    std::string_view value);

// The source identity is the only identity accepted by this public layer.
// Runtime handles, provider identifiers and file paths are deliberately not
// representable. A changed digest at the same revision is stale, not a
// replacement for the previous source.
struct AuthoringSourceIdentity {
    std::string source_namespace;
    std::uint64_t asset_id = 0U;
    core::Revision revision;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const AuthoringSourceIdentity&) const noexcept = default;
};

// One reusable authored field. The sample payload is external and content
// addressed; this record contains its semantic contract, not a runtime buffer
// or a provider result. The same primitive covers orientation, strands, weave,
// feathers, grains, tissue, skin, injury overlays, morphology and biomechanics.
struct FieldRecord {
    static constexpr std::size_t kMaxSemanticBytes = 128U;

    FieldId id;
    FieldKind kind = FieldKind::custom;
    FieldValueKind value_kind = FieldValueKind::scalar;
    FieldRepresentation representation = FieldRepresentation::statistical_shading;
    std::string domain;
    std::string coordinate_frame;
    std::string units;
    std::uint64_t sample_count = 0U;
    std::uint64_t stable_seed = 0U;
    std::optional<FieldId> coverage_field;
    assets::Sha256Digest data_digest;
    double minimum = 0.0;
    double maximum = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

// A durable anatomical or production structure. A kinematic joint is an
// optional correspondence to an existing public KinematicGraph; it is never
// the source of anatomy and carries an exact graph digest when present.
struct StructureRecord {
    static constexpr std::size_t kMaxSemanticBytes = 128U;

    StructureId id;
    StructureKind kind = StructureKind::surface;
    std::optional<StructureId> parent;
    std::string semantic_type;
    std::string coordinate_frame;
    std::optional<kinematic::JointId> kinematic_joint;
    std::optional<assets::Sha256Digest> kinematic_graph_digest;
    std::optional<assets::Sha256Digest> geometry_digest;
    std::optional<EllipsoidProxyProfile> proxy_surface;

    [[nodiscard]] core::Result<void> validate() const;
};

// A relation between two authored structures. Anchors and evidence remain
// semantic and digest-bound; current pose, force, damage, health and other
// game-owned state are absent by construction.
struct AttachmentRecord {
    static constexpr std::size_t kMaxAnchorBytes = 128U;

    AttachmentId id;
    StructureId first_structure;
    StructureId second_structure;
    RelationKind relation = RelationKind::attached_to;
    std::string first_anchor;
    std::string second_anchor;
    std::optional<FieldId> driver_field;
    assets::Sha256Digest evidence_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

// A semantic layer groups fields without flattening them into one material or
// one generic mesh. It is source authoring data, not a runtime residency or a
// simulation state machine.
struct LayerRecord {
    static constexpr std::size_t kMaxFields = 4096U;
    static constexpr std::size_t kMaxFamilyFields = 64U;
    static constexpr std::size_t kMaxSemanticBytes = 128U;
    static constexpr std::size_t kMaxUncertaintyBytes = 512U;

    LayerId id;
    LayerKind kind = LayerKind::microstructure;
    StructureId owner;
    std::optional<LayerId> parent;
    std::uint32_t order = 0U;
    std::string semantic_type;
    std::vector<FieldId> fields;
    std::optional<LayerFamily> family;
    // A separate content digest and explicit uncertainty note are mandatory
    // for typed family layers. They do not assert that an estimate is true.
    std::optional<assets::Sha256Digest> uncertainty_digest;
    std::string uncertainty_statement;

    [[nodiscard]] core::Result<void> validate() const;
};

// A named authored morphology stage. This is a source timeline (for example,
// juvenile/adult/aged), not current organism history and not a gameplay injury
// timeline.
struct StageRecord {
    static constexpr std::size_t kMaxOverrides = 4096U;

    StageId id;
    std::string name;
    double normalized_position = 0.0;
    std::vector<FieldId> field_overrides;

    [[nodiscard]] core::Result<void> validate() const;
};

// A biological representation variant is an already-produced, digest-bound
// product reference. Cartographer records the correspondence and evidence;
// downstream consumer decides how to execute a private product later.
struct RepresentationVariant {
    static constexpr std::size_t kMaxLayers = 4096U;

    RepresentationId id;
    RepresentationKind kind = RepresentationKind::gameplay;
    assets::Sha256Digest artifact_digest;
    assets::Sha256Digest error_envelope_digest;
    assets::Sha256Digest correspondence_digest;
    std::uint64_t resident_bytes = 0U;
    RepresentationContinuityId continuity;
    std::vector<LayerId> layers;

    [[nodiscard]] core::Result<void> validate() const;
};

class RepresentationFamily final {
public:
    static constexpr std::size_t kMaxVariants = 16U;

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    void set_id(std::uint64_t value) noexcept { id_ = value; }
    [[nodiscard]] core::Result<void> upsert_variant(RepresentationVariant value);
    [[nodiscard]] const RepresentationVariant* find(RepresentationId id) const noexcept;
    [[nodiscard]] const std::vector<RepresentationVariant>& variants() const noexcept {
        return variants_;
    }
    [[nodiscard]] core::Result<void> validate() const;

private:
    std::uint64_t id_ = 0U;
    std::vector<RepresentationVariant> variants_;

    friend class BiologicalAuthoringDocument;
};

// A prepared/publication receipt is the public handoff boundary. It binds an
// externally produced product to the exact authoring digest and target profile
// without publishing a worker, runtime handle, network endpoint or telemetry.
struct PreparedPublication {
    static constexpr std::size_t kMaxRepresentations = 16U;

    AuthoringSourceIdentity source;
    assets::Sha256Digest authoring_digest;
    assets::Sha256Digest prepared_digest;
    std::string target_profile;
    std::string compiler;
    std::vector<RepresentationId> representations;
    assets::Sha256Digest receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_receipt() const;
    [[nodiscard]] core::Result<void> refresh_receipt_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<PreparedPublication> deserialize(
        std::string_view text);
};

class BiologicalAuthoringDocument final {
public:
    static constexpr std::uint32_t kSchemaVersion = 3U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U * 1024U;
    static constexpr std::size_t kMaxStructures = 65'536U;
    static constexpr std::size_t kMaxFields = 131'072U;
    static constexpr std::size_t kMaxAttachments = 131'072U;
    static constexpr std::size_t kMaxLayers = 65'536U;
    static constexpr std::size_t kMaxStages = 256U;
    static constexpr std::size_t kMaxOrganismFamilyBytes = 128U;

    AuthoringSourceIdentity source;
    std::string organism_family;

    [[nodiscard]] core::Result<void> insert_structure(StructureRecord value);
    [[nodiscard]] core::Result<void> insert_field(FieldRecord value);
    [[nodiscard]] core::Result<void> insert_attachment(AttachmentRecord value);
    [[nodiscard]] core::Result<void> insert_layer(LayerRecord value);
    [[nodiscard]] core::Result<void> insert_stage(StageRecord value);
    [[nodiscard]] core::Result<void> set_representation_family(
        RepresentationFamily value);

    [[nodiscard]] const std::map<StructureId, StructureRecord>& structures() const noexcept {
        return structures_;
    }
    [[nodiscard]] const std::map<FieldId, FieldRecord>& fields() const noexcept {
        return fields_;
    }
    [[nodiscard]] const std::map<AttachmentId, AttachmentRecord>& attachments() const noexcept {
        return attachments_;
    }
    [[nodiscard]] const std::map<LayerId, LayerRecord>& layers() const noexcept {
        return layers_;
    }
    [[nodiscard]] const std::map<StageId, StageRecord>& stages() const noexcept {
        return stages_;
    }
    [[nodiscard]] const std::optional<RepresentationFamily>& representations() const noexcept {
        return representations_;
    }

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> validate_for_publication() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<BiologicalAuthoringDocument> deserialize(
        std::string_view text);

private:
    std::map<StructureId, StructureRecord> structures_;
    std::map<FieldId, FieldRecord> fields_;
    std::map<AttachmentId, AttachmentRecord> attachments_;
    std::map<LayerId, LayerRecord> layers_;
    std::map<StageId, StageRecord> stages_;
    std::optional<RepresentationFamily> representations_;
};

// The reimport gate preserves source identity and every previously published
// authored identity/continuity. It does not touch runtime state because none
// is present in either document.
[[nodiscard]] core::Result<void> verify_reimport_continuity(
    const BiologicalAuthoringDocument& before,
    const BiologicalAuthoringDocument& after);

// A prepared product is admissible only when its source and canonical
// authoring digest still match the current document and every requested
// representation remains present. Stale or forged handoffs fail closed.
[[nodiscard]] core::Result<void> validate_prepared_publication(
    const BiologicalAuthoringDocument& document,
    const PreparedPublication& publication);

} // namespace carto::biological
