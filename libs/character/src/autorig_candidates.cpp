#include <carto/character/autorig_candidates.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <span>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

assets::Sha256Digest hash_text(const std::string& text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

double distance(core::Vec3d a, core::Vec3d b) noexcept {
    return std::hypot(std::hypot(a.x - b.x, a.y - b.y), a.z - b.z);
}

double vector_dot(core::Vec3d a, core::Vec3d b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double point_segment_distance(
    core::Vec3d point,
    core::Vec3d start,
    core::Vec3d end) noexcept {
    const core::Vec3d segment{end.x - start.x, end.y - start.y, end.z - start.z};
    const double length_squared = vector_dot(segment, segment);
    if (!std::isfinite(length_squared))
        return std::numeric_limits<double>::quiet_NaN();
    if (length_squared <= 1.0e-24) return distance(point, start);
    const core::Vec3d offset{point.x - start.x, point.y - start.y, point.z - start.z};
    const double t = std::clamp(vector_dot(offset, segment) / length_squared, 0.0, 1.0);
    return distance(point, {start.x + t * segment.x,
        start.y + t * segment.y, start.z + t * segment.z});
}

double segment_distance(
    core::Vec3d first_start,
    core::Vec3d first_end,
    core::Vec3d second_start,
    core::Vec3d second_end) noexcept {
    const double endpoint_distances[] = {
        point_segment_distance(first_start, second_start, second_end),
        point_segment_distance(first_end, second_start, second_end),
        point_segment_distance(second_start, first_start, first_end),
        point_segment_distance(second_end, first_start, first_end)};
    if (std::any_of(std::begin(endpoint_distances), std::end(endpoint_distances),
            [](double value) { return !std::isfinite(value); }))
        return std::numeric_limits<double>::quiet_NaN();
    double result = *std::min_element(
        std::begin(endpoint_distances), std::end(endpoint_distances));
    const core::Vec3d first{first_end.x - first_start.x,
        first_end.y - first_start.y, first_end.z - first_start.z};
    const core::Vec3d second{second_end.x - second_start.x,
        second_end.y - second_start.y, second_end.z - second_start.z};
    const core::Vec3d offset{first_start.x - second_start.x,
        first_start.y - second_start.y, first_start.z - second_start.z};
    const double first_length = vector_dot(first, first);
    const double second_length = vector_dot(second, second);
    const double cross_term = vector_dot(first, second);
    const double denominator = first_length * second_length - cross_term * cross_term;
    if (!std::isfinite(first_length) || !std::isfinite(second_length) ||
        !std::isfinite(cross_term) || !std::isfinite(denominator))
        return std::numeric_limits<double>::quiet_NaN();
    if (denominator > 1.0e-24) {
        const double first_parameter =
            (cross_term * vector_dot(second, offset) - second_length * vector_dot(first, offset)) /
            denominator;
        const double second_parameter =
            (first_length * vector_dot(second, offset) - cross_term * vector_dot(first, offset)) /
            denominator;
        if (first_parameter >= 0.0 && first_parameter <= 1.0 &&
            second_parameter >= 0.0 && second_parameter <= 1.0) {
            const core::Vec3d first_point{
                first_start.x + first_parameter * first.x,
                first_start.y + first_parameter * first.y,
                first_start.z + first_parameter * first.z};
            const core::Vec3d second_point{
                second_start.x + second_parameter * second.x,
                second_start.y + second_parameter * second.y,
                second_start.z + second_parameter * second.z};
            result = std::min(result, distance(first_point, second_point));
        }
    }
    return result;
}

struct SurfaceBounds final {
    core::Vec3d minimum{};
    core::Vec3d maximum{};

    [[nodiscard]] bool intersects_segment(
        core::Vec3d start, core::Vec3d end) const noexcept {
        const std::array<double, 3U> origin{start.x, start.y, start.z};
        const std::array<double, 3U> delta{
            end.x - start.x, end.y - start.y, end.z - start.z};
        const std::array<double, 3U> low{minimum.x, minimum.y, minimum.z};
        const std::array<double, 3U> high{maximum.x, maximum.y, maximum.z};
        double t_min = 0.0;
        double t_max = 1.0;
        for (std::size_t axis = 0U; axis < origin.size(); ++axis) {
            if (delta[axis] == 0.0) {
                if (origin[axis] < low[axis] || origin[axis] > high[axis]) return false;
                continue;
            }
            double first = (low[axis] - origin[axis]) / delta[axis];
            double second = (high[axis] - origin[axis]) / delta[axis];
            if (first > second) std::swap(first, second);
            t_min = std::max(t_min, first);
            t_max = std::min(t_max, second);
            if (t_min > t_max) return false;
        }
        return true;
    }

    void include(const SurfaceBounds& other) noexcept {
        minimum.x = std::min(minimum.x, other.minimum.x);
        minimum.y = std::min(minimum.y, other.minimum.y);
        minimum.z = std::min(minimum.z, other.minimum.z);
        maximum.x = std::max(maximum.x, other.maximum.x);
        maximum.y = std::max(maximum.y, other.maximum.y);
        maximum.z = std::max(maximum.z, other.maximum.z);
    }
};

struct SurfaceTriangleRecord final {
    SurfaceBounds bounds;
    core::Vec3d centroid;
    std::uint32_t triangle_index = 0U;
};

struct SurfaceBvhNode final {
    SurfaceBounds bounds;
    std::uint32_t first = 0U;
    std::uint32_t second = 0U;
    std::uint32_t begin = 0U;
    std::uint32_t count = 0U;
};

enum class SurfaceQueryResult : std::uint8_t {
    clear,
    crossing,
    budget_exceeded,
};

class SourceSurfaceIndex final {
public:
    static constexpr std::size_t kHardMaxTriangles = 300'000U;
    static constexpr std::size_t kHardMaxTriangleTests = 8'000'000U;

    explicit SourceSurfaceIndex(const geometry::CompiledMesh& mesh) : mesh_(mesh) {
        const std::size_t triangle_count = mesh_.indices.size() / 3U;
        if (!mesh_.valid() || triangle_count == 0U ||
            triangle_count > kHardMaxTriangles) return;
        triangles_.reserve(triangle_count);
        order_.reserve(triangle_count);
        for (std::size_t triangle = 0U; triangle < triangle_count; ++triangle) {
            const std::size_t offset = triangle * 3U;
            const auto first = mesh_.positions[mesh_.indices[offset]];
            const auto second = mesh_.positions[mesh_.indices[offset + 1U]];
            const auto third = mesh_.positions[mesh_.indices[offset + 2U]];
            SurfaceBounds bounds{
                {std::min({first.x, second.x, third.x}),
                 std::min({first.y, second.y, third.y}),
                 std::min({first.z, second.z, third.z})},
                {std::max({first.x, second.x, third.x}),
                 std::max({first.y, second.y, third.y}),
                 std::max({first.z, second.z, third.z})}};
            triangles_.push_back({bounds,
                {(first.x + second.x + third.x) / 3.0,
                 (first.y + second.y + third.y) / 3.0,
                 (first.z + second.z + third.z) / 3.0},
                static_cast<std::uint32_t>(triangle)});
            order_.push_back(static_cast<std::uint32_t>(triangle));
        }
        root_ = build_node(0U, static_cast<std::uint32_t>(order_.size()));
        valid_ = !nodes_.empty();
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    [[nodiscard]] SurfaceQueryResult query_segment(
        core::Vec3d start,
        core::Vec3d end,
        std::size_t& triangle_tests) const {
        if (!valid_) return SurfaceQueryResult::budget_exceeded;
        std::array<std::uint32_t, 64U> pending{};
        std::size_t pending_count = 1U;
        pending[0U] = root_;
        while (pending_count != 0U) {
            const std::uint32_t node_index = pending[--pending_count];
            const auto& node = nodes_[node_index];
            if (!node.bounds.intersects_segment(start, end)) continue;
            if (node.count != 0U) {
                for (std::uint32_t offset = 0U; offset < node.count; ++offset) {
                    if (triangle_tests >= kHardMaxTriangleTests)
                        return SurfaceQueryResult::budget_exceeded;
                    ++triangle_tests;
                    const auto& record = triangles_[order_[node.begin + offset]];
                    const std::size_t triangle_offset =
                        static_cast<std::size_t>(record.triangle_index) * 3U;
                    const auto first = mesh_.positions[mesh_.indices[triangle_offset]];
                    const auto second = mesh_.positions[mesh_.indices[triangle_offset + 1U]];
                    const auto third = mesh_.positions[mesh_.indices[triangle_offset + 2U]];
                    if (segment_crosses_triangle_interior(
                            start, end, first, second, third))
                        return SurfaceQueryResult::crossing;
                }
            } else {
                // Push right before left so traversal order is stable and left-first.
                if (pending_count + 2U > pending.size())
                    return SurfaceQueryResult::budget_exceeded;
                pending[pending_count++] = node.second;
                pending[pending_count++] = node.first;
            }
        }
        return SurfaceQueryResult::clear;
    }

private:
    static constexpr std::uint32_t kLeafTriangleCount = 8U;
    static constexpr double kEndpointParameterTolerance = 1.0e-9;

    [[nodiscard]] static double axis_value(core::Vec3d value, std::size_t axis) noexcept {
        return axis == 0U ? value.x : axis == 1U ? value.y : value.z;
    }

    [[nodiscard]] std::uint32_t build_node(std::uint32_t begin, std::uint32_t end) {
        SurfaceBvhNode node;
        const double infinity = std::numeric_limits<double>::infinity();
        node.bounds = {{infinity, infinity, infinity},
                       {-infinity, -infinity, -infinity}};
        core::Vec3d centroid_minimum{infinity, infinity, infinity};
        core::Vec3d centroid_maximum{-infinity, -infinity, -infinity};
        for (std::uint32_t index = begin; index < end; ++index) {
            const auto& triangle = triangles_[order_[index]];
            node.bounds.include(triangle.bounds);
            centroid_minimum.x = std::min(centroid_minimum.x, triangle.centroid.x);
            centroid_minimum.y = std::min(centroid_minimum.y, triangle.centroid.y);
            centroid_minimum.z = std::min(centroid_minimum.z, triangle.centroid.z);
            centroid_maximum.x = std::max(centroid_maximum.x, triangle.centroid.x);
            centroid_maximum.y = std::max(centroid_maximum.y, triangle.centroid.y);
            centroid_maximum.z = std::max(centroid_maximum.z, triangle.centroid.z);
        }
        const std::uint32_t node_index = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back(node);
        const std::uint32_t count = end - begin;
        if (count <= kLeafTriangleCount) {
            nodes_[node_index].begin = begin;
            nodes_[node_index].count = count;
            return node_index;
        }
        const std::array<double, 3U> extent{
            centroid_maximum.x - centroid_minimum.x,
            centroid_maximum.y - centroid_minimum.y,
            centroid_maximum.z - centroid_minimum.z};
        std::size_t axis = 0U;
        if (extent[1U] > extent[axis]) axis = 1U;
        if (extent[2U] > extent[axis]) axis = 2U;
        std::stable_sort(order_.begin() + begin, order_.begin() + end,
            [this, axis](std::uint32_t left, std::uint32_t right) {
                const double left_center = axis_value(triangles_[left].centroid, axis);
                const double right_center = axis_value(triangles_[right].centroid, axis);
                return left_center != right_center
                    ? left_center < right_center
                    : triangles_[left].triangle_index < triangles_[right].triangle_index;
            });
        const std::uint32_t middle = begin + count / 2U;
        const std::uint32_t left = build_node(begin, middle);
        const std::uint32_t right = build_node(middle, end);
        nodes_[node_index].first = left;
        nodes_[node_index].second = right;
        return node_index;
    }

    [[nodiscard]] static bool segment_crosses_triangle_interior(
        core::Vec3d start,
        core::Vec3d end,
        core::Vec3d first,
        core::Vec3d second,
        core::Vec3d third) noexcept {
        const core::Vec3d direction{end.x - start.x, end.y - start.y, end.z - start.z};
        const core::Vec3d edge_one{second.x - first.x, second.y - first.y, second.z - first.z};
        const core::Vec3d edge_two{third.x - first.x, third.y - first.y, third.z - first.z};
        const double direction_length = std::hypot(direction.x, direction.y, direction.z);
        const double first_length = std::hypot(edge_one.x, edge_one.y, edge_one.z);
        const double second_length = std::hypot(edge_two.x, edge_two.y, edge_two.z);
        if (!std::isfinite(direction_length) || !std::isfinite(first_length) ||
            !std::isfinite(second_length) || direction_length == 0.0 ||
            first_length == 0.0 || second_length == 0.0) return false;
        const double length_scale = std::max({
            direction_length, first_length, second_length});
        const core::Vec3d scaled_direction{
            direction.x / length_scale, direction.y / length_scale, direction.z / length_scale};
        const core::Vec3d scaled_edge_one{
            edge_one.x / length_scale, edge_one.y / length_scale, edge_one.z / length_scale};
        const core::Vec3d scaled_edge_two{
            edge_two.x / length_scale, edge_two.y / length_scale, edge_two.z / length_scale};
        const core::Vec3d p = core::cross(scaled_direction, scaled_edge_two);
        const double determinant = vector_dot(scaled_edge_one, p);
        const double determinant_scale =
            (direction_length / length_scale) *
            (first_length / length_scale) * (second_length / length_scale);
        const double determinant_tolerance =
            64.0 * std::numeric_limits<double>::epsilon() * determinant_scale;
        if (!std::isfinite(determinant) || !std::isfinite(determinant_scale) ||
            std::abs(determinant) <= determinant_tolerance) return false;
        const double inverse = 1.0 / determinant;
        const core::Vec3d offset{
            (start.x - first.x) / length_scale,
            (start.y - first.y) / length_scale,
            (start.z - first.z) / length_scale};
        const double u = vector_dot(offset, p) * inverse;
        if (!std::isfinite(u) || u < -1.0e-12 || u > 1.0 + 1.0e-12) return false;
        const core::Vec3d q = core::cross(offset, scaled_edge_one);
        const double v = vector_dot(scaled_direction, q) * inverse;
        if (!std::isfinite(v) || v < -1.0e-12 || u + v > 1.0 + 1.0e-12) return false;
        const double t = vector_dot(scaled_edge_two, q) * inverse;
        return std::isfinite(t) && t > kEndpointParameterTolerance &&
            t < 1.0 - kEndpointParameterTolerance;
    }

    const geometry::CompiledMesh& mesh_;
    std::vector<SurfaceTriangleRecord> triangles_;
    std::vector<std::uint32_t> order_;
    std::vector<SurfaceBvhNode> nodes_;
    std::uint32_t root_ = 0U;
    bool valid_ = false;
};

double mesh_diagonal(const geometry::EditableMesh& mesh) {
    const auto vertices = mesh.vertices_sorted();
    if (vertices.empty()) return 0.0;
    core::Vec3d minimum = vertices.front().position;
    core::Vec3d maximum = minimum;
    for (const auto& vertex : vertices) {
        minimum.x = std::min(minimum.x, vertex.position.x);
        minimum.y = std::min(minimum.y, vertex.position.y);
        minimum.z = std::min(minimum.z, vertex.position.z);
        maximum.x = std::max(maximum.x, vertex.position.x);
        maximum.y = std::max(maximum.y, vertex.position.y);
        maximum.z = std::max(maximum.z, vertex.position.z);
    }
    return distance(minimum, maximum);
}

assets::Sha256Digest policy_digest(const AutorigCandidatePolicy& policy) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "CARTO_AUTORIG_CANDIDATE_POLICY_V4 " << policy.max_candidates << ' '
        << policy.max_joints_per_candidate << ' ' << policy.minimum_joint_separation << ' '
        << policy.minimum_length_to_mesh_ratio << ' '
        << policy.maximum_length_to_mesh_ratio << ' ' << policy.part_coverage_weight << ' '
        << policy.nonadjacent_clearance_weight << ' ' << policy.minimum_bone_clearance << ' '
        << policy.length_reasonableness_weight << ' ' << policy.joint_budget_weight << ' '
        << policy.bilateral_reflection_weight << ' '
        << policy.bend_arc_regularity_weight << ' ' << policy.branch_coverage_weight << ' '
        << policy.skeleton_self_clearance_weight << ' '
        << policy.source_surface_crossing_weight << ' '
        << policy.control_cost_weight << '\n';
    const auto text = out.str();
    return hash_text(text);
}

assets::Sha256Digest constraints_digest(const AutorigConstraintSet& constraints) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "CARTO_AUTORIG_CONSTRAINTS_V2 " << constraints.source.asset_id << ' '
        << constraints.source.content_digest.hex() << ' ' << constraints.mesh_revision.value()
        << ' ' << constraints.analysis_digest.hex() << '\n';
    auto parts = constraints.parts;
    std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    for (const auto& part : parts) {
        out << "P " << std::quoted(part.id) << ' ' << std::quoted(part.semantic_role) << ' '
            << std::quoted(part.parent_id.value_or(std::string{})) << ' '
            << static_cast<unsigned>(part.side) << '\n';
    }
    auto landmarks = constraints.landmarks;
    std::sort(landmarks.begin(), landmarks.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    for (const auto& landmark : landmarks) {
        const auto& provenance = landmark.provenance;
        out << "L " << std::quoted(landmark.id) << ' ' << std::quoted(landmark.part_id) << ' '
            << std::quoted(landmark.semantic_role) << ' '
            << static_cast<unsigned>(landmark.side) << ' ' << landmark.vertex.value << ' '
            << std::quoted(provenance.assertion_id) << ' '
            << std::quoted(provenance.author_id) << ' ' << std::quoted(provenance.rationale)
            << ' ' << std::quoted(provenance.evidence_reference) << ' '
            << static_cast<unsigned>(provenance.origin) << '\n';
    }
    auto regions = constraints.allowed_regions;
    std::sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    for (const auto& region : regions) {
        const auto& provenance = region.provenance;
        out << "R " << std::quoted(region.id) << ' ' << std::quoted(region.part_id) << ' '
            << std::quoted(provenance.assertion_id) << ' '
            << std::quoted(provenance.author_id) << ' ' << std::quoted(provenance.rationale)
            << ' ' << std::quoted(provenance.evidence_reference) << ' '
            << static_cast<unsigned>(provenance.origin);
        for (const auto vertex : region.vertices) out << ' ' << vertex.value;
        out << '\n';
    }
    auto mechanical_ports = constraints.mechanical_ports;
    std::sort(mechanical_ports.begin(), mechanical_ports.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (const auto& hint : mechanical_ports) {
        const auto& provenance = hint.provenance;
        out << "M " << std::quoted(hint.id) << ' '
            << std::quoted(hint.parent_part_id) << ' ' << std::quoted(hint.child_part_id) << ' '
            << std::quoted(hint.parent_landmark_id) << ' '
            << std::quoted(hint.child_landmark_id) << ' '
            << static_cast<unsigned>(hint.kind) << ' ' << hint.axis.x << ' ' << hint.axis.y << ' '
            << hint.axis.z << ' ' << hint.lower_limit << ' ' << hint.upper_limit << ' '
            << std::quoted(provenance.assertion_id) << ' '
            << std::quoted(provenance.author_id) << ' ' << std::quoted(provenance.rationale)
            << ' ' << std::quoted(provenance.evidence_reference) << ' '
            << static_cast<unsigned>(provenance.origin) << '\n';
    }
    const auto text = out.str();
    return hash_text(text);
}

assets::Sha256Digest candidate_digest(const AutorigCandidate& candidate) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "CARTO_AUTORIG_CANDIDATE_V4\n";
    for (const auto& joint : candidate.joints) {
        out << "J " << joint.id.value << ' ' << (joint.parent ? joint.parent->value : 0U)
            << ' ' << std::quoted(joint.part_id) << ' ' << std::quoted(joint.landmark_id)
            << ' ' << std::quoted(joint.semantic_role) << ' '
            << static_cast<unsigned>(joint.side) << ' ' << joint.source_vertex.value
            << ' ' << joint.position.x << ' ' << joint.position.y << ' ' << joint.position.z
            << ' ' << std::quoted(joint.provenance.assertion_id) << ' '
            << std::quoted(joint.provenance.author_id) << ' '
            << std::quoted(joint.provenance.rationale) << ' '
            << std::quoted(joint.provenance.evidence_reference) << ' '
            << static_cast<unsigned>(joint.provenance.origin) << '\n';
    }
    for (const auto& connection : candidate.mechanical_connections) {
        out << "M " << std::quoted(connection.port_hint_id) << ' '
            << connection.parent_joint.value << ' ' << connection.child_joint.value << ' '
            << std::quoted(connection.parent_port_landmark_id) << ' '
            << std::quoted(connection.child_port_landmark_id) << ' '
            << connection.parent_port_vertex.value << ' ' << connection.child_port_vertex.value
            << ' ' << static_cast<unsigned>(connection.kind) << ' ' << connection.axis.x << ' '
            << connection.axis.y << ' ' << connection.axis.z << ' '
            << connection.lower_limit << ' ' << connection.upper_limit << ' '
            << std::quoted(connection.provenance.assertion_id) << ' '
            << std::quoted(connection.provenance.author_id) << ' '
            << std::quoted(connection.provenance.rationale) << ' '
            << std::quoted(connection.provenance.evidence_reference) << ' '
            << static_cast<unsigned>(connection.provenance.origin) << '\n';
    }
    for (const auto& part : candidate.unrepresented_parts)
        out << "U " << std::quoted(part) << '\n';
    out << "S " << candidate.score.part_coverage << ' '
        << candidate.score.nonadjacent_clearance << ' '
        << candidate.score.length_reasonableness << ' ' << candidate.score.joint_budget << ' '
        << candidate.score.bilateral_reflection_fit << ' '
        << candidate.score.bend_arc_regularity << ' '
        << candidate.score.branch_coverage << ' '
        << candidate.score.skeleton_self_clearance << ' '
        << candidate.score.source_surface_clearance << ' '
        << candidate.score.control_cost_fraction << ' '
        << candidate.score.ranking_score << ' '
        << candidate.score.skeleton_to_mesh_diagonal << ' '
        << candidate.score.evaluated_nonadjacent_pairs << ' '
        << candidate.score.close_nonadjacent_pairs << ' '
        << candidate.score.evaluated_bilateral_pairs << ' '
        << candidate.score.evaluated_bend_arc_transitions << ' '
        << candidate.score.evaluated_nonadjacent_bones << ' '
        << candidate.score.overlapping_nonadjacent_bones << ' '
        << candidate.score.evaluated_source_surface_bones << ' '
        << candidate.score.source_surface_crossing_bones << ' '
        << candidate.score.evaluated_control_links << '\n';
    return hash_text(out.str());
}

assets::Sha256Digest candidate_set_digest(const AutorigCandidateSet& result) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "CARTO_AUTORIG_CANDIDATE_SET_V4 " << result.source.asset_id << ' '
        << result.source.content_digest.hex() << ' ' << result.mesh_revision.value() << ' '
        << result.mesh_geometry_digest.hex() << ' ' << result.analysis_digest.hex() << ' '
        << result.constraints_digest.hex() << ' ' << result.policy_digest.hex() << '\n';
    for (const auto& candidate : result.ranked_candidates)
        out << candidate.digest.hex() << '\n';
    const auto text = out.str();
    return hash_text(text);
}

} // namespace

