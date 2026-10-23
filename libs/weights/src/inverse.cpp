#include <carto/weights/inverse.hpp>

#include "weights_internal.hpp"

#include <carto/character/weights_record.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace carto::weights {
namespace {

constexpr std::string_view kInverseMagic = "CARTOGRAPHER_WEIGHTS_INVERSE_RECEIPT";
constexpr std::string_view kRefineMagic = "CARTOGRAPHER_WEIGHTS_REFINE_RECEIPT";
constexpr std::uint32_t kPowerIterations = 256U;
constexpr double kPruneEpsilon = 1e-12;

core::Result<void> validate_positions(
    const kinematic::SkinBinding& binding,
    std::span<const core::Vec3d> rest_positions,
    std::span<const core::Vec3d> posed_positions) {
    if (rest_positions.size() != posed_positions.size()) {
        return core::Result<void>::failure(detail::invalid(
            "inverse deformation requires matching rest and posed vertex counts"));
    }
    for (const core::Vec3d& position : rest_positions) {
        if (!position.finite()) {
            return core::Result<void>::failure(detail::invalid(
                "inverse deformation requires finite rest positions"));
        }
    }
    for (const core::Vec3d& position : posed_positions) {
        if (!position.finite()) {
            return core::Result<void>::failure(detail::invalid(
                "inverse deformation requires finite posed positions"));
        }
    }
    for (const auto& [vertex, influences] : binding.influences) {
        static_cast<void>(influences);
        if (vertex >= rest_positions.size()) {
            return core::Result<void>::failure(detail::invalid(
                "inverse deformation positions do not cover every influenced vertex"));
        }
    }
    return core::Result<void>::success();
}

double rms_residual(
    const kinematic::SkinBinding& binding,
    const kinematic::KinematicGraph& graph,
    std::span<const core::Vec3d> rest_positions,
    std::span<const core::Vec3d> posed_positions,
    const std::map<kinematic::JointId, core::Transform>& pose) {
    double square_sum = 0.0;
    std::size_t count = 0U;
    for (const auto& [vertex, influences] : binding.influences) {
        const core::Vec3d blended =
            detail::linear_blend(influences, rest_positions[vertex], graph, pose);
        const core::Vec3d delta = blended - posed_positions[vertex];
        square_sum += delta.length_squared();
        ++count;
    }
    if (count == 0U) return 0.0;
    return std::sqrt(square_sum / static_cast<double>(count));
}

void append_u64_le(std::string& output, std::uint64_t value) {
    for (std::uint32_t byte = 0U; byte < 8U; ++byte) {
        output.push_back(static_cast<char>((value >> (byte * 8U)) & 0xffU));
    }
}

assets::Sha256Digest observation_digest(
    std::span<const core::Vec3d> rest_positions,
    std::span<const core::Vec3d> posed_positions) {
    std::string canonical = "CARTOGRAPHER_INVERSE_OBSERVATION_V1";
    canonical.push_back('\0');
    append_u64_le(canonical, static_cast<std::uint64_t>(rest_positions.size()));
    append_u64_le(canonical, static_cast<std::uint64_t>(posed_positions.size()));
    const auto append_positions = [&](std::span<const core::Vec3d> positions) {
        for (const core::Vec3d& position : positions) {
            for (const double component : {position.x, position.y, position.z}) {
                append_u64_le(canonical, std::bit_cast<std::uint64_t>(component));
            }
        }
    };
    append_positions(rest_positions);
    append_positions(posed_positions);
    return detail::digest_of(canonical);
}

assets::Sha256Digest hypothesis_digest(
    const assets::Sha256Digest& graph_digest,
    const assets::Sha256Digest& binding_digest) {
    std::string canonical = "CARTOGRAPHER_INVERSE_RIG_HYPOTHESIS_V1\n";
    canonical += graph_digest.hex();
    canonical.push_back('\n');
    canonical += binding_digest.hex();
    canonical.push_back('\n');
    return detail::digest_of(canonical);
}

std::uint64_t capped_add(std::uint64_t left, std::uint64_t right,
                         std::uint64_t cap) noexcept {
    if (left > cap || right > cap || left > cap - right) return cap + 1U;
    return left + right;
}

std::uint64_t capped_multiply(std::uint64_t left, std::uint64_t right,
                              std::uint64_t cap) noexcept {
    if (left != 0U && right > cap / left) return cap + 1U;
    return left * right;
}

std::uint64_t estimate_hypothesis_work(
    std::size_t position_count,
    std::size_t influenced_vertex_count,
    std::size_t joint_count,
    std::uint32_t iterations,
    std::uint64_t cap) noexcept {
    const auto positions = static_cast<std::uint64_t>(position_count);
    const auto vertices = static_cast<std::uint64_t>(influenced_vertex_count);
    const auto joints = static_cast<std::uint64_t>(joint_count);
    const std::uint64_t joint_vertex_work = capped_multiply(
        capped_multiply(vertices, joints, cap), 8U, cap);
    const std::uint64_t residual_work = capped_multiply(vertices, 8U, cap);
    const std::uint64_t per_iteration = capped_add(
        capped_add(positions, joint_vertex_work, cap), residual_work, cap);
    return capped_multiply(per_iteration, iterations, cap);
}

std::string_view inverse_hypothesis_status_name(InverseHypothesisStatus status) noexcept {
    switch (status) {
    case InverseHypothesisStatus::rejected: return "rejected";
    case InverseHypothesisStatus::incomplete: return "incomplete";
    case InverseHypothesisStatus::converged: return "converged";
    }
    return "unknown";
}

bool safe_review_text(std::string_view value, std::size_t limit,
                      bool allow_newlines) noexcept {
    if (value.empty() || value.size() > limit) return false;
    return std::all_of(value.begin(), value.end(), [allow_newlines](unsigned char byte) {
        return byte >= 0x20U || (allow_newlines && (byte == '\n' || byte == '\t'));
    }) && value.find('\x7f') == std::string_view::npos;
}

void append_length_prefixed(std::string& output, std::string_view value) {
    append_u64_le(output, static_cast<std::uint64_t>(value.size()));
    output.append(value);
}

// Closed-form weighted unit-scale rigid fit (Horn's quaternion method) of
// pairs (source -> target) with weights. Singular inputs (zero total weight or
// degenerate covariance) fail closed instead of assuming an inverse exists.
core::Result<core::Transform> fit_rigid(
    const std::vector<core::Vec3d>& sources,
    const std::vector<core::Vec3d>& targets,
    const std::vector<double>& weights) {
    double total_weight = 0.0;
    core::Vec3d source_centroid{};
    core::Vec3d target_centroid{};
    for (std::size_t index = 0U; index < sources.size(); ++index) {
        total_weight += weights[index];
        source_centroid = source_centroid + sources[index] * weights[index];
        target_centroid = target_centroid + targets[index] * weights[index];
    }
    if (sources.empty() || total_weight <= 0.0 || !std::isfinite(total_weight)) {
        return core::Result<core::Transform>::failure(detail::validation(
            "rigid fit is singular: no weighted correspondence"));
    }
    source_centroid = source_centroid * (1.0 / total_weight);
    target_centroid = target_centroid * (1.0 / total_weight);

    // Cross-covariance M[row][col] = sum w (s_row - cs_row)(t_col - ct_col).
    std::array<std::array<double, 3>, 3> matrix{};
    for (std::size_t index = 0U; index < sources.size(); ++index) {
        const core::Vec3d source = sources[index] - source_centroid;
        const core::Vec3d target = targets[index] - target_centroid;
        const double weight = weights[index];
        for (std::uint32_t row = 0U; row < 3U; ++row) {
            const double source_component =
                row == 0U ? source.x : (row == 1U ? source.y : source.z);
            for (std::uint32_t col = 0U; col < 3U; ++col) {
                const double target_component =
                    col == 0U ? target.x : (col == 1U ? target.y : target.z);
                matrix[row][col] += weight * source_component * target_component;
            }
        }
    }

    // Horn's symmetric 4x4 N matrix (quaternion ordering w, x, y, z).
    const double m00 = matrix[0][0];
    const double m01 = matrix[0][1];
    const double m02 = matrix[0][2];
    const double m10 = matrix[1][0];
    const double m11 = matrix[1][1];
    const double m12 = matrix[1][2];
    const double m20 = matrix[2][0];
    const double m21 = matrix[2][1];
    const double m22 = matrix[2][2];
    const double trace = m00 + m11 + m22;
    std::array<std::array<double, 4>, 4> horn{
        std::array<double, 4>{trace, m12 - m21, m20 - m02, m01 - m10},
        std::array<double, 4>{m12 - m21, m00 - m11 - m22, m01 + m10, m20 + m02},
        std::array<double, 4>{m20 - m02, m01 + m10, -m00 + m11 - m22, m12 + m21},
        std::array<double, 4>{m01 - m10, m20 + m02, m12 + m21, -m00 - m11 + m22},
    };

    // Shift by a positive diagonal so power iteration converges to the largest
    // eigenvalue (all eigenvalues positive) with the same eigenvectors.
    double shift = 1.0;
    for (const auto& row : horn) {
        for (const double entry : row) shift = std::max(shift, std::abs(entry));
    }
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        horn[index][index] += shift;
    }

