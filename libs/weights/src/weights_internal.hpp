#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/character/weights_record.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/skin.hpp>
#include <carto/weights/paint.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <map>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace carto::weights::detail {

// Influences at or below this weight are pruned before renormalization
// (unless locked, which is validated instead).
inline constexpr double kPruneEpsilon = 1e-12;

inline core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

inline core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic version_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

template <typename T>
bool read_integer(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

inline bool read_double(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    return true;
}

inline bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

inline bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_integer(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

inline bool read_digest(std::istream& input, assets::Sha256Digest& digest) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) return false;
    digest = parsed.value();
    return true;
}

inline void write_vec3(std::ostream& output, const core::Vec3d& value) {
    output << value.x << ' ' << value.y << ' ' << value.z;
}

inline bool read_vec3(std::istream& input, core::Vec3d& value) {
    return static_cast<bool>(input >> value.x >> value.y >> value.z);
}

inline assets::Sha256Digest digest_of(const std::string& canonical) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
}

// Mesh adjacency as sorted unique neighbor index lists. Triangle indices are
// already canonical order in a CompiledMesh, and neighbor lists are sorted so
// every downstream consumer iterates in one deterministic order.
inline std::vector<std::vector<std::uint32_t>> build_adjacency(
    std::size_t vertex_count,
    std::span<const std::uint32_t> indices) {
    std::vector<std::vector<std::uint32_t>> adjacency(vertex_count);
    for (std::size_t corner = 0U; corner + 2U < indices.size(); corner += 3U) {
        const std::uint32_t triangle[3] = {
            indices[corner], indices[corner + 1U], indices[corner + 2U]};
        for (std::uint32_t edge = 0U; edge < 3U; ++edge) {
            const std::uint32_t first = triangle[edge];
            const std::uint32_t second = triangle[(edge + 1U) % 3U];
            adjacency[first].push_back(second);
            adjacency[second].push_back(first);
        }
    }
    for (std::vector<std::uint32_t>& neighbors : adjacency) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }
    return adjacency;
}

inline double distance_between(const core::Vec3d& first, const core::Vec3d& second) {
    return (first - second).length();
}

// One joint's deformation of a point, applied about the joint's rest anchor so
// the identity pose is an exact no-op. This is the single documented
// deformation rule shared by lowering error evaluation, inverse recovery and
// weight refinement; it is a Cartographer cpu_reference model, not a VANTA
// runtime claim.
inline core::Vec3d deform_about_anchor(
    const core::Vec3d& point,
    const core::Vec3d& anchor,
    const core::Transform& transform) {
    const core::Vec3d local = point - anchor;
    return transform.translation +
        transform.rotation.normalized().rotate(
            core::componentwise_multiply(transform.scale, local)) +
        anchor;
}

inline core::Vec3d joint_anchor(
    const kinematic::KinematicGraph& graph,
    kinematic::JointId joint) {
    const kinematic::KinematicNode* node = graph.find(joint);
    return node == nullptr ? core::Vec3d{} : node->rest.translation;
}

// Linear blend of one vertex's influences under a pose. Joints absent from the
// pose are identity; anchors come from the graph rest transforms.
inline core::Vec3d linear_blend(
    const std::vector<kinematic::JointInfluence>& influences,
    const core::Vec3d& point,
    const kinematic::KinematicGraph& graph,
    const std::map<kinematic::JointId, core::Transform>& pose) {
    core::Vec3d blended{};
    for (const kinematic::JointInfluence& influence : influences) {
        const auto posed = pose.find(influence.joint);
        const core::Transform transform =
            posed == pose.end() ? core::Transform::identity() : posed->second;
        blended = blended +
            deform_about_anchor(point, joint_anchor(graph, influence.joint), transform) *
                influence.weight;
    }
    return blended;
}