core::Result<void> AutorigCandidate::validate() const {
    if (joints.empty() || joints.size() > AutorigCandidatePolicy::kHardMaxJointsPerCandidate ||
        selected_landmark_ids.size() != joints.size() ||
        unrepresented_parts.size() > AutorigConstraintSet::kMaxParts ||
        digest.is_zero() || !std::isfinite(score.part_coverage) ||
        !std::isfinite(score.nonadjacent_clearance) ||
        !std::isfinite(score.length_reasonableness) ||
        !std::isfinite(score.joint_budget) || !std::isfinite(score.ranking_score) ||
        !std::isfinite(score.bilateral_reflection_fit) ||
        !std::isfinite(score.bend_arc_regularity) ||
        !std::isfinite(score.branch_coverage) ||
        !std::isfinite(score.skeleton_self_clearance) ||
        !std::isfinite(score.source_surface_clearance) ||
        !std::isfinite(score.control_cost_fraction) ||
        !std::isfinite(score.skeleton_to_mesh_diagonal) ||
        score.part_coverage < 0.0 || score.part_coverage > 1.0 ||
        score.nonadjacent_clearance < 0.0 || score.nonadjacent_clearance > 1.0 ||
        score.length_reasonableness < 0.0 || score.length_reasonableness > 1.0 ||
        score.joint_budget < 0.0 || score.joint_budget > 1.0 ||
        score.bilateral_reflection_fit < 0.0 || score.bilateral_reflection_fit > 1.0 ||
        score.bend_arc_regularity < 0.0 || score.bend_arc_regularity > 1.0 ||
        score.branch_coverage < 0.0 || score.branch_coverage > 1.0 ||
        score.skeleton_self_clearance < 0.0 || score.skeleton_self_clearance > 1.0 ||
        score.source_surface_clearance < 0.0 || score.source_surface_clearance > 1.0 ||
        score.control_cost_fraction < 0.0 || score.control_cost_fraction > 1.0 ||
        score.ranking_score < 0.0 || score.ranking_score > 1.0 ||
        score.skeleton_to_mesh_diagonal < 0.0 ||
        score.close_nonadjacent_pairs > score.evaluated_nonadjacent_pairs ||
        score.overlapping_nonadjacent_bones > score.evaluated_nonadjacent_bones ||
        score.source_surface_crossing_bones > score.evaluated_source_surface_bones ||
        score.evaluated_source_surface_bones > joints.size() - 1U ||
        (score.evaluated_source_surface_bones == 0U &&
            score.source_surface_clearance != 0.0) ||
        (score.evaluated_source_surface_bones != 0U &&
            std::abs(score.source_surface_clearance -
                (1.0 - static_cast<double>(score.source_surface_crossing_bones) /
                    static_cast<double>(score.evaluated_source_surface_bones))) > 1.0e-12) ||
        score.evaluated_bilateral_pairs > joints.size() / 2U ||
        score.evaluated_bend_arc_transitions > joints.size() ||
        score.evaluated_nonadjacent_bones > joints.size() * joints.size() ||
        score.evaluated_control_links != mechanical_connections.size() ||
        (score.evaluated_control_links == 0U && score.control_cost_fraction != 0.0)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "autorig candidate score, bounds, or digest is invalid"));
    }
    std::map<AutorigCandidateJointId, std::size_t> joint_indices;
    std::map<std::string, const AutorigCandidateJoint*, std::less<>> joints_by_part;
    std::set<std::string> part_ids;
    std::set<std::string> landmark_ids;
    std::size_t roots = 0U;
    for (std::size_t index = 0U; index < joints.size(); ++index) {
        const auto& joint = joints[index];
        const AutorigLandmarkConstraint evidence{
            joint.landmark_id, joint.part_id, joint.semantic_role, joint.side,
            joint.source_vertex, joint.provenance};
        if (!joint.id || joint.id.value != index + 1U || !joint.source_vertex ||
            !joint.position.finite() || !part_ids.insert(joint.part_id).second ||
            !landmark_ids.insert(joint.landmark_id).second ||
            selected_landmark_ids[index] != joint.landmark_id ||
            !evidence.validate()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "autorig candidate joint identity, anchor, provenance, or order is invalid"));
        }
        joint_indices.emplace(joint.id, index);
        joints_by_part.emplace(joint.part_id, &joint);
        if (!joint.parent) {
            ++roots;
        } else if (!joint_indices.contains(*joint.parent) ||
            joint_indices.at(*joint.parent) >= index) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "autorig candidate parent must refer to an earlier preview joint"));
        }
    }
    if (roots != 1U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "autorig candidate must contain exactly one preview root"));
    }
    if (mechanical_connections.size() > AutorigConstraintSet::kMaxMechanicalPortHints) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "autorig candidate mechanical connection count exceeds its hard bound"));
    }
    std::set<std::string> connection_ids;
    std::set<AutorigCandidateJointId> connection_children;
    std::string previous_connection_id;
    for (const auto& connection : mechanical_connections) {
        const auto parent = joint_indices.find(connection.parent_joint);
        const auto child = joint_indices.find(connection.child_joint);
        if (!connection_ids.insert(connection.port_hint_id).second ||
            connection.port_hint_id.empty() ||
            (!previous_connection_id.empty() &&
                connection.port_hint_id <= previous_connection_id) ||
            !connection_children.insert(connection.child_joint).second ||
            parent == joint_indices.end() || child == joint_indices.end() ||
            connection.parent_joint == connection.child_joint ||
            !joints[child->second].parent ||
            *joints[child->second].parent != connection.parent_joint) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "autorig mechanical proposal must bind unique authored links to direct preview joints"));
        }
        const AutorigMechanicalPortHint evidence{
            connection.port_hint_id,
            joints[parent->second].part_id,
            joints[child->second].part_id,
            connection.parent_port_landmark_id,
            connection.child_port_landmark_id,
            connection.kind,
            connection.axis,
            connection.lower_limit,
            connection.upper_limit,
            connection.provenance};
        if (!connection.parent_port_vertex || !connection.child_port_vertex ||
            !evidence.validate()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "autorig mechanical proposal port identities or provenance are invalid"));
        }
        previous_connection_id = connection.port_hint_id;
    }
    std::set<std::string> omitted;
    for (const auto& part : unrepresented_parts) {
        if (part.empty() || part_ids.contains(part) || !omitted.insert(part).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "autorig candidate omitted-part list is duplicated or conflicts with a joint"));
        }
    }
    if (candidate_digest(*this) != digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "autorig candidate content does not match its digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> AutorigCandidateSet::validate() const {
    if (auto checked = source.validate(); !checked) return checked;
    if (mesh_revision.value() == 0U || mesh_geometry_digest.is_zero() ||
        analysis_digest.is_zero() || constraints_digest.is_zero() || policy_digest.is_zero() ||
        ranked_candidates.empty() ||
        ranked_candidates.size() > AutorigCandidatePolicy::kHardMaxCandidates ||
        digest.is_zero()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "autorig candidate-set source, receipt, or bounds are invalid"));
    }
    for (std::size_t index = 0U; index < ranked_candidates.size(); ++index) {
        const auto& candidate = ranked_candidates[index];
        if (auto checked = candidate.validate(); !checked) return checked;
        if (index != 0U) {
            const auto& previous = ranked_candidates[index - 1U];
            if (candidate.score.ranking_score > previous.score.ranking_score ||
                (candidate.score.ranking_score == previous.score.ranking_score &&
                    candidate.selected_landmark_ids < previous.selected_landmark_ids)) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "autorig candidate set is not ordered by its recorded ranking"));
            }
        }
    }
    if (candidate_set_digest(*this) != digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "autorig candidate-set content does not match its digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> AutorigCandidatePolicy::validate() const {
    const double sum = part_coverage_weight + nonadjacent_clearance_weight +
        length_reasonableness_weight + joint_budget_weight + bilateral_reflection_weight +
        bend_arc_regularity_weight + branch_coverage_weight + skeleton_self_clearance_weight +
        source_surface_crossing_weight + control_cost_weight;
    if (max_candidates == 0U || max_candidates > kHardMaxCandidates ||
        max_joints_per_candidate == 0U ||
        max_joints_per_candidate > kHardMaxJointsPerCandidate ||
        !std::isfinite(minimum_joint_separation) || minimum_joint_separation < 0.0 ||
        !std::isfinite(minimum_bone_clearance) || minimum_bone_clearance < 0.0 ||
        !std::isfinite(minimum_length_to_mesh_ratio) ||
        !std::isfinite(maximum_length_to_mesh_ratio) ||
        minimum_length_to_mesh_ratio <= 0.0 ||
        maximum_length_to_mesh_ratio < minimum_length_to_mesh_ratio ||
        !std::isfinite(part_coverage_weight) || part_coverage_weight < 0.0 ||
        !std::isfinite(nonadjacent_clearance_weight) || nonadjacent_clearance_weight < 0.0 ||
        !std::isfinite(length_reasonableness_weight) || length_reasonableness_weight < 0.0 ||
        !std::isfinite(joint_budget_weight) || joint_budget_weight < 0.0 ||
        !std::isfinite(bilateral_reflection_weight) || bilateral_reflection_weight < 0.0 ||
        !std::isfinite(bend_arc_regularity_weight) || bend_arc_regularity_weight < 0.0 ||
        !std::isfinite(branch_coverage_weight) || branch_coverage_weight < 0.0 ||
        !std::isfinite(skeleton_self_clearance_weight) || skeleton_self_clearance_weight < 0.0 ||
        !std::isfinite(source_surface_crossing_weight) ||
            source_surface_crossing_weight < 0.0 ||
        !std::isfinite(control_cost_weight) || control_cost_weight < 0.0 ||
        !std::isfinite(sum) || std::abs(sum - 1.0) > 1.0e-9) {
        return core::Result<void>::failure(invalid(
            "autorig candidate policy requires bounded budgets, finite thresholds, and weights summing to one"));
    }
    return core::Result<void>::success();
}