    std::array<double, 4> vector{1.0, 0.0, 0.0, 0.0};
    for (std::uint32_t iteration = 0U; iteration < kPowerIterations; ++iteration) {
        std::array<double, 4> next{};
        for (std::uint32_t row = 0U; row < 4U; ++row) {
            for (std::uint32_t col = 0U; col < 4U; ++col) {
                next[row] += horn[row][col] * vector[col];
            }
        }
        double magnitude = 0.0;
        for (const double entry : next) magnitude += entry * entry;
        magnitude = std::sqrt(magnitude);
        if (!std::isfinite(magnitude) || magnitude <= 0.0) {
            return core::Result<core::Transform>::failure(detail::validation(
                "rigid fit is singular: the orientation system is degenerate"));
        }
        double change = 0.0;
        for (std::uint32_t index = 0U; index < 4U; ++index) {
            next[index] /= magnitude;
            change = std::max(change, std::abs(next[index] - vector[index]));
        }
        vector = next;
        if (change < 1e-15) break;
    }

    core::Quaternion rotation{
        vector[1], vector[2], vector[3], vector[0],
    };
    if (!rotation.normalizable()) {
        return core::Result<core::Transform>::failure(detail::validation(
            "rigid fit produced a non-finite rotation"));
    }
    rotation = rotation.normalized();
    if (rotation.w < 0.0) {
        rotation = {-rotation.x, -rotation.y, -rotation.z, -rotation.w};
    }
    core::Transform transform;
    transform.rotation = rotation;
    transform.translation = target_centroid - rotation.rotate(source_centroid);
    transform.scale = {1.0, 1.0, 1.0};
    return core::Result<core::Transform>::success(transform);
}

double rotation_delta(
    const core::Quaternion& before,
    const core::Quaternion& after) noexcept {
    const core::Quaternion inverse{-before.x, -before.y, -before.z, before.w};
    core::Quaternion delta = after * inverse;
    if (!delta.normalizable()) return 0.0;
    delta = delta.normalized();
    const double w = std::clamp(std::abs(delta.w), -1.0, 1.0);
    return 2.0 * std::acos(w);
}

} // namespace

core::Result<void> InverseSolveOptions::validate() const {
    if (max_iterations == 0U || max_iterations > kMaxIterations) {
        return core::Result<void>::failure(detail::invalid(
            "inverse solve iterations exceed the bound"));
    }
    if (!std::isfinite(tolerance) || tolerance <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "inverse solve tolerance must be positive and finite"));
    }
    if (!std::isfinite(influence_epsilon) || influence_epsilon <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "inverse solve influence epsilon must be positive and finite"));
    }
    return core::Result<void>::success();
}

core::Result<void> InverseSolveReceipt::validate() const {
    if (input_binding_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid(
            "inverse solve receipt requires the input binding digest"));
    }
    if (max_iterations == 0U || iterations_used > max_iterations) {
        return core::Result<void>::failure(detail::validation(
            "inverse solve receipt iteration counts are inconsistent"));
    }
    for (const double value : {initial_rms_residual, final_rms_residual,
                               final_translation_delta, final_rotation_delta}) {
        if (!std::isfinite(value) || value < 0.0) {
            return core::Result<void>::failure(detail::validation(
                "inverse solve receipt metrics must be finite and non-negative"));
        }
    }
    return core::Result<void>::success();
}

