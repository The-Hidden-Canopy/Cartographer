#include <carto/production/sculpt_stack.hpp>

#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace carto::production {
namespace {

constexpr std::string_view kStackMagicV1 = "CARTO_SCULPT_STACK 1";
constexpr std::string_view kStackMagicV2 = "CARTO_SCULPT_STACK 2";
constexpr std::string_view kStackMagicV3 = "CARTO_SCULPT_STACK 3";
constexpr std::string_view kStackMagicV4 = "CARTO_SCULPT_STACK 4";
constexpr std::size_t kMaxStackBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaxStackSamples = 65'536U;
constexpr double kMinNormalLengthSquared = 1e-24;
constexpr double kMaxSampleCoordinate = 1e9;

class CanonicalBytes final {
public:
    void text(std::string_view value) {
        u64(static_cast<std::uint64_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void u64(std::uint64_t value) {
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    }

    void f64(double value) {
        u64(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
    }

    void digest(const assets::Sha256Digest& digest_value) {
        bytes_.insert(bytes_.end(), digest_value.bytes.begin(), digest_value.bytes.end());
    }

    [[nodiscard]] assets::Sha256Digest hash() const { return assets::sha256(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

std::string encode_hex(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded;
}

std::optional<std::string> decode_hex(std::string_view value, std::size_t maximum_bytes) {
    if (value.size() % 2U != 0U || value.size() / 2U > maximum_bytes) {
        return std::nullopt;
    }
    const auto nibble = [](char character) -> std::optional<std::uint8_t> {
        if (character >= '0' && character <= '9') {
            return static_cast<std::uint8_t>(character - '0');
        }
        if (character >= 'a' && character <= 'f') {
            return static_cast<std::uint8_t>(10 + character - 'a');
        }
        return std::nullopt;
    };
    std::string decoded;
    decoded.reserve(value.size() / 2U);
    for (std::size_t index = 0U; index < value.size(); index += 2U) {
        const auto high = nibble(value[index]);
        const auto low = nibble(value[index + 1U]);
        if (!high || !low) return std::nullopt;
        decoded.push_back(static_cast<char>((*high << 4U) | *low));
    }
    return decoded;
}

std::string u64_hex(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded(16U, '0');
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        const std::size_t shift = (encoded.size() - index - 1U) * 4U;
        encoded[index] = digits[(value >> shift) & 0x0fU];
    }
    return encoded;
}

std::string f64_hex(double value) {
    return u64_hex(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value));
}

std::optional<double> parse_f64_hex(std::string_view value) {
    if (value.size() != 16U) return std::nullopt;
    std::uint64_t bits = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), bits, 16);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return std::bit_cast<double>(bits);
}

std::optional<std::string> read_line(std::istringstream& input) {
    std::string line;
    if (!std::getline(input, line)) return std::nullopt;
    return line;
}

std::optional<std::istringstream> read_record(
    std::istringstream& input,
    std::string_view expected_tag) {
    const auto line = read_line(input);
    if (!line) return std::nullopt;
    std::istringstream record(*line);
    std::string tag;
    if (!(record >> tag) || tag != expected_tag) return std::nullopt;
    return std::optional<std::istringstream>(std::move(record));
}

bool record_finished(std::istringstream& record) {
    record >> std::ws;
    return record.eof();
}

std::string mesh_identity(std::string_view project_namespace, std::uint64_t mesh_asset) {
    return std::string(project_namespace) + "/mesh/" + std::to_string(mesh_asset);
}

bool uses_normal_displacement(SculptStrokeMode mode) {
    return mode == SculptStrokeMode::add || mode == SculptStrokeMode::subtract ||
        mode == SculptStrokeMode::inflate || mode == SculptStrokeMode::clay ||
        mode == SculptStrokeMode::crease || mode == SculptStrokeMode::surface_noise;
}

bool within_sample_bounds(core::Vec3d value) {
    return value.finite() && std::abs(value.x) <= kMaxSampleCoordinate &&
        std::abs(value.y) <= kMaxSampleCoordinate &&
        std::abs(value.z) <= kMaxSampleCoordinate;
}

bool same_vec3(core::Vec3d left, core::Vec3d right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}


} // namespace

core::Result<void> SculptSurfaceAnchor::validate() const {
    if (!face || !vertices[0] || !vertices[1] || !vertices[2] ||
        vertices[0] == vertices[1] || vertices[0] == vertices[2] ||
        vertices[1] == vertices[2]) {
        return core::Result<void>::failure(invalid(
            "SculptStack surface anchor requires one face and three distinct vertices"));
    }
    double total = 0.0;
    for (const double weight : barycentric) {
        if (!std::isfinite(weight) || weight < 0.0 || weight > 1.0) {
            return core::Result<void>::failure(validation(
                "SculptStack surface barycentric weights must be finite and in [0, 1]"));
        }
        total += weight;
    }
    if (!std::isfinite(total) || std::abs(total - 1.0) > 1.0e-12) {
        return core::Result<void>::failure(validation(
            "SculptStack surface barycentric weights must sum to one"));
    }
    return core::Result<void>::success();
}

core::Result<core::Vec3d> resolve_sculpt_surface_anchor(
    const geometry::EditableMesh& mesh,
    const SculptSurfaceAnchor& anchor) {
    if (auto result = anchor.validate(); !result) {
        return core::Result<core::Vec3d>::failure(result.error());
    }
    const geometry::Face* face = mesh.find_face(anchor.face);
    if (face == nullptr) {
        return core::Result<core::Vec3d>::failure(stale(
            "SculptStack surface anchor face no longer exists"));
    }
    bool belongs_to_face_fan = false;
    for (std::size_t index = 1U; index + 1U < face->vertices.size(); ++index) {
        if (anchor.vertices[0] == face->vertices[0] &&
            anchor.vertices[1] == face->vertices[index] &&
            anchor.vertices[2] == face->vertices[index + 1U]) {
            belongs_to_face_fan = true;
            break;
        }
    }
    if (!belongs_to_face_fan) {
        return core::Result<core::Vec3d>::failure(validation(
            "SculptStack surface anchor triangle is not in the face's deterministic fan"));
    }
    core::Vec3d position{};
    for (std::size_t index = 0U; index < anchor.vertices.size(); ++index) {
        const geometry::Vertex* vertex = mesh.find_vertex(anchor.vertices[index]);
        if (vertex == nullptr) {
            return core::Result<core::Vec3d>::failure(stale(
                "SculptStack surface anchor vertex no longer exists"));
        }
        const double weight = anchor.barycentric[index];
        position.x += vertex->position.x * weight;
        position.y += vertex->position.y * weight;
        position.z += vertex->position.z * weight;
    }
    if (!position.finite()) {
        return core::Result<core::Vec3d>::failure(validation(
            "SculptStack surface anchor resolves to a non-finite position"));
    }
    return core::Result<core::Vec3d>::success(position);
}

core::Result<SculptStrokeSample> make_sculpt_surface_sample(
    const geometry::EditableMesh& mesh,
    SculptSurfaceAnchor anchor,
    double pressure) {
    if (!std::isfinite(pressure) || pressure < 0.0 || pressure > 1.0) {
        return core::Result<SculptStrokeSample>::failure(invalid(
            "SculptStack surface sample pressure must be finite and in [0, 1]"));
    }
    const auto position = resolve_sculpt_surface_anchor(mesh, anchor);
    if (!position) return core::Result<SculptStrokeSample>::failure(position.error());
    if (std::abs(position.value().x) > kMaxSampleCoordinate ||
        std::abs(position.value().y) > kMaxSampleCoordinate ||
        std::abs(position.value().z) > kMaxSampleCoordinate) {
        return core::Result<SculptStrokeSample>::failure(validation(
            "SculptStack surface sample position exceeds the coordinate bound"));
    }
    return core::Result<SculptStrokeSample>::success(SculptStrokeSample{
        position.value(), pressure, std::move(anchor)});
}

core::Result<void> SculptStroke::validate() const {
    if (!safe_text(identity, 128U) || !std::isfinite(radius) || radius <= 0.0 ||
        radius > 1'000'000.0 || !std::isfinite(strength) || strength < 0.0 ||
        strength > 10'000.0 || !std::isfinite(symmetry_plane_x) ||
        std::abs(symmetry_plane_x) > kMaxSampleCoordinate ||
        !safe_text(mask_layer_id, 128U, true) || samples.empty() ||
        samples.size() > kMaxSculptSamplesPerStroke) {
        return core::Result<void>::failure(invalid(
            "SculptStack stroke identity, settings, mask, or sample count is invalid"));
    }
    switch (mode) {
    case SculptStrokeMode::add:
    case SculptStrokeMode::subtract:
    case SculptStrokeMode::inflate:
    case SculptStrokeMode::clay:
    case SculptStrokeMode::grab:
    case SculptStrokeMode::smooth:
    case SculptStrokeMode::pinch:
    case SculptStrokeMode::flatten:
    case SculptStrokeMode::scrape:
    case SculptStrokeMode::polish:
    case SculptStrokeMode::crease:
    case SculptStrokeMode::surface_noise:
        break;
    default:
        return core::Result<void>::failure(invalid("SculptStack stroke mode is invalid"));
    }
    if (symmetry != SculptSymmetryMode::none &&
        symmetry != SculptSymmetryMode::mirror_x &&
        symmetry != SculptSymmetryMode::mirror_plane &&
        symmetry != SculptSymmetryMode::radial) {
        return core::Result<void>::failure(invalid("SculptStack symmetry mode is invalid"));
    }
    const bool local_symmetry = symmetry == SculptSymmetryMode::mirror_plane ||
        symmetry == SculptSymmetryMode::radial;
    if (local_symmetry) {
        if (!within_sample_bounds(symmetry_origin_local) ||
            !within_sample_bounds(symmetry_axis_local) ||
            !std::isfinite(symmetry_axis_local.length_squared()) ||
            symmetry_axis_local.length_squared() <= kMinNormalLengthSquared) {
            return core::Result<void>::failure(validation(
                "SculptStack local symmetry requires a finite bounded origin and nonzero axis"));
        }
    } else if (!same_vec3(symmetry_origin_local, {}) ||
        !same_vec3(symmetry_axis_local, {1.0, 0.0, 0.0})) {
        return core::Result<void>::failure(validation(
            "SculptStack unused local symmetry settings must retain canonical defaults"));
    }
    if ((symmetry == SculptSymmetryMode::radial &&
            (radial_symmetry_count < 2U ||
                radial_symmetry_count > kMaxSculptRadialSymmetryCopies)) ||
        (symmetry != SculptSymmetryMode::radial && radial_symmetry_count != 4U)) {
        return core::Result<void>::failure(validation(
            "SculptStack radial symmetry copy count is outside its supported bound"));
    }
    if (direction_mode != SculptStrokeDirectionMode::surface_normal &&
        direction_mode != SculptStrokeDirectionMode::surface_tangent &&
        direction_mode != SculptStrokeDirectionMode::view_aligned) {
        return core::Result<void>::failure(invalid(
            "SculptStack brush direction mode is invalid"));
    }
    if (direction_mode != SculptStrokeDirectionMode::surface_normal &&
        !uses_normal_displacement(mode)) {
        return core::Result<void>::failure(invalid(
            "SculptStack brush direction mode is unsupported for this brush"));
    }
    for (const SculptStrokeSample& sample : samples) {
        if (!sample.position.finite() ||
            std::abs(sample.position.x) > kMaxSampleCoordinate ||
            std::abs(sample.position.y) > kMaxSampleCoordinate ||
            std::abs(sample.position.z) > kMaxSampleCoordinate ||
            !std::isfinite(sample.pressure) || sample.pressure < 0.0 ||
            sample.pressure > 1.0) {
            return core::Result<void>::failure(validation(
                "SculptStack samples require finite bounded positions and pressure"));
        }
        if (sample.surface_anchor.has_value()) {
            if (auto result = sample.surface_anchor->validate(); !result) return result;
        }
        if (direction_mode == SculptStrokeDirectionMode::surface_normal) {
            if (sample.direction_axis.has_value()) {
                return core::Result<void>::failure(validation(
                    "SculptStack normal-direction samples cannot carry a captured axis"));
            }
        } else if (!sample.direction_axis.has_value() ||
            !sample.direction_axis->finite() ||
            std::abs(sample.direction_axis->x) > kMaxSampleCoordinate ||
            std::abs(sample.direction_axis->y) > kMaxSampleCoordinate ||
            std::abs(sample.direction_axis->z) > kMaxSampleCoordinate ||
            !std::isfinite(sample.direction_axis->length_squared()) ||
            sample.direction_axis->length_squared() <= kMinNormalLengthSquared) {
            return core::Result<void>::failure(validation(
                "SculptStack tangent and view directions require a finite nonzero captured axis"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> SculptStack::validate() const {
    if (!safe_text(project_namespace, 128U) || mesh_asset == 0U ||
        source.identity != mesh_identity(project_namespace, mesh_asset) ||
        (format_version != 1U && format_version != 2U && format_version != 3U &&
            format_version != kSculptStackVersion) ||
        layers.size() > kMaxSculptLayers) {
        return core::Result<void>::failure(invalid(
            "SculptStack project, mesh, source identity, or layer count is invalid"));
    }
    if (auto result = source.validate(); !result) return result;
    std::set<std::string> layer_ids;
    std::set<std::string> stroke_ids;
    std::size_t total_samples = 0U;
    for (const SculptLayer& layer : layers) {
        if (!safe_text(layer.identity, 128U) || !safe_text(layer.name, 128U) ||
            !std::isfinite(layer.weight) || layer.weight < 0.0 || layer.weight > 1.0 ||
            layer.strokes.size() > kMaxSculptStrokesPerLayer ||
            !layer_ids.insert(layer.identity).second) {
            return core::Result<void>::failure(validation(
                "SculptStack layer identity, name, weight, or stroke count is invalid"));
        }
        for (const SculptStroke& stroke : layer.strokes) {
            if (auto result = stroke.validate(); !result) return result;
            if (format_version >= 4U && stroke.symmetry != SculptSymmetryMode::mirror_x &&
                stroke.symmetry_plane_x != 0.0) {
                return core::Result<void>::failure(validation(
                    "SculptStack unused X-mirror setting must be zero for this symmetry mode"));
            }
            if (format_version == 1U) {
                if (stroke.mode != SculptStrokeMode::add &&
                    stroke.mode != SculptStrokeMode::subtract) {
                    return core::Result<void>::failure(validation(
                        "SculptStack v1 cannot encode the expanded brush modes"));
                }
                for (const SculptStrokeSample& sample : stroke.samples) {
                    if (sample.surface_anchor.has_value()) {
                        return core::Result<void>::failure(validation(
                            "SculptStack v1 cannot encode surface-constrained samples"));
                    }
                }
            }
            if (format_version < 3U) {
                if (stroke.direction_mode != SculptStrokeDirectionMode::surface_normal) {
                    return core::Result<void>::failure(validation(
                        "SculptStack v1/v2 cannot encode brush direction modes"));
                }
                for (const SculptStrokeSample& sample : stroke.samples) {
                    if (sample.direction_axis.has_value()) {
                        return core::Result<void>::failure(validation(
                            "SculptStack v1/v2 cannot encode captured brush directions"));
                    }
                }
            }
            if (format_version < 4U &&
                stroke.symmetry != SculptSymmetryMode::none &&
                stroke.symmetry != SculptSymmetryMode::mirror_x) {
                return core::Result<void>::failure(validation(
                    "SculptStack v1/v2/v3 cannot encode local planar or radial symmetry"));
            }
            if (!stroke_ids.insert(stroke.identity).second) {
                return core::Result<void>::failure(validation(
                    "SculptStack stroke identities must be unique across layers"));
            }
            if (stroke.samples.size() > kMaxStackSamples - total_samples) {
                return core::Result<void>::failure(validation(
                    "SculptStack exceeds its total sample count bound"));
            }
            total_samples += stroke.samples.size();
        }
    }
    return core::Result<void>::success();
}

std::string SculptStack::serialize() const {
    if (!validate()) return {};
    std::ostringstream output;
    const std::string_view magic = format_version == 1U ? kStackMagicV1 :
        (format_version == 2U ? kStackMagicV2 :
            (format_version == 3U ? kStackMagicV3 : kStackMagicV4));
    output << magic << '\n'
           << "PROJECT " << encode_hex(project_namespace) << '\n'
           << "MESH " << mesh_asset << '\n'
           << "SOURCE " << encode_hex(source.identity) << ' '
           << source.revision.value() << ' ' << source.content_digest.hex() << '\n'
           << "LAYERS " << layers.size() << '\n';
    for (const SculptLayer& layer : layers) {
        output << "LAYER " << encode_hex(layer.identity) << ' '
               << encode_hex(layer.name) << ' ' << (layer.visible ? 1 : 0) << ' '
               << f64_hex(layer.weight) << ' ' << layer.strokes.size() << '\n';
        for (const SculptStroke& stroke : layer.strokes) {
            output << "STROKE " << encode_hex(stroke.identity) << ' '
                   << static_cast<std::uint32_t>(stroke.mode) << ' '
                   << static_cast<std::uint32_t>(stroke.symmetry) << ' ';
            if (format_version >= 3U) {
                output << static_cast<std::uint32_t>(stroke.direction_mode) << ' ';
            }
            output << (stroke.enabled ? 1 : 0) << ' '
                   << f64_hex(stroke.symmetry_plane_x) << ' '
                   << f64_hex(stroke.radius) << ' ' << f64_hex(stroke.strength) << ' '
                   << (stroke.mask_layer_id.empty() ? "-" : encode_hex(stroke.mask_layer_id)) << ' ';
            if (format_version >= 4U) {
                output << f64_hex(stroke.symmetry_origin_local.x) << ' '
                       << f64_hex(stroke.symmetry_origin_local.y) << ' '
                       << f64_hex(stroke.symmetry_origin_local.z) << ' '
                       << f64_hex(stroke.symmetry_axis_local.x) << ' '
                       << f64_hex(stroke.symmetry_axis_local.y) << ' '
                       << f64_hex(stroke.symmetry_axis_local.z) << ' '
                       << stroke.radial_symmetry_count << ' ';
            }
            output << stroke.samples.size() << '\n';
            for (const SculptStrokeSample& sample : stroke.samples) {
                output << "SAMPLE " << f64_hex(sample.position.x) << ' '
                       << f64_hex(sample.position.y) << ' '
                       << f64_hex(sample.position.z) << ' '
                       << f64_hex(sample.pressure);
                if (sample.surface_anchor.has_value()) {
                    const SculptSurfaceAnchor& anchor = *sample.surface_anchor;
                    output << " SURFACE " << anchor.face.value << ' '
                           << anchor.vertices[0].value << ' '
                           << anchor.vertices[1].value << ' '
                           << anchor.vertices[2].value << ' '
                           << f64_hex(anchor.barycentric[0]) << ' '
                           << f64_hex(anchor.barycentric[1]) << ' '
                           << f64_hex(anchor.barycentric[2]);
                }
                if (sample.direction_axis.has_value()) {
                    output << " DIRECTION " << f64_hex(sample.direction_axis->x) << ' '
                           << f64_hex(sample.direction_axis->y) << ' '
                           << f64_hex(sample.direction_axis->z);
                }
                output << '\n';
            }
        }
    }
    output << "END_SCULPT_STACK\n";
    const std::string serialized = output.str();
    return serialized.size() <= kMaxStackBytes ? serialized : std::string{};
}

core::Result<assets::Sha256Digest> SculptStack::digest() const {
    const std::string serialized = serialize();
    if (serialized.empty()) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "cannot digest an invalid or oversized SculptStack"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(serialized.data());
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>(bytes, serialized.size())));
}

core::Result<SculptStack> SculptStack::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxStackBytes) {
        return core::Result<SculptStack>::failure(validation(
            "serialized SculptStack is empty or exceeds the size bound"));
    }
    std::istringstream input{std::string(text)};
    const auto magic = read_line(input);
    if (!magic || (*magic != kStackMagicV1 && *magic != kStackMagicV2 &&
            *magic != kStackMagicV3 && *magic != kStackMagicV4)) {
        return core::Result<SculptStack>::failure(validation(
            "serialized SculptStack magic or version is invalid"));
    }
    SculptStack stack;
    stack.format_version = *magic == kStackMagicV1 ? 1U :
        (*magic == kStackMagicV2 ? 2U :
            (*magic == kStackMagicV3 ? 3U : 4U));
    auto project_record = read_record(input, "PROJECT");
    std::string project_hex;
    if (!project_record || !(*project_record >> project_hex) ||
        !record_finished(*project_record)) {
        return core::Result<SculptStack>::failure(validation(
            "serialized SculptStack project record is invalid"));
    }
    const auto decoded_project = decode_hex(project_hex, 128U);
    if (!decoded_project) return core::Result<SculptStack>::failure(validation(
        "serialized SculptStack project namespace encoding is invalid"));
    stack.project_namespace = *decoded_project;

    auto mesh_record = read_record(input, "MESH");
    std::uint64_t mesh_asset = 0U;
    if (!mesh_record || !(*mesh_record >> mesh_asset) || !record_finished(*mesh_record)) {
        return core::Result<SculptStack>::failure(validation(
            "serialized SculptStack mesh record is invalid"));
    }
    stack.mesh_asset = mesh_asset;

    auto source_record = read_record(input, "SOURCE");
    std::string source_identity_hex;
    std::uint64_t source_revision = 0U;
    std::string source_digest_hex;
    if (!source_record || !(*source_record >> source_identity_hex >> source_revision >>
            source_digest_hex) || !record_finished(*source_record)) {
        return core::Result<SculptStack>::failure(validation(
            "serialized SculptStack source binding is invalid"));
    }
    const auto source_identity = decode_hex(source_identity_hex, 1024U);
    const auto source_digest = assets::Sha256Digest::from_hex(source_digest_hex);
    if (!source_identity || !source_digest || source_digest.value().is_zero()) {
        return core::Result<SculptStack>::failure(validation(
            "serialized SculptStack source identity or digest is invalid"));
    }
    stack.source = BakeMeshBinding{
        *source_identity, core::Revision(source_revision), source_digest.value()};

    auto layers_record = read_record(input, "LAYERS");
    std::uint64_t layer_count = 0U;
    if (!layers_record || !(*layers_record >> layer_count) ||
        !record_finished(*layers_record) || layer_count > kMaxSculptLayers) {
        return core::Result<SculptStack>::failure(validation(
            "serialized SculptStack layer count is invalid"));
    }
    stack.layers.reserve(static_cast<std::size_t>(layer_count));
    std::size_t total_samples = 0U;
    for (std::uint64_t layer_index = 0U; layer_index < layer_count; ++layer_index) {
        auto layer_record = read_record(input, "LAYER");
        std::string layer_id_hex;
        std::string layer_name_hex;
        std::uint32_t visible = 0U;
        std::string weight_hex;
        std::uint64_t stroke_count = 0U;
        if (!layer_record || !(*layer_record >> layer_id_hex >> layer_name_hex >> visible >>
                weight_hex >> stroke_count) || !record_finished(*layer_record) ||
            visible > 1U || stroke_count > kMaxSculptStrokesPerLayer) {
            return core::Result<SculptStack>::failure(validation(
                "serialized SculptStack layer record is invalid"));
        }
        const auto layer_id = decode_hex(layer_id_hex, 128U);
        const auto layer_name = decode_hex(layer_name_hex, 128U);
        const auto weight = parse_f64_hex(weight_hex);
        if (!layer_id || !layer_name || !weight) {
            return core::Result<SculptStack>::failure(validation(
                "serialized SculptStack layer text or weight encoding is invalid"));
        }
        SculptLayer layer{*layer_id, *layer_name, visible != 0U, *weight, {}};
        layer.strokes.reserve(static_cast<std::size_t>(stroke_count));
        for (std::uint64_t stroke_index = 0U; stroke_index < stroke_count; ++stroke_index) {
            auto stroke_record = read_record(input, "STROKE");
            std::string stroke_id_hex;
            std::uint32_t mode = 0U;
            std::uint32_t symmetry = 0U;
            std::uint32_t direction_mode = 0U;
            std::uint32_t enabled = 0U;
            std::string plane_hex;
            std::string radius_hex;
            std::string strength_hex;
            std::string mask_hex;
            std::array<std::string, 6> local_symmetry_hex;
            std::uint32_t radial_symmetry_count = 4U;
            std::uint64_t sample_count = 0U;
            bool stroke_header_valid = stroke_record.has_value() &&
                static_cast<bool>(*stroke_record >> stroke_id_hex >> mode >> symmetry);
            if (stroke_header_valid && stack.format_version >= 3U) {
                stroke_header_valid = static_cast<bool>(*stroke_record >> direction_mode);
            }
            if (stroke_header_valid) {
                stroke_header_valid = static_cast<bool>(*stroke_record >> enabled >> plane_hex >>
                    radius_hex >> strength_hex >> mask_hex);
            }
            if (stroke_header_valid && stack.format_version >= 4U) {
                stroke_header_valid = static_cast<bool>(*stroke_record >>
                    local_symmetry_hex[0] >> local_symmetry_hex[1] >> local_symmetry_hex[2] >>
                    local_symmetry_hex[3] >> local_symmetry_hex[4] >> local_symmetry_hex[5] >>
                    radial_symmetry_count);
            }
            if (stroke_header_valid) {
                stroke_header_valid = static_cast<bool>(*stroke_record >> sample_count);
            }
            if (!stroke_header_valid ||
                !record_finished(*stroke_record) ||
                mode > (stack.format_version == 1U ? 1U : 11U) ||
                symmetry > (stack.format_version >= 4U ? 3U : 1U) ||
                direction_mode > 2U || enabled > 1U ||
                sample_count == 0U || sample_count > kMaxSculptSamplesPerStroke ||
                sample_count > kMaxStackSamples - total_samples) {
                return core::Result<SculptStack>::failure(validation(
                    "serialized SculptStack stroke record is invalid"));
            }
            const auto stroke_id = decode_hex(stroke_id_hex, 128U);
            const auto plane = parse_f64_hex(plane_hex);
            const auto radius = parse_f64_hex(radius_hex);
            const auto strength = parse_f64_hex(strength_hex);
            const auto mask_id = mask_hex == "-" ? std::optional<std::string>(std::string{}) :
                decode_hex(mask_hex, 128U);
            std::array<std::optional<double>, 6> local_symmetry_values;
            if (stack.format_version >= 4U) {
                for (std::size_t index = 0U; index < local_symmetry_hex.size(); ++index) {
                    local_symmetry_values[index] = parse_f64_hex(local_symmetry_hex[index]);
                }
            }
            const bool local_symmetry_encoding_valid = stack.format_version < 4U ||
                std::all_of(local_symmetry_values.begin(), local_symmetry_values.end(),
                    [](const std::optional<double>& value) { return value.has_value(); });
            if (!stroke_id || !plane || !radius || !strength || !mask_id ||
                !local_symmetry_encoding_valid) {
                return core::Result<SculptStack>::failure(validation(
                    "serialized SculptStack stroke text or numeric encoding is invalid"));
            }
            SculptStroke stroke;
            stroke.identity = *stroke_id;
            stroke.mode = static_cast<SculptStrokeMode>(mode);
            stroke.symmetry = static_cast<SculptSymmetryMode>(symmetry);
            stroke.direction_mode = static_cast<SculptStrokeDirectionMode>(direction_mode);
            stroke.enabled = enabled != 0U;
            stroke.symmetry_plane_x = *plane;
            stroke.radius = *radius;
            stroke.strength = *strength;
            stroke.mask_layer_id = *mask_id;
            if (stack.format_version >= 4U) {
                stroke.symmetry_origin_local = {*local_symmetry_values[0],
                    *local_symmetry_values[1], *local_symmetry_values[2]};
                stroke.symmetry_axis_local = {*local_symmetry_values[3],
                    *local_symmetry_values[4], *local_symmetry_values[5]};
                stroke.radial_symmetry_count = radial_symmetry_count;
            }
            stroke.samples.reserve(static_cast<std::size_t>(sample_count));
            for (std::uint64_t sample_index = 0U; sample_index < sample_count; ++sample_index) {
                auto sample_record = read_record(input, "SAMPLE");
                std::string x_hex;
                std::string y_hex;
                std::string z_hex;
                std::string pressure_hex;
                if (!sample_record || !(*sample_record >> x_hex >> y_hex >> z_hex >> pressure_hex)) {
                    return core::Result<SculptStack>::failure(validation(
                        "serialized SculptStack sample record is invalid"));
                }
                const auto x = parse_f64_hex(x_hex);
                const auto y = parse_f64_hex(y_hex);
                const auto z = parse_f64_hex(z_hex);
                const auto pressure = parse_f64_hex(pressure_hex);
                if (!x || !y || !z || !pressure) {
                    return core::Result<SculptStack>::failure(validation(
                        "serialized SculptStack sample numeric encoding is invalid"));
                }
                SculptStrokeSample sample{{*x, *y, *z}, *pressure};
                bool saw_surface = false;
                bool saw_direction = false;
                std::string sample_tag;
                while (*sample_record >> sample_tag) {
                    if (sample_tag == "SURFACE" && stack.format_version >= 2U &&
                        !saw_surface) {
                        saw_surface = true;
                        std::uint64_t face_id = 0U;
                        std::uint64_t first_vertex = 0U;
                        std::uint64_t second_vertex = 0U;
                        std::uint64_t third_vertex = 0U;
                        std::string first_weight_hex;
                        std::string second_weight_hex;
                        std::string third_weight_hex;
                        if (!(*sample_record >> face_id >> first_vertex >> second_vertex >>
                            third_vertex >> first_weight_hex >> second_weight_hex >>
                            third_weight_hex)) {
                            return core::Result<SculptStack>::failure(validation(
                                "serialized SculptStack surface anchor is invalid"));
                        }
                        const auto first_weight = parse_f64_hex(first_weight_hex);
                        const auto second_weight = parse_f64_hex(second_weight_hex);
                        const auto third_weight = parse_f64_hex(third_weight_hex);
                        if (!first_weight || !second_weight || !third_weight) {
                            return core::Result<SculptStack>::failure(validation(
                                "serialized SculptStack surface weights are invalid"));
                        }
                        sample.surface_anchor = SculptSurfaceAnchor{
                            geometry::FaceId{face_id},
                            {geometry::VertexId{first_vertex}, geometry::VertexId{second_vertex},
                                geometry::VertexId{third_vertex}},
                            {*first_weight, *second_weight, *third_weight}};
                    } else if (sample_tag == "DIRECTION" &&
                        stack.format_version >= 3U && !saw_direction) {
                        saw_direction = true;
                        std::string axis_x_hex;
                        std::string axis_y_hex;
                        std::string axis_z_hex;
                        if (!(*sample_record >> axis_x_hex >> axis_y_hex >> axis_z_hex)) {
                            return core::Result<SculptStack>::failure(validation(
                                "serialized SculptStack captured brush direction is invalid"));
                        }
                        const auto axis_x = parse_f64_hex(axis_x_hex);
                        const auto axis_y = parse_f64_hex(axis_y_hex);
                        const auto axis_z = parse_f64_hex(axis_z_hex);
                        if (!axis_x || !axis_y || !axis_z) {
                            return core::Result<SculptStack>::failure(validation(
                                "serialized SculptStack captured brush direction encoding is invalid"));
                        }
                        sample.direction_axis = core::Vec3d{*axis_x, *axis_y, *axis_z};
                    } else {
                        return core::Result<SculptStack>::failure(validation(
                            "serialized SculptStack sample tag is invalid or unsupported"));
                    }
                }
                if (!record_finished(*sample_record)) {
                    return core::Result<SculptStack>::failure(validation(
                        "serialized SculptStack sample has trailing fields"));
                }
                stroke.samples.push_back(std::move(sample));
            }
            total_samples += static_cast<std::size_t>(sample_count);
            layer.strokes.push_back(std::move(stroke));
        }
        stack.layers.push_back(std::move(layer));
    }
    const auto end = read_line(input);
    input >> std::ws;
    if (!end || *end != "END_SCULPT_STACK" || !input.eof()) {
        return core::Result<SculptStack>::failure(validation(
            "serialized SculptStack has a missing terminator or trailing records"));
    }
    if (auto result = stack.validate(); !result) {
        return core::Result<SculptStack>::failure(result.error());
    }
    if (stack.serialize() != text) {
        return core::Result<SculptStack>::failure(validation(
            "serialized SculptStack is not in canonical form"));
    }
    return core::Result<SculptStack>::success(std::move(stack));
}



core::Result<SculptStack> make_sculpt_stack(
    const project::ProjectDocument& document,
    std::uint64_t mesh_asset) {
    const auto mesh = document.meshes().find(mesh_asset);
    if (mesh == document.meshes().end()) {
        return core::Result<SculptStack>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "SculptStack target mesh asset does not exist"));
    }
    SculptStack stack;
    stack.project_namespace = document.source_namespace();
    stack.mesh_asset = mesh_asset;
    stack.source.identity = mesh_identity(stack.project_namespace, mesh_asset);
    const auto binding = bind_bake_source(stack.source.identity, mesh->second);
    if (!binding) return core::Result<SculptStack>::failure(binding.error());
    stack.source = binding.value();
    return core::Result<SculptStack>::success(std::move(stack));
}


core::Result<void> stage_sculpt_stack(
    project::ProjectTransaction& transaction,
    const SculptStack& stack,
    std::optional<assets::Sha256Digest> expected_current_digest) {
    const project::ProjectDocument& staged = transaction.staged_document();
    if (staged.source_namespace() != stack.project_namespace) {
        return core::Result<void>::failure(stale(
            "SculptStack belongs to another project namespace"));
    }
    const auto mesh = staged.meshes().find(stack.mesh_asset);
    if (mesh == staged.meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "SculptStack transaction mesh does not exist"));
    }
    const auto binding = bind_bake_source(stack.source.identity, mesh->second);
    if (!binding) return core::Result<void>::failure(binding.error());
    if (binding.value() != stack.source) {
        return core::Result<void>::failure(stale(
            "SculptStack source binding is stale for the transaction mesh"));
    }
    if (auto result = stack.validate(); !result) return result;

    const auto existing = staged.sculpt_stacks().find(stack.mesh_asset);
    if (existing == staged.sculpt_stacks().end()) {
        if (expected_current_digest.has_value()) {
            return core::Result<void>::failure(stale(
                "expected SculptStack digest is absent from the staged project"));
        }
    } else if (!expected_current_digest.has_value() ||
        existing->second.digest != *expected_current_digest) {
        return core::Result<void>::failure(stale(
            "updating an existing SculptStack requires its current digest"));
    }

    const std::string serialized = stack.serialize();
    const auto digest = stack.digest();
    if (serialized.empty() || !digest) {
        return core::Result<void>::failure(validation(
            "cannot stage an invalid or oversized SculptStack"));
    }
    return transaction.set_sculpt_stack(stack.mesh_asset, serialized, digest.value());
}

std::string sculpt_stack_transaction_operation(const assets::Sha256Digest& stack_digest) {
    if (stack_digest.is_zero()) return {};
    return "sculpt-stack.v1." + stack_digest.hex();
}

} // namespace carto::production
