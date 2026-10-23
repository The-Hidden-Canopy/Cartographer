#include <carto/face/receipt.hpp>

#include "text_format.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace carto::face {
namespace {

constexpr std::string_view kReceiptMagic = "CARTO_FACE_EVALUATION_RECEIPT";

using detail::invalid;
using detail::parse_error;
using detail::safe_token;
using detail::validation;
using detail::version_error;

void write_digest(std::ostringstream& output, const assets::Sha256Digest& digest) {
    detail::write_json_string(output, digest.hex());
}

core::Result<assets::Sha256Digest> read_digest(
    std::istringstream& input,
    const char* label) {
    const auto hex = detail::read_quoted(input, label, 64U);
    if (!hex) return core::Result<assets::Sha256Digest>::failure(hex.error());
    const auto parsed = assets::Sha256Digest::from_hex(hex.value());
    if (!parsed) {
        return core::Result<assets::Sha256Digest>::failure(parse_error(
            std::string(label) + " digest is malformed"));
    }
    return parsed;
}

core::Result<std::uint64_t> read_uint(std::istringstream& input, const char* label) {
    const auto token = detail::read_token(input, label);
    if (!token) return core::Result<std::uint64_t>::failure(token.error());
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(
        token.value().data(), token.value().data() + token.value().size(), value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != token.value().data() + token.value().size()) {
        return core::Result<std::uint64_t>::failure(parse_error(
            std::string(label) + " integer is invalid"));
    }
    return core::Result<std::uint64_t>::success(value);
}

} // namespace

core::Result<void> FacialEvaluationReceipt::validate() const {
    if (schema_version != kSchemaVersion) {
        return core::Result<void>::failure(version_error(
            "facial evaluation receipt schema version is not supported"));
    }
    if (!character_id) {
        return core::Result<void>::failure(invalid(
            "facial evaluation receipt must name a character identity"));
    }
    if (!safe_token(evaluation_identity, kMaxIdentityBytes)) {
        return core::Result<void>::failure(invalid(
            "facial evaluation receipt identity is not a bounded safe token"));
    }
    if (expression_digests.size() > kMaxExpressionDigests) {
        return core::Result<void>::failure(validation(
            "facial evaluation receipt exceeds its expression digest bound"));
    }
    for (const assets::Sha256Digest& digest : expression_digests) {
        if (digest.is_zero()) {
            return core::Result<void>::failure(validation(
                "facial evaluation receipt expression digest must be nonzero"));
        }
    }
    if (!std::is_sorted(expression_digests.begin(), expression_digests.end()) ||
        std::adjacent_find(expression_digests.begin(), expression_digests.end()) !=
            expression_digests.end()) {
        return core::Result<void>::failure(validation(
            "facial evaluation receipt expression digests must be sorted and unique"));
    }
    if (!std::isfinite(max_observed_morph_weight) ||
        max_observed_morph_weight < 0.0 ||
        max_observed_morph_weight > kMaxObservedMorphWeight) {
        return core::Result<void>::failure(validation(
            "facial evaluation receipt max observed morph weight is out of bounds"));
    }
    if (auto result = lipsync.validate(); !result) return result;
    return core::Result<void>::success();
}

