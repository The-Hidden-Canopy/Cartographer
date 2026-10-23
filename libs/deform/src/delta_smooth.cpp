#include <carto/deform/delta_smooth.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace carto::deform {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

struct Frame {
    core::Vec3d tangent;
    core::Vec3d bitangent;
    core::Vec3d normal;
};

std::vector<core::Vec3d> smooth(
    std::span<const core::Vec3d> positions,
    const std::vector<std::vector<std::uint32_t>>& neighbours,
    const DeltaSmoothSettings& settings) {
    std::vector<core::Vec3d> current(positions.begin(), positions.end());
    std::vector<core::Vec3d> next(current.size());
    for (std::uint32_t pass = 0U; pass < settings.iterations; ++pass) {
        for (std::size_t vertex = 0U; vertex < current.size(); ++vertex) {
            const auto& around = neighbours[vertex];
            if (around.empty()) {
                next[vertex] = current[vertex];
                continue;
            }
            core::Vec3d average{};
            for (const std::uint32_t neighbour : around) average = average + current[neighbour];
            average = average * (1.0 / static_cast<double>(around.size()));
            next[vertex] = current[vertex] + (average - current[vertex]) * settings.step;
        }
        std::swap(current, next);
    }
    return current;
}

// Area-weighted vertex normal plus a tangent toward the lowest-index
// neighbour, Gram-Schmidt orthogonalized. nullopt when degenerate.
std::optional<Frame> frame_at(
    std::uint32_t vertex,
    const std::vector<core::Vec3d>& positions,
    const std::vector<std::uint32_t>& triangles,
    const std::vector<std::uint32_t>& incident,
    const std::vector<std::uint32_t>& neighbours) {
    core::Vec3d normal{};
    for (const std::uint32_t triangle : incident) {
        const core::Vec3d& a = positions[triangles[triangle * 3U]];
        const core::Vec3d& b = positions[triangles[triangle * 3U + 1U]];
        const core::Vec3d& c = positions[triangles[triangle * 3U + 2U]];
        normal = normal + core::cross(b - a, c - a);
    }
    const double normal_length = normal.length();
    if (neighbours.empty() || !std::isfinite(normal_length) || normal_length <= 1e-300) {
        return std::nullopt;
    }
    normal = normal * (1.0 / normal_length);
    const core::Vec3d edge = positions[neighbours.front()] - positions[vertex];
    core::Vec3d tangent = edge - normal * core::dot(edge, normal);
    const double tangent_length = tangent.length();
    const double scale = edge.length();
    if (!std::isfinite(tangent_length) || tangent_length <= 1e-12 * std::max(scale, 1e-300)) {
        return std::nullopt;
    }
    tangent = tangent * (1.0 / tangent_length);
    return Frame{tangent, core::cross(normal, tangent), normal};
}

void hash_double(std::ostringstream& output, double value) {
    output << std::setprecision(17) << value << ' ';
}

} // namespace

core::Result<void> DeltaSmoothSettings::validate() const {
    if (iterations == 0U || iterations > kMaxIterations) {
        return core::Result<void>::failure(invalid(
            "delta smooth iterations must be inside [1, 256]"));
    }
    if (!std::isfinite(step) || step <= 0.0 || step > 1.0) {
        return core::Result<void>::failure(invalid(
            "delta smooth step must be inside (0, 1]"));
    }
    return core::Result<void>::success();
}

