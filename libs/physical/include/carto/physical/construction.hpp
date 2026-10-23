#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/physical/material_intent.hpp>
#include <carto/physical/types.hpp>
#include <carto/surface/process.hpp>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace carto::physical {

enum class ConstructionKind : std::uint8_t {
    solid,
    shell,
    layered,
    filled,
};

enum class ThicknessSource : std::uint8_t {
    authored,
    derived_from_geometry,
};

[[nodiscard]] std::string_view construction_kind_name(ConstructionKind kind) noexcept;
[[nodiscard]] std::string_view thickness_source_name(ThicknessSource source) noexcept;

// One construction layer: a physical thickness plus the material intent that
// fills it. Layer order matches the authoring stratigraphy outermost first.
struct ConstructionLayer {
    double nominal_thickness = 0.0;
    PhysicalMaterialIntent material;

    [[nodiscard]] core::Result<void> validate() const;
};

// Provider-neutral construction profile: how matter is distributed inside the
// authored mesh volume. It references downstream consumer catalog identity through intents
// and references authoring stratigraphy through digest-checked surface
// records; it never embeds catalog constants or runtime handles.
struct ConstructionProfile {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t kMaxLayers = 64U;
    static constexpr double kMaxWallThicknessMeters = 1.0e3;

    ConstructionKind kind = ConstructionKind::solid;
    core::Revision source_revision;
    // solid: fills the whole enclosed volume.
    // shell: wall material forming a thin wall of wall_thickness.
    // layered: ordered layers from the outside in.
    // filled: wall of wall_thickness around an interior filled with
    //   fill_material.
    PhysicalMaterialIntent primary_material;
    std::optional<double> wall_thickness;
    std::optional<ThicknessSource> wall_thickness_source;
    std::vector<ConstructionLayer> layers;
    std::optional<PhysicalMaterialIntent> fill_material;
    // Layered constructions bind the authoring truth: a copy of the surface
    // stratigraphy this profile was derived from. The layer count and
    // thicknesses must correspond exactly, so drift fails validation.
    std::optional<surface::MaterialStratigraphy> stratigraphy;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<ConstructionProfile> deserialize(
        std::string_view text);
};

namespace detail {
void encode_construction(
    std::ostream& output, const ConstructionProfile& profile);
core::Result<void> decode_construction(
    std::istream& input, ConstructionProfile& profile);
} // namespace detail

} // namespace carto::physical