core::Result<AutorigCandidateSet> generate_autorig_candidates(
    const geometry::EditableMesh& mesh,
    const AutorigAnalysisRequest& analysis_request,
    const AutorigConstraintSet& constraints,
    const AutorigCandidatePolicy& policy) {
    if (auto checked = policy.validate(); !checked)
        return core::Result<AutorigCandidateSet>::failure(checked.error());
    if (auto checked = mesh.validate(); !checked)
        return core::Result<AutorigCandidateSet>::failure(checked.error());
    const auto analyzed = analyze_autorig_features(mesh, analysis_request);
    if (!analyzed) return core::Result<AutorigCandidateSet>::failure(analyzed.error());
    if (auto checked = constraints.validate_against(analyzed.value()); !checked)
        return core::Result<AutorigCandidateSet>::failure(checked.error());

    const double diagonal = mesh_diagonal(mesh);
    if (!std::isfinite(diagonal) || diagonal <= 1.0e-12) {
        return core::Result<AutorigCandidateSet>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "candidate ranking requires non-degenerate mesh bounds; no proposal was produced"));
    }

    std::map<std::string, const AutorigPartonomyNode*, std::less<>> parts;
    for (const auto& part : constraints.parts) parts.emplace(part.id, &part);
    std::map<std::string, std::vector<const AutorigLandmarkConstraint*>, std::less<>> anchors;
    for (const auto& landmark : constraints.landmarks)
        anchors[landmark.part_id].push_back(&landmark);
    for (auto& [part_id, values] : anchors) {
        (void)part_id;
        std::sort(values.begin(), values.end(), [](const auto* a, const auto* b) {
            return a->id < b->id;
        });
    }

    const auto root = std::find_if(constraints.parts.begin(), constraints.parts.end(),
        [](const auto& part) { return !part.parent_id.has_value(); });
    if (root == constraints.parts.end() || !anchors.contains(root->id)) {
        return core::Result<AutorigCandidateSet>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "the explicit root part has no authored landmark; candidate generation will not guess a root"));
    }

    std::vector<const AutorigPartonomyNode*> ordered_parts;
    ordered_parts.reserve(constraints.parts.size());
    for (const auto& part : constraints.parts) ordered_parts.push_back(&part);
    const auto depth = [&parts](const AutorigPartonomyNode* part) {
        std::size_t value = 0U;
        auto parent = part->parent_id;
        while (parent) {
            ++value;
            parent = parts.at(*parent)->parent_id;
        }
        return value;
    };
    std::stable_sort(ordered_parts.begin(), ordered_parts.end(), [&](const auto* a, const auto* b) {
        const auto da = depth(a);
        const auto db = depth(b);
        return da != db ? da < db : a->id < b->id;
    });

    std::vector<const AutorigPartonomyNode*> represented_parts;
    std::size_t combinations = 1U;
    for (const auto* part : ordered_parts) {
        const auto found = anchors.find(part->id);
        if (found == anchors.end()) continue;
        if (found->second.empty() || combinations > policy.max_candidates / found->second.size()) {
            return core::Result<AutorigCandidateSet>::failure(invalid(
                "authored anchor alternatives exceed the candidate budget; no partial ranking was returned"));
        }
        combinations *= found->second.size();
        represented_parts.push_back(part);
    }
    if (represented_parts.empty() ||
        represented_parts.size() > policy.max_joints_per_candidate) {
        return core::Result<AutorigCandidateSet>::failure(invalid(
            "authored candidate joints exceed the configured per-candidate budget"));
    }
    const std::size_t pairs_per_candidate = represented_parts.size() < 2U ? 0U :
        represented_parts.size() * (represented_parts.size() - 1U) / 2U;
    if (pairs_per_candidate != 0U && combinations >
            AutorigCandidatePolicy::kHardMaxPairChecks / pairs_per_candidate) {
        return core::Result<AutorigCandidateSet>::failure(invalid(
            "clearance evaluation exceeds the hard pair-check budget; no partial ranking was returned"));
    }

    std::optional<geometry::CompiledMesh> compiled_surface_mesh;
    std::optional<SourceSurfaceIndex> source_surface_index;
    if (policy.source_surface_crossing_weight > 0.0) {
        auto compiled = mesh.compile();
        if (!compiled)
            return core::Result<AutorigCandidateSet>::failure(compiled.error());
        if (compiled.value().source_revision != analyzed.value().mesh_revision ||
            !compiled.value().valid()) {
            return core::Result<AutorigCandidateSet>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "source-surface ranking requires a compiled current-revision mesh"));
        }
        const std::size_t triangle_count = compiled.value().indices.size() / 3U;
        if (triangle_count == 0U ||
            triangle_count > SourceSurfaceIndex::kHardMaxTriangles) {
            return core::Result<AutorigCandidateSet>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "source-surface ranking requires 1..300000 compiled triangles; no partial ranking was returned"));
        }
        compiled_surface_mesh.emplace(std::move(compiled).value());
        source_surface_index.emplace(*compiled_surface_mesh);
        if (!source_surface_index->valid()) {
            return core::Result<AutorigCandidateSet>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "source-surface index could not be built within its bounded representation"));
        }
    }
    std::size_t source_surface_triangle_tests = 0U;

    AutorigCandidateSet result;
    result.source = analyzed.value().source;
    result.mesh_revision = analyzed.value().mesh_revision;
    result.mesh_geometry_digest = analyzed.value().mesh_geometry_digest;
    result.analysis_digest = analyzed.value().analysis_digest;
    result.constraints_digest = constraints_digest(constraints);
    result.policy_digest = policy_digest(policy);

    std::map<std::string, const AutorigLandmarkConstraint*, std::less<>> selected;
    const auto build_candidate = [&]() -> core::Result<AutorigCandidate> {
        AutorigCandidate candidate;
        std::map<std::string, AutorigCandidateJointId, std::less<>> joint_ids;
        double skeleton_length = 0.0;
        for (const auto* part : ordered_parts) {
            const auto chosen = selected.find(part->id);
            if (chosen == selected.end()) {
                candidate.unrepresented_parts.push_back(part->id);
                continue;
            }
            const auto* landmark = chosen->second;
            const auto* vertex = mesh.find_vertex(landmark->vertex);
            if (vertex == nullptr || !vertex->position.finite()) {
                return core::Result<AutorigCandidate>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "authored landmark vertex is missing or non-finite in the current mesh"));
            }
            AutorigCandidateJoint joint;
            joint.id = AutorigCandidateJointId{
                static_cast<std::uint64_t>(candidate.joints.size() + 1U)};
            joint.part_id = part->id;
            joint.landmark_id = landmark->id;
            joint.semantic_role = landmark->semantic_role;
            joint.side = landmark->side;
            joint.source_vertex = landmark->vertex;
            joint.position = vertex->position;
            joint.provenance = landmark->provenance;
            auto parent = part->parent_id;
            while (parent) {
                const auto found = joint_ids.find(*parent);
                if (found != joint_ids.end()) {
                    joint.parent = found->second;
                    break;
                }
                parent = parts.at(*parent)->parent_id;
            }
            if (joint.parent) {
                const auto parent_joint = std::find_if(candidate.joints.begin(),
                    candidate.joints.end(), [&](const auto& item) {
                        return item.id == *joint.parent;
                    });
                skeleton_length += distance(joint.position, parent_joint->position);
            }
            joint_ids.emplace(part->id, joint.id);
            candidate.selected_landmark_ids.push_back(landmark->id);
            candidate.joints.push_back(std::move(joint));
        }
        std::vector<const AutorigMechanicalPortHint*> mechanical_hints;
        mechanical_hints.reserve(constraints.mechanical_ports.size());
        for (const auto& hint : constraints.mechanical_ports)
            mechanical_hints.push_back(&hint);
        std::sort(mechanical_hints.begin(), mechanical_hints.end(), [](const auto* a, const auto* b) {
            return a->id < b->id;
        });
        std::map<std::string, const AutorigLandmarkConstraint*, std::less<>> landmarks_by_id;
        for (const auto& landmark : constraints.landmarks)
            landmarks_by_id.emplace(landmark.id, &landmark);
        for (const auto* hint : mechanical_hints) {
            const auto parent = joint_ids.find(hint->parent_part_id);
            const auto child = joint_ids.find(hint->child_part_id);
            if (parent == joint_ids.end() || child == joint_ids.end()) {
                return core::Result<AutorigCandidate>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "mechanical port hint cannot be represented because a linked part has no authored preview joint"));
            }
            const auto port_parent = landmarks_by_id.find(hint->parent_landmark_id);
            const auto port_child = landmarks_by_id.find(hint->child_landmark_id);
            if (port_parent == landmarks_by_id.end() || port_child == landmarks_by_id.end()) {
                return core::Result<AutorigCandidate>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "mechanical port landmark disappeared after constraint admission"));
            }
            const auto child_joint = std::find_if(candidate.joints.begin(), candidate.joints.end(),
                [&child](const auto& joint) { return joint.id == child->second; });
            if (child_joint == candidate.joints.end() || !child_joint->parent ||
                *child_joint->parent != parent->second) {
                return core::Result<AutorigCandidate>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "mechanical port hint does not resolve to the authored direct preview-joint edge"));
            }
            candidate.mechanical_connections.push_back({
                hint->id, parent->second, child->second,
                hint->parent_landmark_id, hint->child_landmark_id,
                port_parent->second->vertex, port_child->second->vertex,
                hint->kind, hint->axis, hint->lower_limit, hint->upper_limit,
                hint->provenance});
        }
        candidate.score.evaluated_control_links = candidate.mechanical_connections.size();
        if (candidate.score.evaluated_control_links != 0U) {
            const auto actuated_links = std::count_if(
                candidate.mechanical_connections.begin(), candidate.mechanical_connections.end(),
                [](const auto& connection) {
                    return connection.kind == kinematic::JointKind::hinge ||
                        connection.kind == kinematic::JointKind::slider;
                });
            candidate.score.control_cost_fraction =
                static_cast<double>(actuated_links) /
                static_cast<double>(candidate.score.evaluated_control_links);
        }
        candidate.score.part_coverage = static_cast<double>(candidate.joints.size()) /
            static_cast<double>(constraints.parts.size());
        std::map<std::string, std::size_t, std::less<>> part_child_counts;
        for (const auto& part : constraints.parts) {
            if (part.parent_id) ++part_child_counts[*part.parent_id];
        }
        std::size_t total_authored_branch_edges = 0U;
        std::size_t represented_authored_branch_edges = 0U;
        for (const auto& [parent_id, child_count] : part_child_counts) {
            if (child_count < 2U) continue;
            total_authored_branch_edges += child_count;
            for (const auto& part : constraints.parts) {
                if (part.parent_id && *part.parent_id == parent_id &&
                    joint_ids.contains(part.id))
                    ++represented_authored_branch_edges;
            }
        }
        candidate.score.branch_coverage = total_authored_branch_edges == 0U ? 1.0 :
            static_cast<double>(represented_authored_branch_edges) /
                static_cast<double>(total_authored_branch_edges);
        candidate.score.skeleton_to_mesh_diagonal = skeleton_length / diagonal;
        candidate.score.length_reasonableness =
            candidate.score.skeleton_to_mesh_diagonal < policy.minimum_length_to_mesh_ratio
                ? candidate.score.skeleton_to_mesh_diagonal /
                    policy.minimum_length_to_mesh_ratio
                : (candidate.score.skeleton_to_mesh_diagonal >
                        policy.maximum_length_to_mesh_ratio
                    ? policy.maximum_length_to_mesh_ratio /
                        candidate.score.skeleton_to_mesh_diagonal
                    : 1.0);
        candidate.score.joint_budget = 1.0 -
            static_cast<double>(candidate.joints.size()) /
                static_cast<double>(policy.max_joints_per_candidate);
        struct SideRoleCandidates final {
            std::vector<const AutorigCandidateJoint*> left;
            std::vector<const AutorigCandidateJoint*> right;
        };
        std::map<std::string, SideRoleCandidates, std::less<>> side_roles;
        for (const auto& joint : candidate.joints) {
            const auto& part_role = parts.at(joint.part_id)->semantic_role;
            auto& grouped = side_roles[part_role];
            if (joint.side == AutorigPartSide::left) grouped.left.push_back(&joint);
            else if (joint.side == AutorigPartSide::right) grouped.right.push_back(&joint);
        }
        double bilateral_error_ratio_sum = 0.0;
        for (const auto& [role, grouped] : side_roles) {
            (void)role;
            // Multiple same-role parts on either side are ambiguous here. Do
            // not invent a pairing based on names or iteration order.
            if (grouped.left.size() != 1U || grouped.right.size() != 1U) continue;
            auto reflected_left = grouped.left.front()->position;
            switch (analysis_request.symmetry_axis) {
            case AutorigAxis::x:
                reflected_left.x = 2.0 * analysis_request.symmetry_plane_offset - reflected_left.x;
                break;
            case AutorigAxis::y:
                reflected_left.y = 2.0 * analysis_request.symmetry_plane_offset - reflected_left.y;
                break;
            case AutorigAxis::z:
                reflected_left.z = 2.0 * analysis_request.symmetry_plane_offset - reflected_left.z;
                break;
            }
            bilateral_error_ratio_sum += distance(
                reflected_left, grouped.right.front()->position) / diagonal;
            ++candidate.score.evaluated_bilateral_pairs;
        }
        if (candidate.score.evaluated_bilateral_pairs != 0U) {
            const double mean_error_ratio = bilateral_error_ratio_sum /
                static_cast<double>(candidate.score.evaluated_bilateral_pairs);
            candidate.score.bilateral_reflection_fit = 1.0 - std::clamp(mean_error_ratio, 0.0, 1.0);
        }
        std::map<std::uint64_t, std::size_t> joint_index_by_id;
        for (std::size_t index = 0U; index < candidate.joints.size(); ++index)
            joint_index_by_id.emplace(candidate.joints[index].id.value, index);
        std::vector<std::vector<std::size_t>> child_indices(candidate.joints.size());
        for (std::size_t index = 0U; index < candidate.joints.size(); ++index) {
            if (candidate.joints[index].parent)
                child_indices[joint_index_by_id.at(candidate.joints[index].parent->value)]
                    .push_back(index);
        }
        if (policy.source_surface_crossing_weight > 0.0) {
            if (!source_surface_index) {
                return core::Result<AutorigCandidate>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "source-surface ranking index is unavailable; no partial candidate was returned"));
            }
            for (const auto& joint : candidate.joints) {
                if (!joint.parent) continue;
                const auto parent_index = joint_index_by_id.at(joint.parent->value);
                ++candidate.score.evaluated_source_surface_bones;
                const auto query = source_surface_index->query_segment(
                    candidate.joints[parent_index].position, joint.position,
                    source_surface_triangle_tests);
                if (query == SurfaceQueryResult::budget_exceeded) {
                    return core::Result<AutorigCandidate>::failure(core::Diagnostic(
                        core::ErrorCode::unsupported,
                        "source-surface triangle-query budget exceeded; no partial ranking was returned"));
                }
                if (query == SurfaceQueryResult::crossing)
                    ++candidate.score.source_surface_crossing_bones;
            }
            if (candidate.score.evaluated_source_surface_bones != 0U) {
                candidate.score.source_surface_clearance = 1.0 -
                    static_cast<double>(candidate.score.source_surface_crossing_bones) /
                    static_cast<double>(candidate.score.evaluated_source_surface_bones);
            }
        }
        std::vector<std::optional<double>> bend_angles(candidate.joints.size());
        for (std::size_t index = 0U; index < candidate.joints.size(); ++index) {
            const auto& joint = candidate.joints[index];
            if (!joint.parent || child_indices[index].size() != 1U) continue;
            const auto parent_index = joint_index_by_id.at(joint.parent->value);
            const auto child_index = child_indices[index].front();
            const auto& parent_position = candidate.joints[parent_index].position;
            const auto& child_position = candidate.joints[child_index].position;
            const double incoming_length = distance(parent_position, joint.position);
            const double outgoing_length = distance(joint.position, child_position);
            if (incoming_length <= 1.0e-12 || outgoing_length <= 1.0e-12) continue;
            const double dot =
                (joint.position.x - parent_position.x) *
                    (child_position.x - joint.position.x) +
                (joint.position.y - parent_position.y) *
                    (child_position.y - joint.position.y) +
                (joint.position.z - parent_position.z) *
                    (child_position.z - joint.position.z);
            bend_angles[index] = std::acos(std::clamp(
                dot / (incoming_length * outgoing_length), -1.0, 1.0));
        }
        double bend_arc_smoothness_sum = 0.0;
        for (std::size_t index = 0U; index < candidate.joints.size(); ++index) {
            const auto& joint = candidate.joints[index];
            if (!joint.parent || !bend_angles[index]) continue;
            const auto parent_index = joint_index_by_id.at(joint.parent->value);
            if (!bend_angles[parent_index]) continue;
            bend_arc_smoothness_sum += 1.0 - std::abs(
                *bend_angles[index] - *bend_angles[parent_index]) /
                3.14159265358979323846;
            ++candidate.score.evaluated_bend_arc_transitions;
        }
        if (candidate.score.evaluated_bend_arc_transitions != 0U) {
            candidate.score.bend_arc_regularity = bend_arc_smoothness_sum /
                static_cast<double>(candidate.score.evaluated_bend_arc_transitions);
        }
        for (std::size_t a = 0U; a < candidate.joints.size(); ++a) {
            for (std::size_t b = a + 1U; b < candidate.joints.size(); ++b) {
                const bool direct_parent_pair =
                    (candidate.joints[a].parent &&
                        *candidate.joints[a].parent == candidate.joints[b].id) ||
                    (candidate.joints[b].parent &&
                        *candidate.joints[b].parent == candidate.joints[a].id);
                if (direct_parent_pair) continue;
                ++candidate.score.evaluated_nonadjacent_pairs;
                if (distance(candidate.joints[a].position, candidate.joints[b].position) <
                    policy.minimum_joint_separation)
                    ++candidate.score.close_nonadjacent_pairs;
            }
        }
        candidate.score.nonadjacent_clearance =
            candidate.score.evaluated_nonadjacent_pairs == 0U ? 1.0 :
                1.0 - static_cast<double>(candidate.score.close_nonadjacent_pairs) /
                    static_cast<double>(candidate.score.evaluated_nonadjacent_pairs);
        for (std::size_t first_child = 0U;
             first_child < candidate.joints.size(); ++first_child) {
            const auto& first_joint = candidate.joints[first_child];
            if (!first_joint.parent) continue;
            const auto first_parent = joint_index_by_id.at(first_joint.parent->value);
            for (std::size_t second_child = first_child + 1U;
                 second_child < candidate.joints.size(); ++second_child) {
                const auto& second_joint = candidate.joints[second_child];
                if (!second_joint.parent) continue;
                const auto second_parent = joint_index_by_id.at(second_joint.parent->value);
                if (first_parent == second_parent || first_parent == second_child ||
                    second_parent == first_child)
                    continue;
                ++candidate.score.evaluated_nonadjacent_bones;
                const auto clearance = segment_distance(
                    candidate.joints[first_parent].position, first_joint.position,
                    candidate.joints[second_parent].position, second_joint.position);
                if (!std::isfinite(clearance)) {
                    return core::Result<AutorigCandidate>::failure(core::Diagnostic(
                        core::ErrorCode::unsupported,
                        "skeleton self-clearance exceeds the finite numeric domain; no partial ranking was returned"));
                }
                if (clearance <= 1.0e-12 || clearance < policy.minimum_bone_clearance)
                    ++candidate.score.overlapping_nonadjacent_bones;
            }
        }
        candidate.score.skeleton_self_clearance =
            candidate.score.evaluated_nonadjacent_bones == 0U ? 1.0 :
                1.0 - static_cast<double>(candidate.score.overlapping_nonadjacent_bones) /
                    static_cast<double>(candidate.score.evaluated_nonadjacent_bones);
        const double source_surface_contribution =
            candidate.score.evaluated_source_surface_bones == 0U
                ? 0.5 : candidate.score.source_surface_clearance;
        candidate.score.ranking_score =
            candidate.score.part_coverage * policy.part_coverage_weight +
            candidate.score.nonadjacent_clearance * policy.nonadjacent_clearance_weight +
            candidate.score.length_reasonableness * policy.length_reasonableness_weight +
            candidate.score.joint_budget * policy.joint_budget_weight +
            candidate.score.bilateral_reflection_fit * policy.bilateral_reflection_weight +
            candidate.score.bend_arc_regularity * policy.bend_arc_regularity_weight +
            candidate.score.branch_coverage * policy.branch_coverage_weight +
            candidate.score.skeleton_self_clearance * policy.skeleton_self_clearance_weight +
            source_surface_contribution * policy.source_surface_crossing_weight +
            (candidate.score.evaluated_control_links == 0U
                ? 0.5 : 1.0 - candidate.score.control_cost_fraction) *
                policy.control_cost_weight;
        candidate.digest = candidate_digest(candidate);
        return core::Result<AutorigCandidate>::success(std::move(candidate));
    };

    const auto enumerate = [&](auto&& self, std::size_t index) -> core::Result<void> {
        if (index == represented_parts.size()) {
            auto candidate = build_candidate();
            if (!candidate) return core::Result<void>::failure(candidate.error());
            result.ranked_candidates.push_back(std::move(candidate).value());
            return core::Result<void>::success();
        }
        const auto* part = represented_parts[index];
        for (const auto* landmark : anchors.at(part->id)) {
            selected[part->id] = landmark;
            if (auto generated = self(self, index + 1U); !generated) return generated;
        }
        selected.erase(part->id);
        return core::Result<void>::success();
    };
    if (auto generated = enumerate(enumerate, 0U); !generated)
        return core::Result<AutorigCandidateSet>::failure(generated.error());

    std::stable_sort(result.ranked_candidates.begin(), result.ranked_candidates.end(),
        [](const auto& a, const auto& b) {
            if (a.score.ranking_score != b.score.ranking_score)
                return a.score.ranking_score > b.score.ranking_score;
            return a.selected_landmark_ids < b.selected_landmark_ids;
        });
    result.digest = candidate_set_digest(result);
    if (auto checked = result.validate(); !checked)
        return core::Result<AutorigCandidateSet>::failure(checked.error());
    return core::Result<AutorigCandidateSet>::success(std::move(result));
}

