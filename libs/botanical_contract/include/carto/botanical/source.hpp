#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace carto::botanical {

enum class Habit : std::uint8_t { tree = 0U, shrub = 1U, grass = 2U };
enum class LeafForm : std::uint8_t {
    needle = 0U, lanceolate = 1U, broad = 2U, strap = 3U, scale = 4U
};

// Portable input data for an explicitly requested private build. This
// contract contains no mesh generator or derived geometry implementation.
struct Recipe final {
    Habit habit = Habit::tree;
    LeafForm leaf_form = LeafForm::needle;
    std::uint64_t seed = 1U;
    core::Vec3d growth_axis{0.0, 1.0, 0.0};
    core::Vec3d reference_axis{1.0, 0.0, 0.0};
    double height = 8.0;
    double crown_radius = 2.5;
    double stem_radius = 0.16;
    std::uint32_t stem_count = 1U;
    std::uint32_t branch_levels = 5U;
    std::uint32_t branches_per_level = 4U;
    std::uint32_t secondary_branches = 2U;
    double crown_base = 0.18;
    double crown_tip = 0.96;
    double branch_pitch = 0.35;
    double branch_length = 0.78;
    double branch_taper = 0.14;
    std::uint32_t leaves_per_branch = 6U;
    double leaf_length = 0.28;
    double leaf_width = 0.06;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Source final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxIdentityBytes = 128U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U;

    std::string identity;
    core::Revision revision;
    Recipe recipe;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<Source> deserialize(std::string_view text);
};

} // namespace carto::botanical
