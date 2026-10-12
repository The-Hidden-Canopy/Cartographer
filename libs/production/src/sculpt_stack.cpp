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
constexpr std::string_view kEvaluationMagic = "CARTO_SCULPT_EVALUATION 1";
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

core::Vec3d add(core::Vec3d left, core::Vec3d right) {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

core::Vec3d subtract(core::Vec3d left, core::Vec3d right) {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

core::Vec3d multiply(core::Vec3d value, double scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}

double distance_squared(core::Vec3d left, core::Vec3d right) {
    const core::Vec3d delta = subtract(left, right);
    return core::dot(delta, delta);
}

bool uses_normal_displacement(SculptStrokeMode mode) {
    return mode == SculptStrokeMode::add || mode == SculptStrokeMode::subtract ||
        mode == SculptStrokeMode::inflate || mode == SculptStrokeMode::clay ||
        mode == SculptStrokeMode::crease || mode == SculptStrokeMode::surface_noise;
}

core::Vec3d reflect_x(core::Vec3d value) {
    value.x = -value.x;
    return value;
}

core::Vec3d brush_direction(
    SculptStrokeDirectionMode mode,
    const std::optional<core::Vec3d>& axis,
    core::Vec3d normal) {
    if (mode == SculptStrokeDirectionMode::surface_normal) return normal;
    if (!axis.has_value()) return {};
    if (mode == SculptStrokeDirectionMode::view_aligned) return axis->normalized();
    const core::Vec3d tangent = subtract(*axis, multiply(normal, core::dot(*axis, normal)));
    if (!tangent.finite() || tangent.length_squared() <= kMinNormalLengthSquared) return {};
    return tangent.normalized();
}

struct SculptBrushCopy {
    core::Vec3d center;
    std::optional<core::Vec3d> direction_axis;
    core::Vec3d grab_delta;
};

std::uint32_t symmetry_copy_count(const SculptStroke& stroke) {
    if (stroke.symmetry == SculptSymmetryMode::radial) {
        return stroke.radial_symmetry_count;
    }
    return stroke.symmetry == SculptSymmetryMode::none ? 1U : 2U;
}

core::Vec3d rotate_about_axis(core::Vec3d value, core::Vec3d unit_axis, double angle) {
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    return add(add(multiply(value, cosine),
            multiply(core::cross(unit_axis, value), sine)),
        multiply(unit_axis, core::dot(unit_axis, value) * (1.0 - cosine)));
}

core::Vec3d reflect_about_plane_vector(core::Vec3d value, core::Vec3d unit_normal) {
    return subtract(value, multiply(unit_normal, 2.0 * core::dot(value, unit_normal)));
}

core::Vec3d reflect_about_plane_point(
    core::Vec3d point,
    core::Vec3d origin,
    core::Vec3d unit_normal) {
    return subtract(point, multiply(unit_normal,
        2.0 * core::dot(subtract(point, origin), unit_normal)));
}

bool within_sample_bounds(core::Vec3d value) {
    return value.finite() && std::abs(value.x) <= kMaxSampleCoordinate &&
        std::abs(value.y) <= kMaxSampleCoordinate &&
        std::abs(value.z) <= kMaxSampleCoordinate;
}

bool same_vec3(core::Vec3d left, core::Vec3d right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

core::Result<std::vector<SculptBrushCopy>> make_sculpt_brush_copies(
    const SculptStroke& stroke,
    const SculptStrokeSample& sample,
    core::Vec3d grab_delta) {
    const std::uint32_t copy_count = symmetry_copy_count(stroke);
    std::vector<SculptBrushCopy> copies;
    copies.reserve(copy_count);

    const auto append = [&](core::Vec3d center, core::Vec3d transformed_delta,
                            core::Vec3d transformed_axis) -> bool {
        if (!within_sample_bounds(center) || !transformed_delta.finite() ||
            (sample.direction_axis.has_value() && !transformed_axis.finite())) {
            return false;
        }
        copies.push_back(SculptBrushCopy{
            center,
            sample.direction_axis.has_value()
                ? std::optional<core::Vec3d>(transformed_axis)
                : std::nullopt,
            transformed_delta});
        return true;
    };

    if (stroke.symmetry == SculptSymmetryMode::none) {
        if (!append(sample.position, grab_delta, sample.direction_axis.value_or(core::Vec3d{}))) {
            return core::Result<std::vector<SculptBrushCopy>>::failure(validation(
                "SculptStack brush copy is outside its finite coordinate bounds"));
        }
    } else if (stroke.symmetry == SculptSymmetryMode::mirror_x) {
        core::Vec3d center = sample.position;
        center.x = 2.0 * stroke.symmetry_plane_x - center.x;
        const core::Vec3d direction = sample.direction_axis.has_value()
            ? reflect_x(*sample.direction_axis) : core::Vec3d{};
        if (!append(sample.position, grab_delta, sample.direction_axis.value_or(core::Vec3d{})) ||
            !append(center, reflect_x(grab_delta), direction)) {
            return core::Result<std::vector<SculptBrushCopy>>::failure(validation(
                "SculptStack X-mirror brush copy is outside its finite coordinate bounds"));
        }
    } else if (stroke.symmetry == SculptSymmetryMode::mirror_plane) {
        const core::Vec3d normal = stroke.symmetry_axis_local.normalized();
        const core::Vec3d mirrored_center = reflect_about_plane_point(
            sample.position, stroke.symmetry_origin_local, normal);
        const core::Vec3d original_axis = sample.direction_axis.value_or(core::Vec3d{});
        if (!append(sample.position, grab_delta, original_axis) ||
            !append(mirrored_center, reflect_about_plane_vector(grab_delta, normal),
                reflect_about_plane_vector(original_axis, normal))) {
            return core::Result<std::vector<SculptBrushCopy>>::failure(validation(
                "SculptStack planar-mirror brush copy is outside its finite coordinate bounds"));
        }
    } else if (stroke.symmetry == SculptSymmetryMode::radial) {
        const core::Vec3d axis = stroke.symmetry_axis_local.normalized();
        const core::Vec3d original_axis = sample.direction_axis.value_or(core::Vec3d{});
        for (std::uint32_t copy_index = 0U; copy_index < copy_count; ++copy_index) {
            const double angle = 2.0 * std::numbers::pi_v<double> *
                static_cast<double>(copy_index) / static_cast<double>(copy_count);
            const core::Vec3d center = copy_index == 0U ? sample.position : add(
                stroke.symmetry_origin_local,
                rotate_about_axis(subtract(sample.position, stroke.symmetry_origin_local),
                    axis, angle));
            const core::Vec3d transformed_delta = copy_index == 0U
                ? grab_delta : rotate_about_axis(grab_delta, axis, angle);
            const core::Vec3d transformed_axis = copy_index == 0U
                ? original_axis : rotate_about_axis(original_axis, axis, angle);
            if (!append(center, transformed_delta, transformed_axis)) {
                return core::Result<std::vector<SculptBrushCopy>>::failure(validation(
                    "SculptStack radial brush copy is outside its finite coordinate bounds"));
            }
        }
    }
    return core::Result<std::vector<SculptBrushCopy>>::success(std::move(copies));
}

core::Vec3d face_normal(const geometry::EditableMesh& mesh, const geometry::Face& face) {
    core::Vec3d normal{};
    if (face.vertices.size() < 3U) return normal;
    for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
        const geometry::Vertex* current = mesh.find_vertex(face.vertices[index]);
        const geometry::Vertex* next = mesh.find_vertex(
            face.vertices[(index + 1U) % face.vertices.size()]);
        if (current == nullptr || next == nullptr) return {};
        normal.x += (current->position.y - next->position.y) *
            (current->position.z + next->position.z);
        normal.y += (current->position.z - next->position.z) *
            (current->position.x + next->position.x);
        normal.z += (current->position.x - next->position.x) *
            (current->position.y + next->position.y);
    }
    return normal.normalized();
}

std::map<geometry::VertexId, core::Vec3d> vertex_normals(
    const geometry::EditableMesh& mesh) {
    std::map<geometry::VertexId, core::Vec3d> normals;
    for (const geometry::Vertex& vertex : mesh.vertices_sorted()) {
        normals.emplace(vertex.id, core::Vec3d{});
    }
    for (const geometry::Face& face : mesh.faces_sorted()) {
        const core::Vec3d normal = face_normal(mesh, face);
        if (!normal.finite() || normal.length_squared() <= kMinNormalLengthSquared) continue;
        for (const geometry::VertexId id : face.vertices) {
            auto iterator = normals.find(id);
            if (iterator != normals.end()) {
                iterator->second = add(iterator->second, normal);
            }
        }
    }
    for (auto& [id, normal] : normals) {
        static_cast<void>(id);
        if (normal.finite() && normal.length_squared() > kMinNormalLengthSquared) {
            normal = normal.normalized();
        } else {
            normal = {};
        }
    }
    return normals;
}

core::Result<std::vector<double>> read_vertex_mask(
    const geometry::EditableMesh& mesh,
    std::string_view layer_id,
    const std::vector<geometry::Vertex>& vertices) {
    std::vector<double> values(vertices.size(), 0.0);
    if (layer_id.empty()) return core::Result<std::vector<double>>::success(std::move(values));
    const attributes::AttributeLayer* layer = mesh.attributes().find(layer_id);
    if (layer == nullptr) {
        return core::Result<std::vector<double>>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "SculptStack mask layer does not exist on its source mesh"));
    }
    if (layer->descriptor.domain != attributes::AttributeDomain::vertex ||
        (layer->descriptor.type != attributes::AttributeType::float32 &&
         layer->descriptor.type != attributes::AttributeType::float64) ||
        layer->values.size() != vertices.size()) {
        return core::Result<std::vector<double>>::failure(validation(
            "SculptStack mask must be a complete vertex float32 or float64 layer"));
    }
    for (std::size_t index = 0U; index < layer->values.size(); ++index) {
        double value = 0.0;
        if (const auto* f32 = std::get_if<float>(&layer->values[index])) value = *f32;
        else if (const auto* f64 = std::get_if<double>(&layer->values[index])) value = *f64;
        else {
            return core::Result<std::vector<double>>::failure(validation(
                "SculptStack mask contains a value with the wrong stored type"));
        }
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return core::Result<std::vector<double>>::failure(validation(
                "SculptStack mask values must remain in the closed interval [0, 1]"));
        }
        values[index] = value;
    }
    return core::Result<std::vector<double>>::success(std::move(values));
}

