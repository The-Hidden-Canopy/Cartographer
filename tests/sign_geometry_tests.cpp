#include <carto/geometry/sign.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

carto::geometry::SignTextSource motel_sign() {
    carto::geometry::SignTextSource source;
    source.source_identity = "fixture.sign.motel8";
    source.source_revision = carto::core::Revision{1U};
    source.text = "MOTEL 8\nVACANCY";
    source.board_width = 4.0;
    source.board_height = 1.5;
    source.glyph_height = 0.5;
    source.line_spacing = 0.08;
    return source;
}

carto::geometry::SignTextSource price_board() {
    carto::geometry::SignTextSource source;
    source.source_identity = "fixture.sign.pine-ridge-prices";
    source.source_revision = carto::core::Revision{1U};
    source.text = "PINE RIDGE\nGAS $3.99\nDIESEL $4.29";
    source.board_width = 5.8;
    source.board_height = 2.0;
    source.glyph_height = 0.42;
    source.border = 0.15;
    source.line_spacing = 0.08;
    return source;
}

void motel_sign_source_and_mesh_are_deterministic() {
    const auto source = motel_sign();
    require(static_cast<bool>(source.validate()), "Motel sign source is invalid");
    const auto serialized = source.serialize();
    require(!serialized.empty(), "Motel sign source did not serialize");
    const auto reopened = carto::geometry::SignTextSource::deserialize(serialized);
    require(reopened && reopened.value().serialize() == serialized,
        "Motel sign source did not round-trip canonically");

    const auto product = carto::geometry::build_sign_text(source);
    const auto repeated = carto::geometry::build_sign_text(source);
    require(product && repeated, "Motel sign geometry generation failed");
    require(product.value().panel.validate() && product.value().lettering.validate(),
        "Motel sign produced invalid editable meshes");
    const auto panel_vertices = product.value().panel.vertices_sorted();
    const auto lettering_vertices = product.value().lettering.vertices_sorted();
    double panel_front_y = -std::numeric_limits<double>::infinity();
    double lettering_front_y = -std::numeric_limits<double>::infinity();
    double panel_min_z = std::numeric_limits<double>::infinity();
    double panel_max_z = -std::numeric_limits<double>::infinity();
    for (const auto& vertex : panel_vertices) {
        panel_front_y = std::max(panel_front_y, vertex.position.y);
        panel_min_z = std::min(panel_min_z, vertex.position.z);
        panel_max_z = std::max(panel_max_z, vertex.position.z);
    }
    for (const auto& vertex : lettering_vertices) {
        lettering_front_y = std::max(lettering_front_y, vertex.position.y);
    }
    require(std::abs(panel_front_y - source.board_depth * 0.5) < 1e-12 &&
        lettering_front_y > panel_front_y && panel_max_z > panel_min_z,
        "sign panel must lie face-on in Cartographer's X/Z plane with raised +Y lettering");
    require(product.value().glyphs.size() == 13U,
        "Motel sign did not preserve individual non-space glyph details");
    const auto& motel_glyphs = product.value().glyphs;
    require(motel_glyphs.front().word_index == 0U &&
        motel_glyphs[4U].word_index == 0U &&
        motel_glyphs[5U].word_index == 1U &&
        motel_glyphs[6U].word_index == 2U &&
        motel_glyphs.back().word_index == 2U,
        "sign word indices do not follow visible whitespace-delimited words");
    require(motel_glyphs[0U].word_stable_id == motel_glyphs[4U].word_stable_id &&
        motel_glyphs[4U].word_stable_id != motel_glyphs[5U].word_stable_id &&
        motel_glyphs[5U].word_stable_id != motel_glyphs[6U].word_stable_id &&
        motel_glyphs[6U].word_stable_id == motel_glyphs.back().word_stable_id,
        "sign glyphs do not retain deterministic word-level identity");
    std::uint64_t expected_first_face = 1U;
    std::size_t total_glyph_faces = 0U;
    for (const auto& glyph : motel_glyphs) {
        require(glyph.first_face_id == expected_first_face && glyph.face_count > 0U,
            "glyph face ranges must be nonempty, ordered, and contiguous");
        expected_first_face += static_cast<std::uint64_t>(glyph.face_count);
        total_glyph_faces += glyph.face_count;
    }
    require(total_glyph_faces == product.value().lettering.faces_sorted().size() &&
        expected_first_face == product.value().lettering.faces_sorted().size() + 1U,
        "glyph face ranges do not address every generated lettering face exactly once");
    require(product.value().glyphs.front().stable_id == repeated.value().glyphs.front().stable_id &&
        product.value().glyphs.back().stable_id == repeated.value().glyphs.back().stable_id,
        "Motel sign glyph identities are not deterministic");
    require(product.value().lettering.vertices_sorted().size() ==
            repeated.value().lettering.vertices_sorted().size() &&
        product.value().lettering.faces_sorted().size() ==
            repeated.value().lettering.faces_sorted().size(),
        "Motel sign topology changed between identical builds");
    const auto combined = carto::geometry::combine_sign_text(product.value());
    require(static_cast<bool>(combined), "Motel sign pieces did not combine");
    const auto* material_slots = combined.value().attributes().find(
        "cartographer.material_slot");
    require(material_slots != nullptr &&
        material_slots->descriptor.domain == carto::attributes::AttributeDomain::face &&
        material_slots->descriptor.type == carto::attributes::AttributeType::uint32 &&
        material_slots->values.size() == combined.value().face_count(),
        "combined sign does not carry a complete face-domain material-slot layer");
    const std::size_t panel_face_count = product.value().panel.face_count();
    require(panel_face_count > 0U && panel_face_count < material_slots->values.size(),
        "sign panel and lettering must have independent nonempty face ranges");
    for (std::size_t index = 0U; index < material_slots->values.size(); ++index) {
        const auto expected_slot = index < panel_face_count
            ? carto::geometry::kSignTextPanelMaterialSlot
            : carto::geometry::kSignTextLetteringMaterialSlot;
        require(std::get<std::uint32_t>(material_slots->values[index]) == expected_slot,
            "sign faces lost their stable panel-versus-lettering material slot");
    }
    const auto first_vertices = product.value().lettering.vertices_sorted();
    const auto repeated_vertices = repeated.value().lettering.vertices_sorted();
    for (std::size_t index = 0U; index < first_vertices.size(); ++index) {
        require(first_vertices[index].position.x == repeated_vertices[index].position.x &&
            first_vertices[index].position.y == repeated_vertices[index].position.y &&
            first_vertices[index].position.z == repeated_vertices[index].position.z,
            "Motel sign geometry is not bitwise deterministic");
    }

    auto revised = source;
    revised.source_revision = carto::core::Revision{2U};
    revised.text = "MOTEL 9\nVACANCY";
    const auto revised_product = carto::geometry::build_sign_text(revised);
    require(revised_product && revised_product.value().glyphs.size() == product.value().glyphs.size(),
        "single-character sign revision failed to generate");
    require(revised_product.value().glyphs[6U].stable_id == product.value().glyphs[6U].stable_id,
        "editing a character changed the stable identity for its text position");

    auto leading_space = source;
    leading_space.text = "  A B";
    const auto spaced_product = carto::geometry::build_sign_text(leading_space);
    require(spaced_product && spaced_product.value().glyphs.size() == 2U &&
        spaced_product.value().glyphs[0U].word_index == 0U &&
        spaced_product.value().glyphs[1U].word_index == 1U,
        "leading whitespace must not shift the first visible word index");
}

