#include <carto/geometry/sign.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace carto::geometry {

namespace {

constexpr std::array<std::array<std::uint8_t, 7U>, 36U> kGlyphRows{{
    {{0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}}, // 0
    {{0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}}, // 1
    {{0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}}, // 2
    {{0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e}}, // 3
    {{0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}}, // 4
    {{0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e}}, // 5
    {{0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e}}, // 6
    {{0x1f,0x01,0x02,0x04,0x08,0x08,0x08}}, // 7
    {{0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}}, // 8
    {{0x0e,0x11,0x11,0x0f,0x01,0x01,0x0e}}, // 9
    {{0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}}, // A
    {{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}}, // B
    {{0x0e,0x11,0x10,0x10,0x10,0x11,0x0e}}, // C
    {{0x1e,0x11,0x11,0x11,0x11,0x11,0x1e}}, // D
    {{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}}, // E
    {{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}}, // F
    {{0x0e,0x11,0x10,0x17,0x11,0x11,0x0f}}, // G
    {{0x11,0x11,0x11,0x1f,0x11,0x11,0x11}}, // H
    {{0x0e,0x04,0x04,0x04,0x04,0x04,0x0e}}, // I
    {{0x07,0x02,0x02,0x02,0x12,0x12,0x0c}}, // J
    {{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, // K
    {{0x10,0x10,0x10,0x10,0x10,0x10,0x1f}}, // L
    {{0x11,0x1b,0x15,0x15,0x11,0x11,0x11}}, // M
    {{0x11,0x19,0x15,0x13,0x11,0x11,0x11}}, // N
    {{0x0e,0x11,0x11,0x11,0x11,0x11,0x0e}}, // O
    {{0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}}, // P
    {{0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}}, // Q
    {{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}}, // R
    {{0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}}, // S
    {{0x1f,0x04,0x04,0x04,0x04,0x04,0x04}}, // T
    {{0x11,0x11,0x11,0x11,0x11,0x11,0x0e}}, // U
    {{0x11,0x11,0x11,0x11,0x11,0x0a,0x04}}, // V
    {{0x11,0x11,0x11,0x15,0x15,0x15,0x0a}}, // W
    {{0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}}, // X
    {{0x11,0x11,0x0a,0x04,0x04,0x04,0x04}}, // Y
    {{0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}}, // Z
}};

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_identity(std::string_view identity) noexcept {
    if (identity.empty() || identity.size() > SignTextSource::kMaxIdentityBytes) return false;
    for (const char value : identity) {
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-')) {
            return false;
        }
    }
    return identity.front() != '.' && identity.front() != '_' && identity.front() != '-';
}

bool valid_alignment(SignTextAlignment alignment) noexcept {
    return alignment == SignTextAlignment::left ||
        alignment == SignTextAlignment::center || alignment == SignTextAlignment::right;
}

bool supported_character(char value) noexcept {
    if (value == '\n' || value == ' ' || value == '.' || value == ':' ||
        value == '-' || value == '$' || value == '/') return true;
    return (value >= '0' && value <= '9') ||
        (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

const std::array<std::uint8_t, 7U>* rows_for(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return &kGlyphRows[static_cast<std::size_t>(value - '0')];
    }
    if (value >= 'a' && value <= 'z') value = static_cast<char>(value - 'a' + 'A');
    if (value >= 'A' && value <= 'Z') {
        return &kGlyphRows[10U + static_cast<std::size_t>(value - 'A')];
    }
    return nullptr;
}

std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    const auto mixed = value ^ (value >> 31U);
    return mixed == 0U ? 1U : mixed;
}

std::uint64_t identity_hash(std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : value) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void append_box(
    std::vector<Vertex>& vertices,
    std::vector<Face>& faces,
    std::uint64_t& next_vertex,
    std::uint64_t& next_face,
    core::Vec3d center,
    core::Vec3d size) {
    const double x = size.x * 0.5;
    const double y = size.y * 0.5;
    const double z = size.z * 0.5;
    std::array<VertexId, 8U> ids{};
    const std::array<core::Vec3d, 8U> offsets{{
        {-x,-y,-z}, {x,-y,-z}, {x,y,-z}, {-x,y,-z},
        {-x,-y,z}, {x,-y,z}, {x,y,z}, {-x,y,z},
    }};
    for (std::size_t index = 0U; index < offsets.size(); ++index) {
        ids[index] = VertexId{next_vertex++};
        vertices.push_back(Vertex{ids[index], center + offsets[index]});
    }
    const std::array<std::array<std::size_t, 4U>, 6U> box_faces{{
        {{0U,3U,2U,1U}}, {{4U,5U,6U,7U}}, {{0U,4U,7U,3U}},
        {{1U,2U,6U,5U}}, {{0U,1U,5U,4U}}, {{3U,7U,6U,2U}},
    }};
    for (const auto& indices : box_faces) {
        faces.push_back(Face{FaceId{next_face++}, {
            ids[indices[0U]], ids[indices[1U]], ids[indices[2U]], ids[indices[3U]]}});
    }
}

bool read_double(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token) || token.empty()) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value,
        std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() &&
        std::isfinite(value);
}

} // namespace

