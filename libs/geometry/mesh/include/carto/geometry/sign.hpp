#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::geometry {

inline constexpr std::uint32_t kSignTextPanelMaterialSlot = 1U;
inline constexpr std::uint32_t kSignTextLetteringMaterialSlot = 2U;

enum class SignTextAlignment : std::uint8_t {
    left = 0U,
    center = 1U,
    right = 2U,
};

struct SignTextSource final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxIdentityBytes = 128U;
    static constexpr std::size_t kMaxTextBytes = 256U;
    static constexpr std::size_t kMaxLines = 4U;
    static constexpr std::size_t kMaxColumns = 128U;
    static constexpr std::size_t kMaxGlyphDetails = 256U;
    static constexpr std::size_t kMaxPixelBlocks = 16'384U;

    std::string source_identity;
    core::Revision source_revision;
    std::string text;
    SignTextAlignment alignment = SignTextAlignment::center;
    double board_width = 4.0;
    double board_height = 1.6;
    double board_depth = 0.08;
    double border = 0.12;
    double glyph_height = 0.6;
    double glyph_depth = 0.035;
    double tracking = 0.04;
    double line_spacing = 0.08;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<SignTextSource> deserialize(
        std::string_view text);
};

// Stable identity is positional within a sign source: changing the displayed
// character preserves the detail identity at that text position.
struct SignGlyphDetail final {
    std::uint64_t stable_id = 0U;
    std::uint64_t word_stable_id = 0U;
    std::size_t text_byte_offset = 0U;
    char character = ' ';
    std::uint32_t line = 0U;
    std::uint32_t column = 0U;
    std::uint32_t word_index = 0U;
    std::uint64_t first_face_id = 0U;
    std::size_t face_count = 0U;
    core::Vec3d center;
};

// Panel and raised lettering remain separate meshes so downstream material
// authoring can assign independent surface roles without reconstructing text.
// The panel lies in X/Z and faces +Y in Cartographer's Y-up authoring frame.
struct SignTextProduct final {
    std::string source_identity;
    core::Revision source_revision;
    EditableMesh panel;
    EditableMesh lettering;
    std::vector<SignGlyphDetail> glyphs;
};

[[nodiscard]] core::Result<SignTextProduct> build_sign_text(
    const SignTextSource& source);
[[nodiscard]] core::Result<EditableMesh> combine_sign_text(
    const SignTextProduct& product);

} // namespace carto::geometry