std::string InverseSolveReceipt::serialize() const {
    std::ostringstream output;
    output << kInverseMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "INPUT " << input_binding_digest.hex() << '\n';
    output << "MAX_ITERATIONS " << max_iterations << '\n';
    output << "ITERATIONS_USED " << iterations_used << '\n';
    output << "CONVERGED " << (converged ? 1U : 0U) << '\n';
    output << "INITIAL_RMS " << initial_rms_residual << '\n';
    output << "FINAL_RMS " << final_rms_residual << '\n';
    output << "FINAL_TRANSLATION_DELTA " << final_translation_delta << '\n';
    output << "FINAL_ROTATION_DELTA " << final_rotation_delta << '\n';
    output << "JOINTS " << solved_joint_count << '\n';
    output << "END\n";
    return output.str();
}

core::Result<InverseSolveReceipt> InverseSolveReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<InverseSolveReceipt>::failure(detail::parse_error(
            "inverse solve receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    std::uint32_t converged_token = 0U;
    if (!(input >> magic) || magic != kInverseMagic ||
        !detail::read_integer(input, version) || version != kSchemaVersion) {
        return core::Result<InverseSolveReceipt>::failure(detail::parse_error(
            "inverse solve receipt framing is invalid"));
    }
    InverseSolveReceipt receipt;
    if (!detail::require_token(input, "INPUT") ||
        !detail::read_digest(input, receipt.input_binding_digest) ||
        !detail::require_token(input, "MAX_ITERATIONS") ||
        !detail::read_integer(input, receipt.max_iterations) ||
        !detail::require_token(input, "ITERATIONS_USED") ||
        !detail::read_integer(input, receipt.iterations_used) ||
        !detail::require_token(input, "CONVERGED") ||
        !detail::read_integer(input, converged_token) ||
        !detail::require_token(input, "INITIAL_RMS") ||
        !detail::read_double(input, receipt.initial_rms_residual) ||
        !detail::require_token(input, "FINAL_RMS") ||
        !detail::read_double(input, receipt.final_rms_residual) ||
        !detail::require_token(input, "FINAL_TRANSLATION_DELTA") ||
        !detail::read_double(input, receipt.final_translation_delta) ||
        !detail::require_token(input, "FINAL_ROTATION_DELTA") ||
        !detail::read_double(input, receipt.final_rotation_delta) ||
        !detail::require_token(input, "JOINTS") ||
        !detail::read_integer(input, receipt.solved_joint_count) ||
        !detail::require_token(input, "END")) {
        return core::Result<InverseSolveReceipt>::failure(detail::parse_error(
            "inverse solve receipt body is invalid"));
    }
    if (converged_token > 1U) {
        return core::Result<InverseSolveReceipt>::failure(detail::parse_error(
            "inverse solve receipt converged flag is invalid"));
    }
    receipt.converged = converged_token == 1U;
    if (auto result = receipt.validate(); !result) {
        return core::Result<InverseSolveReceipt>::failure(result.error());
    }
    return core::Result<InverseSolveReceipt>::success(std::move(receipt));
}

core::Result<assets::Sha256Digest> InverseSolveReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

core::Result<InverseSolveOutput> recover_joint_transforms(
    const kinematic::SkinBinding& binding,
    const kinematic::KinematicGraph& graph,
    std::span<const core::Vec3d> rest_positions,
    std::span<const core::Vec3d> posed_positions,
    const InverseSolveOptions& options) {
    if (auto result = binding.validate(); !result) {
        return core::Result<InverseSolveOutput>::failure(result.error());
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<InverseSolveOutput>::failure(result.error());
    }
    if (auto result = options.validate(); !result) {
        return core::Result<InverseSolveOutput>::failure(result.error());
    }
    if (auto result = validate_positions(binding, rest_positions, posed_positions);
        !result) {
        return core::Result<InverseSolveOutput>::failure(result.error());
    }
    const auto binding_digest = binding.canonical_digest();
    if (!binding_digest) {
        return core::Result<InverseSolveOutput>::failure(binding_digest.error());
    }

    // Solved joints: every joint referenced by the binding, in sorted order.
    std::vector<kinematic::JointId> joints;
    for (const auto& [vertex, influences] : binding.influences) {
        for (const kinematic::JointInfluence& influence : influences) {
            if (std::find(joints.begin(), joints.end(), influence.joint) == joints.end()) {
                joints.push_back(influence.joint);
            }
        }
    }
    std::sort(joints.begin(), joints.end());
    if (joints.empty()) {
        return core::Result<InverseSolveOutput>::failure(detail::invalid(
            "inverse solve requires a binding with at least one influence"));
    }

    std::map<kinematic::JointId, core::Transform> pose;
    for (const kinematic::JointId joint : joints) {
        pose.emplace(joint, core::Transform::identity());
    }

    const double initial_rms =
        rms_residual(binding, graph, rest_positions, posed_positions, pose);
    double best_rms = initial_rms;
    std::map<kinematic::JointId, core::Transform> best_pose = pose;

    InverseSolveReceipt receipt;
    receipt.input_binding_digest = binding_digest.value();
    receipt.max_iterations = options.max_iterations;
    receipt.initial_rms_residual = initial_rms;
    receipt.final_rms_residual = initial_rms;
    receipt.solved_joint_count = joints.size();

    for (std::uint32_t iteration = 0U; iteration < options.max_iterations; ++iteration) {
        double max_translation_delta = 0.0;
        double max_rotation_delta = 0.0;
        for (const kinematic::JointId joint : joints) {
            const core::Vec3d anchor = detail::joint_anchor(graph, joint);
            std::vector<core::Vec3d> sources;
            std::vector<core::Vec3d> targets;
            std::vector<double> weights;
            for (const auto& [vertex, influences] : binding.influences) {
                const auto found = std::find_if(
                    influences.begin(), influences.end(),
                    [joint](const kinematic::JointInfluence& influence) {
                        return influence.joint == joint;
                    });
                if (found == influences.end() || found->weight < options.influence_epsilon) {
                    continue;
                }
                const double weight = found->weight;
                const core::Vec3d point = rest_positions[vertex];
                core::Vec3d blended =
                    detail::linear_blend(influences, point, graph, pose);
                const core::Vec3d own = detail::deform_about_anchor(
                    point, anchor, pose[joint]) * weight;
                blended = blended - own;
                const core::Vec3d target =
                    (posed_positions[vertex] - blended) * (1.0 / weight);
                sources.push_back(point);
                targets.push_back(target);
                weights.push_back(weight * weight);
            }
            const auto fitted = fit_rigid(sources, targets, weights);
            if (!fitted) {
                return core::Result<InverseSolveOutput>::failure(
                    fitted.error().with_context(
                        "joint=" + std::to_string(joint.value)));
            }
            // fit_rigid returns the deformation as a global rigid map
            // (rotation about the origin); the pose convention shared with
            // deform_about_anchor rotates about the joint anchor, so convert
            // the translation back: t = c - a + R*a.
            core::Transform anchor_relative = fitted.value();
            anchor_relative.translation =
                fitted.value().translation +
                fitted.value().rotation.rotate(anchor) - anchor;
            const core::Transform& previous = pose[joint];
            max_translation_delta = std::max(
                max_translation_delta,
                (anchor_relative.translation - previous.translation).length());
            max_rotation_delta = std::max(
                max_rotation_delta,
                rotation_delta(previous.rotation, anchor_relative.rotation));
            pose[joint] = anchor_relative;
        }
        const double current_rms =
            rms_residual(binding, graph, rest_positions, posed_positions, pose);
        receipt.iterations_used = iteration + 1U;
        receipt.final_translation_delta = max_translation_delta;
        receipt.final_rotation_delta = max_rotation_delta;
        if (current_rms < best_rms) {
            best_rms = current_rms;
            best_pose = pose;
        }
        if (max_translation_delta < options.tolerance &&
            max_rotation_delta < options.tolerance) {
            receipt.converged = true;
            receipt.final_rms_residual = current_rms;
            break;
        }
        receipt.final_rms_residual = best_rms;
    }
    if (!receipt.converged) {
        // Iteration bound hit: report the best-valid iterand, explicitly
        // marked incomplete.
        receipt.final_rms_residual = best_rms;
    }

    InverseSolveOutput output;
    output.joint_transforms = std::move(best_pose);
    output.receipt = std::move(receipt);
    return core::Result<InverseSolveOutput>::success(std::move(output));
}

core::Result<void> InverseRigHypothesisOptions::validate() const {
    if (max_hypotheses == 0U || max_hypotheses > kHardMaxHypotheses ||
        max_solver_work_units == 0U ||
        max_solver_work_units > kHardMaxSolverWorkUnits) {
        return core::Result<void>::failure(detail::invalid(
            "inverse rig hypothesis count or work budget is outside its hard bound"));
    }
    return solver.validate();
}

core::Result<InverseRigHypothesisSearch> evaluate_inverse_rig_hypotheses(
    std::span<const InverseRigHypothesisInput> hypotheses,
    const assets::Sha256Digest& expected_source_mesh_digest,
    std::span<const core::Vec3d> rest_positions,
    std::span<const core::Vec3d> posed_positions,
    const InverseRigHypothesisOptions& options) {
    if (auto result = options.validate(); !result) {
        return core::Result<InverseRigHypothesisSearch>::failure(result.error());
    }
    if (expected_source_mesh_digest.is_zero()) {
        return core::Result<InverseRigHypothesisSearch>::failure(detail::invalid(
            "inverse rig hypothesis search requires a source mesh digest"));
    }
    if (hypotheses.empty() || hypotheses.size() > options.max_hypotheses ||
        hypotheses.size() > InverseRigHypothesisOptions::kHardMaxHypotheses) {
        return core::Result<InverseRigHypothesisSearch>::failure(detail::invalid(
            "inverse rig hypothesis set is empty or exceeds its candidate bound"));
    }
    if (rest_positions.empty() || rest_positions.size() != posed_positions.size() ||
        rest_positions.size() >
            InverseRigHypothesisOptions::kHardMaxObservationVertices ||
        rest_positions.size() >
            static_cast<std::size_t>(kinematic::SkinBinding::kMaxVertexIndex) + 1U) {
        return core::Result<InverseRigHypothesisSearch>::failure(detail::invalid(
            "inverse rig hypothesis observation has mismatched, empty, or oversized positions"));
    }
    for (const core::Vec3d& position : rest_positions) {
        if (!position.finite()) {
            return core::Result<InverseRigHypothesisSearch>::failure(detail::invalid(
                "inverse rig hypothesis observation contains non-finite rest positions"));
        }
    }
    for (const core::Vec3d& position : posed_positions) {
        if (!position.finite()) {
            return core::Result<InverseRigHypothesisSearch>::failure(detail::invalid(
                "inverse rig hypothesis observation contains non-finite posed positions"));
        }
    }

    struct PendingHypothesis final {
        std::size_t result_index = 0U;
        const InverseRigHypothesisInput* input = nullptr;
    };

    InverseRigHypothesisSearch search;
    search.source_mesh_digest = expected_source_mesh_digest;
    search.observation_digest = observation_digest(rest_positions, posed_positions);
    search.ranked_hypotheses.reserve(hypotheses.size());
    std::vector<PendingHypothesis> pending;
    pending.reserve(hypotheses.size());
    std::map<std::uint64_t, std::size_t> id_counts;
    for (const auto& hypothesis : hypotheses) {
        if (hypothesis.id != 0U) ++id_counts[hypothesis.id];
    }

    std::optional<std::uint64_t> source_mesh_asset;
    std::optional<assets::Sha256Digest> candidate_mesh_digest;
    std::uint64_t estimated_work = 0U;
    const auto refuse = [](InverseRigHypothesisResult& row, std::string message) {
        row.status = InverseHypothesisStatus::rejected;
        row.diagnostic = std::move(message);
    };

    for (std::size_t index = 0U; index < hypotheses.size(); ++index) {
        const InverseRigHypothesisInput& hypothesis = hypotheses[index];
        InverseRigHypothesisResult row;
        row.id = hypothesis.id;
        row.input_ordinal = static_cast<std::uint32_t>(index + 1U);
        if (hypothesis.id == 0U || id_counts[hypothesis.id] != 1U) {
            refuse(row, hypothesis.id == 0U
                ? "inverse rig hypothesis identity must be non-zero"
                : "inverse rig hypothesis identities must be unique");
        } else if (hypothesis.graph == nullptr || hypothesis.binding == nullptr) {
            refuse(row, "inverse rig hypothesis is missing its graph or skin binding");
        } else {
            const auto graph_digest = hypothesis.graph->canonical_digest();
            const auto binding_digest = hypothesis.binding->canonical_digest();
            if (!graph_digest) {
                refuse(row, graph_digest.error().message);
            } else if (!binding_digest) {
                row.graph_digest = graph_digest.value();
                refuse(row, binding_digest.error().message);
            } else {
                row.graph_digest = graph_digest.value();
                row.binding_digest = binding_digest.value();
                row.hypothesis_digest = hypothesis_digest(
                    graph_digest.value(), binding_digest.value());
                if (hypothesis.binding->graph_digest != graph_digest.value()) {
                    refuse(row, "inverse rig hypothesis binding is stale for its graph");
                } else if (hypothesis.binding->mesh_digest != expected_source_mesh_digest) {
                    refuse(row,
                        "inverse rig hypothesis binding does not match the expected source mesh revision");
                } else if (hypothesis.binding->influences.empty()) {
                    refuse(row, "inverse rig hypothesis binding has no influenced vertices");
                } else if (auto result = validate_positions(
                               *hypothesis.binding, rest_positions, posed_positions);
                           !result) {
                    refuse(row, result.error().message);
                } else if (source_mesh_asset.has_value() &&
                           (*source_mesh_asset != hypothesis.binding->mesh_asset ||
                            *candidate_mesh_digest != hypothesis.binding->mesh_digest)) {
                    refuse(row,
                        "inverse rig hypothesis targets a different source mesh revision");
                } else {
                    std::map<kinematic::JointId, bool> referenced_joints;
                    for (const auto& [vertex, influences] : hypothesis.binding->influences) {
                        static_cast<void>(vertex);
                        for (const auto& influence : influences) {
                            referenced_joints.emplace(influence.joint, true);
                        }
                    }
                    const std::uint64_t work = estimate_hypothesis_work(
                        rest_positions.size(), hypothesis.binding->influences.size(),
                        referenced_joints.size(), options.solver.max_iterations,
                        options.max_solver_work_units);
                    estimated_work = capped_add(
                        estimated_work, work, options.max_solver_work_units);
                    if (estimated_work > options.max_solver_work_units) {
                        return core::Result<InverseRigHypothesisSearch>::failure(
                            detail::invalid(
                                "inverse rig hypothesis search exceeds its bounded solver work"));
                    }
                    if (!source_mesh_asset.has_value()) {
                        source_mesh_asset = hypothesis.binding->mesh_asset;
                        candidate_mesh_digest = hypothesis.binding->mesh_digest;
                    }
                    pending.push_back({search.ranked_hypotheses.size(), &hypothesis});
                }
            }
        }
        search.ranked_hypotheses.push_back(std::move(row));
    }

    for (const PendingHypothesis& item : pending) {
        InverseRigHypothesisResult& row = search.ranked_hypotheses[item.result_index];
        const auto solved = recover_joint_transforms(
            *item.input->binding, *item.input->graph,
            rest_positions, posed_positions, options.solver);
        if (!solved) {
            refuse(row, solved.error().message);
            continue;
        }
        row.status = solved.value().receipt.converged
            ? InverseHypothesisStatus::converged
            : InverseHypothesisStatus::incomplete;
        row.final_rms_residual = solved.value().receipt.final_rms_residual;
        row.solve_receipt = solved.value().receipt;
        if (!solved.value().receipt.converged) {
            row.diagnostic =
                "inverse fit reached its iteration bound; result remains incomplete";
        }
    }

    std::sort(search.ranked_hypotheses.begin(), search.ranked_hypotheses.end(),
        [](const InverseRigHypothesisResult& left,
           const InverseRigHypothesisResult& right) {
            if (left.final_rms_residual.has_value() !=
                right.final_rms_residual.has_value()) {
                return left.final_rms_residual.has_value();
            }
            if (left.final_rms_residual.has_value() &&
                *left.final_rms_residual != *right.final_rms_residual) {
                return *left.final_rms_residual < *right.final_rms_residual;
            }
            if (left.hypothesis_digest.has_value() !=
                right.hypothesis_digest.has_value()) {
                return left.hypothesis_digest.has_value();
            }
            if (left.hypothesis_digest.has_value()) {
                const std::string left_digest = left.hypothesis_digest->hex();
                const std::string right_digest = right.hypothesis_digest->hex();
                if (left_digest != right_digest) return left_digest < right_digest;
            }
            if (left.id != right.id) return left.id < right.id;
            return left.input_ordinal < right.input_ordinal;
        });
    return core::Result<InverseRigHypothesisSearch>::success(std::move(search));
}

core::Result<void> InverseRigHypothesisLock::validate() const {
    if (hypothesis_id == 0U ||
        (preview_status != InverseHypothesisStatus::incomplete &&
         preview_status != InverseHypothesisStatus::converged) ||
        source_mesh_digest.is_zero() || observation_digest.is_zero() || graph_digest.is_zero() ||
        binding_digest.is_zero() || hypothesis_digest.is_zero() ||
        selection_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid(
            "inverse rig hypothesis lock has invalid identity, status, or digests"));
    }
    if (auto result = preview_receipt.validate(); !result) return result;
    if (preview_receipt.input_binding_digest != binding_digest ||
        preview_receipt.converged !=
            (preview_status == InverseHypothesisStatus::converged)) {
        return core::Result<void>::failure(detail::validation(
            "inverse rig hypothesis lock does not match its preview receipt"));
    }
    if (!safe_review_text(reviewer_identity, kMaxReviewerBytes, false) ||
        !safe_review_text(justification, kMaxJustificationBytes, true)) {
        return core::Result<void>::failure(detail::invalid(
            "inverse rig reviewer identity or justification is missing or unsafe"));
    }
    if (detail::digest_of(canonical_without_digest()) != selection_digest) {
        return core::Result<void>::failure(detail::validation(
            "inverse rig hypothesis lock digest does not match its review fields"));
    }
    return core::Result<void>::success();
}

