#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::scientific {

template <typename Tag>
struct Id {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const Id&) const noexcept = default;
};

struct ModelEntityTag;
struct RegionTag;
struct FieldTag;
struct StudyTag;
struct BoundaryTag;
struct InterfaceTag;
struct SourceTag;
struct LoadTag;
struct ProbeTag;
struct ParameterTag;
struct ResultSetTag;
struct StructureTag;
struct GrowthDomainTag;
struct AttachmentTag;
struct LandmarkTag;
struct GeneTag;
struct StageTag;
struct InstructionTag;
struct TraceTag;
struct LineageTag;
struct MutationTag;
struct RecombinationTag;
struct MorphometricTag;
struct SignatureTag;
struct ComparisonTag;
struct ContigTag;
struct VariantTag;
struct SequenceFeatureTag;
struct AnatomyTermTag;
struct AnatomyPartTag;
struct TissueRegionTag;
struct AnatomyRelationTag;
struct AnatomyBindingTag;
struct ReplayTag;
struct VisualizationTag;
struct ImportTag;
struct CapabilityTag;
struct RegulatoryElementTag;
struct RegulatoryEdgeTag;
struct ExpressionTag;

using ModelEntityId = Id<ModelEntityTag>;
using RegionId = Id<RegionTag>;
using FieldId = Id<FieldTag>;
using StudyId = Id<StudyTag>;
using BoundaryId = Id<BoundaryTag>;
using InterfaceId = Id<InterfaceTag>;
using SourceId = Id<SourceTag>;
using LoadId = Id<LoadTag>;
using ProbeId = Id<ProbeTag>;
using ParameterId = Id<ParameterTag>;
using ResultSetId = Id<ResultSetTag>;
using StructureId = Id<StructureTag>;
using GrowthDomainId = Id<GrowthDomainTag>;
using AttachmentId = Id<AttachmentTag>;
using LandmarkId = Id<LandmarkTag>;
using GeneId = Id<GeneTag>;
using StageId = Id<StageTag>;
using InstructionId = Id<InstructionTag>;
using TraceId = Id<TraceTag>;
using LineageId = Id<LineageTag>;
using MutationId = Id<MutationTag>;
using RecombinationId = Id<RecombinationTag>;
using MorphometricId = Id<MorphometricTag>;
using SignatureId = Id<SignatureTag>;
using ComparisonId = Id<ComparisonTag>;
using ContigId = Id<ContigTag>;
using VariantId = Id<VariantTag>;
using SequenceFeatureId = Id<SequenceFeatureTag>;
using AnatomyTermId = Id<AnatomyTermTag>;
using AnatomyPartId = Id<AnatomyPartTag>;
using TissueRegionId = Id<TissueRegionTag>;
using AnatomyRelationId = Id<AnatomyRelationTag>;
using AnatomyBindingId = Id<AnatomyBindingTag>;
using ReplayId = Id<ReplayTag>;
using VisualizationId = Id<VisualizationTag>;
using ImportId = Id<ImportTag>;
using CapabilityId = Id<CapabilityTag>;
using RegulatoryElementId = Id<RegulatoryElementTag>;
using RegulatoryEdgeId = Id<RegulatoryEdgeTag>;
using ExpressionId = Id<ExpressionTag>;

enum class FieldKind : std::uint8_t {
    scalar,
    vector,
    tensor,
    categorical,
};

enum class Association : std::uint8_t {
    point,
    edge,
    face,
    cell,
    continuous,
    sample_set,
};

[[nodiscard]] std::string_view field_kind_name(FieldKind kind) noexcept;
[[nodiscard]] std::string_view association_name(Association association) noexcept;

struct Provenance {
    std::string source_reference;
    std::string release;
    std::string license;
    std::string provider;
    std::string imported_at_utc;
    std::optional<assets::Sha256Digest> content_digest;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] core::Result<void> validate() const;
};