assets::Sha256Digest AutorigReviewAcceptance::canonical_digest() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    const auto write_text = [&out](std::string_view value) {
        out << value.size() << ':' << value << '\n';
    };
    out << "CARTO_AUTORIG_REVIEW_ACCEPTANCE_V2\n"
        << source.asset_id << ' ' << source.content_digest.hex() << ' '
        << mesh_revision.value() << ' ' << mesh_geometry_digest.hex() << '\n'
        << constraints_digest.hex() << ' ' << policy_digest.hex() << ' '
        << candidate_set_digest.hex() << ' ' << candidate_digest.hex() << '\n';
    write_text(reviewer_decision.assertion_id);
    write_text(reviewer_decision.author_id);
    write_text(reviewer_decision.rationale);
    write_text(reviewer_decision.evidence_reference);
    out << static_cast<unsigned int>(reviewer_decision.origin) << '\n';
    return hash_text(out.str());
}

core::Result<void> AutorigReviewAcceptance::validate() const {
    if (auto checked = source.validate(); !checked) return checked;
    if (mesh_revision.value() == 0U || mesh_geometry_digest.is_zero() ||
        constraints_digest.is_zero() || policy_digest.is_zero() ||
        candidate_set_digest.is_zero() || candidate_digest.is_zero() ||
        reviewer_decision.origin != AutorigConstraintOrigin::human_authored ||
        !reviewer_decision.validate() || digest.is_zero() ||
        digest != canonical_digest()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "autorig review acceptance must bind the exact source and selected candidate"));
    }
    return core::Result<void>::success();
}

