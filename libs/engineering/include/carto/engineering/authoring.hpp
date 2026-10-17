#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::engineering {

using Id = std::uint64_t;

enum class SourceKind : std::uint8_t { step_ap242, bom, drawing, specification };
enum class TopologyKind : std::uint8_t {
    assembly, part, occurrence, body, face, edge, vertex, feature, datum, topology_set
};
enum class RequirementKind : std::uint8_t {
    dimensional_distance, diameter, radius, angle, thickness, clearance, position,
    flatness, parallelism, perpendicularity, concentricity, surface_condition,
    presence_absence, visual_surface, assembly_relationship, material_verification
};
enum class ExtractionState : std::uint8_t {
    manual, imported_structured, machine_extracted_proposed, human_verified,
    human_corrected, rejected, stale
};
enum class ReconciliationStatus : std::uint8_t {
    exact, matched_by_external_id, matched_by_part_number, quantity_mismatch,
    revision_mismatch, material_mismatch, source_only, assembly_only, ambiguous, unresolved
};
enum class InspectionKind : std::uint8_t {
    point_measurement, distance, angle, diameter, thickness, clearance, surface_scan,
    visual_surface_check, feature_presence, datum_capture, dimensional_feature_check
};
enum class InspectionStatus : std::uint8_t { authored, disabled, stale };
enum class TargetStatus : std::uint8_t {
    proposed, accepted, rejected, stale, unreachable, collision, occluded,
    runtime_verified, runtime_failed
};
enum class WorkcellEntityKind : std::uint8_t {
    inspection_assembly, fixture, robot_base, sensor_tool, reference_frame, operator_exclusion
};
enum class KeepOutKind : std::uint8_t { box, sphere, cylinder, mesh };
enum class DerivedProductStatus : std::uint8_t { current, stale, rejected, unverified };
enum class PathSampleStatus : std::uint8_t {
    valid, rejected, stale, unreachable, collision, clearance_warning, unverified
};

[[nodiscard]] const char* source_kind_name(SourceKind value) noexcept;
[[nodiscard]] const char* topology_kind_name(TopologyKind value) noexcept;
[[nodiscard]] const char* requirement_kind_name(RequirementKind value) noexcept;
[[nodiscard]] const char* extraction_state_name(ExtractionState value) noexcept;
[[nodiscard]] const char* reconciliation_status_name(ReconciliationStatus value) noexcept;
[[nodiscard]] const char* inspection_kind_name(InspectionKind value) noexcept;
[[nodiscard]] bool inspection_kind_compatible(
    RequirementKind requirement, InspectionKind characteristic) noexcept;
[[nodiscard]] const char* inspection_status_name(InspectionStatus value) noexcept;
[[nodiscard]] const char* target_status_name(TargetStatus value) noexcept;
[[nodiscard]] const char* workcell_entity_kind_name(WorkcellEntityKind value) noexcept;
[[nodiscard]] const char* keep_out_kind_name(KeepOutKind value) noexcept;
[[nodiscard]] const char* derived_product_status_name(DerivedProductStatus value) noexcept;
[[nodiscard]] const char* path_sample_status_name(PathSampleStatus value) noexcept;

struct EngineeringSource {
    Id id = 0U;
    SourceKind kind = SourceKind::step_ap242;
    std::string locator;
    std::string revision;
    assets::Sha256Digest digest;
    std::string provider_identity;
    std::string provider_version;
    assets::Sha256Digest import_settings_digest;
    std::string import_run_id;
    std::string units = "millimeters";
    bool lossy = false;
    std::vector<std::string> diagnostics;
    [[nodiscard]] core::Result<void> validate() const;
};

struct EngineeringPart {
    Id id = 0U;
    std::string external_id;
    std::string name;
    std::string revision;
    std::string material;
    std::optional<double> nominal_mass;
    Id source_id = 0U;
    [[nodiscard]] core::Result<void> validate() const;
};

struct EngineeringOccurrence {
    Id id = 0U;
    std::string external_id;
    Id part_id = 0U;
    std::optional<Id> parent_id;
    core::Transform transform = core::Transform::identity();
    std::uint32_t quantity = 1U;
    Id source_id = 0U;
    [[nodiscard]] core::Result<void> validate() const;
};

struct TopologyBinding {
    Id id = 0U;
    Id source_id = 0U;
    Id part_id = 0U;
    std::optional<Id> occurrence_id;
    TopologyKind kind = TopologyKind::face;
    std::string external_id;
    std::string source_revision;
    std::uint64_t display_mesh_asset = 0U;
    std::uint32_t display_triangle = 0U;
    [[nodiscard]] core::Result<void> validate() const;
};

