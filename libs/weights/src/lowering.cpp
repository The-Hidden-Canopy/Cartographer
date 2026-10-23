#include <carto/weights/lowering.hpp>

#include "weights_internal.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <utility>
#include <vector>

namespace carto::weights {
namespace {

constexpr std::string_view kReceiptMagic = "CARTOGRAPHER_WEIGHTS_LOWER_RECEIPT";
constexpr double kTieEpsilon = 1e-30;
constexpr double kPruneEpsilon = 1e-12;

void normalize_subset(std::vector<kinematic::JointInfluence>& subset) {
    double sum = 0.0;
    for (const kinematic::JointInfluence& influence : subset) sum += influence.weight;
    if (sum <= kPruneEpsilon) return;
    for (kinematic::JointInfluence& influence : subset) influence.weight /= sum;
    double rescaled = 0.0;
    for (const kinematic::JointInfluence& influence : subset) rescaled += influence.weight;
    auto& largest = *std::max_element(subset.begin(), subset.end(), [](const auto& a, const auto& b) {
        if (a.weight != b.weight) return a.weight < b.weight;
        return b.joint < a.joint;
    });
    largest.weight += 1.0 - rescaled;
    std::sort(subset.begin(), subset.end(), [](const auto& a, const auto& b) {
        return a.joint < b.joint;
    });
}

} // namespace

core::Result<void> LoweringPose::validate() const {
    if (joint_transforms.size() > kMaxJoints) {
        return core::Result<void>::failure(detail::invalid(
            "lowering pose exceeds its joint bound"));
    }
    for (const auto& [joint, transform] : joint_transforms) {
        if (!joint || !transform.finite()) {
            return core::Result<void>::failure(detail::invalid(
                "lowering pose carries an invalid joint transform"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> LowerEightToFourReceipt::validate() const {
    if (input_binding_digest.is_zero() || output_binding_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid(
            "lowering receipt requires input and output binding digests"));
    }
    if (max_influences == 0U || max_influences > 4U) {
        return core::Result<void>::failure(detail::validation(
            "lowering receipt max influences must be inside 1..4"));
    }
    if (!std::isfinite(error_bound) || error_bound < 0.0 ||
        !std::isfinite(evaluated_max_error) || evaluated_max_error < 0.0 ||
        !std::isfinite(evaluated_rms_error) || evaluated_rms_error < 0.0) {
        return core::Result<void>::failure(detail::validation(
            "lowering receipt error values must be finite and non-negative"));
    }
    if (evaluation_pose_count == 0U) {
        return core::Result<void>::failure(detail::validation(
            "lowering receipt requires at least one evaluation pose"));
    }
    if (per_vertex.size() > kMaxRecordedVertices) {
        return core::Result<void>::failure(detail::invalid(
            "lowering receipt per-vertex errors exceed the bound"));
    }
    for (std::size_t index = 0U; index < per_vertex.size(); ++index) {
        const VertexLoweringError& entry = per_vertex[index];
        if (!std::isfinite(entry.max_displacement) || entry.max_displacement < 0.0 ||
            !std::isfinite(entry.rms_displacement) || entry.rms_displacement < 0.0) {
            return core::Result<void>::failure(detail::validation(
                "lowering receipt per-vertex errors must be finite and non-negative"));
        }
        if (index > 0U && !(per_vertex[index - 1U].vertex < entry.vertex)) {
            return core::Result<void>::failure(detail::validation(
                "lowering receipt per-vertex errors must be sorted by vertex"));
        }
    }
    return core::Result<void>::success();
}

std::string LowerEightToFourReceipt::serialize() const {
    std::ostringstream output;
    output << kReceiptMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "INPUT " << input_binding_digest.hex() << '\n';
    output << "OUTPUT " << output_binding_digest.hex() << '\n';
    output << "MAX_INFLUENCES " << max_influences << '\n';
    output << "BOUND " << error_bound << '\n';
    output << "MAX_ERROR " << evaluated_max_error << '\n';
    output << "RMS_ERROR " << evaluated_rms_error << '\n';
    output << "POSES " << evaluation_pose_count << '\n';
    output << "VERTICES " << per_vertex.size() << '\n';
    for (const VertexLoweringError& entry : per_vertex) {
        output << "VERTEX_ERROR " << entry.vertex << ' ' << entry.max_displacement << ' '
               << entry.rms_displacement << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<LowerEightToFourReceipt> LowerEightToFourReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<LowerEightToFourReceipt>::failure(detail::parse_error(
            "lowering receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kReceiptMagic ||
        !detail::read_integer(input, version) || version != kSchemaVersion) {
        return core::Result<LowerEightToFourReceipt>::failure(detail::parse_error(
            "lowering receipt framing is invalid"));
    }
    LowerEightToFourReceipt receipt;
    std::size_t vertex_count = 0U;
    if (!detail::require_token(input, "INPUT") ||
        !detail::read_digest(input, receipt.input_binding_digest) ||
        !detail::require_token(input, "OUTPUT") ||
        !detail::read_digest(input, receipt.output_binding_digest) ||
        !detail::require_token(input, "MAX_INFLUENCES") ||
        !detail::read_integer(input, receipt.max_influences) ||
        !detail::require_token(input, "BOUND") ||
        !detail::read_double(input, receipt.error_bound) ||
        !detail::require_token(input, "MAX_ERROR") ||
        !detail::read_double(input, receipt.evaluated_max_error) ||
        !detail::require_token(input, "RMS_ERROR") ||
        !detail::read_double(input, receipt.evaluated_rms_error) ||
        !detail::require_token(input, "POSES") ||
        !detail::read_integer(input, receipt.evaluation_pose_count) ||
        !detail::require_token(input, "VERTICES") ||
        !detail::read_count(input, kMaxRecordedVertices, vertex_count)) {
        return core::Result<LowerEightToFourReceipt>::failure(detail::parse_error(
            "lowering receipt body is invalid"));
    }
    receipt.per_vertex.reserve(vertex_count);
    for (std::size_t index = 0U; index < vertex_count; ++index) {
        VertexLoweringError entry;
        if (!detail::require_token(input, "VERTEX_ERROR") ||
            !detail::read_integer(input, entry.vertex) ||
            !detail::read_double(input, entry.max_displacement) ||
            !detail::read_double(input, entry.rms_displacement)) {
            return core::Result<LowerEightToFourReceipt>::failure(detail::parse_error(
                "lowering receipt vertex error is invalid"));
        }
        receipt.per_vertex.push_back(entry);
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<LowerEightToFourReceipt>::failure(detail::parse_error(
            "lowering receipt terminator is missing"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<LowerEightToFourReceipt>::failure(result.error());
    }
    return core::Result<LowerEightToFourReceipt>::success(std::move(receipt));
}

core::Result<assets::Sha256Digest> LowerEightToFourReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

core::Result<LowerEightToFourOutput> lower_eight_to_four(
    const kinematic::SkinBinding& binding,
    const kinematic::KinematicGraph& graph,
    std::span<const core::Vec3d> positions,
    double error_bound,
    std::span<const LoweringPose> evaluation_poses) {
    if (auto result = binding.validate(); !result) {
        return core::Result<LowerEightToFourOutput>::failure(result.error());
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<LowerEightToFourOutput>::failure(result.error());
    }
    if (!std::isfinite(error_bound) || error_bound < 0.0) {
        return core::Result<LowerEightToFourOutput>::failure(detail::invalid(
            "lowering requires a finite non-negative error bound"));
    }
    if (evaluation_poses.empty()) {
        return core::Result<LowerEightToFourOutput>::failure(detail::invalid(
            "lowering requires at least one representative evaluation pose; "
            "influence loss cannot be measured otherwise"));
    }
    for (const LoweringPose& pose : evaluation_poses) {
        if (auto result = pose.validate(); !result) {
            return core::Result<LowerEightToFourOutput>::failure(result.error());
        }
    }
    for (const auto& [vertex, influences] : binding.influences) {
        static_cast<void>(influences);
        if (vertex >= positions.size()) {
            return core::Result<LowerEightToFourOutput>::failure(detail::invalid(
                "lowering positions do not cover every influenced vertex"));
        }
    }

    kinematic::SkinBinding output = binding;
    LowerEightToFourReceipt receipt;
    receipt.max_influences = 4U;
    receipt.error_bound = error_bound;
    receipt.evaluation_pose_count = evaluation_poses.size();

    double worst = 0.0;
    double sum_square_accum = 0.0;
    std::size_t vertex_error_count = 0U;

    for (const auto& [vertex, influences] : binding.influences) {
        const core::Vec3d point = positions[vertex];
        if (influences.size() <= 4U) {
            // Nothing to lower; the vertex is already inside the profile.
            continue;
        }

        std::vector<core::Vec3d> full_deformations;
        full_deformations.reserve(evaluation_poses.size());
        for (const LoweringPose& pose : evaluation_poses) {
            full_deformations.push_back(
                detail::linear_blend(influences, point, graph, pose.joint_transforms));
        }

        // Exhaustive best-subset search over the bounded influence count
        // (at most 8 choose 4 = 70 candidates). Subsets are enumerated in
        // joint-sorted lexicographic order; strict improvement replaces the
        // incumbent so ties resolve deterministically to the smallest ids.
        double best_error = std::numeric_limits<double>::infinity();
        std::vector<kinematic::JointInfluence> best_subset;
        for (std::size_t a = 0U; a + 3U < influences.size(); ++a) {
            for (std::size_t b = a + 1U; b + 2U < influences.size(); ++b) {
                for (std::size_t c = b + 1U; c + 1U < influences.size(); ++c) {
                    for (std::size_t d = c + 1U; d < influences.size(); ++d) {
                        std::vector<kinematic::JointInfluence> subset = {
                            influences[a], influences[b], influences[c], influences[d]};
                        normalize_subset(subset);
                        double error = 0.0;
                        for (std::size_t pose = 0U; pose < evaluation_poses.size(); ++pose) {
                            const core::Vec3d lowered_point = detail::linear_blend(
                                subset, point, graph,
                                evaluation_poses[pose].joint_transforms);
                            const core::Vec3d delta = lowered_point - full_deformations[pose];
                            error += delta.length_squared();
                        }
                        if (error < best_error - kTieEpsilon) {
                            best_error = error;
                            best_subset = std::move(subset);
                        }
                    }
                }
            }
        }
        if (best_subset.empty()) {
            return core::Result<LowerEightToFourOutput>::failure(detail::validation(
                "lowering could not select a four-influence subset"));
        }

        double vertex_max = 0.0;
        double vertex_square_sum = 0.0;
        for (std::size_t pose = 0U; pose < evaluation_poses.size(); ++pose) {
            const core::Vec3d lowered_point = detail::linear_blend(
                best_subset, point, graph, evaluation_poses[pose].joint_transforms);
            const double displacement =
                (lowered_point - full_deformations[pose]).length();
            vertex_max = std::max(vertex_max, displacement);
            vertex_square_sum += displacement * displacement;
        }
        worst = std::max(worst, vertex_max);
        sum_square_accum += vertex_square_sum /
            static_cast<double>(evaluation_poses.size());
        ++vertex_error_count;

        VertexLoweringError record;
        record.vertex = vertex;
        record.max_displacement = vertex_max;
        record.rms_displacement = std::sqrt(
            vertex_square_sum / static_cast<double>(evaluation_poses.size()));
        receipt.per_vertex.push_back(record);

        output.influences[vertex] = std::move(best_subset);
    }

    if (worst > error_bound) {
        return core::Result<LowerEightToFourOutput>::failure(detail::validation(
            "INFLUENCE_LOSS_EXCEEDS_BUDGET: evaluated maximum vertex "
            "displacement exceeds the declared quality envelope; publication "
            "is refused")
            .with_context("evaluated_max_error=" + std::to_string(worst))
            .with_context("error_bound=" + std::to_string(error_bound)));
    }

    if (auto result = output.validate(); !result) {
        return core::Result<LowerEightToFourOutput>::failure(result.error());
    }
    const auto input_digest = binding.canonical_digest();
    const auto output_digest = output.canonical_digest();
    if (!input_digest) return core::Result<LowerEightToFourOutput>::failure(input_digest.error());
    if (!output_digest) return core::Result<LowerEightToFourOutput>::failure(output_digest.error());

    receipt.input_binding_digest = input_digest.value();
    receipt.output_binding_digest = output_digest.value();
    receipt.evaluated_max_error = worst;
    receipt.evaluated_rms_error = vertex_error_count == 0U
        ? 0.0
        : std::sqrt(sum_square_accum / static_cast<double>(vertex_error_count));

    LowerEightToFourOutput lowered;
    lowered.binding = std::move(output);
    lowered.receipt = std::move(receipt);
    return core::Result<LowerEightToFourOutput>::success(std::move(lowered));
}

core::Result<void> validate_upcast_reconciliation(
    const kinematic::SkinBinding& lowered,
    const kinematic::SkinBinding& full,
    double weight_tolerance) {
    if (auto result = lowered.validate(); !result) {
        return core::Result<void>::failure(result.error());
    }
    if (auto result = full.validate(); !result) {
        return core::Result<void>::failure(result.error());
    }
    if (!std::isfinite(weight_tolerance) || weight_tolerance <= 0.0 ||
        weight_tolerance > kinematic::SkinBinding::kWeightSumTolerance * 100.0) {
        return core::Result<void>::failure(detail::invalid(
            "upcast reconciliation tolerance must be positive and small"));
    }
    if (!(lowered.mesh_digest == full.mesh_digest) ||
        !(lowered.graph_digest == full.graph_digest)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "upcast reconciliation requires identical mesh and graph revisions"));
    }
    for (const auto& [vertex, lowered_influences] : lowered.influences) {
        const auto found = full.influences.find(vertex);
        if (found == full.influences.end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "upcast reconciliation found a vertex absent from the full binding")
                .with_context("vertex=" + std::to_string(vertex)));
        }
        for (const kinematic::JointInfluence& influence : lowered_influences) {
            const auto match = std::find_if(
                found->second.begin(), found->second.end(),
                [&](const kinematic::JointInfluence& candidate) {
                    return candidate.joint == influence.joint;
                });
            if (match == found->second.end() ||
                std::abs(match->weight - influence.weight) > weight_tolerance) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "upcast reconciliation found a lowered influence that does "
                    "not survive in the full binding")
                    .with_context("vertex=" + std::to_string(vertex)));
            }
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::weights