void price_board_layout_and_adversarial_bounds_are_enforced() {
    const auto source = price_board();
    const auto source_validation = source.validate();
    require(static_cast<bool>(source_validation), source_validation ? "" :
        source_validation.error().message);
    const auto product = carto::geometry::build_sign_text(source);
    require(product && product.value().glyphs.size() == 28U,
        "Pine Ridge board did not generate all semantically addressable glyphs");
    require(product.value().panel.validate() && product.value().lettering.validate(),
        "Pine Ridge board meshes do not validate");

    auto left = source;
    left.alignment = carto::geometry::SignTextAlignment::left;
    auto center = source;
    center.alignment = carto::geometry::SignTextAlignment::center;
    auto right = source;
    right.alignment = carto::geometry::SignTextAlignment::right;
    const auto left_product = carto::geometry::build_sign_text(left);
    const auto center_product = carto::geometry::build_sign_text(center);
    const auto right_product = carto::geometry::build_sign_text(right);
    require(left_product && center_product && right_product,
        "supported sign alignments failed to generate");
    require(left_product.value().glyphs.front().center.x <
            center_product.value().glyphs.front().center.x &&
        center_product.value().glyphs.front().center.x <
            right_product.value().glyphs.front().center.x,
        "text alignment does not change glyph layout in the requested direction");

    auto unsupported = source;
    unsupported.text = "PINE RIDGE ©";
    require(!unsupported.validate(), "unsupported non-ASCII glyph was silently replaced");
    auto too_wide = source;
    too_wide.board_width = 0.5;
    require(!too_wide.validate(), "text overflowing its board was accepted");
    auto tracking_overflow = source;
    tracking_overflow.text = "AAAA";
    tracking_overflow.board_width = 1.9;
    tracking_overflow.board_height = 1.6;
    tracking_overflow.glyph_height = 0.5;
    tracking_overflow.border = 0.1;
    tracking_overflow.tracking = 0.1;
    require(!tracking_overflow.validate(),
        "tracking-induced text overflow was accepted by source validation");
    auto too_many_lines = source;
    too_many_lines.text = "A\nB\nC\nD\nE";
    require(!too_many_lines.validate(), "excess sign lines were accepted");
    auto too_many_columns = source;
    too_many_columns.text = "AAAAAAAAAAAAAAAAAAAAAA";
    too_many_columns.board_width = 1.0;
    too_many_columns.board_height = 1.0;
    too_many_columns.glyph_height = 0.01;
    too_many_columns.border = 0.1;
    too_many_columns.tracking = 0.0;
    too_many_columns.line_spacing = 0.0;
    require(!too_many_columns.validate(), "excess bounded glyph columns were accepted");
    auto invalid_revision = source;
    invalid_revision.source_revision = carto::core::Revision{0U};
    require(!invalid_revision.validate(), "zero source revision was accepted");

    const auto malformed = carto::geometry::SignTextSource::deserialize(
        "CARTOGRAPHER_SIGN_TEXT 9\n");
    require(!malformed, "future sign-source schema was accepted");
}

} // namespace

int main() {
    try {
        motel_sign_source_and_mesh_are_deterministic();
        price_board_layout_and_adversarial_bounds_are_enforced();
        std::cout << "sign geometry tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