std::string FacialEvaluationReceipt::serialize() const {
    std::ostringstream output;
    output << kReceiptMagic << ' ' << kSchemaVersion << '\n';
    output << "CHARACTER " << character_id.value << '\n';
    output << "IDENTITY " << detail::json_string(evaluation_identity) << '\n';
    output << "EXPRESSIONS " << expression_digests.size() << '\n';
    for (const assets::Sha256Digest& digest : expression_digests) {
        output << "EXPRESSION_SHA256 ";
        write_digest(output, digest);
        output << '\n';
    }
    output << "MAX_OBSERVED_MORPH_WEIGHT ";
    detail::write_double(output, max_observed_morph_weight);
    output << '\n';
    output << "CLAMPED_TERMS " << clamped_terms << '\n';
    output << "VISEME_EVAL_BEGIN\n";
    output << lipsync.serialize();
    output << "VISEME_EVAL_END\n";
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> FacialEvaluationReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<FacialEvaluationReceipt> FacialEvaluationReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<FacialEvaluationReceipt>::failure(parse_error(
            "facial evaluation receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = detail::expect_magic(input, kReceiptMagic, kSchemaVersion); !result) {
        return core::Result<FacialEvaluationReceipt>::failure(result.error());
    }
    FacialEvaluationReceipt receipt;
    const auto character_tag = detail::read_token(input, "CHARACTER");
    if (!character_tag) {
        return core::Result<FacialEvaluationReceipt>::failure(character_tag.error());
    }
    if (character_tag.value() != "CHARACTER") {
        return core::Result<FacialEvaluationReceipt>::failure(parse_error(
            "facial evaluation receipt framing is invalid: CHARACTER"));
    }
    const auto character = read_uint(input, "CHARACTER");
    if (!character) {
        return core::Result<FacialEvaluationReceipt>::failure(character.error());
    }
    receipt.character_id = character::CharacterId{character.value()};
    const auto identity_tag = detail::read_token(input, "IDENTITY");
    if (!identity_tag) {
        return core::Result<FacialEvaluationReceipt>::failure(identity_tag.error());
    }
    if (identity_tag.value() != "IDENTITY") {
        return core::Result<FacialEvaluationReceipt>::failure(parse_error(
            "facial evaluation receipt framing is invalid: IDENTITY"));
    }
    const auto identity = detail::read_quoted(input, "IDENTITY", kMaxIdentityBytes);
    if (!identity) {
        return core::Result<FacialEvaluationReceipt>::failure(identity.error());
    }
    receipt.evaluation_identity = identity.value();
    const auto expressions_tag = detail::read_token(input, "EXPRESSIONS");
    if (!expressions_tag) {
        return core::Result<FacialEvaluationReceipt>::failure(expressions_tag.error());
    }
    if (expressions_tag.value() != "EXPRESSIONS") {
        return core::Result<FacialEvaluationReceipt>::failure(parse_error(
            "facial evaluation receipt framing is invalid: EXPRESSIONS"));
    }
    const auto expression_count = read_uint(input, "EXPRESSIONS");
    if (!expression_count) {
        return core::Result<FacialEvaluationReceipt>::failure(expression_count.error());
    }
    if (expression_count.value() > kMaxExpressionDigests) {
        return core::Result<FacialEvaluationReceipt>::failure(parse_error(
            "facial evaluation receipt expression count exceeds its bound"));
    }
    for (std::uint64_t index = 0U; index < expression_count.value(); ++index) {
        const auto tag = detail::read_token(input, "EXPRESSION_SHA256");
        if (!tag) {
            return core::Result<FacialEvaluationReceipt>::failure(tag.error());
        }
        if (tag.value() != "EXPRESSION_SHA256") {
            return core::Result<FacialEvaluationReceipt>::failure(parse_error(
                "facial evaluation receipt framing is invalid: EXPRESSION_SHA256"));
        }
        const auto digest = read_digest(input, "EXPRESSION_SHA256");
        if (!digest) {
            return core::Result<FacialEvaluationReceipt>::failure(digest.error());
        }
        receipt.expression_digests.push_back(digest.value());
    }
    const auto weight_tag = detail::read_token(input, "MAX_OBSERVED_MORPH_WEIGHT");
    if (!weight_tag) {
        return core::Result<FacialEvaluationReceipt>::failure(weight_tag.error());
    }
    if (weight_tag.value() != "MAX_OBSERVED_MORPH_WEIGHT") {
        return core::Result<FacialEvaluationReceipt>::failure(parse_error(
            "facial evaluation receipt framing is invalid: MAX_OBSERVED_MORPH_WEIGHT"));
    }
    const auto weight_token = detail::read_token(input, "MAX_OBSERVED_MORPH_WEIGHT");
    if (!weight_token) {
        return core::Result<FacialEvaluationReceipt>::failure(weight_token.error());
    }
    const auto weight = detail::parse_double_token(
        weight_token.value(), "MAX_OBSERVED_MORPH_WEIGHT");
    if (!weight) return core::Result<FacialEvaluationReceipt>::failure(weight.error());
    receipt.max_observed_morph_weight = weight.value();
    const auto clamped_tag = detail::read_token(input, "CLAMPED_TERMS");
    if (!clamped_tag) {
        return core::Result<FacialEvaluationReceipt>::failure(clamped_tag.error());
    }
    if (clamped_tag.value() != "CLAMPED_TERMS") {
        return core::Result<FacialEvaluationReceipt>::failure(parse_error(
            "facial evaluation receipt framing is invalid: CLAMPED_TERMS"));
    }
    const auto clamped = read_uint(input, "CLAMPED_TERMS");
    if (!clamped) return core::Result<FacialEvaluationReceipt>::failure(clamped.error());
    receipt.clamped_terms = clamped.value();
    const auto begin_tag = detail::read_token(input, "VISEME_EVAL_BEGIN");
    if (!begin_tag) {
        return core::Result<FacialEvaluationReceipt>::failure(begin_tag.error());
    }
    if (begin_tag.value() != "VISEME_EVAL_BEGIN") {
        return core::Result<FacialEvaluationReceipt>::failure(parse_error(
            "facial evaluation receipt framing is invalid: VISEME_EVAL_BEGIN"));
    }
    std::string payload;
    std::string line;
    bool closed = false;
    while (std::getline(input, line)) {
        if (line == "VISEME_EVAL_END") {
            closed = true;
            break;
        }
        payload += line;
        payload += '\n';
    }
    if (!closed) {
        return core::Result<FacialEvaluationReceipt>::failure(parse_error(
            "facial evaluation receipt viseme evaluation is unterminated"));
    }
    const auto evaluation = LipsyncEvaluation::deserialize(payload);
    if (!evaluation) {
        return core::Result<FacialEvaluationReceipt>::failure(evaluation.error());
    }
    receipt.lipsync = evaluation.value();
    if (auto result = detail::expect_end(input); !result) {
        return core::Result<FacialEvaluationReceipt>::failure(result.error());
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<FacialEvaluationReceipt>::failure(result.error());
    }
    return core::Result<FacialEvaluationReceipt>::success(std::move(receipt));
}

core::Result<FacialEvaluationReceipt> build_facial_evaluation_receipt(
    character::CharacterId character_id,
    std::string evaluation_identity,
    std::span<const assets::Sha256Digest> expression_digests,
    const BlendedExpression& blend,
    const LipsyncEvaluation& lipsync) {
    if (!character_id) {
        return core::Result<FacialEvaluationReceipt>::failure(invalid(
            "facial evaluation receipt must name a character identity"));
    }
    if (!safe_token(evaluation_identity, FacialEvaluationReceipt::kMaxIdentityBytes)) {
        return core::Result<FacialEvaluationReceipt>::failure(invalid(
            "facial evaluation receipt identity is not a bounded safe token"));
    }
    if (expression_digests.size() > FacialEvaluationReceipt::kMaxExpressionDigests) {
        return core::Result<FacialEvaluationReceipt>::failure(invalid(
            "facial evaluation receipt exceeds its expression digest bound"));
    }
    if (auto result = blend.validate(); !result) {
        return core::Result<FacialEvaluationReceipt>::failure(result.error());
    }
    if (auto result = lipsync.validate(); !result) {
        return core::Result<FacialEvaluationReceipt>::failure(result.error());
    }
    FacialEvaluationReceipt receipt;
    receipt.character_id = character_id;
    receipt.evaluation_identity = std::move(evaluation_identity);
    std::set<assets::Sha256Digest> sorted(
        expression_digests.begin(), expression_digests.end());
    receipt.expression_digests.assign(sorted.begin(), sorted.end());
    receipt.lipsync = lipsync;
    double maximum = 0.0;
    for (const auto& [target, weight] : blend.morph_weights) {
        (void)target;
        maximum = std::max(maximum, weight);
    }
    receipt.max_observed_morph_weight = maximum;
    receipt.clamped_terms = 0U;
    if (auto result = receipt.validate(); !result) {
        return core::Result<FacialEvaluationReceipt>::failure(result.error());
    }
    return core::Result<FacialEvaluationReceipt>::success(std::move(receipt));
}

} // namespace carto::face