core::Result<AutorigCandidateReview> AutorigCandidateReview::begin(
    const geometry::EditableMesh& mesh,
    const AutorigAnalysisRequest& analysis_request,
    const AutorigConstraintSet& constraints,
    const AutorigCandidatePolicy& policy) {
    auto candidates = generate_autorig_candidates(
        mesh, analysis_request, constraints, policy);
    if (!candidates)
        return core::Result<AutorigCandidateReview>::failure(candidates.error());
    return core::Result<AutorigCandidateReview>::success(AutorigCandidateReview(
        mesh, analysis_request, constraints, policy, std::move(candidates).value()));
}

core::Result<void> AutorigCandidateReview::select_candidate(
    const assets::Sha256Digest& candidate_digest) {
    const auto found = std::find_if(candidates_.ranked_candidates.begin(),
        candidates_.ranked_candidates.end(), [&candidate_digest](const auto& candidate) {
            return candidate.digest == candidate_digest;
        });
    if (found == candidates_.ranked_candidates.end()) {
        return core::Result<void>::failure(invalid(
            "autorig review selection must name a candidate in the current source-bound set"));
    }
    selected_candidate_digest_ = candidate_digest;
    return core::Result<void>::success();
}

core::Result<void> AutorigCandidateReview::lock_landmark(
    std::string_view part_id,
    std::string_view landmark_id) {
    if (!selected_candidate_digest_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "select a current candidate before locking one of its authored landmarks"));
    }
    const auto candidate = std::find_if(candidates_.ranked_candidates.begin(),
        candidates_.ranked_candidates.end(), [this](const auto& item) {
            return item.digest == *selected_candidate_digest_;
        });
    if (candidate == candidates_.ranked_candidates.end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected autorig candidate is no longer in the current proposal set"));
    }
    const auto joint = std::find_if(candidate->joints.begin(), candidate->joints.end(),
        [part_id, landmark_id](const auto& item) {
            return item.part_id == part_id && item.landmark_id == landmark_id;
        });
    if (joint == candidate->joints.end()) {
        return core::Result<void>::failure(invalid(
            "a landmark lock must match the selected candidate's exact part and anchor"));
    }
    locked_landmarks_.insert_or_assign(std::string(part_id), std::string(landmark_id));
    return core::Result<void>::success();
}

