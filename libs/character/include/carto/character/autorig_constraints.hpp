#pragma once

#include <carto/character/autorig_analysis.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/core/math.hpp>
#include <carto/geometry/ids.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace carto::character {

enum class AutorigPartSide : std::uint8_t {
    unspecified = 0U,
    center = 1U,
    left = 2U,
    right = 3U,
};

enum class AutorigConstraintOrigin : std::uint8_t {
    human_authored = 0U,
    imported_reference = 1U,
};

struct AutorigConstraintProvenance final {
    std::string assertion_id;
    std::string author_id;
    std::string rationale;
    std::string evidence_reference;
    AutorigConstraintOrigin origin = AutorigConstraintOrigin::human_authored;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AutorigPartonomyNode final {
    std::string id;
    std::string semantic_role;
    std::optional<std::string> parent_id;
    AutorigPartSide side = AutorigPartSide::unspecified;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AutorigLandmarkConstraint final {
    std::string id;
    std::string part_id;
    std::string semantic_role;
    AutorigPartSide side = AutorigPartSide::unspecified;
    geometry::VertexId vertex;
    AutorigConstraintProvenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

// An authored mesh-vertex region that limits which source anchors may be used
// for one part. It constrains candidate admission; it does not predict joints
// between vertices or infer anatomy from geometry.
struct AutorigAllowedRegionConstraint final {
    std::string id;
    std::string part_id;
    std::vector<geometry::VertexId> vertices;
    AutorigConstraintProvenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

// Authored rigid-link port evidence for mechanical or mixed sources. Ports
// must resolve to vertices on distinct analyzed mesh components. The axis is
// mesh-local; limits remain in the caller's declared source-unit convention.
struct AutorigMechanicalPortHint final {
    std::string id;
    std::string parent_part_id;
    std::string child_part_id;
    std::string parent_landmark_id;
    std::string child_landmark_id;
    kinematic::JointKind kind = kinematic::JointKind::fixed;
    core::Vec3d axis{};
    double lower_limit = 0.0;
    double upper_limit = 0.0;
    AutorigConstraintProvenance provenance;

    [[nodiscard]] core::Result<void> validate() const;
};

// Explicit, provenance-bearing semantic input for later candidate proposals.
// Admission validates the exact analysis binding and refuses conflicting
// role/side assignments; it does not infer, resolve, or commit a skeleton.
struct AutorigConstraintSet final {
    static constexpr std::size_t kMaxParts = 4'096U;
    static constexpr std::size_t kMaxLandmarks = 8'192U;
    static constexpr std::size_t kMaxAllowedRegions = 4'096U;
    static constexpr std::size_t kMaxMechanicalPortHints = 8'192U;
    static constexpr std::size_t kMaxTotalAllowedRegionVertices =
        AutorigAnalysisBudget::kHardMaxVertices;

    MeshAssetRef source;
    core::Revision mesh_revision;
    assets::Sha256Digest analysis_digest;
    std::vector<AutorigPartonomyNode> parts;
    std::vector<AutorigLandmarkConstraint> landmarks;
    std::vector<AutorigAllowedRegionConstraint> allowed_regions;
    std::vector<AutorigMechanicalPortHint> mechanical_ports;

    [[nodiscard]] core::Result<void> validate_against(
        const AutorigFeatureAnalysis& analysis) const;
};

} // namespace carto::character
