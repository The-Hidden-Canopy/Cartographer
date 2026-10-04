#pragma once

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/ids.hpp>
#include <carto/surface/core.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace carto::surface {

using MaterialId = std::uint64_t;

struct MaterialLayer {
    MaterialId material = 0U;
    double nominal_thickness = 0.0;
    double variation = 0.0;
    std::string process_class;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MaterialStratigraphy {
    SurfaceDomainId domain;
    core::Revision source_revision;
    std::vector<MaterialLayer> layers;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

struct MaterialExposure {
    MaterialId material = 0U;
    std::uint32_t layer_index = 0U;
    double depth_into_layer = 0.0;
    bool reaches_substrate = false;

    [[nodiscard]] core::Result<void> validate() const;
};

[[nodiscard]] core::Result<MaterialExposure> resolve_material_exposure(
    const MaterialStratigraphy& stratigraphy,
    double damage_depth);

struct WeatheringControls {
    double age = 0.0;
    double rain_exposure = 0.0;
    double solar_exposure = 0.0;
    double moisture = 0.0;
    double traffic = 0.0;
    double maintenance = 0.0;
    std::uint64_t seed = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct WeatheringState {
    SurfaceDomainId domain;
    core::Revision source_revision;
    double runoff = 0.0;
    double moisture_retention = 0.0;
    double solar_fade = 0.0;
    double abrasion = 0.0;
    double oxidation = 0.0;
    double biological_growth = 0.0;
    double contact_wear = 0.0;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

[[nodiscard]] core::Result<WeatheringState> derive_weathering(
    const SurfaceAnalysisRecord& analysis,
    const WeatheringControls& controls);

enum class ManufacturingProcess {
    generic,
    cut,
    mill,
    turn,
    grind,
    weld,
    cast,
    formwork,
    pour,
    stain,
    sanding,
    mold,
    parting,
};

struct ManufacturingStep {
    std::uint32_t sequence = 0U;
    ManufacturingProcess process = ManufacturingProcess::generic;
    core::Vec3d direction{1.0, 0.0, 0.0};
    double intensity = 0.0;
    std::string evidence;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ManufacturingHistory {
    SurfaceDomainId domain;
    core::Revision source_revision;
    std::vector<ManufacturingStep> steps;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

enum class ContactType {
    proximity,
    resting,
    penetration,
    fastened,
    sealed,
    welded,
    hinged,
    embedded,
};

struct SurfaceContact {
    SurfaceDomainId a;
    SurfaceDomainId b;
    ContactType type = ContactType::proximity;
    double proximity = 0.0;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

struct ContactGraph {
    core::Revision source_revision;
    std::vector<SurfaceContact> contacts;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

enum class EdgeProcess {
    generic,
    machined,
    cast,
    molded,
    folded_sheet,
    painted_corner,
    wood_cut,
    stone_fracture,
    weathered_concrete,
    weld_seam,
    organic_boundary,
};

enum class ProfileShape {
    circular,
    elliptical,
    chamfer,
    asymmetric,
    bead,
    fracture,
};

struct EdgeProfile {
    geometry::EdgeId edge;
    EdgeProcess process = EdgeProcess::generic;
    ProfileShape shape = ProfileShape::circular;
    double nominal_radius = 0.0;
    double radius_variance = 0.0;
    double wear = 0.0;
    double damage = 0.0;
    std::string material_transition;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MaterialGeometryPolicy {
    MaterialId material = 0U;
    double target_feature_scale = 0.0;
    double max_displacement = 0.0;
    std::uint32_t max_generated_faces = 0U;
    std::uint64_t seed = 0U;
    std::string generator_version = "cartographer-material-geometry/0.1";

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct MaterialGeometryIntent {
    SurfaceDomainId domain;
    geometry::FaceId source_face;
    core::Revision source_revision;
    MaterialGeometryPolicy policy;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

[[nodiscard]] core::Result<MaterialGeometryIntent> make_material_geometry_intent(
    SurfaceDomainId domain,
    geometry::FaceId source_face,
    core::Revision source_revision,
    const MaterialGeometryPolicy& policy);

} // namespace carto::surface