std::string InverseRigHypothesisLock::canonical_without_digest() const {
    std::string canonical = "CARTOGRAPHER_INVERSE_RIG_LOCK_V1";
    canonical.push_back('\0');
    append_u64_le(canonical, hypothesis_id);
    append_length_prefixed(canonical, inverse_hypothesis_status_name(preview_status));
    append_length_prefixed(canonical, source_mesh_digest.hex());
    append_length_prefixed(canonical, observation_digest.hex());
    append_length_prefixed(canonical, graph_digest.hex());
    append_length_prefixed(canonical, binding_digest.hex());
    append_length_prefixed(canonical, hypothesis_digest.hex());
    append_length_prefixed(canonical, preview_receipt.serialize());
    append_length_prefixed(canonical, reviewer_identity);
    append_length_prefixed(canonical, justification);
    return canonical;
}

core::Result<void> InverseRigHypothesisLock::refresh_selection_digest() {
    if (hypothesis_id == 0U || source_mesh_digest.is_zero() || observation_digest.is_zero() ||
        graph_digest.is_zero() || binding_digest.is_zero() ||
        hypothesis_digest.is_zero() ||
        (preview_status != InverseHypothesisStatus::incomplete &&
         preview_status != InverseHypothesisStatus::converged) ||
        !safe_review_text(reviewer_identity, kMaxReviewerBytes, false) ||
        !safe_review_text(justification, kMaxJustificationBytes, true)) {
        return core::Result<void>::failure(detail::invalid(
            "inverse rig hypothesis lock cannot be digested before required review fields are valid"));
    }
    if (auto result = preview_receipt.validate(); !result) return result;
    if (preview_receipt.input_binding_digest != binding_digest ||
        preview_receipt.converged !=
            (preview_status == InverseHypothesisStatus::converged)) {
        return core::Result<void>::failure(detail::validation(
            "inverse rig hypothesis lock does not match its preview receipt"));
    }
    selection_digest = detail::digest_of(canonical_without_digest());
    return core::Result<void>::success();
}