core::Result<void> AutorigCandidateReview::edit_landmark(
    AutorigLandmarkConstraint replacement) {
    if (auto checked = replacement.validate(); !checked) return checked;
    const auto part = std::find_if(constraints_.parts.begin(), constraints_.parts.end(),
        [&replacement](const auto& item) { return item.id == replacement.part_id; });
    if (part == constraints_.parts.end()) {
        return core::Result<void>::failure(invalid(
            "an edited landmark must reference an existing authored part"));
    }
    const auto existing = std::find_if(constraints_.landmarks.begin(),
        constraints_.landmarks.end(), [&replacement](const auto& item) {
            return item.id == replacement.id;
        });
    const std::string accepted_id = replacement.id;
    if (existing != constraints_.landmarks.end()) {
        if (existing->part_id != replacement.part_id) {
            return core::Result<void>::failure(invalid(
                "an edited landmark cannot be reassigned to another authored part"));
        }
        *existing = std::move(replacement);
    } else {
        if (constraints_.landmarks.size() >= AutorigConstraintSet::kMaxLandmarks) {
            return core::Result<void>::failure(invalid(
                "edited landmark exceeds the authored constraint bound"));
        }
        constraints_.landmarks.push_back(std::move(replacement));
    }
    const auto lock = locked_landmarks_.find(part->id);
    if (lock != locked_landmarks_.end()) {
        lock->second = accepted_id;
    }
    selected_candidate_digest_.reset();
    return core::Result<void>::success();
}

