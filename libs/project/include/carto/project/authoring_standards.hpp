#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::project {

enum class ProjectStandardCategory : std::uint8_t {
    interior_door_width,
    guest_room_wall_height,
    stair_rise,
    stair_run,
    bevel_width,
    texel_density,
    lightmap_density,
    grid_increment,
    material_family,
    naming_policy,
    collision_profile,
    common_socket,
    unit_convention,
};

struct ProjectStandardMeasurementEvidence {
    std::uint64_t object_id = 0U;
    std::uint64_t mesh_asset = 0U;
    core::Revision mesh_revision;
    double measured_value_meters = 0.0;

    [[nodiscard]] bool operator==(const ProjectStandardMeasurementEvidence&) const noexcept =
        default;
};

struct ProjectStandardEvidence {
    static constexpr double kMaximumToleranceMeters = 0.25;

    core::Revision source_revision;
    double tolerance_meters = 0.0;
    std::vector<ProjectStandardMeasurementEvidence> measurements;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate(ProjectStandardCategory category) const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest(
        ProjectStandardCategory category) const;
    [[nodiscard]] bool operator==(const ProjectStandardEvidence&) const noexcept = default;
};

// A standard is project source data. Numeric categories use a positive scalar
// and a category-specific unit; text categories preserve an explicit value.
// Presence in ProjectAuthoringStandards means a human approved it. Optional
// measurement evidence records the exact source scope for an approved proposal.
struct ProjectAuthoringStandard {
    std::string id;
    std::string label;
    ProjectStandardCategory category = ProjectStandardCategory::naming_policy;
    std::string value;
    std::string unit;
    std::string rationale;
    std::optional<ProjectStandardEvidence> evidence;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const ProjectAuthoringStandard&) const noexcept = default;
};

class ProjectAuthoringStandards final {
public:
    static constexpr std::uint32_t kSchemaVersion = 2U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;
    static constexpr std::size_t kMaxRecords = 512U;

    [[nodiscard]] core::Result<void> set(ProjectAuthoringStandard standard);
    [[nodiscard]] core::Result<void> erase(std::string_view id);
    [[nodiscard]] const std::map<std::string, ProjectAuthoringStandard>& records() const noexcept {
        return records_;
    }
    [[nodiscard]] const ProjectAuthoringStandard* find(std::string_view id) const;
    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<ProjectAuthoringStandards> deserialize(
        std::string_view text);

private:
    std::map<std::string, ProjectAuthoringStandard> records_;
};

} // namespace carto::project