double radial_falloff(double distance, double radius) {
    const double remaining = 1.0 - distance / radius;
    if (remaining <= 0.0) return 0.0;
    const double t = std::min(1.0, remaining);
    return t * t * (3.0 - 2.0 * t);
}

class StrokeSpatialIndex final {
public:
    explicit StrokeSpatialIndex(const std::vector<geometry::Vertex>& vertices)
        : vertices_(vertices) {
        x_order_.reserve(vertices.size());
        for (std::size_t index = 0U; index < vertices.size(); ++index) {
            x_order_.push_back(index);
        }
        std::sort(x_order_.begin(), x_order_.end(), [this](std::size_t left, std::size_t right) {
            const double left_x = vertices_[left].position.x;
            const double right_x = vertices_[right].position.x;
            return left_x < right_x || (left_x == right_x && left < right);
        });
    }

    void append_candidates(
        core::Vec3d center,
        double radius,
        std::uint32_t generation,
        const std::vector<std::uint8_t>& eligible,
        std::vector<std::uint32_t>& seen,
        std::vector<std::size_t>& candidates) const {
        const double minimum_x = center.x - radius;
        const double maximum_x = center.x + radius;
        const auto first = std::lower_bound(
            x_order_.begin(), x_order_.end(), minimum_x,
            [this](std::size_t index, double x) {
                return vertices_[index].position.x < x;
            });
        const auto last = std::upper_bound(
            first, x_order_.end(), maximum_x,
            [this](double x, std::size_t index) {
                return x < vertices_[index].position.x;
            });
        for (auto iterator = first; iterator != last; ++iterator) {
            const std::size_t index = *iterator;
            if (eligible[index] == 0U || seen[index] == generation) continue;
            seen[index] = generation;
            candidates.push_back(index);
        }
    }

private:
    const std::vector<geometry::Vertex>& vertices_;
    std::vector<std::size_t> x_order_;
};