// Deterministic lock-aware renormalization shared by every weight producer
// (paint strokes, region edits, transfer): locked weights stay fixed, unlocked
// weights scale into the remaining mass, the residual rounding lands on the
// largest unlocked weight, and a locked sum above one refuses instead of
// silently renormalizing.
inline core::Result<void> renormalize_with_locks(
    std::uint32_t vertex,
    std::vector<kinematic::JointInfluence>& influences,
    const WeightLocks* locks) {
    const auto is_locked = [locks, vertex](kinematic::JointId joint) {
        return locks != nullptr && locks->locked(vertex, joint);
    };
    double locked_sum = 0.0;
    double unlocked_sum = 0.0;
    for (const kinematic::JointInfluence& influence : influences) {
        if (is_locked(influence.joint)) {
            if (!std::isfinite(influence.weight) || influence.weight <= 0.0) {
                return core::Result<void>::failure(validation(
                    "a locked skin weight must stay positive and finite"));
            }
            locked_sum += influence.weight;
        } else {
            unlocked_sum += influence.weight;
        }
    }
    if (locked_sum > 1.0 + kinematic::SkinBinding::kWeightSumTolerance) {
        return core::Result<void>::failure(validation(
            "locked weight sum exceeds one; the edit is refused rather than "
            "silently renormalized")
            .with_context("vertex=" + std::to_string(vertex)));
    }
    const bool has_unlocked = std::any_of(influences.begin(), influences.end(),
        [&](const kinematic::JointInfluence& influence) {
            return !is_locked(influence.joint);
        });
    if (!has_unlocked) {
        if (std::abs(locked_sum - 1.0) > kinematic::SkinBinding::kWeightSumTolerance) {
            return core::Result<void>::failure(validation(
                "a fully locked vertex no longer sums to one"));
        }
        return core::Result<void>::success();
    }
    const double target_unlocked = 1.0 - locked_sum;
    // Deterministic order: iterate the already joint-sorted list and scale
    // each unlocked weight; the residual rounding mass lands on the largest
    // unlocked weight.
    auto largest_unlocked = influences.end();
    for (auto it = influences.begin(); it != influences.end(); ++it) {
        if (is_locked(it->joint)) continue;
        it->weight = unlocked_sum > kPruneEpsilon
            ? it->weight * (target_unlocked / unlocked_sum)
            : 0.0;
        if (largest_unlocked == influences.end() ||
            it->weight > largest_unlocked->weight) {
            largest_unlocked = it;
        }
    }
    double rescaled = 0.0;
    for (const kinematic::JointInfluence& influence : influences) {
        if (!is_locked(influence.joint)) rescaled += influence.weight;
    }
    if (largest_unlocked != influences.end()) {
        largest_unlocked->weight += target_unlocked - rescaled;
    }
    return core::Result<void>::success();
}

// Shared per-vertex admission for weight edits: prune dead influences, push
// the list through the lock-aware normalizer, refuse an empty result, sort by
// joint id and re-admit through character::validate_influences.
inline core::Result<void> admit_edited_vertex(
    std::uint32_t vertex,
    std::vector<kinematic::JointInfluence>& working,
    const WeightLocks* locks) {
    const auto is_locked = [locks, vertex](kinematic::JointId joint) {
        return locks != nullptr && locks->locked(vertex, joint);
    };
    working.erase(
        std::remove_if(working.begin(), working.end(),
            [&](const kinematic::JointInfluence& influence) {
                return influence.weight <= kPruneEpsilon && !is_locked(influence.joint);
            }),
        working.end());
    if (auto result = renormalize_with_locks(vertex, working, locks); !result) {
        return result;
    }
    if (working.empty()) {
        return core::Result<void>::failure(validation(
            "a weight edit would leave a vertex without a valid influence")
            .with_context("vertex=" + std::to_string(vertex)));
    }
    std::sort(working.begin(), working.end(), [](const auto& a, const auto& b) {
        return a.joint < b.joint;
    });
    const auto admitted = character::validate_influences(working);
    if (!admitted) {
        return core::Result<void>::failure(
            admitted.error().with_context("vertex=" + std::to_string(vertex)));
    }
    return core::Result<void>::success();
}

} // namespace carto::weights::detail