core::Result<InverseRigHypothesisLock> lock_inverse_rig_hypothesis(
    const InverseRigHypothesisSearch& search,
    std::span<const InverseRigHypothesisInput> hypotheses,
    std::uint64_t hypothesis_id,
    std::string reviewer_identity,
    std::string justification) {
    if (hypothesis_id == 0U || search.source_mesh_digest.is_zero() ||
        search.observation_digest.is_zero()) {
        return core::Result<InverseRigHypothesisLock>::failure(detail::invalid(
            "inverse rig hypothesis lock requires a named candidate and observation"));
    }
    const InverseRigHypothesisResult* selected = nullptr;
    for (const auto& row : search.ranked_hypotheses) {
        if (row.id != hypothesis_id) continue;
        if (selected != nullptr) {
            return core::Result<InverseRigHypothesisLock>::failure(detail::validation(
                "inverse rig search contains an ambiguous candidate identity"));
        }
        selected = &row;
    }
    if (selected == nullptr ||
        selected->status == InverseHypothesisStatus::rejected ||
        !selected->solve_receipt.has_value() ||
        !selected->graph_digest.has_value() ||
        !selected->binding_digest.has_value() ||
        !selected->hypothesis_digest.has_value()) {
        return core::Result<InverseRigHypothesisLock>::failure(detail::validation(
            "inverse rig hypothesis is unavailable for reviewer selection"));
    }

    const InverseRigHypothesisInput* input = nullptr;
    for (const auto& candidate : hypotheses) {
        if (candidate.id != hypothesis_id) continue;
        if (input != nullptr) {
            return core::Result<InverseRigHypothesisLock>::failure(detail::validation(
                "inverse rig input contains an ambiguous candidate identity"));
        }
        input = &candidate;
    }
    if (input == nullptr || input->graph == nullptr || input->binding == nullptr) {
        return core::Result<InverseRigHypothesisLock>::failure(detail::invalid(
            "inverse rig candidate graph or binding is unavailable for reviewer selection"));
    }
    const auto graph_digest = input->graph->canonical_digest();
    const auto binding_digest = input->binding->canonical_digest();
    if (!graph_digest || !binding_digest) {
        return core::Result<InverseRigHypothesisLock>::failure(
            !graph_digest ? graph_digest.error() : binding_digest.error());
    }
    const auto candidate_digest = hypothesis_digest(
        graph_digest.value(), binding_digest.value());
    if (graph_digest.value() != *selected->graph_digest ||
        binding_digest.value() != *selected->binding_digest ||
        input->binding->mesh_digest != search.source_mesh_digest ||
        candidate_digest != *selected->hypothesis_digest ||
        selected->solve_receipt->input_binding_digest != binding_digest.value()) {
        return core::Result<InverseRigHypothesisLock>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "inverse rig candidate changed after its preview search"));
    }

    InverseRigHypothesisLock lock;
    lock.hypothesis_id = hypothesis_id;
    lock.preview_status = selected->status;
    lock.source_mesh_digest = search.source_mesh_digest;
    lock.observation_digest = search.observation_digest;
    lock.graph_digest = graph_digest.value();
    lock.binding_digest = binding_digest.value();
    lock.hypothesis_digest = candidate_digest;
    lock.preview_receipt = *selected->solve_receipt;
    lock.reviewer_identity = std::move(reviewer_identity);
    lock.justification = std::move(justification);
    if (auto result = lock.refresh_selection_digest(); !result) {
        return core::Result<InverseRigHypothesisLock>::failure(result.error());
    }
    return core::Result<InverseRigHypothesisLock>::success(std::move(lock));
}