core::Result<void> apply_stroke(
    geometry::EditableMesh& mesh,
    const SculptLayer& layer,
    const SculptStroke& stroke) {
    if (!layer.visible || layer.weight == 0.0 || !stroke.enabled) {
        return core::Result<void>::success();
    }
    const auto vertices = mesh.vertices_sorted();
    const auto mask = read_vertex_mask(mesh, stroke.mask_layer_id, vertices);
    if (!mask) return core::Result<void>::failure(mask.error());
    const auto normals = vertex_normals(mesh);
    const bool has_neighbor_kernel = stroke.mode == SculptStrokeMode::clay ||
        stroke.mode == SculptStrokeMode::smooth || stroke.mode == SculptStrokeMode::polish;
    std::map<geometry::VertexId, std::size_t> vertex_indices;
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        vertex_indices.emplace(vertices[index].id, index);
    }
    std::vector<std::vector<std::size_t>> neighbors(vertices.size());
    if (has_neighbor_kernel) {
        for (const geometry::Face& face : mesh.faces_sorted()) {
            for (std::size_t corner = 0U; corner < face.vertices.size(); ++corner) {
                const geometry::VertexId first = face.vertices[corner];
                const geometry::VertexId second = face.vertices[
                    (corner + 1U) % face.vertices.size()];
                const auto first_index = vertex_indices.find(first);
                const auto second_index = vertex_indices.find(second);
                if (first_index == vertex_indices.end() || second_index == vertex_indices.end()) {
                    return core::Result<void>::failure(stale(
                        "SculptStack brush adjacency references a missing vertex"));
                }
                if (first_index->second == second_index->second) continue;
                neighbors[first_index->second].push_back(second_index->second);
                neighbors[second_index->second].push_back(first_index->second);
            }
        }
        for (auto& adjacent : neighbors) {
            std::sort(adjacent.begin(), adjacent.end(), [&](std::size_t left, std::size_t right) {
                return vertices[left].id < vertices[right].id;
            });
            adjacent.erase(std::unique(adjacent.begin(), adjacent.end()), adjacent.end());
        }
    }
    std::vector<core::Vec3d> neighbor_averages(vertices.size());
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        if (neighbors[index].empty()) continue;
        core::Vec3d average{};
        std::size_t count = 0U;
        for (const std::size_t adjacent : neighbors[index]) {
            ++count;
            const double inverse_count = 1.0 / static_cast<double>(count);
            average = add(average, multiply(
                subtract(vertices[adjacent].position, average), inverse_count));
            if (!average.finite()) {
                return core::Result<void>::failure(validation(
                    "SculptStack neighbor average is not finite"));
            }
        }
        neighbor_averages[index] = average;
    }

    std::vector<std::uint8_t> eligible(vertices.size(), 0U);
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        const auto normal = normals.find(vertices[index].id);
        if (normal != normals.end() &&
            normal->second.length_squared() > kMinNormalLengthSquared &&
            mask.value()[index] < 1.0) {
            eligible[index] = 1U;
        }
    }

    const std::size_t index_threshold = static_cast<std::size_t>(
        std::bit_width(vertices.size()));
    const bool use_spatial_index = stroke.samples.size() >= index_threshold;
    std::optional<StrokeSpatialIndex> spatial_index;
    if (use_spatial_index) spatial_index.emplace(vertices);
    std::vector<std::uint32_t> seen(vertices.size(), 0U);
    std::vector<std::size_t> candidates;
    std::vector<double> accumulated_falloff(vertices.size(), 0.0);
    std::vector<core::Vec3d> accumulated_displacement(vertices.size());
    std::vector<core::Vec3d> accumulated_brush_direction(vertices.size());

    struct BrushPlane {
        core::Vec3d point;
        core::Vec3d normal;
    };
    const auto find_plane = [&](core::Vec3d center) -> std::optional<BrushPlane> {
        const double radius_squared = stroke.radius * stroke.radius;
        std::optional<std::size_t> nearest;
        double nearest_distance_squared = std::numeric_limits<double>::infinity();
        for (const std::size_t index : candidates) {
            if (eligible[index] == 0U) continue;
            const double distance = distance_squared(vertices[index].position, center);
            if (!std::isfinite(distance) || distance > radius_squared) continue;
            const auto normal = normals.find(vertices[index].id);
            if (normal == normals.end() ||
                normal->second.length_squared() <= kMinNormalLengthSquared) continue;
            if (!nearest.has_value() || distance < nearest_distance_squared ||
                (distance == nearest_distance_squared &&
                    vertices[index].id < vertices[*nearest].id)) {
                nearest = index;
                nearest_distance_squared = distance;
            }
        }
        if (!nearest.has_value()) return std::nullopt;
        return BrushPlane{vertices[*nearest].position, normals.at(vertices[*nearest].id)};
    };

    for (std::size_t sample_index = 0U; sample_index < stroke.samples.size(); ++sample_index) {
        const SculptStrokeSample& sample = stroke.samples[sample_index];
        core::Vec3d grab_delta{};
        if (stroke.mode == SculptStrokeMode::grab && sample_index > 0U) {
            grab_delta = subtract(sample.position, stroke.samples[sample_index - 1U].position);
        }
        const auto copies_result = make_sculpt_brush_copies(stroke, sample, grab_delta);
        if (!copies_result) return core::Result<void>::failure(copies_result.error());
        const std::vector<SculptBrushCopy>& copies = copies_result.value();
        std::uint32_t generation = static_cast<std::uint32_t>(sample_index + 1U);
        if (generation == 0U) generation = 1U;
        if (spatial_index.has_value()) {
            candidates.clear();
            for (const SculptBrushCopy& copy : copies) {
                spatial_index->append_candidates(
                    copy.center, stroke.radius, generation, eligible, seen, candidates);
            }
        } else {
            candidates.clear();
            for (std::size_t index = 0U; index < vertices.size(); ++index) {
                if (eligible[index] != 0U) candidates.push_back(index);
            }
        }

        const bool plane_mode = stroke.mode == SculptStrokeMode::flatten ||
            stroke.mode == SculptStrokeMode::scrape;
        std::vector<std::optional<BrushPlane>> planes(copies.size());
        if (plane_mode) {
            for (std::size_t copy_index = 0U; copy_index < copies.size(); ++copy_index) {
                planes[copy_index] = find_plane(copies[copy_index].center);
            }
        }

        const auto accumulate = [&](std::size_t vertex_index) {
            const geometry::Vertex& vertex = vertices[vertex_index];
            std::array<double, kMaxSculptRadialSymmetryCopies> copy_falloffs{};
            double falloff_sum = 0.0;
            double falloff = 0.0;
            for (std::size_t copy_index = 0U; copy_index < copies.size(); ++copy_index) {
                const double copy_falloff = radial_falloff(
                    std::sqrt(distance_squared(vertex.position, copies[copy_index].center)),
                    stroke.radius);
                copy_falloffs[copy_index] = copy_falloff;
                falloff_sum += copy_falloff;
                falloff = std::max(falloff, copy_falloff);
            }
            if (falloff <= 0.0) return;
            const double weighted_falloff = falloff * sample.pressure;
            accumulated_falloff[vertex_index] += weighted_falloff;

            if (uses_normal_displacement(stroke.mode)) {
                const core::Vec3d normal = normals.at(vertex.id);
                if (falloff_sum > 0.0) {
                    core::Vec3d weighted_direction{};
                    for (std::size_t copy_index = 0U; copy_index < copies.size(); ++copy_index) {
                        weighted_direction = add(weighted_direction, multiply(
                            brush_direction(stroke.direction_mode,
                                copies[copy_index].direction_axis, normal),
                            copy_falloffs[copy_index]));
                    }
                    accumulated_brush_direction[vertex_index] = add(
                        accumulated_brush_direction[vertex_index],
                        multiply(weighted_direction, weighted_falloff / falloff_sum));
                }
            }

            if (stroke.mode == SculptStrokeMode::grab) {
                if (sample_index == 0U) return;
                if (falloff_sum > 0.0) {
                    core::Vec3d weighted_delta{};
                    for (std::size_t copy_index = 0U; copy_index < copies.size(); ++copy_index) {
                        weighted_delta = add(weighted_delta, multiply(
                            copies[copy_index].grab_delta, copy_falloffs[copy_index]));
                    }
                    accumulated_displacement[vertex_index] = add(
                        accumulated_displacement[vertex_index],
                        multiply(weighted_delta, sample.pressure * falloff / falloff_sum));
                }
                return;
            }
            if (stroke.mode == SculptStrokeMode::pinch ||
                stroke.mode == SculptStrokeMode::crease) {
                const core::Vec3d normal = normals.at(vertex.id);
                const auto toward = [&](core::Vec3d center) {
                    const core::Vec3d radial = subtract(center, vertex.position);
                    const core::Vec3d tangent = subtract(
                        radial, multiply(normal, core::dot(radial, normal)));
                    if (!tangent.finite() ||
                        tangent.length_squared() <= kMinNormalLengthSquared) return core::Vec3d{};
                    return tangent.normalized();
                };
                if (falloff_sum > 0.0) {
                    core::Vec3d direction{};
                    for (std::size_t copy_index = 0U; copy_index < copies.size(); ++copy_index) {
                        direction = add(direction, multiply(
                            toward(copies[copy_index].center), copy_falloffs[copy_index]));
                    }
                    accumulated_displacement[vertex_index] = add(
                        accumulated_displacement[vertex_index],
                        multiply(direction, sample.pressure * falloff / falloff_sum));
                }
                return;
            }
            if (plane_mode) {
                double plane_falloff_sum = 0.0;
                core::Vec3d weighted_correction{};
                for (std::size_t copy_index = 0U; copy_index < copies.size(); ++copy_index) {
                    if (!planes[copy_index].has_value() || copy_falloffs[copy_index] <= 0.0) {
                        continue;
                    }
                    plane_falloff_sum += copy_falloffs[copy_index];
                    const BrushPlane& brush_plane = *planes[copy_index];
                    const double distance = core::dot(
                        subtract(vertex.position, brush_plane.point), brush_plane.normal);
                    const double correction = stroke.mode == SculptStrokeMode::scrape
                        ? -std::max(0.0, distance) : -distance;
                    weighted_correction = add(weighted_correction, multiply(
                        brush_plane.normal, correction * copy_falloffs[copy_index]));
                }
                if (plane_falloff_sum <= 0.0) return;
                accumulated_displacement[vertex_index] = add(
                    accumulated_displacement[vertex_index],
                    multiply(weighted_correction,
                        sample.pressure * falloff / plane_falloff_sum));
                return;
            }
            if (stroke.mode != SculptStrokeMode::add &&
                stroke.mode != SculptStrokeMode::subtract &&
                stroke.mode != SculptStrokeMode::inflate &&
                stroke.mode != SculptStrokeMode::clay &&
                stroke.mode != SculptStrokeMode::smooth &&
                stroke.mode != SculptStrokeMode::polish &&
                stroke.mode != SculptStrokeMode::surface_noise) {
                return;
            }
        };

        for (const std::size_t index : candidates) accumulate(index);
    }

    geometry::MeshPatch patch;
    patch.expected_revision = mesh.revision();
    std::uint64_t identity_hash = 14695981039346656037ULL;
    for (const char character : stroke.identity) {
        const auto byte = static_cast<unsigned char>(character);
        identity_hash ^= byte;
        identity_hash *= 1099511628211ULL;
    }
    for (std::size_t vertex_index = 0U; vertex_index < vertices.size(); ++vertex_index) {
        const geometry::Vertex& vertex = vertices[vertex_index];
        const auto normal = normals.find(vertex.id);
        if (eligible[vertex_index] == 0U || normal == normals.end()) continue;
        const double unmasked_weight = 1.0 - mask.value()[vertex_index];
        core::Vec3d after = vertex.position;
        if (stroke.mode == SculptStrokeMode::grab ||
            stroke.mode == SculptStrokeMode::pinch) {
            const double scale = stroke.strength * layer.weight * unmasked_weight;
            after = add(vertex.position, multiply(
                accumulated_displacement[vertex_index], scale));
        } else if (stroke.mode == SculptStrokeMode::flatten ||
            stroke.mode == SculptStrokeMode::scrape) {
            const double total_weight = accumulated_falloff[vertex_index];
            if (total_weight <= 0.0) continue;
            const double alpha = std::clamp(
                stroke.strength * layer.weight * unmasked_weight *
                    std::min(1.0, total_weight),
                0.0, 1.0);
            after = add(vertex.position, multiply(
                accumulated_displacement[vertex_index], alpha / total_weight));
        } else if (stroke.mode == SculptStrokeMode::smooth ||
            stroke.mode == SculptStrokeMode::polish) {
            if (neighbors[vertex_index].empty()) continue;
            const double alpha = std::clamp(
                accumulated_falloff[vertex_index] * stroke.strength * layer.weight *
                    unmasked_weight,
                0.0, 1.0);
            core::Vec3d relaxation = subtract(
                neighbor_averages[vertex_index], vertex.position);
            if (stroke.mode == SculptStrokeMode::polish) {
                relaxation = subtract(relaxation, multiply(
                    normal->second, core::dot(relaxation, normal->second)));
            }
            after = add(vertex.position, multiply(relaxation, alpha));
        } else if (stroke.mode == SculptStrokeMode::surface_noise) {
            std::uint64_t value = identity_hash ^ vertices[vertex_index].id.value;
            value += 0x9e3779b97f4a7c15ULL;
            value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
            value ^= value >> 31U;
            const double unit_noise = static_cast<double>(value >> 11U) /
                9007199254740992.0;
            const double signed_noise = unit_noise * 2.0 - 1.0;
            after = add(vertex.position, multiply(
                accumulated_brush_direction[vertex_index],
                signed_noise * layer.weight * stroke.strength * unmasked_weight));
        } else {
            const double mode_sign = stroke.mode == SculptStrokeMode::subtract ||
                stroke.mode == SculptStrokeMode::crease ? -1.0 : 1.0;
            after = add(vertex.position, multiply(
                accumulated_brush_direction[vertex_index],
                layer.weight * stroke.strength * mode_sign * unmasked_weight));
            if (stroke.mode == SculptStrokeMode::clay && !neighbors[vertex_index].empty()) {
                const double amount = accumulated_falloff[vertex_index] * layer.weight *
                    stroke.strength * unmasked_weight;
                const core::Vec3d toward_average = subtract(
                    neighbor_averages[vertex_index], vertex.position);
                const core::Vec3d tangent = subtract(
                    toward_average,
                    multiply(normal->second, core::dot(toward_average, normal->second)));
                const double relaxation = std::min(0.25, std::abs(amount) / stroke.radius);
                after = add(after, multiply(tangent, relaxation));
            }
            if (stroke.mode == SculptStrokeMode::crease) {
                const double tangent_scale = stroke.strength * layer.weight * unmasked_weight;
                after = add(after, multiply(
                    accumulated_displacement[vertex_index], tangent_scale));
            }
        }
        if (after.x == vertex.position.x && after.y == vertex.position.y &&
            after.z == vertex.position.z) continue;
        if (!after.finite()) {
            return core::Result<void>::failure(validation(
                "SculptStack stroke produced a non-finite vertex position"));
        }
        patch.vertex_changes.push_back(geometry::VertexChange{
            vertex.id, vertex.position, after});
    }
    if (patch.vertex_changes.empty()) return core::Result<void>::success();
    return mesh.apply_patch(patch);
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
        position = add(position, multiply(vertex->position, anchor.barycentric[index]));
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