core::Result<void> SignTextSource::validate() const {
    if (!valid_identity(source_identity) || source_revision.value() == 0U ||
        source_revision.exhausted() || text.empty() || text.size() > kMaxTextBytes ||
        !valid_alignment(alignment)) {
        return core::Result<void>::failure(invalid(
            "sign source identity, revision, text, or alignment is invalid"));
    }
    std::size_t line_count = 1U;
    std::size_t current_columns = 0U;
    std::size_t current_characters = 0U;
    std::size_t max_columns = 0U;
    std::size_t max_characters = 0U;
    std::size_t glyph_count = 0U;
    for (const char value : text) {
        if (!supported_character(value)) {
            return core::Result<void>::failure(invalid(
                "sign text contains a character outside the bounded built-in glyph set"));
        }
        if (value == '\n') {
            max_columns = std::max(max_columns, current_columns);
            max_characters = std::max(max_characters, current_characters);
            current_columns = 0U;
            current_characters = 0U;
            ++line_count;
            continue;
        }
        if (value != ' ') ++glyph_count;
        ++current_characters;
        if (current_columns != 0U) current_columns++;
        current_columns += 5U;
    }
    max_columns = std::max(max_columns, current_columns);
    max_characters = std::max(max_characters, current_characters);
    if (line_count > kMaxLines || max_columns == 0U || max_columns > kMaxColumns ||
        glyph_count == 0U || glyph_count > kMaxGlyphDetails) {
        return core::Result<void>::failure(invalid(
            "sign text exceeds its bounded line, column, or glyph-detail count"));
    }
    if (!std::isfinite(board_width) || board_width < 0.1 || board_width > 1'000.0 ||
        !std::isfinite(board_height) || board_height < 0.1 || board_height > 1'000.0 ||
        !std::isfinite(board_depth) || board_depth < 0.001 || board_depth > 100.0 ||
        !std::isfinite(border) || border < 0.0 || border > 100.0 ||
        !std::isfinite(glyph_height) || glyph_height < 0.01 || glyph_height > 100.0 ||
        !std::isfinite(glyph_depth) || glyph_depth < 0.001 || glyph_depth > 100.0 ||
        !std::isfinite(tracking) || tracking < 0.0 || tracking > 10.0 ||
        !std::isfinite(line_spacing) || line_spacing < 0.0 || line_spacing > 100.0) {
        return core::Result<void>::failure(invalid(
            "sign panel and glyph dimensions exceed their supported bounds"));
    }
    const double cell = glyph_height / 7.0;
    const double content_height = static_cast<double>(line_count) * glyph_height +
        static_cast<double>(line_count - 1U) * line_spacing;
    const double measured_text_width = max_characters == 0U ? 0.0 :
        static_cast<double>(max_columns) * cell +
            static_cast<double>(max_characters - 1U) * tracking;
    if (2.0 * border >= board_width || 2.0 * border >= board_height ||
        measured_text_width > board_width - 2.0 * border ||
        content_height > board_height - 2.0 * border) {
        return core::Result<void>::failure(invalid(
            "sign text does not fit inside the authored panel and border"));
    }
    return core::Result<void>::success();
}