core::Result<InverseRigHypothesisRerun> rerun_locked_inverse_rig_hypothesis(
    const InverseRigHypothesisLock& selection,
    const InverseRigHypothesisInput& hypothesis,
    const assets::Sha256Digest& source_mesh_digest,
    std::span<const core::Vec3d> rest_positions,
    std::span<const core::Vec3d> posed_positions,
    const InverseRigHypothesisOptions& options) {
    if (auto result = selection.validate(); !result) {
        return core::Result<InverseRigHypothesisRerun>::failure(result.error());
    }
    if (auto result = options.validate(); !result) {
        return core::Result<InverseRigHypothesisRerun>::failure(result.error());
    }
    if (source_mesh_digest.is_zero() || source_mesh_digest != selection.source_mesh_digest ||
        hypothesis.id != selection.hypothesis_id || hypothesis.graph == nullptr ||
        hypothesis.binding == nullptr) {
        return core::Result<InverseRigHypothesisRerun>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "locked inverse rig candidate identity or source is no longer available"));
    }
    if (rest_positions.empty() || rest_positions.size() != posed_positions.size() ||
        rest_positions.size() >
            InverseRigHypothesisOptions::kHardMaxObservationVertices ||
        rest_positions.size() >
            static_cast<std::size_t>(kinematic::SkinBinding::kMaxVertexIndex) + 1U) {
        return core::Result<InverseRigHypothesisRerun>::failure(detail::invalid(
            "locked inverse rig rerun requires matching nonempty position arrays"));
    }
    for (const core::Vec3d& position : rest_positions) {
        if (!position.finite()) {
            return core::Result<InverseRigHypothesisRerun>::failure(detail::invalid(
                "locked inverse rig rerun contains non-finite rest positions"));
        }
    }
    for (const core::Vec3d& position : posed_positions) {
        if (!position.finite()) {
            return core::Result<InverseRigHypothesisRerun>::failure(detail::invalid(
                "locked inverse rig rerun contains non-finite posed positions"));
        }
    }
    const auto current_graph_digest = hypothesis.graph->canonical_digest();
    const auto current_binding_digest = hypothesis.binding->canonical_digest();
    if (!current_graph_digest || !current_binding_digest) {
        return core::Result<InverseRigHypothesisRerun>::failure(
            !current_graph_digest ? current_graph_digest.error() :
                                    current_binding_digest.error());
    }
    const auto current_hypothesis_digest = hypothesis_digest(
        current_graph_digest.value(), current_binding_digest.value());
    const auto current_observation_digest =
        observation_digest(rest_positions, posed_positions);
    if (current_graph_digest.value() != selection.graph_digest ||
        current_binding_digest.value() != selection.binding_digest ||
        hypothesis.binding->graph_digest != current_graph_digest.value() ||
        hypothesis.binding->mesh_digest != selection.source_mesh_digest ||
        current_hypothesis_digest != selection.hypothesis_digest ||
        current_observation_digest != selection.observation_digest) {
        return core::Result<InverseRigHypothesisRerun>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "locked inverse rig candidate or mesh observation changed before rerun"));
    }

    if (auto result = validate_positions(
            *hypothesis.binding, rest_positions, posed_positions); !result) {
        return core::Result<InverseRigHypothesisRerun>::failure(result.error());
    }
    std::map<kinematic::JointId, bool> referenced_joints;
    for (const auto& [vertex, influences] : hypothesis.binding->influences) {
        static_cast<void>(vertex);
        for (const auto& influence : influences) {
            referenced_joints.emplace(influence.joint, true);
        }
    }
    const std::uint64_t work = estimate_hypothesis_work(
        rest_positions.size(), hypothesis.binding->influences.size(),
        referenced_joints.size(), options.solver.max_iterations,
        options.max_solver_work_units);
    if (work > options.max_solver_work_units) {
        return core::Result<InverseRigHypothesisRerun>::failure(detail::invalid(
            "locked inverse rig rerun exceeds its bounded solver work"));
    }

    const auto solved = recover_joint_transforms(
        *hypothesis.binding, *hypothesis.graph,
        rest_positions, posed_positions, options.solver);
    if (!solved) return core::Result<InverseRigHypothesisRerun>::failure(solved.error());
    InverseRigHypothesisRerun rerun;
    rerun.selection_digest = selection.selection_digest;
    rerun.observation_digest = current_observation_digest;
    rerun.hypothesis_digest = current_hypothesis_digest;
    rerun.status = solved.value().receipt.converged
        ? InverseHypothesisStatus::converged
        : InverseHypothesisStatus::incomplete;
    rerun.solution = solved.value();
    return core::Result<InverseRigHypothesisRerun>::success(std::move(rerun));
}