core::Result<void> SculptEvaluationArtifact::refresh_digests() {
    if (auto result = stack.validate(); !result) return result;
    if (algorithm != "carto.sculpt.cpu.brush-kernels.v5" ||
        evidence_class != "carto.cpu-reference" || vertices.empty() ||
        vertices.size() > kMaxSculptVertices || !std::isfinite(maximum_displacement) ||
        maximum_displacement < 0.0) {
        return core::Result<void>::failure(validation(
            "SculptStack evaluation artifact identity or output bounds are invalid"));
    }
    const auto settings = stack.digest();
    if (!settings) return core::Result<void>::failure(settings.error());
    settings_digest = settings.value();
    byte_size = static_cast<std::uint64_t>(vertices.size()) * 32ULL;

    CanonicalBytes output;
    output.text(kEvaluationMagic);
    output.u64(static_cast<std::uint64_t>(vertices.size()));
    geometry::VertexId previous{};
    for (const geometry::Vertex& vertex : vertices) {
        if (!vertex.id || (previous && !(previous < vertex.id)) || !vertex.position.finite()) {
            return core::Result<void>::failure(validation(
                "SculptStack evaluation output has duplicate identities or invalid positions"));
        }
        previous = vertex.id;
        output.u64(vertex.id.value);
        output.f64(vertex.position.x);
        output.f64(vertex.position.y);
        output.f64(vertex.position.z);
    }
    output_digest = output.hash();

    CanonicalBytes artifact;
    artifact.text("CARTO_SCULPT_EVALUATION_ARTIFACT 1");
    artifact.text(stack.serialize());
    artifact.text(algorithm);
    artifact.text(evidence_class);
    artifact.digest(settings_digest);
    artifact.digest(output_digest);
    artifact.u64(byte_size);
    artifact.f64(maximum_displacement);
    artifact.u64(static_cast<std::uint64_t>(vertices.size()));
    for (const geometry::Vertex& vertex : vertices) {
        artifact.u64(vertex.id.value);
        artifact.f64(vertex.position.x);
        artifact.f64(vertex.position.y);
        artifact.f64(vertex.position.z);
    }
    artifact_digest = artifact.hash();
    return core::Result<void>::success();
}