core::Result<DeltaSmoothBinding> DeltaSmoothBinding::build(
    std::span<const core::Vec3d> bind_positions,
    std::span<const std::uint32_t> triangle_indices,
    const DeltaSmoothSettings& settings) {
    if (auto result = settings.validate(); !result) {
        return core::Result<DeltaSmoothBinding>::failure(result.error());
    }
    if (bind_positions.empty() || bind_positions.size() > kMaxVertices) {
        return core::Result<DeltaSmoothBinding>::failure(invalid(
            "delta smooth requires a bounded non-empty bind mesh"));
    }
    if (triangle_indices.empty() || triangle_indices.size() % 3U != 0U) {
        return core::Result<DeltaSmoothBinding>::failure(invalid(
            "delta smooth requires whole triangles"));
    }
    for (const core::Vec3d& position : bind_positions) {
        if (!position.finite()) {
            return core::Result<DeltaSmoothBinding>::failure(invalid(
                "delta smooth bind positions must be finite"));
        }
    }

    DeltaSmoothBinding binding;
    binding.settings_ = settings;
    const std::size_t vertex_count = bind_positions.size();
    binding.neighbours_.assign(vertex_count, {});
    binding.incident_triangles_.assign(vertex_count, {});
    binding.triangles_.assign(triangle_indices.begin(), triangle_indices.end());
    for (std::size_t corner = 0U; corner < triangle_indices.size(); corner += 3U) {
        const std::array<std::uint32_t, 3> triangle{
            triangle_indices[corner], triangle_indices[corner + 1U],
            triangle_indices[corner + 2U]};
        for (const std::uint32_t vertex : triangle) {
            if (vertex >= vertex_count) {
                return core::Result<DeltaSmoothBinding>::failure(invalid(
                    "delta smooth triangle references a vertex outside the mesh"));
            }
        }
        if (triangle[0] == triangle[1] || triangle[1] == triangle[2] ||
            triangle[0] == triangle[2]) {
            return core::Result<DeltaSmoothBinding>::failure(invalid(
                "delta smooth triangle repeats a corner"));
        }
        const auto index = static_cast<std::uint32_t>(corner / 3U);
        for (std::size_t edge = 0U; edge < 3U; ++edge) {
            const std::uint32_t first = triangle[edge];
            const std::uint32_t second = triangle[(edge + 1U) % 3U];
            binding.neighbours_[first].push_back(second);
            binding.neighbours_[second].push_back(first);
            binding.incident_triangles_[first].push_back(index);
        }
    }
    for (auto& around : binding.neighbours_) {
        std::sort(around.begin(), around.end());
        around.erase(std::unique(around.begin(), around.end()), around.end());
    }

    const std::vector<core::Vec3d> smoothed =
        smooth(bind_positions, binding.neighbours_, settings);
    binding.offsets_.assign(vertex_count, std::nullopt);
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        const auto vertex = static_cast<std::uint32_t>(index);
        if (binding.incident_triangles_[vertex].empty()) continue;
        const auto frame = frame_at(vertex, smoothed, binding.triangles_,
            binding.incident_triangles_[vertex], binding.neighbours_[vertex]);
        if (!frame.has_value()) {
            return core::Result<DeltaSmoothBinding>::failure(validation(
                "delta smooth bind frame is degenerate")
                .with_context("vertex=" + std::to_string(vertex)));
        }
        const core::Vec3d delta = bind_positions[vertex] - smoothed[vertex];
        binding.offsets_[vertex] = core::Vec3d{
            core::dot(delta, frame->tangent),
            core::dot(delta, frame->bitangent),
            core::dot(delta, frame->normal)};
    }

    std::ostringstream canonical;
    canonical << "CARTOGRAPHER_DELTA_SMOOTH 1\n" << settings.iterations << ' ';
    hash_double(canonical, settings.step);
    canonical << '\n' << vertex_count << ' ' << binding.triangles_.size() << '\n';
    for (const std::uint32_t corner : binding.triangles_) canonical << corner << ' ';
    canonical << '\n';
    for (const auto& offset : binding.offsets_) {
        if (!offset.has_value()) {
            canonical << "- ";
            continue;
        }
        hash_double(canonical, offset->x);
        hash_double(canonical, offset->y);
        hash_double(canonical, offset->z);
    }
    const std::string text = canonical.str();
    binding.digest_ = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    return core::Result<DeltaSmoothBinding>::success(std::move(binding));
}

core::Result<DeltaSmoothResult> DeltaSmoothBinding::apply(
    std::span<const core::Vec3d> deformed_positions) const {
    if (deformed_positions.size() != offsets_.size()) {
        return core::Result<DeltaSmoothResult>::failure(invalid(
            "delta smooth input is not index-aligned with its bind mesh"));
    }
    for (const core::Vec3d& position : deformed_positions) {
        if (!position.finite()) {
            return core::Result<DeltaSmoothResult>::failure(invalid(
                "delta smooth input positions must be finite"));
        }
    }
    const std::vector<core::Vec3d> smoothed = smooth(deformed_positions, neighbours_, settings_);
    DeltaSmoothResult result;
    result.positions.resize(offsets_.size());
    double total = 0.0;
    for (std::size_t index = 0U; index < offsets_.size(); ++index) {
        const auto vertex = static_cast<std::uint32_t>(index);
        if (!offsets_[vertex].has_value()) {
            result.positions[vertex] = deformed_positions[vertex];
            continue;
        }
        const auto frame = frame_at(vertex, smoothed, triangles_,
            incident_triangles_[vertex], neighbours_[vertex]);
        if (!frame.has_value()) {
            return core::Result<DeltaSmoothResult>::failure(validation(
                "delta smooth deformed frame collapsed")
                .with_context("vertex=" + std::to_string(vertex)));
        }
        const core::Vec3d& offset = *offsets_[vertex];
        result.positions[vertex] = smoothed[vertex] + frame->tangent * offset.x +
            frame->bitangent * offset.y + frame->normal * offset.z;
        const double correction = (result.positions[vertex] - deformed_positions[vertex]).length();
        result.max_correction = std::max(result.max_correction, correction);
        total += correction;
    }
    result.mean_correction = total / static_cast<double>(offsets_.size());
    return core::Result<DeltaSmoothResult>::success(std::move(result));
}

} // namespace carto::deform