struct EngineeringBomLine {
    Id id = 0U;
    std::string part_number;
    std::string revision;
    std::string description;
    std::string material;
    double quantity = 0.0;
    std::string unit;
    Id source_id = 0U;
    std::optional<Id> mapped_part_id;
    ReconciliationStatus reconciliation = ReconciliationStatus::unresolved;
    [[nodiscard]] core::Result<void> validate() const;
};

struct RequirementProvenance {
    Id source_id = 0U;
    std::uint32_t page = 0U;
    std::string section;
    std::string callout;
    std::string drawing_zone;
    std::string extractor_identity;
    double extraction_confidence = 1.0;
    std::string original_text;
    [[nodiscard]] core::Result<void> validate() const;
};

struct EngineeringRequirement {
    Id id = 0U;
    RequirementKind kind = RequirementKind::dimensional_distance;
    ExtractionState extraction = ExtractionState::manual;
    RequirementProvenance provenance;
    std::vector<Id> topology_ids;
    double nominal = 0.0;
    double plus = 0.0;
    double minus = 0.0;
    std::string unit = "millimeters";
    std::vector<std::string> datum_refs;
    std::vector<std::string> modifier_refs;
    std::string acceptance_rule;
    bool enabled = true;
    std::string human_review_justification;
    [[nodiscard]] core::Result<void> validate() const;
};

struct SensorConstraint {
    std::string sensor_type;
    std::string tool_center_frame;
    double min_standoff = 0.0;
    double max_standoff = 0.0;
    double preferred_standoff = 0.0;
    double orientation_cone_degrees = 180.0;
    double field_of_view_degrees = 0.0;
    double min_clearance = 0.0;
    bool line_of_sight_required = false;
    bool contact_required = false;
    double measurement_duration_seconds = 0.0;
    [[nodiscard]] core::Result<void> validate() const;
};

struct CoverageConstraint {
    std::string kind = "single_point";
    double percentage = 0.0;
    [[nodiscard]] core::Result<void> validate() const;
};

struct AccessConstraint {
    std::vector<std::string> allowed_approach_directions;
    std::vector<std::string> forbidden_approach_directions;
    double minimum_approach_clearance = 0.0;
    bool fixture_occlusion_allowed = false;
    bool robot_base_reposition_permitted = false;
    [[nodiscard]] core::Result<void> validate() const;
};

struct InspectionCharacteristic {
    Id id = 0U;
    Id requirement_id = 0U;
    InspectionKind kind = InspectionKind::point_measurement;
    std::vector<Id> topology_ids;
    SensorConstraint sensor;
    CoverageConstraint coverage;
    AccessConstraint access;
    std::uint32_t priority = 0U;
    InspectionStatus status = InspectionStatus::authored;
    [[nodiscard]] core::Result<void> validate() const;
};

struct InspectionTarget {
    Id id = 0U;
    Id characteristic_id = 0U;
    Id source_topology_id = 0U;
    core::Transform sensor_pose = core::Transform::identity();
    double standoff = 0.0;
    TargetStatus status = TargetStatus::proposed;
    std::string generation_basis;
    bool human_authored = false;
    [[nodiscard]] core::Result<void> validate() const;
};

struct WorkcellEntity {
    Id id = 0U;
    WorkcellEntityKind kind = WorkcellEntityKind::fixture;
    std::string name;
    core::Transform transform = core::Transform::identity();
    std::string source;
    bool enabled = true;
    [[nodiscard]] core::Result<void> validate() const;
};

struct KeepOutRegion {
    Id id = 0U;
    KeepOutKind kind = KeepOutKind::box;
    std::string purpose;
    core::Transform transform = core::Transform::identity();
    core::Vec3d dimensions{1.0, 1.0, 1.0};
    std::string source;
    bool enabled = true;
    [[nodiscard]] core::Result<void> validate() const;
};

struct EngineeringWorkcell {
    Id id = 0U;
    std::string revision;
    std::vector<WorkcellEntity> entities;
    std::vector<KeepOutRegion> keep_outs;
    std::vector<WorkcellEntity> frames;
    std::string robot_model_identity;
    std::string robot_source_identity;
    std::string active_tool_frame;
    std::string capability_profile;
    std::string sensor_identity;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] core::Result<void> validate() const;
};

struct DerivedProductState {
    Id id = 0U;
    assets::Sha256Digest basis_digest;
    core::Revision basis_revision;
    DerivedProductStatus status = DerivedProductStatus::current;
    std::string stale_reason;
    [[nodiscard]] core::Result<void> validate() const;
};

struct PathSample {
    std::uint32_t sequence = 0U;
    double normalized = 0.0;
    core::Transform tool_pose = core::Transform::identity();
    std::optional<Id> target_id;
    PathSampleStatus status = PathSampleStatus::unverified;
    double clearance = 0.0;
    [[nodiscard]] core::Result<void> validate() const;
};

