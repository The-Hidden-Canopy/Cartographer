#pragma once

#include <carto/biological/authoring.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::biological {

// A read-only, indexed grayscale raster derived from an exact scalar sample
// blob. The strip has no spatial interpretation; it is explicitly a preview,
// never a replacement for authored field data.
struct DerivedFieldVisualization final {
    static constexpr std::size_t kMaxSamples = 1'048'576U;
    static constexpr std::string_view kLabel =
        "DERIVED FIELD PREVIEW - indexed scalar samples; not spatial truth";

    AuthoringSourceIdentity source;
    assets::Sha256Digest authoring_digest;
    FieldId field;
    assets::Sha256Digest sample_blob_digest;
    std::uint32_t width = 0U;
    std::uint32_t height = 1U;
    std::string label;
    std::vector<std::uint8_t> rgba;
    assets::Sha256Digest visualization_digest;

    [[nodiscard]] core::Result<void> validate_against(
        const BiologicalAuthoringDocument& document,
        std::span<const std::uint8_t> sample_blob) const;
};

// Sample blob schema: 8-byte magic "CBF32LE1", uint64 little-endian sample
// count, then exactly sample_count IEEE-754 float32 little-endian values.
// The blob must match FieldRecord::data_digest and declared value bounds.
// Only scalar statistical-shading fields are supported by this preview.
[[nodiscard]] core::Result<DerivedFieldVisualization>
render_scalar_field_preview(
    const BiologicalAuthoringDocument& document,
    FieldId field,
    std::span<const std::uint8_t> sample_blob,
    std::size_t pixel_budget = DerivedFieldVisualization::kMaxSamples);

} // namespace carto::biological
