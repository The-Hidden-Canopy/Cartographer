#include <carto/deform/inverse_hypothesis.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::deform {
namespace {

constexpr std::size_t kMaxHypotheses = 256U;
constexpr std::size_t kMaxSearchPointEvaluations = 20'000'000U;
constexpr std::size_t kMaxReviewerDecisionBytes = 1024U;
constexpr std::size_t kMaxSourceVertices =
    static_cast<std::size_t>(kinematic::SkinBinding::kMaxVertexIndex) + 1U;
constexpr std::string_view kHypothesisDomain = "carto.inverse-skin-hypothesis.v1";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_identity(std::string_view value) {
    return !value.empty() && value.size() <= 128U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
                byte == '-';
        });
}

bool safe_decision(std::string_view value) {
    return !value.empty() && value.size() <= kMaxReviewerDecisionBytes &&
        std::any_of(value.begin(), value.end(), [](unsigned char byte) { return byte != ' '; }) &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\r' && byte != '\n';
        });
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_double(std::vector<std::uint8_t>& bytes, double value) {
    if (value == 0.0) value = 0.0;
    append_u64(bytes, std::bit_cast<std::uint64_t>(value));
}

void append_transform(std::vector<std::uint8_t>& bytes, const core::Transform& value) {
    append_double(bytes, value.translation.x);
    append_double(bytes, value.translation.y);
    append_double(bytes, value.translation.z);
    append_double(bytes, value.rotation.x);
    append_double(bytes, value.rotation.y);
    append_double(bytes, value.rotation.z);
    append_double(bytes, value.rotation.w);
    append_double(bytes, value.scale.x);
    append_double(bytes, value.scale.y);
    append_double(bytes, value.scale.z);
}