struct GeneratedPath {
    Id id = 0U;
    assets::Sha256Digest basis_digest;
    core::Revision basis_revision;
    DerivedProductStatus status = DerivedProductStatus::unverified;
    std::vector<PathSample> samples;
    [[nodiscard]] core::Result<void> validate() const;
};

struct CapabilityEvidence {
    std::vector<std::string> preserved;
    std::vector<std::string> derived;
    std::vector<std::string> unsupported;
    std::vector<std::string> runtime_validation;
    [[nodiscard]] core::Result<void> validate() const;
};

struct WorkcellRevisionRecord {
    std::string revision;
    std::string previous_revision;
    std::string subject_kind;
    Id subject_id = 0U;
    assets::Sha256Digest state_digest;
    std::string justification;
    friend bool operator==(const WorkcellRevisionRecord&, const WorkcellRevisionRecord&) = default;
};

class AuthoringDocument final {
public:
    static constexpr std::uint32_t kSchemaVersion = 3U;
    static constexpr std::size_t kMaxSerializedBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t kMaxCollectionEntries = 100'000U;

    std::string project_namespace;
    core::Revision project_revision;
    std::vector<EngineeringSource> sources;
    std::vector<EngineeringPart> parts;
    std::vector<EngineeringOccurrence> occurrences;
    std::vector<TopologyBinding> topology;
    std::vector<EngineeringBomLine> bom;
    std::vector<EngineeringRequirement> requirements;
    std::vector<InspectionCharacteristic> characteristics;
    std::vector<InspectionTarget> targets;
    std::optional<EngineeringWorkcell> workcell;
    std::vector<WorkcellRevisionRecord> workcell_revision_history;
    std::optional<GeneratedPath> path;
    std::vector<DerivedProductState> derived_products;
    CapabilityEvidence evidence;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<AuthoringDocument> deserialize(std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] core::Result<void> invalidate_source(
        Id source_id, std::string new_revision, assets::Sha256Digest new_digest);
};

inline constexpr std::size_t kMaxBomCsvBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxRequirementCsvBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxEngineeringEvidenceBytes = 64U * 1024U * 1024U;

struct RequirementCsvRecord {
    std::uint32_t page = 0U;
    std::string section;
    std::string callout;
    RequirementKind kind = RequirementKind::dimensional_distance;
    std::string topology_external_id;
    double nominal = 0.0;
    double plus = 0.0;
    double minus = 0.0;
    std::string unit;
    std::string acceptance_rule;
    std::string original_text;
};

// Parses the bounded Cartographer BOM CSV profile. The header order is fixed:
// part_number,revision,description,material,quantity,unit.
[[nodiscard]] core::Result<std::vector<EngineeringBomLine>> parse_bom_csv(
    std::string_view bytes, Id source_id, Id first_line_id);

// Parses the bounded, explicit requirement subset. Records require an exact
// topology external ID and remain proposals until a human reviews them.
[[nodiscard]] core::Result<std::vector<RequirementCsvRecord>> parse_requirement_csv(
    std::string_view bytes);

// Reconciles only exact engineering external IDs. Unknown units, ambiguous IDs,
// and unsupported occurrence hierarchies remain unresolved instead of being
// guessed. Assembly-only rows are emitted so omissions remain visible.
[[nodiscard]] core::Result<std::vector<EngineeringBomLine>> reconcile_bom_lines(
    const AuthoringDocument& authoring,
    std::vector<EngineeringBomLine> imported_lines,
    Id source_id,
    Id first_line_id);

// Returns null when the part is absent, has no placed occurrence, or its
// quantity cannot be established from the bounded occurrence hierarchy.
[[nodiscard]] std::optional<double> assembly_quantity_for_part(
    const AuthoringDocument& authoring, Id part_id);

struct InspectionPackage final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 48U * 1024U * 1024U;

    std::string project_namespace;
    core::Revision project_revision;
    assets::Sha256Digest authoring_digest;
    std::string authoring_bytes;
    std::string publication_profile = "dv012-inspection-v1";
    CapabilityEvidence evidence;
    assets::Sha256Digest publication_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<InspectionPackage> deserialize(std::string_view text);
    [[nodiscard]] static core::Result<InspectionPackage> publish(
        const AuthoringDocument& authoring, core::Revision project_revision,
        std::string project_namespace);
};

enum class VantaAdmissionStatus : std::uint8_t { admit, refuse };

struct VantaAdmissionResult {
    VantaAdmissionStatus status = VantaAdmissionStatus::refuse;
    assets::Sha256Digest admitted_publication_digest;
    assets::Sha256Digest admitted_authoring_digest;
    std::vector<std::string> reasons;
    [[nodiscard]] core::Result<void> validate() const;
};

[[nodiscard]] core::Result<VantaAdmissionResult> admit_for_vanta(
    const InspectionPackage& package);
[[nodiscard]] core::Result<AuthoringDocument> make_dv012_fixture();

} // namespace carto::engineering