struct GeometryBinding {
    // Empty kind/reference is an explicit semantic-only binding. Otherwise
    // kind is provider-neutral (mesh, curve, volume, point_set, graph, ...)
    // and reference identifies the authored geometry or external binding.
    std::string kind;
    std::string reference;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Region {
    RegionId id;
    std::string semantic_type;
    GeometryBinding geometry;
    std::optional<RegionId> parent_region;
    std::string material_or_tissue_binding;
    std::vector<std::string> tags;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Boundary {
    BoundaryId id;
    std::string semantic_type;
    GeometryBinding geometry;
    std::optional<RegionId> region;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Interface {
    InterfaceId id;
    std::string semantic_type;
    RegionId first_region;
    RegionId second_region;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Source {
    SourceId id;
    std::string semantic_type;
    std::optional<RegionId> region;
    std::string units;
    double value = 0.0;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Load {
    LoadId id;
    std::string semantic_type;
    std::optional<RegionId> region;
    std::string units;
    double value = 0.0;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Parameter {
    ParameterId id;
    std::string name;
    std::string units;
    double value = 0.0;
    std::optional<double> lower_bound;
    std::optional<double> upper_bound;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived parameter assignment data for a bounded study sweep. These records
// are planning inputs/results only; they do not execute a provider or solver.
struct ParameterAssignment {
    ParameterId parameter;
    double value = 0.0;

    friend constexpr bool operator==(const ParameterAssignment&, const ParameterAssignment&) = default;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ParameterSweepAxis {
    ParameterId parameter;
    std::vector<double> values;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ParameterSweepPreview {
    StudyId study;
    core::Revision source_model_revision;
    std::string strategy;
    std::vector<ParameterAssignment> base_assignments;
    std::vector<ParameterSweepAxis> axes;
    std::vector<std::vector<ParameterAssignment>> cases;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Field {
    FieldId id;
    FieldKind kind = FieldKind::scalar;
    Association association = Association::continuous;
    std::string units;
    std::string coordinate_frame;
    std::string time_or_stage;
    std::optional<StudyId> source_study;
    std::optional<RegionId> sample_domain;
    std::string provider;
    std::optional<assets::Sha256Digest> data_digest;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Probe {
    ProbeId id;
    std::string name;
    std::optional<RegionId> region;
    std::optional<FieldId> field;
    std::string location_reference;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Study {
    StudyId id;
    core::Revision source_model_revision;
    std::vector<RegionId> regions;
    std::vector<BoundaryId> boundaries;
    std::vector<InterfaceId> interfaces;
    std::vector<SourceId> sources;
    std::vector<LoadId> loads;
    std::vector<ParameterId> parameters;
    std::string solver_configuration;
    std::string provider;
    std::vector<FieldId> requested_outputs;
    std::string acceptance_metadata;
    std::optional<assets::Sha256Digest> study_digest;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ResultSet {
    ResultSetId id;
    StudyId source_study;
    std::vector<FieldId> fields;
    std::vector<ProbeId> probes;
    std::string provider;
    std::string solver_version;
    std::optional<assets::Sha256Digest> source_study_digest;
    std::optional<assets::Sha256Digest> result_digest;
    std::string convergence_evidence;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MorphologyStructure {
    StructureId id;
    std::string semantic_class;
    std::optional<StructureId> parent_structure;
    std::string laterality;
    std::string developmental_origin;
    GeometryBinding geometry;
    std::string local_frame;
    std::vector<LandmarkId> landmarks;
    std::string confidence;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MorphologyAttachment {
    AttachmentId id;
    StructureId first_structure;
    StructureId second_structure;
    std::string relationship;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MorphologyLandmark {
    LandmarkId id;
    std::string name;
    std::optional<StructureId> structure;
    core::Vec3d position;
    std::string coordinate_frame;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MorphologyGrowthDomain {
    GrowthDomainId id;
    StructureId structure;
    std::optional<FieldId> driving_field;
    core::Vec3d growth_direction;
    double growth_rate = 0.0;
    core::Vec3d anisotropy{1.0, 1.0, 1.0};
    std::optional<double> maximum_extent;
    std::string start_stage;
    std::string end_stage;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived, read-only landmark measurement data. Coordinates are interpreted
// only in the admitted landmark frame; this does not infer surface geometry.
struct MorphologyMeasurementPreview {
    StructureId structure;
    std::string metric;
    std::string units;
    std::string coordinate_frame;
    std::vector<LandmarkId> landmarks;
    std::vector<double> values;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived, read-only principal-axis data over an explicitly owned landmark
// cloud. Ambiguous eigenvalue spectra are rejected instead of receiving an
// invented tie-break.
struct MorphologyPrincipalAxesPreview {
    StructureId structure;
    std::string coordinate_frame;
    std::vector<LandmarkId> landmarks;
    core::Vec3d centroid;
    std::array<core::Vec3d, 3> axes{};
    std::array<double, 3> variances{};

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived, read-only landmark correspondence data for a paired morphology
// preview. This is intentionally not persisted as provider or solver output.
struct MorphologyLandmarkDelta {
    LandmarkId reference_landmark;
    LandmarkId candidate_landmark;
    core::Vec3d displacement;
    double distance = 0.0;
};

// Explicit, derived landmark correspondence input for paired morphology
// previews. It is not persisted as provider or solver output.
struct MorphologyLandmarkCorrespondence {
    LandmarkId reference_landmark;
    LandmarkId candidate_landmark;

    [[nodiscard]] core::Result<void> validate() const;
};

// Explicit surface sample metadata for a paired morphology preview. The
// positions remain bound to admitted landmarks; normals, curvature, and
// thickness are caller-supplied measurements and are never inferred here.
struct MorphologySurfaceSample {
    LandmarkId reference_landmark;
    LandmarkId candidate_landmark;
    core::Vec3d reference_normal{0.0, 0.0, 1.0};
    core::Vec3d candidate_normal{0.0, 0.0, 1.0};
    double reference_curvature = 0.0;
    double candidate_curvature = 0.0;
    double reference_thickness = 0.0;
    double candidate_thickness = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MorphologySurfaceDelta {
    LandmarkId reference_landmark;
    LandmarkId candidate_landmark;
    core::Vec3d displacement;
    double distance = 0.0;
    double normal_displacement = 0.0;
    double curvature_delta = 0.0;
    double thickness_delta = 0.0;
    double normal_angle = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived, read-only paired surface metadata. This is a shared-frame CPU
// reference projection over explicit samples; it is not a registered surface,
// provider result, rendered heatmap, or persisted geometry.
struct MorphologySurfaceDeltaPreview {
    StructureId reference_structure;
    StructureId candidate_structure;
    std::string coordinate_frame;
    std::vector<MorphologySurfaceDelta> deltas;
    double rms_displacement = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct BilateralMorphologyPreview {
    StructureId reference_structure;
    StructureId candidate_structure;
    std::string alignment_mode;
    std::vector<MorphologyLandmarkDelta> landmark_deltas;
    double rms_distance = 0.0;
    // The two-argument shared-frame preview keeps the identity transform.
    // Aligned previews expose the deterministic candidate-to-reference
    // transform used for the derived deltas.
    core::Vec3d alignment_translation;
    core::Quaternion alignment_rotation = core::Quaternion::identity();
    double alignment_scale = 1.0;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived landmark-set exchange result. The tabular envelope carries stable
// landmark identity, structure ownership, coordinates, and frame metadata;
// it does not pretend to carry provider-specific geometry or solver output.
struct MorphologyLandmarkSetExport {
    std::string text;
    std::vector<std::string> feature_loss_notes;
};

class MorphologyModel {
public:
    static constexpr std::uint32_t kSchemaVersion = 2U;

    std::string organism_identity;
    std::string body_plan;
    std::vector<std::string> axes;
    std::string symmetry;

    [[nodiscard]] core::Result<void> insert_structure(MorphologyStructure value);
    [[nodiscard]] core::Result<void> insert_attachment(MorphologyAttachment value);
    [[nodiscard]] core::Result<void> insert_landmark(MorphologyLandmark value);
    // Binds an admitted landmark to an existing structure while preserving
    // the model's reference-validation boundary.
    [[nodiscard]] core::Result<void> attach_landmark_to_structure(
        LandmarkId landmark,
        StructureId structure);
    [[nodiscard]] core::Result<void> insert_growth_domain(MorphologyGrowthDomain value);
    [[nodiscard]] const std::map<StructureId, MorphologyStructure>& structures() const noexcept {
        return structures_;
    }
    [[nodiscard]] const std::map<AttachmentId, MorphologyAttachment>& attachments() const noexcept {
        return attachments_;
    }
    [[nodiscard]] const std::map<LandmarkId, MorphologyLandmark>& landmarks() const noexcept {
        return landmarks_;
    }
    [[nodiscard]] const std::map<GrowthDomainId, MorphologyGrowthDomain>& growth_domains() const noexcept {
        return growth_domains_;
    }
    // CPU-only measurements over explicitly owned landmarks. Supported
    // metrics are "distance", "segment-length", "arc-length", "angle",
    // "branch-angle", "ratio", "area", "volume", "centroid", and
    // "bounding-dimensions". Arc length uses the caller's ordered polyline;
    // area uses an ordered planar polygon; volume uses exactly four landmarks
    // as an explicit tetrahedron. A ratio uses four landmarks as two ordered
    // segments and returns segment A / segment B. These previews do not infer
    // curves, surfaces, or provider geometry.
    [[nodiscard]] core::Result<MorphologyMeasurementPreview> preview_measurement(
        StructureId structure,
        std::string_view metric,
        std::span<const LandmarkId> landmarks) const;
    [[nodiscard]] core::Result<MorphologyPrincipalAxesPreview> preview_principal_axes(
        StructureId structure,
        std::span<const LandmarkId> landmarks) const;
    // CPU-only shared-frame landmark correspondence. This preserves the
    // original identity-transform behavior.
    [[nodiscard]] core::Result<BilateralMorphologyPreview> preview_bilateral_landmarks(
        StructureId reference_structure,
        StructureId candidate_structure) const;
    // CPU-only aligned correspondence. Supported modes are "landmark",
    // "rigid", "scale-normalized", and "principal-axis"; principal-axis
    // rejects ambiguous eigenvalue spectra and remains a CPU reference preview,
    // not statistical morphospace execution.
    [[nodiscard]] core::Result<BilateralMorphologyPreview> preview_bilateral_landmarks(
        StructureId reference_structure,
        StructureId candidate_structure,
        std::string_view alignment_mode) const;
    // Explicit correspondence avoids relying on cross-specimen landmark
    // names. An empty map retains the name-based compatibility behavior.
    [[nodiscard]] core::Result<BilateralMorphologyPreview> preview_bilateral_landmarks(
        StructureId reference_structure,
        StructureId candidate_structure,
        std::span<const MorphologyLandmarkCorrespondence> correspondences,
        std::string_view alignment_mode) const;
    // CPU-only shared-frame surface metadata projection. Positions come from
    // explicitly owned landmarks; normals, curvature, and thickness are
    // caller-supplied and remain derived inspection data.
    [[nodiscard]] core::Result<MorphologySurfaceDeltaPreview> preview_surface_delta(
        StructureId reference_structure,
        StructureId candidate_structure,
        std::span<const MorphologySurfaceSample> samples) const;
    [[nodiscard]] core::Result<MorphologyLandmarkSetExport> export_landmark_set() const;
    [[nodiscard]] static core::Result<MorphologyModel> import_landmark_set(
        std::string_view text,
        MorphologyModel reference_model,
        Provenance provenance);
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<MorphologyModel> deserialize(std::string_view text);

private:
    std::map<StructureId, MorphologyStructure> structures_;
    std::map<AttachmentId, MorphologyAttachment> attachments_;
    std::map<LandmarkId, MorphologyLandmark> landmarks_;
    std::map<GrowthDomainId, MorphologyGrowthDomain> growth_domains_;
};

struct MorphometricObservation {
    MorphometricId id;
    StructureId structure;
    std::string metric;
    std::string units;
    double value = 0.0;
    std::string method;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MorphologySignature {
    SignatureId id;
    StructureId structure;
    std::string descriptor;
    std::string coordinate_frame;
    std::string normalization;
    std::vector<double> components;
    std::vector<std::string> component_labels;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MorphologyComparison {
    ComparisonId id;
    StructureId reference_structure;
    StructureId candidate_structure;
    std::string metric;
    double distance = 0.0;
    double similarity = 0.0;
    std::optional<SignatureId> reference_signature;
    std::optional<SignatureId> candidate_signature;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived, read-only signature comparison. The persisted MorphologyComparison
// remains provider-authored; this preview is a CPU reference calculation.
struct MorphologySignatureComparisonPreview {
    SignatureId reference_signature;
    SignatureId candidate_signature;
    StructureId reference_structure;
    StructureId candidate_structure;
    std::string metric;
    std::vector<double> component_deltas;
    double distance = 0.0;
    double similarity = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

// Explicit source bindings for the derived evolutionary morphospace preview.
// A lineage does not implicitly own a morphology signature: callers must
// provide the mapping so that the preview cannot silently join unrelated
// specimens or provider labels.
struct EvolutionaryMorphospaceBinding {
    LineageId lineage;
    SignatureId signature;

    [[nodiscard]] core::Result<void> validate() const;
};

struct EvolutionaryMorphospacePoint {
    LineageId lineage;
    SignatureId signature;
    std::uint64_t generation = 0U;
    core::Vec3d position;

    [[nodiscard]] core::Result<void> validate() const;
};

struct EvolutionaryMorphospaceEdge {
    LineageId parent;
    LineageId child;
    core::Vec3d displacement;
    double distance = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived, read-only CPU reference projection of authored morphology
// signatures onto a bounded 2D or 3D component space. The preview may draw
// lineage trajectories, including both parents of a recombination node, but
// it never infers morphology, executes a provider, or persists the projected
// coordinates as source truth.
struct EvolutionaryMorphospacePreview {
    std::string projection;
    std::string normalization;
    std::uint32_t dimensions = 0U;
    std::string descriptor;
    std::string coordinate_frame;
    std::string signature_normalization;
    std::vector<std::string> component_labels;
    std::vector<EvolutionaryMorphospacePoint> points;
    std::vector<EvolutionaryMorphospaceEdge> trajectories;

    [[nodiscard]] core::Result<void> validate() const;
};

class MorphometricModel {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;

    [[nodiscard]] core::Result<void> insert_observation(MorphometricObservation value);
    [[nodiscard]] core::Result<void> insert_signature(MorphologySignature value);
    [[nodiscard]] core::Result<void> insert_comparison(MorphologyComparison value);
    [[nodiscard]] const std::map<MorphometricId, MorphometricObservation>& observations() const noexcept {
        return observations_;
    }
    [[nodiscard]] const std::map<SignatureId, MorphologySignature>& signatures() const noexcept {
        return signatures_;
    }
    [[nodiscard]] const std::map<ComparisonId, MorphologyComparison>& comparisons() const noexcept {
        return comparisons_;
    }
    [[nodiscard]] core::Result<MorphologySignatureComparisonPreview>
    preview_signature_comparison(SignatureId reference_signature,
                                 SignatureId candidate_signature) const;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<MorphometricModel> deserialize(std::string_view text);

private:
    std::map<MorphometricId, MorphometricObservation> observations_;
    std::map<SignatureId, MorphologySignature> signatures_;
    std::map<ComparisonId, MorphologyComparison> comparisons_;
};

struct FunctionalGene {
    GeneId id;
    std::string name;
    std::string group;
    std::string type;
    double value = 0.0;
    std::string units;
    std::optional<double> lower_bound;
    std::optional<double> upper_bound;
    std::string mutation_policy;
    std::string inheritance_policy;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct GenomeDiff {
    std::vector<GeneId> added;
    std::vector<GeneId> removed;
    std::vector<GeneId> changed;
    // Functional gene groups are the public module identity until an explicit
    // provider adaptation layer supplies a richer module graph.
    std::vector<std::string> added_modules;
    std::vector<std::string> removed_modules;
    std::vector<std::string> changed_modules;
    std::vector<GeneId> changed_neural_values;
    bool developmental_program_changed = false;
    bool lineage_changed = false;
    bool source_identity_changed = false;
};

class FunctionalGenome {
public:
    static constexpr std::uint32_t kSchemaVersion = 2U;

    std::string schema;
    std::string developmental_program_reference;
    std::string lineage_reference;
    std::optional<assets::Sha256Digest> source_digest;

    [[nodiscard]] core::Result<void> insert_gene(FunctionalGene value);
    // Authoring edits preserve stable GeneId identity and validate before
    // replacing source state. Project-level callers must still publish the
    // enclosing ScientificModel through the journaled transaction boundary.
    [[nodiscard]] core::Result<void> replace_gene(FunctionalGene value);
    [[nodiscard]] core::Result<void> remove_gene(GeneId id);
    [[nodiscard]] const std::map<GeneId, FunctionalGene>& genes() const noexcept {
        return genes_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] GenomeDiff diff_against(const FunctionalGenome& parent) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<FunctionalGenome> deserialize(std::string_view text);

private:
    std::map<GeneId, FunctionalGene> genes_;
};

struct SequenceContig {
    ContigId id;
    std::string name;
    std::string sequence;
    std::optional<assets::Sha256Digest> content_digest;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SequenceVariant {
    VariantId id;
    ContigId contig;
    std::uint64_t position = 0U;
    std::string reference;
    std::string alternate;
    std::string kind;
    Provenance provenance;
    // Optional provider identifier from an exchange format such as VCF. It
    // is deliberately after provenance so existing aggregate construction
    // remains source-compatible.
    std::string source_identifier;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SequenceFeatureAttribute {
    std::string key;
    std::string value;

    [[nodiscard]] core::Result<void> validate() const;
};

// Sequence annotations are source annotations, not functional-genome genes or
// phenotype truth. Coordinates are kept internally as zero-based, half-open
// intervals so adapters cannot silently mix GFF3's one-based inclusive
// coordinates with the native model.
struct SequenceFeature {
    SequenceFeatureId id;
    ContigId contig;
    std::uint64_t start = 0U;
    std::uint64_t end = 0U;
    std::string type;
    std::string source;
    std::optional<double> score;
    std::string strand = ".";
    std::optional<std::uint8_t> phase;
    std::string source_identifier;
    std::vector<SequenceFeatureAttribute> attributes;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived FASTA export result. FASTA can carry contig names and sequence
// bytes, but not Cartographer provenance, assembly metadata, or variants; the
// loss notes make that boundary explicit to callers.
struct SequenceGenomeFastaExport {
    std::string text;
    std::vector<std::string> feature_loss_notes;
};

// Derived VCF export result. VCF carries reference-relative variants and a
// bounded provenance/identity envelope, but not full contig sequence bytes or
// per-record Cartographer provenance.
struct SequenceGenomeVcfExport {
    std::string text;
    std::vector<std::string> feature_loss_notes;
};

// Derived GFF3 export. GFF3 is an annotation interchange surface; it does
// not make annotations functional genes, developmental instructions, or
// phenotype capabilities. The feature-loss notes make that boundary visible.
struct SequenceGenomeGff3Export {
    std::string text;
    std::vector<std::string> feature_loss_notes;
};

// Derived BED6 export. BED6 can carry a bounded interval, name, integer score,
// and strand, but not GFF3 type/source/phase/attributes or Cartographer
// provenance. The adapter encodes stable identity in the name and reports the
// remaining conversion boundary explicitly.
struct SequenceGenomeBed6Export {
    std::string text;
    std::vector<std::string> feature_loss_notes;
};

// Derived GTF 2.2 export. The bounded adapter preserves the nine-column
// interval surface and quoted attributes but still treats the records as
// annotations rather than functional-genome or phenotype truth.
struct SequenceGenomeGtfExport {
    std::string text;
    std::vector<std::string> feature_loss_notes;
};

class SequenceGenome {
public:
    static constexpr std::uint32_t kSchemaVersion = 3U;
    static constexpr std::size_t kMaxSequenceBytes = 16U * 1024U * 1024U;

    std::string schema;
    std::string assembly;
    std::string sample_reference;
    std::optional<assets::Sha256Digest> source_digest;

    [[nodiscard]] core::Result<void> insert_contig(SequenceContig value);
    [[nodiscard]] core::Result<void> insert_variant(SequenceVariant value);
    [[nodiscard]] core::Result<void> insert_feature(SequenceFeature value);
    [[nodiscard]] const std::map<ContigId, SequenceContig>& contigs() const noexcept {
        return contigs_;
    }
    [[nodiscard]] const std::map<VariantId, SequenceVariant>& variants() const noexcept {
        return variants_;
    }
    [[nodiscard]] const std::map<SequenceFeatureId, SequenceFeature>& features() const noexcept {
        return features_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<SequenceGenome> deserialize(std::string_view text);
    // Provider-neutral, bounded FASTA exchange. Import requires the caller to
    // supply Cartographer provenance/identity because FASTA has no trusted
    // provenance channel; variants are intentionally not inferred from it.
    [[nodiscard]] core::Result<SequenceGenomeFastaExport> export_fasta(
        std::size_t line_width = 80U) const;
    [[nodiscard]] static core::Result<SequenceGenome> import_fasta(
        std::string_view text,
        std::string assembly,
        std::string sample_reference,
        Provenance provenance,
        std::optional<assets::Sha256Digest> source_digest = std::nullopt);
    // Provider-neutral, bounded VCF exchange. VCF is admitted only when the
    // file identifies the exact reference genome, sample, coordinate system,
    // and a provenance envelope. The supplied reference genome is copied and
    // must contain sequence bytes but no pre-existing variants.
    [[nodiscard]] core::Result<SequenceGenomeVcfExport> export_vcf() const;
    [[nodiscard]] static core::Result<SequenceGenome> import_vcf(
        std::string_view text,
        SequenceGenome reference_genome,
        Provenance provenance);
    // Provider-neutral, bounded GFF3 annotation exchange. Import is bound to
    // the exact reference genome digest and admits annotations into an empty
    // feature set only; it never infers functional or phenotypic semantics.
    [[nodiscard]] core::Result<SequenceGenomeGff3Export> export_gff3() const;
    [[nodiscard]] static core::Result<SequenceGenome> import_gff3(
        std::string_view text,
        SequenceGenome reference_genome,
        Provenance provenance);
    // Provider-neutral, bounded BED6 exchange. Import is bound to the exact
    // reference genome digest and admits annotations into an empty feature
    // set; BED6 fields do not acquire functional or phenotypic semantics.
    [[nodiscard]] core::Result<SequenceGenomeBed6Export> export_bed6() const;
    [[nodiscard]] static core::Result<SequenceGenome> import_bed6(
        std::string_view text,
        SequenceGenome reference_genome,
        Provenance provenance);
    // Provider-neutral, bounded GTF 2.2 exchange. Import is reference-bound
    // and refuses to merge into an existing feature set.
    [[nodiscard]] core::Result<SequenceGenomeGtfExport> export_gtf() const;
    [[nodiscard]] static core::Result<SequenceGenome> import_gtf(
        std::string_view text,
        SequenceGenome reference_genome,
        Provenance provenance);

private:
    std::map<ContigId, SequenceContig> contigs_;
    std::map<VariantId, SequenceVariant> variants_;
    std::map<SequenceFeatureId, SequenceFeature> features_;
};

struct AnatomyTerm {
    AnatomyTermId id;
    std::string namespace_name;
    std::string accession;
    std::string label;
    std::string category;
    std::optional<AnatomyTermId> parent_term;
    std::string definition;
    Provenance provenance;
    // Optional ontology release metadata is kept after provenance in the
    // aggregate layout so existing callers remain source-compatible. It is
    // serialized by anatomy schema v3 and absent in v1/v2 records.
    std::string ontology_version;
    std::string source_release;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AnatomyPart {
    AnatomyPartId id;
    StructureId structure;
    AnatomyTermId term;
    std::optional<AnatomyPartId> parent_part;
    std::string role;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct TissueRegion {
    TissueRegionId id;
    std::string tissue_type;
    std::optional<StructureId> structure;
    std::optional<RegionId> region;
    std::string physical_properties;
    std::string developmental_origin;
    std::string stage;
    GeometryBinding geometry;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AnatomyRelation {
    AnatomyRelationId id;
    AnatomyPartId first_part;
    AnatomyPartId second_part;
    std::string relation;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived ontology-table exchange. The table carries semantic term identity
// and release metadata only; it does not publish parts, tissue behavior,
// geometry, or runtime anatomy authority.
struct AnatomyOntologyTableExport {
    std::string text;
    std::vector<std::string> feature_loss_notes;
};

class AnatomyModel {
public:
    static constexpr std::uint32_t kSchemaVersion = 3U;

    std::string schema;

    [[nodiscard]] core::Result<void> insert_term(AnatomyTerm value);
    [[nodiscard]] core::Result<void> insert_part(AnatomyPart value);
    [[nodiscard]] core::Result<void> insert_tissue_region(TissueRegion value);
    [[nodiscard]] core::Result<void> insert_relation(AnatomyRelation value);
    [[nodiscard]] const std::map<AnatomyTermId, AnatomyTerm>& terms() const noexcept {
        return terms_;
    }
    [[nodiscard]] const std::map<AnatomyPartId, AnatomyPart>& parts() const noexcept {
        return parts_;
    }
    [[nodiscard]] const std::map<TissueRegionId, TissueRegion>& tissue_regions() const noexcept {
        return tissue_regions_;
    }
    [[nodiscard]] const std::map<AnatomyRelationId, AnatomyRelation>& relations() const noexcept {
        return relations_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<AnatomyModel> deserialize(std::string_view text);
    [[nodiscard]] core::Result<AnatomyOntologyTableExport> export_ontology_table() const;
    [[nodiscard]] static core::Result<AnatomyModel> import_ontology_table(
        std::string_view text,
        AnatomyModel reference_model,
        Provenance provenance);

private:
    std::map<AnatomyTermId, AnatomyTerm> terms_;
    std::map<AnatomyPartId, AnatomyPart> parts_;
    std::map<TissueRegionId, TissueRegion> tissue_regions_;
    std::map<AnatomyRelationId, AnatomyRelation> relations_;
};

struct AnatomyProviderBinding {
    AnatomyBindingId id;
    AnatomyPartId part;
    std::string semantic_id;
    std::string laterality;
    GeometryBinding source_geometry;
    std::string source_release;
    std::string license;
    std::optional<assets::Sha256Digest> geometry_digest;
    std::string tissue_material;
    std::string local_edit_reference;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

class AnatomyProviderManifest {
public:
    static constexpr std::uint32_t kSchemaVersion = 2U;

    std::string schema;
    std::string provider;
    std::string publication_reference;
    std::string status;
    std::optional<core::Revision> source_model_revision;
    std::optional<assets::Sha256Digest> source_manifest_digest;
    Provenance provenance;

    [[nodiscard]] core::Result<void> insert_binding(AnatomyProviderBinding value);
    [[nodiscard]] const std::map<AnatomyBindingId, AnatomyProviderBinding>& bindings() const noexcept {
        return bindings_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    // Stronger authoring-side gate for a provider candidate. This checks
    // source identity and geometry digests but does not admit a runtime.
    [[nodiscard]] core::Result<void> validate_handoff_candidate(
        core::Revision expected_source_revision) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<AnatomyProviderManifest> deserialize(std::string_view text);

private:
    std::map<AnatomyBindingId, AnatomyProviderBinding> bindings_;
};

// Provider-neutral publication envelope for an anatomy candidate. The
// manifest remains authored Cartographer state; this envelope binds it to the
// source revision and canonical manifest digest without admitting VANTA or
// any other runtime state.
class AnatomyProviderCandidate {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;

    AnatomyProviderManifest manifest;
    core::Revision source_model_revision;
    assets::Sha256Digest manifest_digest;

    [[nodiscard]] static core::Result<AnatomyProviderCandidate> from_manifest(
        AnatomyProviderManifest manifest,
        core::Revision expected_source_revision);
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<AnatomyProviderCandidate> deserialize(
        std::string_view text);
};

struct ScientificReplayCapsule {
    ReplayId id;
    core::Revision source_model_revision;
    std::string provider;
    std::string solver_version;
    std::vector<std::uint64_t> seeds;
    std::vector<ParameterId> parameters;
    std::vector<assets::Sha256Digest> external_dataset_digests;
    std::vector<assets::Sha256Digest> expected_output_digests;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

class ScientificReplayCapsuleModel {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;

    std::string schema;
    std::string reference;
    std::string status;
    Provenance provenance;

    [[nodiscard]] core::Result<void> insert_capsule(ScientificReplayCapsule value);
    [[nodiscard]] const std::map<ReplayId, ScientificReplayCapsule>& capsules() const noexcept {
        return capsules_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<ScientificReplayCapsuleModel> deserialize(
        std::string_view text);

private:
    std::map<ReplayId, ScientificReplayCapsule> capsules_;
};

struct FieldVisualizationSample {
    double source_value = 0.0;
    double normalized_value = 0.0;
    std::array<double, 4> rgba{0.0, 0.0, 0.0, 1.0};
};

// Derived CPU-reference input/output for vector-field inspection. These
// records are intentionally not persisted as provider or renderer output.
struct FieldVectorSample {
    core::Vec3d position;
    core::Vec3d vector;
};

struct FieldVectorGlyph {
    core::Vec3d origin;
    core::Vec3d source_vector;
    core::Vec3d direction;
    double magnitude = 0.0;
    double normalized_magnitude = 0.0;
    double length = 0.0;
    std::array<double, 4> rgba{0.0, 0.0, 0.0, 1.0};
};

struct FieldVisualizationPreview {
    VisualizationId visualization;
    FieldId field;
    std::string mode;
    std::string palette;
    double lower_bound = 0.0;
    double upper_bound = 0.0;
    std::uint64_t clipped_low = 0U;
    std::uint64_t clipped_high = 0U;
    std::vector<FieldVisualizationSample> samples;
    std::vector<double> legend_ticks;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived CPU-reference output for a bounded scalar slice. Samples are stored
// row-major; the plane frame makes their spatial meaning explicit without
// claiming renderer or provider execution.
struct FieldSlicePreview {
    VisualizationId visualization;
    FieldId field;
    std::string mode;
    std::string palette;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    core::Vec3d origin;
    core::Vec3d normal;
    core::Vec3d horizontal_axis;
    core::Vec3d vertical_axis;
    double lower_bound = 0.0;
    double upper_bound = 0.0;
    std::uint64_t clipped_low = 0U;
    std::uint64_t clipped_high = 0U;
    std::vector<FieldVisualizationSample> samples;
    std::vector<double> legend_ticks;

    [[nodiscard]] core::Result<void> validate() const;
};

struct FieldContourSegment {
    core::Vec3d start;
    core::Vec3d end;
    double level = 0.0;
    std::array<double, 4> rgba{0.0, 0.0, 0.0, 1.0};
};

// Derived CPU-reference marching-squares output. It is inspection data only;
// it is not a persisted surface, mesh, or solver result.
struct FieldContourPreview {
    VisualizationId visualization;
    FieldId field;
    std::string mode;
    std::string palette;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    double lower_bound = 0.0;
    double upper_bound = 0.0;
    std::vector<double> levels;
    std::vector<FieldContourSegment> segments;

    [[nodiscard]] core::Result<void> validate() const;
};

struct FieldIsosurfaceTriangle {
    core::Vec3d a;
    core::Vec3d b;
    core::Vec3d c;
    double level = 0.0;
    std::array<double, 4> rgba{0.0, 0.0, 0.0, 1.0};
};

// Derived CPU-reference marching-tetrahedra output for a scalar volume. The
// triangle positions are normalized to the input grid domain [0,1]^3. This
// is inspection data only; it is not a persisted mesh, provider result,
// solver output, or renderer submission.
struct FieldIsosurfacePreview {
    VisualizationId visualization;
    FieldId field;
    std::string mode;
    std::string palette;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t depth = 0U;
    double lower_bound = 0.0;
    double upper_bound = 0.0;
    double level = 0.0;
    std::vector<FieldIsosurfaceTriangle> triangles;

    [[nodiscard]] core::Result<void> validate() const;
};

struct FieldVolumePixel {
    std::vector<double> source_values;
    double source_lower = 0.0;
    double source_upper = 0.0;
    std::uint32_t sample_count = 0U;
    // Premultiplied RGBA accumulated front-to-back along the normalized z ray.
    std::array<double, 4> rgba{0.0, 0.0, 0.0, 0.0};
};

// Derived CPU-reference front-to-back volume projection. Rays use nearest
// samples on a bounded regular volume and emit a normalized 2D image. This
// is inspection data only; it is not a rendered frame, persisted result, or
// provider/solver execution.
struct FieldVolumePreview {
    VisualizationId visualization;
    FieldId field;
    std::string mode;
    std::string palette;
    std::uint32_t volume_width = 0U;
    std::uint32_t volume_height = 0U;
    std::uint32_t volume_depth = 0U;
    std::uint32_t output_width = 0U;
    std::uint32_t output_height = 0U;
    double lower_bound = 0.0;
    double upper_bound = 0.0;
    double opacity_scale = 1.0;
    std::uint64_t clipped_low = 0U;
    std::uint64_t clipped_high = 0U;
    std::vector<FieldVolumePixel> pixels;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived CPU-reference output for a single field probe readout. The value,
// position, and color are inspection data only; they are not a solver result,
// persisted field sample, or renderer output.
struct FieldProbePreview {
    VisualizationId visualization;
    FieldId field;
    ProbeId probe;
    std::string mode;
    std::string palette;
    core::Vec3d position;
    double source_value = 0.0;
    double lower_bound = 0.0;
    double upper_bound = 0.0;
    double normalized_value = 0.5;
    std::array<double, 4> rgba{0.0, 0.0, 0.0, 1.0};
    bool clipped_low = false;
    bool clipped_high = false;

    [[nodiscard]] core::Result<void> validate() const;
};

struct FieldStreamlinePoint {
    core::Vec3d position;
    core::Vec3d vector;
    double magnitude = 0.0;
    double normalized_magnitude = 0.5;
    std::array<double, 4> rgba{0.0, 0.0, 0.0, 1.0};
};

struct FieldStreamline {
    std::vector<FieldStreamlinePoint> points;
    bool terminated_by_boundary = false;
};

// Derived CPU-reference output for bounded two-dimensional vector-field
// streamlines. The input grid and integrated paths are inspection data only;
// they are not solver trajectories, persisted geometry, or renderer output.
struct FieldStreamlinePreview {
    VisualizationId visualization;
    FieldId field;
    std::string mode;
    std::string palette;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    double lower_bound = 0.0;
    double upper_bound = 0.0;
    double step_size = 0.0;
    std::uint32_t max_steps = 0U;
    std::uint64_t clipped_low = 0U;
    std::uint64_t clipped_high = 0U;
    std::vector<FieldStreamline> lines;

    [[nodiscard]] core::Result<void> validate() const;
};

struct FieldVectorGlyphPreview {
    VisualizationId visualization;
    FieldId field;
    std::string mode;
    std::string palette;
    double lower_bound = 0.0;
    double upper_bound = 0.0;
    double glyph_scale = 1.0;
    std::uint64_t clipped_low = 0U;
    std::uint64_t clipped_high = 0U;
    std::uint64_t zero_vectors = 0U;
    std::vector<FieldVectorGlyph> glyphs;

    [[nodiscard]] core::Result<void> validate() const;
};

struct FieldVisualization {
    VisualizationId id;
    FieldId field;
    std::optional<RegionId> domain;
    std::string mode;
    std::string palette;
    std::optional<double> lower_bound;
    std::optional<double> upper_bound;
    std::string stage;
    bool show_legend = true;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
    // CPU-only, derived value-to-color projection for inspection and reference
    // testing. It consumes caller-provided samples and never persists pixels or
    // claims native renderer execution.
    [[nodiscard]] core::Result<FieldVisualizationPreview> preview_samples(
        std::span<const double> values) const;
    // CPU-only, derived slice-plane projection for scalar inspection. The
    // input is a row-major rectangular sample grid and the frame must be a
    // finite right-handed orthonormal basis.
    [[nodiscard]] core::Result<FieldSlicePreview> preview_slice(
        std::span<const double> values,
        std::uint32_t width,
        std::uint32_t height,
        core::Vec3d origin,
        core::Vec3d normal,
        core::Vec3d horizontal_axis,
        core::Vec3d vertical_axis) const;
    // CPU-only, derived marching-squares contour projection for scalar
    // inspection. It emits bounded line segments in normalized grid space.
    [[nodiscard]] core::Result<FieldContourPreview> preview_contours(
        std::span<const double> values,
        std::uint32_t width,
        std::uint32_t height,
        std::span<const double> levels) const;
    // CPU-only, derived marching-tetrahedra isosurface projection for scalar
    // inspection. The input is a row-major regular volume and output
    // positions are normalized to the volume domain.
    [[nodiscard]] core::Result<FieldIsosurfacePreview> preview_isosurface(
        std::span<const double> values,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t depth,
        double level) const;
    // CPU-only, derived front-to-back volume projection for scalar
    // inspection. Rays use nearest regular-volume samples and emit
    // premultiplied RGBA pixels for the bounded output grid.
    [[nodiscard]] core::Result<FieldVolumePreview> preview_volume(
        std::span<const double> values,
        std::uint32_t volume_width,
        std::uint32_t volume_height,
        std::uint32_t volume_depth,
        std::uint32_t output_width,
        std::uint32_t output_height,
        double opacity_scale = 1.0) const;
    // CPU-only, derived single-probe readout for scalar inspection. The
    // caller supplies the sampled position and value; this method never
    // executes a provider or persists a sample.
    [[nodiscard]] core::Result<FieldProbePreview> preview_probe(
        ProbeId probe,
        core::Vec3d position,
        double value) const;
    // CPU-only, derived bounded streamline projection for a two-dimensional
    // regular vector grid and normalized seed positions. Integration is a
    // deterministic inspection aid; it never executes a solver or persists
    // paths.
    [[nodiscard]] core::Result<FieldStreamlinePreview> preview_streamlines(
        std::span<const FieldVectorSample> samples,
        std::uint32_t width,
        std::uint32_t height,
        std::span<const core::Vec3d> seeds,
        double step_size = 0.05,
        std::uint32_t max_steps = 128U) const;
    // CPU-only, derived vector-to-glyph projection for inspection and
    // reference testing. It consumes caller-provided positions/vectors and
    // never persists glyphs or claims native renderer execution.
    [[nodiscard]] core::Result<FieldVectorGlyphPreview> preview_vector_glyphs(
        std::span<const FieldVectorSample> samples,
        double glyph_scale = 1.0) const;
};

class FieldVisualizationModel {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;

    std::string schema;

    [[nodiscard]] core::Result<void> insert_visualization(FieldVisualization value);
    [[nodiscard]] const std::map<VisualizationId, FieldVisualization>& visualizations() const noexcept {
        return visualizations_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<FieldVisualizationModel> deserialize(std::string_view text);

private:
    std::map<VisualizationId, FieldVisualization> visualizations_;
};

struct ScientificImportDisposition {
    std::string source_key;
    std::string disposition;
    std::string target;
    std::string note;
};

struct ScientificImportReceipt {
    ImportId id;
    std::string format;
    std::string source_reference;
    std::string provider;
    std::optional<assets::Sha256Digest> source_digest;
    std::uint64_t records_considered = 0U;
    std::uint64_t records_admitted = 0U;
    std::vector<std::string> warnings;
    std::vector<std::string> feature_loss;
    Provenance provenance;
    // Provider adapters must account for every source-side disposition rather
    // than silently dropping or reinterpreting records. Empty reports remain
    // readable for legacy receipts; new adapters should emit one entry per
    // accounted source key.
    std::vector<ScientificImportDisposition> preservation_report;

    [[nodiscard]] core::Result<void> validate() const;
};

class ScientificImportLedger {
public:
    static constexpr std::uint32_t kSchemaVersion = 2U;

    [[nodiscard]] core::Result<void> insert_receipt(ScientificImportReceipt value);
    [[nodiscard]] const std::map<ImportId, ScientificImportReceipt>& receipts() const noexcept {
        return receipts_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<ScientificImportLedger> deserialize(std::string_view text);

private:
    std::map<ImportId, ScientificImportReceipt> receipts_;
};

// Provider-neutral semantic handoff for a functional genome. The receipt
// identifies the external source and accounts for source-side preservation;
// semantic_digest binds the canonical Cartographer payload. This is an
// adapter boundary only: it does not execute or admit MENAGERIE/VANTA state.
class FunctionalGenomeExchange {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;

    FunctionalGenome genome;
    ScientificImportReceipt import_receipt;
    assets::Sha256Digest semantic_digest;

    [[nodiscard]] static core::Result<FunctionalGenomeExchange> from_import(
        FunctionalGenome genome,
        ScientificImportReceipt import_receipt);
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<FunctionalGenomeExchange> deserialize(
        std::string_view text);
};

struct PhenotypeCapability {
    CapabilityId id;
    std::string name;
    std::string category;
    std::string state;
    std::optional<double> value;
    std::string units;
    std::vector<StructureId> contributing_structures;
    std::vector<GeneId> contributing_genes;
    std::vector<InstructionId> contributing_instructions;
    std::vector<TraceId> contributing_traces;
    std::string derivation;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

class PhenotypeCapabilityModel {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;

    std::string schema;
    std::string compilation_reference;
    std::string status;
    std::optional<assets::Sha256Digest> source_genome_digest;
    std::optional<assets::Sha256Digest> source_morphology_digest;
    std::optional<assets::Sha256Digest> source_development_digest;
    Provenance provenance;

    [[nodiscard]] core::Result<void> insert_capability(PhenotypeCapability value);
    [[nodiscard]] const std::map<CapabilityId, PhenotypeCapability>& capabilities() const noexcept {
        return capabilities_;
    }
    // Derived, read-only trace queries over provider-produced capability
    // receipts. These do not compile phenotype or grant provider authority.
    [[nodiscard]] core::Result<PhenotypeCapability> trace(CapabilityId id) const;
    [[nodiscard]] core::Result<std::vector<CapabilityId>> capabilities_for_structure(
        StructureId id) const;
    [[nodiscard]] core::Result<std::vector<CapabilityId>> capabilities_for_gene(
        GeneId id) const;
    [[nodiscard]] core::Result<std::vector<CapabilityId>> capabilities_for_instruction(
        InstructionId id) const;
    [[nodiscard]] core::Result<std::vector<CapabilityId>> capabilities_for_trace(
        TraceId id) const;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<PhenotypeCapabilityModel> deserialize(std::string_view text);

private:
    std::map<CapabilityId, PhenotypeCapability> capabilities_;
};

struct RegulatoryElement {
    RegulatoryElementId id;
    GeneId gene;
    std::string name;
    std::string kind;
    std::string source_reference;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct RegulatoryEdge {
    RegulatoryEdgeId id;
    GeneId source_gene;
    GeneId target_gene;
    RegulatoryElementId element;
    std::string mode;
    std::string condition;
    std::optional<StageId> stage;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SpatialExpression {
    ExpressionId id;
    GeneId gene;
    std::optional<StructureId> structure;
    std::optional<RegionId> region;
    std::optional<FieldId> field;
    std::optional<StageId> stage;
    std::string mode;
    std::optional<double> value;
    std::string categorical_state;
    std::string units;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

class GeneRegulatoryNetwork {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;

    std::string schema;
    std::string reference;
    std::string status;
    std::optional<assets::Sha256Digest> source_digest;
    Provenance provenance;

    [[nodiscard]] core::Result<void> insert_element(RegulatoryElement value);
    [[nodiscard]] core::Result<void> insert_edge(RegulatoryEdge value);
    [[nodiscard]] core::Result<void> insert_expression(SpatialExpression value);
    [[nodiscard]] const std::map<RegulatoryElementId, RegulatoryElement>& elements() const noexcept {
        return elements_;
    }
    [[nodiscard]] const std::map<RegulatoryEdgeId, RegulatoryEdge>& edges() const noexcept {
        return edges_;
    }
    [[nodiscard]] const std::map<ExpressionId, SpatialExpression>& expressions() const noexcept {
        return expressions_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<GeneRegulatoryNetwork> deserialize(std::string_view text);

private:
    std::map<RegulatoryElementId, RegulatoryElement> elements_;
    std::map<RegulatoryEdgeId, RegulatoryEdge> edges_;
    std::map<ExpressionId, SpatialExpression> expressions_;
};

struct DevelopmentStage {
    StageId id;
    std::uint64_t ordinal = 0U;
    std::string name;
    std::string condition;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct DevelopmentInstruction {
    InstructionId id;
    StageId stage;
    std::string opcode;
    std::string module;
    std::string condition;
    std::vector<ParameterId> parameters;
    std::vector<StructureId> creates;
    std::vector<StructureId> modifies;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived, read-only source comparison for developmental programs. It exposes
// authored stage/instruction/module changes without executing MENAGERIE or
// treating a source diff as provider evidence.
struct DevelopmentalProgramDiff {
    std::vector<StageId> added_stages;
    std::vector<StageId> removed_stages;
    std::vector<StageId> changed_stages;
    std::vector<InstructionId> added_instructions;
    std::vector<InstructionId> removed_instructions;
    std::vector<InstructionId> changed_instructions;
    std::vector<std::string> added_modules;
    std::vector<std::string> removed_modules;
    std::vector<std::string> changed_modules;
    bool source_identity_changed = false;
};

class DevelopmentalProgram {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;

    std::string schema;
    std::string reference;
    std::string description;
    std::optional<assets::Sha256Digest> source_digest;

    [[nodiscard]] core::Result<void> insert_stage(DevelopmentStage value);
    [[nodiscard]] core::Result<void> insert_instruction(DevelopmentInstruction value);
    [[nodiscard]] const std::map<StageId, DevelopmentStage>& stages() const noexcept {
        return stages_;
    }
    [[nodiscard]] const std::map<InstructionId, DevelopmentInstruction>& instructions() const noexcept {
        return instructions_;
    }
    [[nodiscard]] DevelopmentalProgramDiff diff_against(
        const DevelopmentalProgram& parent) const;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<DevelopmentalProgram> deserialize(std::string_view text);

private:
    std::map<StageId, DevelopmentStage> stages_;
    std::map<InstructionId, DevelopmentInstruction> instructions_;
};

struct DevelopmentTraceEntry {
    TraceId id;
    StageId stage;
    InstructionId instruction;
    std::vector<StructureId> created;
    std::vector<StructureId> modified;
    std::string evidence;
    Provenance provenance;
    // Optional provider evidence retained by the authoring trace. These are
    // appended to the aggregate layout for source compatibility with v1
    // callers and are serialized by developmental-trace schema v2.
    std::optional<TissueRegionId> tissue_region;
    std::vector<double> parameter_values;
    // Optional evidence identity for the provider step. These fields are
    // source/result metadata only; they do not claim that Cartographer ran
    // the provider or solver.
    std::optional<std::uint64_t> seed;
    std::optional<assets::Sha256Digest> result_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

class DevelopmentalTrace {
public:
    static constexpr std::uint32_t kSchemaVersion = 3U;

    std::string program_reference;
    std::string genome_reference;
    // When present, these bind the evidence trace to the exact authored
    // program/genome source records admitted in the enclosing model.
    std::optional<assets::Sha256Digest> program_digest;
    std::optional<assets::Sha256Digest> genome_digest;

    [[nodiscard]] core::Result<void> insert_entry(DevelopmentTraceEntry value);
    [[nodiscard]] const std::map<TraceId, DevelopmentTraceEntry>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<DevelopmentalTrace> deserialize(std::string_view text);

private:
    std::map<TraceId, DevelopmentTraceEntry> entries_;
};

// Derived, read-only inspection data for a development timeline. It is not
// persisted as source truth and does not imply that a provider executed the
// authored program.
struct DevelopmentalTimelineStep {
    StageId stage;
    std::uint64_t ordinal = 0U;
    std::string stage_name;
    std::vector<InstructionId> instructions;
    std::vector<TraceId> trace_entries;
    std::vector<StructureId> planned_creates;
    std::vector<StructureId> planned_modifies;
    std::vector<StructureId> observed_creates;
    std::vector<StructureId> observed_modifies;
};

// Derived, read-only impact projection for instruction-focused inspection.
// It joins authored targets with matching evidence-bearing trace entries and
// does not imply that the instruction executed.
struct DevelopmentalInstructionImpact {
    InstructionId instruction;
    StageId stage;
    std::vector<StructureId> planned_creates;
    std::vector<StructureId> planned_modifies;
    std::vector<TraceId> trace_entries;
    std::vector<StructureId> observed_creates;
    std::vector<StructureId> observed_modifies;

    [[nodiscard]] core::Result<void> validate() const;
};

struct LineageNode {
    LineageId id;
    std::optional<LineageId> parent_a;
    std::optional<LineageId> parent_b;
    std::uint64_t generation = 0U;
    std::optional<assets::Sha256Digest> genome_digest;
    std::optional<assets::Sha256Digest> morphology_digest;
    Provenance provenance;
    // External lineage labels are descriptive identity, not an authority
    // boundary. Empty means the node has no provider label.
    std::string source_identifier;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derived Newick export. Newick can represent a rooted single-parent tree,
// but it cannot represent Cartographer's reticulation, receipts, per-node
// provenance, or genome/morphology bindings. Those boundaries are reported
// explicitly and never inferred on import.
struct LineageGraphNewickExport {
    std::string text;
    std::vector<std::string> feature_loss_notes;
};

struct MutationReceipt {
    MutationId id;
    LineageId parent_lineage;
    LineageId child_lineage;
    std::optional<assets::Sha256Digest> parent_genome_digest;
    std::optional<assets::Sha256Digest> child_genome_digest;
    std::uint64_t seed = 0U;
    std::string operator_name;
    std::vector<GeneId> changed_genes;
    std::vector<InstructionId> changed_instructions;
    std::vector<std::string> changed_neural_values;
    std::string outcome;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

struct RecombinationReceipt {
    RecombinationId id;
    LineageId parent_a;
    LineageId parent_b;
    LineageId child_lineage;
    std::optional<assets::Sha256Digest> child_genome_digest;
    std::vector<std::string> module_origins;
    std::optional<MutationId> post_mutation;
    std::string outcome;
    Provenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

class LineageGraph {
public:
    static constexpr std::uint32_t kSchemaVersion = 3U;

    [[nodiscard]] core::Result<void> insert_node(LineageNode value);
    [[nodiscard]] core::Result<void> insert_mutation(MutationReceipt value);
    [[nodiscard]] core::Result<void> insert_recombination(RecombinationReceipt value);
    [[nodiscard]] const std::map<LineageId, LineageNode>& nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] const std::map<MutationId, MutationReceipt>& mutations() const noexcept {
        return mutations_;
    }
    [[nodiscard]] const std::map<RecombinationId, RecombinationReceipt>& recombinations() const noexcept {
        return recombinations_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<LineageGraph> deserialize(std::string_view text);
    [[nodiscard]] core::Result<LineageGraphNewickExport> export_newick() const;
    [[nodiscard]] static core::Result<LineageGraph> import_newick(
        std::string_view text,
        Provenance provenance);

private:
    std::map<LineageId, LineageNode> nodes_;
    std::map<MutationId, MutationReceipt> mutations_;
    std::map<RecombinationId, RecombinationReceipt> recombinations_;
};

class ScientificModel {
public:
    static constexpr std::uint32_t kSchemaVersion = 12U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;

    [[nodiscard]] core::Result<void> insert_region(Region value);
    [[nodiscard]] core::Result<void> insert_boundary(Boundary value);
    [[nodiscard]] core::Result<void> insert_interface(Interface value);
    [[nodiscard]] core::Result<void> insert_source(Source value);
    [[nodiscard]] core::Result<void> insert_load(Load value);
    [[nodiscard]] core::Result<void> insert_parameter(Parameter value);
    [[nodiscard]] core::Result<void> insert_field(Field value);
    [[nodiscard]] core::Result<void> insert_probe(Probe value);
    [[nodiscard]] core::Result<void> insert_study(Study value);
    [[nodiscard]] core::Result<void> insert_result_set(ResultSet value);
    // Builds a deterministic, revision-bound Cartesian sweep plan from the
    // authored study parameters. This is a planning preview, not execution.
    [[nodiscard]] core::Result<ParameterSweepPreview> preview_parameter_sweep(
        StudyId study,
        std::vector<ParameterSweepAxis> axes) const;
    [[nodiscard]] core::Result<void> set_morphology(MorphologyModel value);
    [[nodiscard]] core::Result<void> set_functional_genome(FunctionalGenome value);
    [[nodiscard]] core::Result<void> set_developmental_program(DevelopmentalProgram value);
    [[nodiscard]] core::Result<void> set_developmental_trace(DevelopmentalTrace value);
    [[nodiscard]] core::Result<void> set_lineage_graph(LineageGraph value);
    [[nodiscard]] core::Result<void> set_morphometrics(MorphometricModel value);
    [[nodiscard]] core::Result<void> set_sequence_genome(SequenceGenome value);
    [[nodiscard]] core::Result<void> set_anatomy(AnatomyModel value);
    [[nodiscard]] core::Result<void> set_field_visualizations(FieldVisualizationModel value);
    [[nodiscard]] core::Result<void> set_import_ledger(ScientificImportLedger value);
    [[nodiscard]] core::Result<void> set_phenotype_capabilities(PhenotypeCapabilityModel value);
    [[nodiscard]] core::Result<void> set_gene_regulatory_network(GeneRegulatoryNetwork value);
    [[nodiscard]] core::Result<void> set_anatomy_provider_manifest(AnatomyProviderManifest value);
    [[nodiscard]] core::Result<void> set_replay_capsules(ScientificReplayCapsuleModel value);

    [[nodiscard]] const std::map<RegionId, Region>& regions() const noexcept { return regions_; }
    [[nodiscard]] const std::map<BoundaryId, Boundary>& boundaries() const noexcept { return boundaries_; }
    [[nodiscard]] const std::map<InterfaceId, Interface>& interfaces() const noexcept { return interfaces_; }
    [[nodiscard]] const std::map<SourceId, Source>& sources() const noexcept { return sources_; }
    [[nodiscard]] const std::map<LoadId, Load>& loads() const noexcept { return loads_; }
    [[nodiscard]] const std::map<ParameterId, Parameter>& parameters() const noexcept { return parameters_; }
    [[nodiscard]] const std::map<FieldId, Field>& fields() const noexcept { return fields_; }
    [[nodiscard]] const std::map<ProbeId, Probe>& probes() const noexcept { return probes_; }
    [[nodiscard]] const std::map<StudyId, Study>& studies() const noexcept { return studies_; }
    [[nodiscard]] const std::map<ResultSetId, ResultSet>& result_sets() const noexcept {
        return result_sets_;
    }
    [[nodiscard]] const std::optional<MorphologyModel>& morphology() const noexcept {
        return morphology_;
    }
    [[nodiscard]] const std::optional<FunctionalGenome>& functional_genome() const noexcept {
        return functional_genome_;
    }
    [[nodiscard]] const std::optional<DevelopmentalProgram>& developmental_program() const noexcept {
        return developmental_program_;
    }
    [[nodiscard]] const std::optional<DevelopmentalTrace>& developmental_trace() const noexcept {
        return developmental_trace_;
    }
    [[nodiscard]] const std::optional<LineageGraph>& lineage_graph() const noexcept {
        return lineage_graph_;
    }
    [[nodiscard]] const std::optional<MorphometricModel>& morphometrics() const noexcept {
        return morphometrics_;
    }
    [[nodiscard]] const std::optional<SequenceGenome>& sequence_genome() const noexcept {
        return sequence_genome_;
    }
    [[nodiscard]] const std::optional<AnatomyModel>& anatomy() const noexcept {
        return anatomy_;
    }
    [[nodiscard]] const std::optional<FieldVisualizationModel>& field_visualizations() const noexcept {
        return field_visualizations_;
    }
    [[nodiscard]] const std::optional<ScientificImportLedger>& import_ledger() const noexcept {
        return import_ledger_;
    }
    [[nodiscard]] const std::optional<PhenotypeCapabilityModel>& phenotype_capabilities() const noexcept {
        return phenotype_capabilities_;
    }
    [[nodiscard]] const std::optional<GeneRegulatoryNetwork>& gene_regulatory_network() const noexcept {
        return gene_regulatory_network_;
    }
    [[nodiscard]] const std::optional<AnatomyProviderManifest>& anatomy_provider_manifest() const noexcept {
        return anatomy_provider_manifest_;
    }
    [[nodiscard]] const std::optional<ScientificReplayCapsuleModel>& replay_capsules() const noexcept {
        return replay_capsules_;
    }

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> validate_anatomy_provider_candidate(
        core::Revision expected_source_revision) const;
    // Builds a candidate only after validating that its bindings belong to
    // this scientific model at the supplied source revision.
    [[nodiscard]] core::Result<AnatomyProviderCandidate> build_anatomy_provider_candidate(
        core::Revision expected_source_revision) const;
    // Builds a deterministic stage-ordered inspection projection. An empty
    // model without a developmental program returns an empty projection.
    [[nodiscard]] core::Result<std::vector<DevelopmentalTimelineStep>>
    build_developmental_timeline() const;
    [[nodiscard]] core::Result<DevelopmentalInstructionImpact>
    build_developmental_instruction_impact(InstructionId instruction) const;
    // Builds a deterministic, derived morphospace from explicit
    // lineage-to-signature bindings. Supported normalization is "raw" or
    // "unit-box"; the latter independently scales the leading components to
    // [0,1]. This is a CPU authoring preview, not provider execution or a
    // persisted statistical model.
    [[nodiscard]] core::Result<EvolutionaryMorphospacePreview>
    preview_evolutionary_morphospace(
        std::span<const EvolutionaryMorphospaceBinding> bindings,
        std::uint32_t dimensions = 2U,
        std::string_view normalization = "unit-box") const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<ScientificModel> deserialize(std::string_view text);

private:
    std::map<RegionId, Region> regions_;
    std::map<BoundaryId, Boundary> boundaries_;
    std::map<InterfaceId, Interface> interfaces_;
    std::map<SourceId, Source> sources_;
    std::map<LoadId, Load> loads_;
    std::map<ParameterId, Parameter> parameters_;
    std::map<FieldId, Field> fields_;
    std::map<ProbeId, Probe> probes_;
    std::map<StudyId, Study> studies_;
    std::map<ResultSetId, ResultSet> result_sets_;
    std::optional<MorphologyModel> morphology_;
    std::optional<FunctionalGenome> functional_genome_;
    std::optional<DevelopmentalProgram> developmental_program_;
    std::optional<DevelopmentalTrace> developmental_trace_;
    std::optional<LineageGraph> lineage_graph_;
    std::optional<MorphometricModel> morphometrics_;
    std::optional<SequenceGenome> sequence_genome_;
    std::optional<AnatomyModel> anatomy_;
    std::optional<FieldVisualizationModel> field_visualizations_;
    std::optional<ScientificImportLedger> import_ledger_;
    std::optional<PhenotypeCapabilityModel> phenotype_capabilities_;
    std::optional<GeneRegulatoryNetwork> gene_regulatory_network_;
    std::optional<AnatomyProviderManifest> anatomy_provider_manifest_;
    std::optional<ScientificReplayCapsuleModel> replay_capsules_;
};

} // namespace carto::scientific