core::Result<void> AutorigCandidateReview::replan(
    const geometry::EditableMesh& current_mesh) {
    AutorigConstraintSet constrained = constraints_;
    for (const auto& [part_id, landmark_id] : locked_landmarks_) {
        const auto found = std::find_if(constrained.landmarks.begin(),
            constrained.landmarks.end(), [&part_id, &landmark_id](const auto& item) {
                return item.part_id == part_id && item.id == landmark_id;
            });
        if (found == constrained.landmarks.end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "locked autorig landmark is absent from the current authored constraints"));
        }
        constrained.landmarks.erase(std::remove_if(constrained.landmarks.begin(),
            constrained.landmarks.end(), [&part_id, &landmark_id](const auto& item) {
                return item.part_id == part_id && item.id != landmark_id;
            }), constrained.landmarks.end());
    }
    auto replanned = generate_autorig_candidates(
        current_mesh, request_, constrained, policy_);
    if (!replanned) return core::Result<void>::failure(replanned.error());
    mesh_ = current_mesh;
    candidates_ = std::move(replanned).value();
    selected_candidate_digest_.reset();
    return core::Result<void>::success();
}

core::Result<AutorigReviewAcceptance> AutorigCandidateReview::accept_review(
    AutorigConstraintProvenance reviewer_decision) const {
    if (!selected_candidate_digest_) {
        return core::Result<AutorigReviewAcceptance>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "autorig review acceptance requires a selected current candidate"));
    }
    if (auto checked = candidates_.validate(); !checked)
        return core::Result<AutorigReviewAcceptance>::failure(checked.error());
    const auto candidate = std::find_if(candidates_.ranked_candidates.begin(),
        candidates_.ranked_candidates.end(), [this](const auto& item) {
            return item.digest == *selected_candidate_digest_;
        });
    if (candidate == candidates_.ranked_candidates.end()) {
        return core::Result<AutorigReviewAcceptance>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "autorig review selection is not present in the current proposal set"));
    }
    for (const auto& [part_id, landmark_id] : locked_landmarks_) {
        const auto joint = std::find_if(candidate->joints.begin(), candidate->joints.end(),
            [&part_id, &landmark_id](const auto& item) {
                return item.part_id == part_id && item.landmark_id == landmark_id;
            });
        if (joint == candidate->joints.end()) {
            return core::Result<AutorigReviewAcceptance>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "selected candidate does not satisfy every locked authored landmark"));
        }
    }
    AutorigReviewAcceptance result{
        candidates_.source,
        candidates_.mesh_revision,
        candidates_.mesh_geometry_digest,
        candidates_.constraints_digest,
        candidates_.policy_digest,
        candidates_.digest,
        candidate->digest,
        std::move(reviewer_decision),
        {}};
    result.digest = result.canonical_digest();
    if (auto checked = result.validate(); !checked)
        return core::Result<AutorigReviewAcceptance>::failure(checked.error());
    return core::Result<AutorigReviewAcceptance>::success(std::move(result));
}

} // namespace carto::character
