#pragma once

#include <carto/character/ids.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/face/expression.hpp>
#include <carto/face/lipsync.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::face {

// Derivation receipt for one facial evaluation: which expression records
// participated, the viseme weights at every sampled rational time, the
// unmapped phoneme entries, and the bounds checks observed while blending.
// Wall time is deliberately absent: identity and determinism come from
// digests, revisions and exact rational times.
struct FacialEvaluationReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxExpressionDigests = 256U;
    static constexpr std::size_t kMaxUnmappedPhonemes = 512U;
    static constexpr std::size_t kMaxSamples = 4'096U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;

    std::uint32_t schema_version = kSchemaVersion;
    character::CharacterId character_id;
    // Sorted, unique canonical digests of every expression record blended into
    // this evaluation.
    std::vector<assets::Sha256Digest> expression_digests;
    // Viseme weights at the evaluation sample times, with unmapped entries.
    LipsyncEvaluation lipsync;
    // Bounds checks: the largest blended morph weight observed and how many
    // blended terms were clamped into the authored domain during evaluation.
    // Cartographer evaluators never clamp silently, so a receipt built by
    // build_facial_evaluation_receipt records zero; a downstream evaluator
    // that clamps records its count here instead of hiding it. A receipt whose
    // observed weight is non-finite or exceeds kMaxObservedMorphWeight fails
    // validation.
    double max_observed_morph_weight = 0.0;
    std::uint64_t clamped_terms = 0U;
    // Backend identity of the evaluator, e.g. "cpu_reference/face_eval/v1";
    // a different evaluator is a different receipt.
    std::string evaluation_identity;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<FacialEvaluationReceipt> deserialize(
        std::string_view text);
};

// Builds a canonical facial evaluation receipt from blended expressions and a
// lipsync evaluation. The expression digests are sorted and deduplicated, the
// bounds summary is measured from the blend, and every admitted input is
// revalidated so a hostile caller cannot smuggle non-finite weights into the
// receipt. This builder never clamps, so clamped_terms is recorded as zero.
[[nodiscard]] core::Result<FacialEvaluationReceipt> build_facial_evaluation_receipt(
    character::CharacterId character_id,
    std::string evaluation_identity,
    std::span<const assets::Sha256Digest> expression_digests,
    const BlendedExpression& blend,
    const LipsyncEvaluation& lipsync);

} // namespace carto::face