core::Result<void> WeightRefinementOptions::validate() const {
    if (max_iterations == 0U || max_iterations > kMaxIterations) {
        return core::Result<void>::failure(detail::invalid(
            "weight refinement iterations exceed the bound"));
    }
    if (!std::isfinite(step_size) || step_size <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "weight refinement step size must be positive and finite"));
    }
    if (!std::isfinite(prune_epsilon) || prune_epsilon <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "weight refinement prune epsilon must be positive and finite"));
    }
    return core::Result<void>::success();
}

core::Result<void> WeightRefinementReceipt::validate() const {
    if (input_binding_digest.is_zero() || output_binding_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid(
            "weight refinement receipt requires input and output digests"));
    }
    for (const double value : {step_size, initial_rms_residual, final_rms_residual}) {
        if (!std::isfinite(value) || value < 0.0) {
            return core::Result<void>::failure(detail::validation(
                "weight refinement receipt metrics must be finite and non-negative"));
        }
    }
    return core::Result<void>::success();
}

std::string WeightRefinementReceipt::serialize() const {
    std::ostringstream output;
    output << kRefineMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "INPUT " << input_binding_digest.hex() << '\n';
    output << "OUTPUT " << output_binding_digest.hex() << '\n';
    output << "ITERATIONS_USED " << iterations_used << '\n';
    output << "STEP_SIZE " << step_size << '\n';
    output << "INITIAL_RMS " << initial_rms_residual << '\n';
    output << "FINAL_RMS " << final_rms_residual << '\n';
    output << "IMPROVED " << (improved ? 1U : 0U) << '\n';
    output << "ADJUSTED_VERTICES " << adjusted_vertex_count << '\n';
    output << "END\n";
    return output.str();
}