core::Result<void> SculptEvaluationArtifact::validate(
    const geometry::EditableMesh& base_mesh) const {
    if (auto result = stack.validate(); !result) return result;
    const auto current = bind_bake_source(stack.source.identity, base_mesh);
    if (!current) return core::Result<void>::failure(current.error());
    if (current.value() != stack.source) {
        return core::Result<void>::failure(stale(
            "SculptStack evaluation source revision or content digest is stale"));
    }
    const auto base_vertices = base_mesh.vertices_sorted();
    if (base_vertices.size() != vertices.size()) {
        return core::Result<void>::failure(validation(
            "SculptStack evaluation output vertex count differs from the base mesh"));
    }
    geometry::MeshPatch patch;
    patch.expected_revision = base_mesh.revision();
    double computed_maximum_displacement = 0.0;
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        if (base_vertices[index].id != vertices[index].id) {
            return core::Result<void>::failure(validation(
                "SculptStack evaluation output does not preserve stable vertex identity"));
        }
        const core::Vec3d delta = subtract(vertices[index].position, base_vertices[index].position);
        computed_maximum_displacement = std::max(
            computed_maximum_displacement, std::sqrt(delta.length_squared()));
        if (vertices[index].position.x != base_vertices[index].position.x ||
            vertices[index].position.y != base_vertices[index].position.y ||
            vertices[index].position.z != base_vertices[index].position.z) {
            patch.vertex_changes.push_back(geometry::VertexChange{
                vertices[index].id, base_vertices[index].position, vertices[index].position});
        }
    }
    if (computed_maximum_displacement != maximum_displacement) {
        return core::Result<void>::failure(validation(
            "SculptStack maximum displacement metric does not match its output"));
    }
    if (!patch.vertex_changes.empty()) {
        geometry::EditableMesh candidate = base_mesh;
        if (auto result = candidate.apply_patch(patch); !result) return result;
    }
    SculptEvaluationArtifact rebuilt = *this;
    if (auto result = rebuilt.refresh_digests(); !result) return result;
    if (rebuilt.settings_digest != settings_digest || rebuilt.output_digest != output_digest ||
        rebuilt.artifact_digest != artifact_digest || rebuilt.byte_size != byte_size) {
        return core::Result<void>::failure(validation(
            "SculptStack evaluation digest does not match its source and output"));
    }
    return core::Result<void>::success();
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

