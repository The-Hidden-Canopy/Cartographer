#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/surface/core.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace carto::surface {

struct EvidenceCaseId {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const EvidenceCaseId&) const noexcept = default;
};

struct SurfaceDependencyId {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const SurfaceDependencyId&) const noexcept = default;
};

struct SurfaceRegion {
    std::uint32_t id = 0U;
    core::Vec3d direction{0.0, 0.0, 1.0};
    double spread = 0.0;
    std::uint32_t sample_count = 1U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

using ViewRegion = SurfaceRegion;
using LightRegion = SurfaceRegion;

struct SurfaceEvidenceCase {
    EvidenceCaseId id;
    SurfaceDomainId domain;
    RepresentationId representation;
    ViewRegion view_region;
    LightRegion light_region;
    SurfaceErrorEnvelope measured_error;
    std::vector<SurfaceDependencyId> dependencies;
    double diagnostic_weight = 1.0;
    core::Revision source_revision;
    std::string source_content_digest;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

struct SurfaceEvidenceQuery {
    SurfaceDomainId domain;
    RepresentationId representation;
    ViewRegion view_region;
    LightRegion light_region;
    core::Revision source_revision;
    std::string source_content_digest;
    std::vector<SurfaceDependencyId> dependencies;

    [[nodiscard]] core::Result<void> validate() const;
};

class SurfaceEvidenceReservoir final {
public:
    static constexpr std::size_t max_cases = 4096U;

    // Records are append-only. Query returns the latest record for an
    // identity, and never falls back to an older stale record.
    [[nodiscard]] core::Result<void> append(SurfaceEvidenceCase evidence);
    [[nodiscard]] core::Result<std::optional<SurfaceEvidenceCase>> query(
        const SurfaceEvidenceQuery& query) const;
    [[nodiscard]] core::Result<void> validate() const;

    [[nodiscard]] const std::vector<SurfaceEvidenceCase>& records() const noexcept {
        return records_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

private:
    std::vector<SurfaceEvidenceCase> records_;
};

} // namespace carto::surface