void append_string(std::vector<std::uint8_t>& bytes, std::string_view value) {
    append_u64(bytes, static_cast<std::uint64_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

core::Result<SkinEvaluationResult> evaluate_hypothesis(
    const InverseSkinHypothesis& hypothesis,
    std::span<const core::Vec3d> bind_positions,
    assets::Sha256Digest source_digest) {
    SkinEvaluationInput input;
    input.bind_positions = bind_positions;
    input.mesh_content_digest = source_digest;
    input.mesh_digest_attested = true;
    input.binding = &hypothesis.binding;
    input.poses = &hypothesis.poses;
    if (hypothesis.method == SkinMethod::linear_blend) return evaluate_lbs(input);
    if (hypothesis.method == SkinMethod::dual_quaternion) return evaluate_dqs(input);
    return core::Result<SkinEvaluationResult>::failure(validation(
        "inverse skin hypothesis method is unsupported"));
}

assets::Sha256Digest digest_candidate_set(
    std::uint64_t mesh_asset,
    const assets::Sha256Digest& source_digest,
    const assets::Sha256Digest& bind_positions_digest,
    const assets::Sha256Digest& observed_digest,
    const std::vector<InverseSkinHypothesisScore>& candidates,
    const std::vector<assets::Sha256Digest>& frontier) {
    std::vector<std::uint8_t> bytes;
    append_string(bytes, kHypothesisDomain);
    append_u64(bytes, mesh_asset);
    bytes.insert(bytes.end(), source_digest.bytes.begin(), source_digest.bytes.end());
    bytes.insert(bytes.end(), bind_positions_digest.bytes.begin(), bind_positions_digest.bytes.end());
    bytes.insert(bytes.end(), observed_digest.bytes.begin(), observed_digest.bytes.end());
    for (const auto& candidate : candidates) {
        append_string(bytes, candidate.identity);
        bytes.insert(bytes.end(), candidate.hypothesis_digest.bytes.begin(),
            candidate.hypothesis_digest.bytes.end());
        bytes.push_back(candidate.evaluated ? 1U : 0U);
        append_string(bytes, candidate.evaluation_diagnostic);
        append_double(bytes, candidate.observed_deviation.mean_deviation);
        append_double(bytes, candidate.observed_deviation.max_deviation);
        bytes.insert(bytes.end(), candidate.evaluated_positions_digest.bytes.begin(),
            candidate.evaluated_positions_digest.bytes.end());
    }
    for (const auto& digest : frontier) {
        bytes.insert(bytes.end(), digest.bytes.begin(), digest.bytes.end());
    }
    return assets::sha256(bytes);
}

} // namespace

core::Result<void> InverseSkinHypothesisSearch::validate() const {
    if (source_mesh_asset == 0U || source_mesh_digest.is_zero() ||
        bind_positions_digest.is_zero() ||
        observed_positions_digest.is_zero() || ranked_candidates.empty() ||
        ranked_candidates.size() > kMaxHypotheses ||
        candidate_set_digest.is_zero() ||
        ambiguous_frontier != (pareto_frontier.size() > 1U)) {
        return core::Result<void>::failure(validation(
            "inverse skin hypothesis search record is malformed"));
    }
    std::set<std::string> identities;
    std::set<assets::Sha256Digest> candidate_digests;
    std::size_t evaluated_count = 0U;
    for (const auto& candidate : ranked_candidates) {
        if (!safe_identity(candidate.identity) || candidate.hypothesis_digest.is_zero() ||
            !identities.insert(candidate.identity).second ||
            !candidate_digests.insert(candidate.hypothesis_digest).second) {
            return core::Result<void>::failure(validation(
                "inverse skin hypothesis search contains invalid or duplicate candidates"));
        }
        if (candidate.evaluated) {
            ++evaluated_count;
            if (!candidate.evaluation_diagnostic.empty() ||
                candidate.evaluated_positions_digest.is_zero()) {
                return core::Result<void>::failure(validation(
                    "evaluated inverse skin candidate has a failure diagnostic or no output digest"));
            }
            if (auto result = candidate.observed_deviation.validate(); !result) return result;
        } else if (candidate.evaluation_diagnostic.empty() ||
            candidate.evaluation_diagnostic.size() > kMaxReviewerDecisionBytes ||
            !candidate.evaluated_positions_digest.is_zero() ||
            candidate.observed_deviation.compared_points != 0U) {
            return core::Result<void>::failure(validation(
                "rejected inverse skin candidate does not preserve a bounded failure reason"));
        }
    }
    if (pareto_frontier.size() > evaluated_count ||
        (evaluated_count > 0U && pareto_frontier.empty()) ||
        (evaluated_count == 0U && !pareto_frontier.empty()) ||
        !std::is_sorted(pareto_frontier.begin(), pareto_frontier.end()) ||
        std::adjacent_find(pareto_frontier.begin(), pareto_frontier.end()) !=
            pareto_frontier.end() ||
        std::any_of(pareto_frontier.begin(), pareto_frontier.end(),
            [&](const auto& digest) { return !candidate_digests.contains(digest); })) {
        return core::Result<void>::failure(validation(
            "inverse skin hypothesis Pareto frontier is invalid"));
    }
    if (candidate_set_digest != digest_candidate_set(source_mesh_asset,
            source_mesh_digest, bind_positions_digest, observed_positions_digest,
            ranked_candidates, pareto_frontier)) {
        return core::Result<void>::failure(validation(
            "inverse skin hypothesis search digest does not match its contents"));
    }
    return core::Result<void>::success();
}

core::Result<void> LockedInverseSkinHypothesis::validate() const {
    if (source_mesh_asset == 0U || source_mesh_digest.is_zero() ||
        bind_positions_digest.is_zero() || candidate_set_digest.is_zero() ||
        hypothesis_digest.is_zero() ||
        observed_positions_digest.is_zero() || !safe_decision(reviewer_decision)) {
        return core::Result<void>::failure(validation(
            "locked inverse skin hypothesis record is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> inverse_skin_hypothesis_digest(
    const InverseSkinHypothesis& hypothesis) {
    if (!safe_identity(hypothesis.identity) ||
        (hypothesis.method != SkinMethod::linear_blend &&
         hypothesis.method != SkinMethod::dual_quaternion)) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "inverse skin hypothesis identity or method is invalid"));
    }
    if (auto result = hypothesis.binding.validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    if (hypothesis.poses.world_rest.empty() ||
        hypothesis.poses.world_rest.size() != hypothesis.poses.world_pose.size()) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "inverse skin hypothesis pose sets are incomplete"));
    }
    auto binding_digest = hypothesis.binding.canonical_digest();
    if (!binding_digest) {
        return core::Result<assets::Sha256Digest>::failure(binding_digest.error());
    }
    std::vector<std::uint8_t> bytes;
    append_string(bytes, kHypothesisDomain);
    append_string(bytes, hypothesis.identity);
    bytes.push_back(static_cast<std::uint8_t>(hypothesis.method));
    bytes.insert(bytes.end(), binding_digest.value().bytes.begin(),
        binding_digest.value().bytes.end());
    append_u64(bytes, static_cast<std::uint64_t>(hypothesis.poses.world_rest.size()));
    for (const auto& [joint, rest] : hypothesis.poses.world_rest) {
        const auto posed = hypothesis.poses.world_pose.find(joint);
        if (joint.value == 0U || posed == hypothesis.poses.world_pose.end() ||
            !rest.finite() || !posed->second.finite()) {
            return core::Result<assets::Sha256Digest>::failure(validation(
                "inverse skin hypothesis has an invalid or unmatched joint pose"));
        }
        append_u64(bytes, joint.value);
        append_transform(bytes, rest);
        append_transform(bytes, posed->second);
    }
    for (const auto& [joint, unused_pose] : hypothesis.poses.world_pose) {
        static_cast<void>(unused_pose);
        if (!hypothesis.poses.world_rest.contains(joint)) {
            return core::Result<assets::Sha256Digest>::failure(validation(
                "inverse skin hypothesis has a posed joint without a rest transform"));
        }
    }
    return core::Result<assets::Sha256Digest>::success(assets::sha256(bytes));
}