core::Result<SculptEvaluationArtifact> compile_sculpt_stack(
    const SculptStack& stack,
    const geometry::EditableMesh& base_mesh) {
    if (auto result = stack.validate(); !result) {
        return core::Result<SculptEvaluationArtifact>::failure(result.error());
    }
    if (base_mesh.vertex_count() == 0U || base_mesh.vertex_count() > kMaxSculptVertices) {
        return core::Result<SculptEvaluationArtifact>::failure(validation(
            "SculptStack base mesh vertex count is outside the supported bound"));
    }
    const auto current = bind_bake_source(stack.source.identity, base_mesh);
    if (!current) return core::Result<SculptEvaluationArtifact>::failure(current.error());
    if (current.value() != stack.source) {
        return core::Result<SculptEvaluationArtifact>::failure(stale(
            "SculptStack is bound to another mesh revision or content digest"));
    }
    std::uint64_t sample_copy_count = 0U;
    for (const SculptLayer& layer : stack.layers) {
        if (!layer.visible || layer.weight == 0.0) continue;
        for (const SculptStroke& stroke : layer.strokes) {
            if (!stroke.enabled) continue;
            const std::uint64_t stroke_work =
                static_cast<std::uint64_t>(stroke.samples.size()) *
                symmetry_copy_count(stroke);
            if (stroke_work > kMaxSculptKernelPairs - sample_copy_count) {
                return core::Result<SculptEvaluationArtifact>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "SculptStack symmetry evaluation exceeds its bounded work budget"));
            }
            sample_copy_count += stroke_work;
        }
    }
    if (sample_copy_count != 0U && static_cast<std::uint64_t>(base_mesh.vertex_count()) >
        kMaxSculptKernelPairs / sample_copy_count) {
        return core::Result<SculptEvaluationArtifact>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "SculptStack evaluation exceeds its bounded vertex-symmetry work budget"));
    }

    geometry::EditableMesh evaluated = base_mesh;
    for (const SculptLayer& layer : stack.layers) {
        for (const SculptStroke& stroke : layer.strokes) {
            for (const SculptStrokeSample& sample : stroke.samples) {
                if (!sample.surface_anchor.has_value()) continue;
                const auto anchored_position = resolve_sculpt_surface_anchor(
                    base_mesh, *sample.surface_anchor);
                if (!anchored_position) {
                    return core::Result<SculptEvaluationArtifact>::failure(
                        anchored_position.error().with_context(
                            "SculptStack surface-constrained sample"));
                }
                if (anchored_position.value().x != sample.position.x ||
                    anchored_position.value().y != sample.position.y ||
                    anchored_position.value().z != sample.position.z) {
                    return core::Result<SculptEvaluationArtifact>::failure(validation(
                        "SculptStack sample position does not match its source surface anchor"));
                }
            }
            if (auto result = apply_stroke(evaluated, layer, stroke); !result) {
                return core::Result<SculptEvaluationArtifact>::failure(result.error().with_context(
                    "SculptStack layer " + layer.identity + " stroke " + stroke.identity));
            }
        }
    }
    SculptEvaluationArtifact artifact;
    artifact.stack = stack;
    artifact.vertices = evaluated.vertices_sorted();
    const auto base_vertices = base_mesh.vertices_sorted();
    for (std::size_t index = 0U; index < artifact.vertices.size(); ++index) {
        const core::Vec3d delta = subtract(
            artifact.vertices[index].position, base_vertices[index].position);
        artifact.maximum_displacement = std::max(
            artifact.maximum_displacement, std::sqrt(delta.length_squared()));
    }
    if (auto result = artifact.refresh_digests(); !result) {
        return core::Result<SculptEvaluationArtifact>::failure(result.error());
    }
    if (auto result = artifact.validate(base_mesh); !result) {
        return core::Result<SculptEvaluationArtifact>::failure(result.error());
    }
    return core::Result<SculptEvaluationArtifact>::success(std::move(artifact));
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