std::string SignTextSource::serialize() const {
    if (auto valid = validate(); !valid) return {};
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "CARTOGRAPHER_SIGN_TEXT " << kSchemaVersion << '\n'
           << std::quoted(source_identity) << ' ' << source_revision.value() << ' '
           << static_cast<unsigned>(alignment) << ' ' << std::quoted(text) << '\n'
           << std::setprecision(17) << board_width << ' ' << board_height << ' '
           << board_depth << ' ' << border << ' ' << glyph_height << ' '
           << glyph_depth << ' ' << tracking << ' ' << line_spacing << '\n'
           << "END\n";
    return output.str();
}

core::Result<SignTextSource> SignTextSource::deserialize(std::string_view text) {
    if (text.empty() || text.size() > 8U * 1024U) {
        return core::Result<SignTextSource>::failure(invalid(
            "sign source serialization is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    SignTextSource source;
    std::uint64_t revision = 0U;
    unsigned alignment = 0U;
    if (!(input >> magic >> version) || magic != "CARTOGRAPHER_SIGN_TEXT" ||
        version != kSchemaVersion || !(input >> std::quoted(source.source_identity) >> revision >>
            alignment >> std::quoted(source.text)) ||
        !read_double(input, source.board_width) || !read_double(input, source.board_height) ||
        !read_double(input, source.board_depth) || !read_double(input, source.border) ||
        !read_double(input, source.glyph_height) || !read_double(input, source.glyph_depth) ||
        !read_double(input, source.tracking) || !read_double(input, source.line_spacing)) {
        return core::Result<SignTextSource>::failure(invalid(
            "sign source serialization header or field is malformed"));
    }
    std::string terminal;
    std::string trailing;
    if (!(input >> terminal) || terminal != "END" || (input >> trailing)) {
        return core::Result<SignTextSource>::failure(invalid(
            "sign source serialization is missing END or contains trailing records"));
    }
    source.source_revision = core::Revision(revision);
    source.alignment = static_cast<SignTextAlignment>(alignment);
    if (auto valid = source.validate(); !valid) {
        return core::Result<SignTextSource>::failure(valid.error());
    }
    if (source.serialize() != text) {
        return core::Result<SignTextSource>::failure(invalid(
            "sign source serialization is not canonical"));
    }
    return core::Result<SignTextSource>::success(std::move(source));
}

core::Result<SignTextProduct> build_sign_text(const SignTextSource& source) {
    if (auto valid = source.validate(); !valid) {
        return core::Result<SignTextProduct>::failure(valid.error());
    }
    std::vector<Vertex> panel_vertex_data;
    std::vector<Face> panel_face_data;
    std::uint64_t panel_next_vertex = 1U;
    std::uint64_t panel_next_face = 1U;
    append_box(panel_vertex_data, panel_face_data, panel_next_vertex, panel_next_face,
        {0.0, 0.0, 0.0}, {source.board_width, source.board_depth, source.board_height});
    EditableMesh panel;
    if (auto inserted = panel.insert_bulk(
            std::move(panel_vertex_data), std::move(panel_face_data)); !inserted) {
        return core::Result<SignTextProduct>::failure(
            inserted.error().with_context("sign panel geometry"));
    }

    const double cell = source.glyph_height / 7.0;
    const double advance = cell * 6.0 + source.tracking;
    const double line_advance = source.glyph_height + source.line_spacing;
    std::vector<std::string_view> lines;
    std::size_t line_begin = 0U;
    for (std::size_t index = 0U; index <= source.text.size(); ++index) {
        if (index == source.text.size() || source.text[index] == '\n') {
            lines.emplace_back(source.text.data() + line_begin, index - line_begin);
            line_begin = index + 1U;
        }
    }
    std::size_t pixel_count = 0U;
    for (const auto line : lines) {
        std::size_t column = 0U;
        for (const char value : line) {
            if (value == ' ') {
                column += 6U;
                continue;
            }
            const auto* glyph = rows_for(value);
            if (glyph != nullptr) {
                for (const auto row : *glyph) {
                    for (std::uint8_t bit = 0U; bit < 5U; ++bit) {
                        if ((row & static_cast<std::uint8_t>(1U << (4U - bit))) != 0U) {
                            ++pixel_count;
                        }
                    }
                }
            } else {
                if (value == '.') pixel_count += 1U;
                else if (value == ':') pixel_count += 2U;
                else if (value == '-') pixel_count += 3U;
                else if (value == '/') pixel_count += 7U;
                else if (value == '$') pixel_count += 17U;
            }
            column += 6U;
        }
        static_cast<void>(column);
    }
    if (pixel_count == 0U || pixel_count > SignTextSource::kMaxPixelBlocks) {
        return core::Result<SignTextProduct>::failure(invalid(
            "sign text generated no pixels or exceeded its bounded geometry count"));
    }
    std::vector<Vertex> glyph_vertices;
    std::vector<Face> glyph_faces;
    glyph_vertices.reserve(pixel_count * 8U);
    glyph_faces.reserve(pixel_count * 6U);
    std::uint64_t next_vertex = 1U;
    std::uint64_t next_face = 1U;
    std::uint64_t source_key = identity_hash(source.source_identity);
    std::vector<SignGlyphDetail> glyph_details;
    glyph_details.reserve(SignTextSource::kMaxGlyphDetails);
    std::vector<std::size_t> word_start_offsets(source.text.size(), 0U);
    std::vector<std::uint32_t> word_indices(source.text.size(), 0U);
    std::size_t word_start = 0U;
    std::uint32_t word_index = 0U;
    bool in_word = false;
    bool found_word = false;
    for (std::size_t offset = 0U; offset < source.text.size(); ++offset) {
        const char value = source.text[offset];
        if (value == ' ' || value == '\n') {
            in_word = false;
            continue;
        }
        if (!in_word) {
            word_start = offset;
            if (found_word) ++word_index;
            found_word = true;
            in_word = true;
        }
        word_start_offsets[offset] = word_start;
        word_indices[offset] = word_index;
    }
    const double content_top = source.board_height * 0.5 - source.border;
    const double front_y = source.board_depth * 0.5 + source.glyph_depth * 0.5;
    const double dot_size = cell * 0.76;
    std::size_t byte_offset = 0U;
    std::size_t detail_count = 0U;
    for (std::size_t line_index = 0U; line_index < lines.size(); ++line_index) {
        const auto line = lines[line_index];
        const double line_width = line.empty() ? 0.0 :
            static_cast<double>(line.size() * 6U - 1U) * cell +
                static_cast<double>(line.size() - 1U) * source.tracking;
        double cursor_x = -source.board_width * 0.5 + source.border;
        if (source.alignment == SignTextAlignment::center) cursor_x = -line_width * 0.5;
        else if (source.alignment == SignTextAlignment::right) {
            cursor_x = source.board_width * 0.5 - source.border - line_width;
        }
        const double line_top = content_top - static_cast<double>(line_index) * line_advance;
        for (std::size_t char_index = 0U; char_index < line.size(); ++char_index) {
            const char value = line[char_index];
            if (value != ' ') {
                const double glyph_center_x = cursor_x + cell * 2.5;
                const double glyph_center_z = line_top - source.glyph_height * 0.5;
                const auto stable_id = mix64(source_key ^ mix64(byte_offset));
                const auto word_stable_id = mix64(
                    source_key ^ mix64(word_start_offsets[byte_offset]) ^
                    0xd6e8feb86659fd93ULL);
                const std::uint64_t first_face_id = next_face;
                glyph_details.push_back(SignGlyphDetail{
                    stable_id, word_stable_id, byte_offset, value,
                    static_cast<std::uint32_t>(line_index),
                    static_cast<std::uint32_t>(char_index),
                    word_indices[byte_offset], first_face_id, 0U,
                    {glyph_center_x, front_y, glyph_center_z}});
                ++detail_count;
                const auto* glyph = rows_for(value);
                if (glyph != nullptr) {
                    for (std::size_t row_index = 0U; row_index < glyph->size(); ++row_index) {
                        const std::uint8_t row = (*glyph)[row_index];
                        for (std::uint8_t column = 0U; column < 5U; ++column) {
                            if ((row & static_cast<std::uint8_t>(1U << (4U - column))) == 0U) continue;
                            const double x = cursor_x + (static_cast<double>(column) + 0.5) * cell;
                            const double z = line_top - (static_cast<double>(row_index) + 0.5) * cell;
                            append_box(glyph_vertices, glyph_faces, next_vertex, next_face,
                                {x, front_y, z}, {dot_size, source.glyph_depth, dot_size});
                        }
                    }
                } else if (value == '.' || value == ':') {
                    const double x = cursor_x + cell * 2.5;
                    const double lower_z = line_top - cell * 6.5;
                    append_box(glyph_vertices, glyph_faces, next_vertex, next_face,
                        {x, front_y, lower_z}, {dot_size, source.glyph_depth, dot_size});
                    if (value == ':') {
                        append_box(glyph_vertices, glyph_faces, next_vertex, next_face,
                            {x, front_y, line_top - cell * 3.5},
                            {dot_size, source.glyph_depth, dot_size});
                    }
                } else if (value == '-') {
                    for (std::uint8_t column = 1U; column < 4U; ++column) {
                        append_box(glyph_vertices, glyph_faces, next_vertex, next_face,
                            {cursor_x + (static_cast<double>(column) + 0.5) * cell,
                                front_y, line_top - cell * 3.5},
                            {dot_size, source.glyph_depth, dot_size});
                    }
                } else if (value == '/') {
                    for (std::uint8_t step = 0U; step < 7U; ++step) {
                        const double t = static_cast<double>(step) / 6.0;
                        append_box(glyph_vertices, glyph_faces, next_vertex, next_face,
                            {cursor_x + (4.5 - t * 4.0) * cell, front_y,
                                line_top - (t * 6.0 + 0.5) * cell},
                            {dot_size * 0.72, source.glyph_depth, dot_size * 0.72});
                    }
                } else if (value == '$') {
                    constexpr std::array<std::uint8_t, 7U> kDollarRows{
                        0x0e,0x10,0x10,0x0e,0x01,0x01,0x0e};
                    for (std::size_t row_index = 0U; row_index < kDollarRows.size(); ++row_index) {
                        const auto row = kDollarRows[row_index];
                        for (std::uint8_t column = 0U; column < 5U; ++column) {
                            if ((row & static_cast<std::uint8_t>(1U << (4U - column))) == 0U) continue;
                            append_box(glyph_vertices, glyph_faces, next_vertex, next_face,
                                {cursor_x + (static_cast<double>(column) + 0.5) * cell,
                                    front_y,
                                    line_top - (static_cast<double>(row_index) + 0.5) * cell},
                                {dot_size, source.glyph_depth, dot_size});
                        }
                    }
                    append_box(glyph_vertices, glyph_faces, next_vertex, next_face,
                        {cursor_x + cell * 2.5, front_y,
                            line_top - source.glyph_height * 0.5},
                        {cell * 0.16, source.glyph_depth * 0.75,
                            source.glyph_height * 1.08});
                }
                glyph_details.back().face_count = static_cast<std::size_t>(
                    next_face - first_face_id);
            }
            cursor_x += advance;
            ++byte_offset;
        }
        if (line_index + 1U < lines.size()) ++byte_offset;
    }
    if (detail_count != glyph_details.size() || glyph_vertices.empty()) {
        return core::Result<SignTextProduct>::failure(invalid(
            "sign text glyph/detail generation is incomplete"));
    }
    EditableMesh lettering;
    if (auto inserted = lettering.insert_bulk(
            std::move(glyph_vertices), std::move(glyph_faces)); !inserted) {
        return core::Result<SignTextProduct>::failure(
            inserted.error().with_context("sign raised lettering geometry"));
    }
    if (auto valid = panel.validate(); !valid) {
        return core::Result<SignTextProduct>::failure(valid.error());
    }
    if (auto valid = lettering.validate(); !valid) {
        return core::Result<SignTextProduct>::failure(valid.error());
    }
    return core::Result<SignTextProduct>::success(SignTextProduct{
        source.source_identity, source.source_revision, std::move(panel),
        std::move(lettering), std::move(glyph_details)});
}

core::Result<EditableMesh> combine_sign_text(const SignTextProduct& product) {
    if (auto valid = product.panel.validate(); !valid) {
        return core::Result<EditableMesh>::failure(valid.error().with_context(
            "sign panel before combination"));
    }
    if (auto valid = product.lettering.validate(); !valid) {
        return core::Result<EditableMesh>::failure(valid.error().with_context(
            "sign lettering before combination"));
    }
    std::vector<Vertex> vertices;
    std::vector<Face> faces;
    std::vector<std::uint32_t> material_slots;
    vertices.reserve(product.panel.vertices_sorted().size() +
        product.lettering.vertices_sorted().size());
    faces.reserve(product.panel.faces_sorted().size() +
        product.lettering.faces_sorted().size());
    material_slots.reserve(product.panel.faces_sorted().size() +
        product.lettering.faces_sorted().size());
    std::uint64_t next_vertex = 1U;
    std::uint64_t next_face = 1U;
    const auto append = [&](const EditableMesh& source,
                            std::uint32_t material_slot) -> core::Result<void> {
        std::map<std::uint64_t, VertexId> remapped_vertices;
        for (const auto& vertex : source.vertices_sorted()) {
            if (next_vertex == 0U) {
                return core::Result<void>::failure(invalid(
                    "combined sign vertex identifiers overflowed"));
            }
            const VertexId id{next_vertex++};
            remapped_vertices.emplace(vertex.id.value, id);
            vertices.push_back(Vertex{id, vertex.position});
        }
        for (const auto& face : source.faces_sorted()) {
            if (next_face == 0U) {
                return core::Result<void>::failure(invalid(
                    "combined sign face identifiers overflowed"));
            }
            std::vector<VertexId> boundary;
            boundary.reserve(face.vertices.size());
            for (const auto original : face.vertices) {
                const auto remapped = remapped_vertices.find(original.value);
                if (remapped == remapped_vertices.end()) {
                    return core::Result<void>::failure(invalid(
                        "sign geometry face references an absent vertex"));
                }
                boundary.push_back(remapped->second);
            }
            faces.push_back(Face{FaceId{next_face++}, std::move(boundary)});
            material_slots.push_back(material_slot);
        }
        return core::Result<void>::success();
    };
    if (auto result = append(product.panel, kSignTextPanelMaterialSlot); !result) {
        return core::Result<EditableMesh>::failure(result.error());
    }
    if (auto result = append(product.lettering, kSignTextLetteringMaterialSlot); !result) {
        return core::Result<EditableMesh>::failure(result.error());
    }
    EditableMesh combined;
    if (auto result = combined.insert_bulk(std::move(vertices), std::move(faces)); !result) {
        return core::Result<EditableMesh>::failure(
            result.error().with_context("combined sign geometry"));
    }
    if (material_slots.size() != combined.face_count()) {
        return core::Result<EditableMesh>::failure(invalid(
            "sign material-slot cardinality does not match combined faces"));
    }
    std::vector<attributes::AttributeValue> slot_values;
    slot_values.reserve(material_slots.size());
    for (const std::uint32_t slot : material_slots) slot_values.emplace_back(slot);
    attributes::AttributeSet slot_attributes({
        {attributes::AttributeDomain::face, combined.face_count()}});
    if (auto result = slot_attributes.add_layer(attributes::AttributeLayer{
            {"cartographer.material_slot", attributes::AttributeDomain::face,
             attributes::AttributeType::uint32}, std::move(slot_values)}); !result) {
        return core::Result<EditableMesh>::failure(
            result.error().with_context("sign material-slot face layer"));
    }
    if (auto result = combined.set_attribute_payload(std::move(slot_attributes), {}); !result) {
        return core::Result<EditableMesh>::failure(
            result.error().with_context("sign material-slot attachment"));
    }
    return core::Result<EditableMesh>::success(std::move(combined));
}

} // namespace carto::geometry