core::Result<InverseSkinHypothesisSearch> search_inverse_skin_hypotheses(
    std::uint64_t source_mesh_asset,
    assets::Sha256Digest source_mesh_digest,
    std::span<const core::Vec3d> bind_positions,
    std::span<const core::Vec3d> observed_positions,
    std::span<const InverseSkinHypothesis> hypotheses,
    double error_tolerance) {
    if (source_mesh_asset == 0U || source_mesh_digest.is_zero() || bind_positions.empty() ||
        bind_positions.size() > kMaxSourceVertices ||
        bind_positions.size() != observed_positions.size() || hypotheses.empty() ||
        hypotheses.size() > kMaxHypotheses || !std::isfinite(error_tolerance) ||
        error_tolerance < 0.0 || error_tolerance > 1'000'000.0 ||
        hypotheses.size() > kMaxSearchPointEvaluations / bind_positions.size()) {
        return core::Result<InverseSkinHypothesisSearch>::failure(invalid(
            "inverse skin search source, observations, candidate count, or tolerance is invalid"));
    }
    for (const auto& position : observed_positions) {
        if (!position.finite()) {
            return core::Result<InverseSkinHypothesisSearch>::failure(invalid(
                "inverse skin observations must be finite"));
        }
    }
    for (const auto& position : bind_positions) {
        if (!position.finite()) {
            return core::Result<InverseSkinHypothesisSearch>::failure(invalid(
                "inverse skin bind positions must be finite"));
        }
    }
    InverseSkinHypothesisSearch search;
    search.source_mesh_asset = source_mesh_asset;
    search.source_mesh_digest = source_mesh_digest;
    search.bind_positions_digest = positions_digest(bind_positions);
    search.observed_positions_digest = positions_digest(observed_positions);
    std::set<std::string> identities;
    std::set<assets::Sha256Digest> candidate_digests;
    for (const auto& hypothesis : hypotheses) {
        if (hypothesis.binding.mesh_asset != source_mesh_asset ||
            hypothesis.binding.mesh_digest != source_mesh_digest) {
            return core::Result<InverseSkinHypothesisSearch>::failure(stale(
                "inverse skin candidate is bound to a different source mesh"));
        }
        if (!identities.insert(hypothesis.identity).second) {
            return core::Result<InverseSkinHypothesisSearch>::failure(validation(
                "inverse skin candidate identities must be unique"));
        }
        auto candidate_digest = inverse_skin_hypothesis_digest(hypothesis);
        if (!candidate_digest) {
            return core::Result<InverseSkinHypothesisSearch>::failure(candidate_digest.error());
        }
        if (!candidate_digests.insert(candidate_digest.value()).second) {
            return core::Result<InverseSkinHypothesisSearch>::failure(validation(
                "inverse skin candidate digests must be unique"));
        }
        auto evaluated = evaluate_hypothesis(hypothesis, bind_positions, source_mesh_digest);
        if (!evaluated) {
            const std::string diagnostic = evaluated.error().message.size() <=
                    kMaxReviewerDecisionBytes
                ? evaluated.error().message : "candidate evaluation failed with an oversized diagnostic";
            search.ranked_candidates.push_back({hypothesis.identity, candidate_digest.value(),
                {}, {}, false, diagnostic});
            continue;
        }
        auto deviation = compute_deviation(evaluated.value().positions, observed_positions);
        if (!deviation) {
            const std::string diagnostic = deviation.error().message.size() <=
                    kMaxReviewerDecisionBytes
                ? deviation.error().message : "candidate comparison failed with an oversized diagnostic";
            search.ranked_candidates.push_back({hypothesis.identity, candidate_digest.value(),
                {}, {}, false, diagnostic});
            continue;
        }
        search.ranked_candidates.push_back({hypothesis.identity, candidate_digest.value(),
            deviation.value(), positions_digest(evaluated.value().positions), true, {}});
    }
    std::sort(search.ranked_candidates.begin(), search.ranked_candidates.end(),
        [](const auto& left, const auto& right) {
            if (left.evaluated != right.evaluated) return left.evaluated;
            if (!left.evaluated) return left.identity < right.identity;
            if (left.observed_deviation.mean_deviation != right.observed_deviation.mean_deviation) {
                return left.observed_deviation.mean_deviation < right.observed_deviation.mean_deviation;
            }
            if (left.observed_deviation.max_deviation != right.observed_deviation.max_deviation) {
                return left.observed_deviation.max_deviation < right.observed_deviation.max_deviation;
            }
            return left.identity < right.identity;
        });
    for (std::size_t candidate = 0U; candidate < search.ranked_candidates.size(); ++candidate) {
        if (!search.ranked_candidates[candidate].evaluated) continue;
        const auto& score = search.ranked_candidates[candidate].observed_deviation;
        const bool dominated = std::any_of(search.ranked_candidates.begin(),
            search.ranked_candidates.end(), [&](const auto& other_candidate) {
                if (!other_candidate.evaluated) return false;
                const auto& other = other_candidate.observed_deviation;
                return other.mean_deviation <= score.mean_deviation + error_tolerance &&
                    other.max_deviation <= score.max_deviation + error_tolerance &&
                    (other.mean_deviation < score.mean_deviation - error_tolerance ||
                     other.max_deviation < score.max_deviation - error_tolerance);
            });
        if (!dominated) {
            search.pareto_frontier.push_back(
                search.ranked_candidates[candidate].hypothesis_digest);
        }
    }
    std::sort(search.pareto_frontier.begin(), search.pareto_frontier.end());
    search.ambiguous_frontier = search.pareto_frontier.size() > 1U;
    search.candidate_set_digest = digest_candidate_set(source_mesh_asset,
        source_mesh_digest, search.bind_positions_digest,
        search.observed_positions_digest,
        search.ranked_candidates, search.pareto_frontier);
    if (auto result = search.validate(); !result) {
        return core::Result<InverseSkinHypothesisSearch>::failure(result.error());
    }
    return core::Result<InverseSkinHypothesisSearch>::success(std::move(search));
}