core::Result<WeightRefinementReceipt> WeightRefinementReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<WeightRefinementReceipt>::failure(detail::parse_error(
            "weight refinement receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    std::uint32_t improved_token = 0U;
    if (!(input >> magic) || magic != kRefineMagic ||
        !detail::read_integer(input, version) || version != kSchemaVersion) {
        return core::Result<WeightRefinementReceipt>::failure(detail::parse_error(
            "weight refinement receipt framing is invalid"));
    }
    WeightRefinementReceipt receipt;
    if (!detail::require_token(input, "INPUT") ||
        !detail::read_digest(input, receipt.input_binding_digest) ||
        !detail::require_token(input, "OUTPUT") ||
        !detail::read_digest(input, receipt.output_binding_digest) ||
        !detail::require_token(input, "ITERATIONS_USED") ||
        !detail::read_integer(input, receipt.iterations_used) ||
        !detail::require_token(input, "STEP_SIZE") ||
        !detail::read_double(input, receipt.step_size) ||
        !detail::require_token(input, "INITIAL_RMS") ||
        !detail::read_double(input, receipt.initial_rms_residual) ||
        !detail::require_token(input, "FINAL_RMS") ||
        !detail::read_double(input, receipt.final_rms_residual) ||
        !detail::require_token(input, "IMPROVED") ||
        !detail::read_integer(input, improved_token) ||
        !detail::require_token(input, "ADJUSTED_VERTICES") ||
        !detail::read_integer(input, receipt.adjusted_vertex_count) ||
        !detail::require_token(input, "END")) {
        return core::Result<WeightRefinementReceipt>::failure(detail::parse_error(
            "weight refinement receipt body is invalid"));
    }
    if (improved_token > 1U) {
        return core::Result<WeightRefinementReceipt>::failure(detail::parse_error(
            "weight refinement receipt improved flag is invalid"));
    }
    receipt.improved = improved_token == 1U;
    if (auto result = receipt.validate(); !result) {
        return core::Result<WeightRefinementReceipt>::failure(result.error());
    }
    return core::Result<WeightRefinementReceipt>::success(std::move(receipt));
}

core::Result<assets::Sha256Digest> WeightRefinementReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

core::Result<WeightRefinementOutput> refine_weights_gradient(
    const kinematic::SkinBinding& binding,
    const kinematic::KinematicGraph& graph,
    std::span<const core::Vec3d> rest_positions,
    std::span<const core::Vec3d> posed_positions,
    const std::map<kinematic::JointId, core::Transform>& joint_transforms,
    const WeightRefinementOptions& options) {
    if (auto result = binding.validate(); !result) {
        return core::Result<WeightRefinementOutput>::failure(result.error());
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<WeightRefinementOutput>::failure(result.error());
    }
    if (auto result = options.validate(); !result) {
        return core::Result<WeightRefinementOutput>::failure(result.error());
    }
    if (auto result = validate_positions(binding, rest_positions, posed_positions);
        !result) {
        return core::Result<WeightRefinementOutput>::failure(result.error());
    }
    for (const auto& [joint, transform] : joint_transforms) {
        if (!joint || !transform.finite()) {
            return core::Result<WeightRefinementOutput>::failure(detail::invalid(
                "weight refinement received an invalid joint transform"));
        }
    }
    const auto input_digest = binding.canonical_digest();
    if (!input_digest) {
        return core::Result<WeightRefinementOutput>::failure(input_digest.error());
    }

    kinematic::SkinBinding current = binding;
    double previous_rms =
        rms_residual(binding, graph, rest_positions, posed_positions, joint_transforms);

    WeightRefinementReceipt receipt;
    receipt.input_binding_digest = input_digest.value();
    receipt.step_size = options.step_size;
    receipt.initial_rms_residual = previous_rms;
    receipt.final_rms_residual = previous_rms;

    for (std::uint32_t iteration = 0U; iteration < options.max_iterations; ++iteration) {
        std::uint64_t adjusted = 0U;
        for (auto& [vertex, influences] : current.influences) {
            static_cast<void>(vertex);
            const core::Vec3d point = rest_positions[vertex];
            std::vector<core::Vec3d> deformed;
            deformed.reserve(influences.size());
            for (const kinematic::JointInfluence& influence : influences) {
                const auto posed = joint_transforms.find(influence.joint);
                const core::Transform transform =
                    posed == joint_transforms.end()
                        ? core::Transform::identity()
                        : posed->second;
                deformed.push_back(detail::deform_about_anchor(
                    point, detail::joint_anchor(graph, influence.joint), transform));
            }
            core::Vec3d blended{};
            for (std::size_t index = 0U; index < influences.size(); ++index) {
                blended = blended + deformed[index] * influences[index].weight;
            }
            const core::Vec3d error = blended - posed_positions[vertex];
            // Simplex-tangent gradient: sum of the joint directions weighted by
            // the weights is exactly the blended point, so this update leaves
            // the weight sum invariant.
            bool changed = false;
            for (std::size_t index = 0U; index < influences.size(); ++index) {
                const double gradient =
                    core::dot(error, deformed[index] - blended);
                const double next =
                    influences[index].weight - options.step_size * gradient;
                if (std::abs(next - influences[index].weight) > 0.0) changed = true;
                influences[index].weight = std::max(0.0, next);
            }
            if (!changed) continue;
            // Prune, keep at least the largest influence, renormalize exactly.
            auto largest = std::max_element(influences.begin(), influences.end(),
                [](const auto& a, const auto& b) {
                    return a.weight < b.weight;
                });
            const kinematic::JointId largest_joint = largest->joint;
            for (kinematic::JointInfluence& influence : influences) {
                if (influence.weight < options.prune_epsilon &&
                    influence.joint != largest_joint) {
                    influence.weight = 0.0;
                }
            }
            influences.erase(
                std::remove_if(influences.begin(), influences.end(),
                    [](const kinematic::JointInfluence& influence) {
                        return influence.weight <= kPruneEpsilon;
                    }),
                influences.end());
            if (influences.empty()) {
                influences.push_back(kinematic::JointInfluence{largest_joint, 1.0});
            }
            double sum = 0.0;
            for (const kinematic::JointInfluence& influence : influences) sum += influence.weight;
            auto& residual_target = *std::max_element(influences.begin(), influences.end(),
                [](const auto& a, const auto& b) { return a.weight < b.weight; });
            for (kinematic::JointInfluence& influence : influences) {
                influence.weight /= sum;
            }
            double rescaled = 0.0;
            for (const kinematic::JointInfluence& influence : influences) rescaled += influence.weight;
            residual_target.weight += 1.0 - rescaled;
            std::sort(influences.begin(), influences.end(), [](const auto& a, const auto& b) {
                return a.joint < b.joint;
            });
            const auto admitted = character::validate_influences(influences);
            if (!admitted) {
                return core::Result<WeightRefinementOutput>::failure(
                    admitted.error().with_context(
                        "vertex=" + std::to_string(vertex)));
            }
            ++adjusted;
        }
        receipt.iterations_used = iteration + 1U;
        receipt.adjusted_vertex_count = adjusted;
        const double current_rms =
            rms_residual(current, graph, rest_positions, posed_positions, joint_transforms);
        receipt.final_rms_residual = current_rms;
        if (std::abs(previous_rms - current_rms) < 1e-15) {
            previous_rms = current_rms;
            break;
        }
        previous_rms = current_rms;
        if (adjusted == 0U) break;
    }

    if (auto result = current.validate(); !result) {
        return core::Result<WeightRefinementOutput>::failure(result.error());
    }
    const auto output_digest = current.canonical_digest();
    if (!output_digest) {
        return core::Result<WeightRefinementOutput>::failure(output_digest.error());
    }
    receipt.output_binding_digest = output_digest.value();
    receipt.improved = receipt.final_rms_residual < receipt.initial_rms_residual;

    WeightRefinementOutput output;
    output.binding = std::move(current);
    output.receipt = std::move(receipt);
    return core::Result<WeightRefinementOutput>::success(std::move(output));
}

} // namespace carto::weights