core::Result<LockedInverseSkinHypothesis> lock_inverse_skin_hypothesis(
    const InverseSkinHypothesisSearch& search,
    const assets::Sha256Digest& hypothesis_digest,
    std::string reviewer_decision) {
    if (auto result = search.validate(); !result) {
        return core::Result<LockedInverseSkinHypothesis>::failure(result.error());
    }
    const auto found = std::find_if(search.ranked_candidates.begin(),
        search.ranked_candidates.end(), [&](const auto& candidate) {
            return candidate.evaluated && candidate.hypothesis_digest == hypothesis_digest;
        });
    if (search.source_mesh_asset == 0U || search.source_mesh_digest.is_zero() ||
        search.candidate_set_digest.is_zero() || found == search.ranked_candidates.end() ||
        !safe_decision(reviewer_decision)) {
        return core::Result<LockedInverseSkinHypothesis>::failure(invalid(
            "locking an inverse skin candidate requires a reviewed search result and rationale"));
    }
    return core::Result<LockedInverseSkinHypothesis>::success({search.source_mesh_asset,
        search.source_mesh_digest, search.bind_positions_digest,
        search.candidate_set_digest, hypothesis_digest, search.observed_positions_digest,
        std::move(reviewer_decision)});
}

core::Result<InverseSkinRerun> rerun_locked_inverse_skin_hypothesis(
    const LockedInverseSkinHypothesis& locked,
    const InverseSkinHypothesis& hypothesis,
    std::span<const core::Vec3d> bind_positions,
    std::span<const core::Vec3d> observed_positions,
    assets::Sha256Digest current_source_mesh_digest) {
    if (auto result = locked.validate(); !result) {
        return core::Result<InverseSkinRerun>::failure(result.error());
    }
    if (current_source_mesh_digest != locked.source_mesh_digest ||
        hypothesis.binding.mesh_asset != locked.source_mesh_asset ||
        hypothesis.binding.mesh_digest != locked.source_mesh_digest) {
        return core::Result<InverseSkinRerun>::failure(stale(
            "locked inverse skin rerun source mesh changed"));
    }
    auto candidate_digest = inverse_skin_hypothesis_digest(hypothesis);
    if (!candidate_digest || candidate_digest.value() != locked.hypothesis_digest) {
        return core::Result<InverseSkinRerun>::failure(stale(
            "locked inverse skin candidate changed before rerun"));
    }
    const auto actual_source_digest = positions_digest(bind_positions);
    if (actual_source_digest != locked.bind_positions_digest ||
        observed_positions.size() != bind_positions.size() ||
        positions_digest(observed_positions) != locked.observed_positions_digest) {
        return core::Result<InverseSkinRerun>::failure(stale(
            "locked inverse skin rerun source positions or observations changed"));
    }
    auto evaluated = evaluate_hypothesis(hypothesis, bind_positions, current_source_mesh_digest);
    if (!evaluated) {
        return core::Result<InverseSkinRerun>::failure(evaluated.error());
    }
    auto deviation = compute_deviation(evaluated.value().positions, observed_positions);
    if (!deviation) return core::Result<InverseSkinRerun>::failure(deviation.error());
    const auto result_digest = positions_digest(evaluated.value().positions);
    return core::Result<InverseSkinRerun>::success({locked.source_mesh_digest,
        current_source_mesh_digest, actual_source_digest, locked.hypothesis_digest, result_digest,
        deviation.value(), std::move(evaluated.value().positions)});
}

} // namespace carto::deform
