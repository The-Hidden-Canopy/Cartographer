#include <carto/face/expression.hpp>

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

constexpr std::string_view kExpressionMagic = "CARTO_FACE_EXPRESSION";

using detail::invalid;
using detail::parse_error;
using detail::safe_token;
using detail::validation;

core::Result<void> validate_morph_term(const MorphBlendTerm& term) {
    if (!term.target) {
        return core::Result<void>::failure(invalid(
            "morph blend term must name a morph target identity"));
    }
    if (!std::isfinite(term.weight) || term.weight <= 0.0 ||
        term.weight > kMaxMorphTermWeight) {
        return core::Result<void>::failure(invalid(
            "morph blend term weight must be finite and within (0, kMaxMorphTermWeight]"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_control_term(const ControlOffsetTerm& term) {
    if (!term.control) {
        return core::Result<void>::failure(invalid(
            "control offset term must name a control identity"));
    }
    if (term.axes_mask == 0U ||
        (term.axes_mask & ~character::kAllowedAxesMask) != 0U) {
        return core::Result<void>::failure(invalid(
            "control offset term axes mask must be a nonempty subset of the allowed axes"));
    }
    for (std::size_t axis = 0U; axis < character::kAxisCount; ++axis) {
        if ((term.axes_mask & (1U << axis)) == 0U) continue;
        if (!std::isfinite(term.offsets[axis]) ||
            std::abs(term.offsets[axis]) > kMaxControlOffsetMagnitude) {
            return core::Result<void>::failure(invalid(
                "control offset term offsets must be finite and bounded"));
        }
    }
    return core::Result<void>::success();
}

template <typename Term>
core::Result<void> insert_sorted_unique(std::vector<Term>& terms, Term term) {
    const auto position = std::lower_bound(terms.begin(), terms.end(), term);
    if (position != terms.end() && *position == term) {
        return core::Result<void>::failure(invalid(
            "expression term already exists for this identity"));
    }
    terms.insert(position, std::move(term));
    return core::Result<void>::success();
}

core::Result<std::uint64_t> read_count(std::istringstream& input, const char* label) {
    const auto token = detail::read_token(input, label);
    if (!token) return core::Result<std::uint64_t>::failure(token.error());
    std::uint64_t count = 0U;
    const auto parsed = std::from_chars(
        token.value().data(), token.value().data() + token.value().size(), count);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != token.value().data() + token.value().size()) {
        return core::Result<std::uint64_t>::failure(parse_error(
            std::string(label) + " count is invalid"));
    }
    return core::Result<std::uint64_t>::success(count);
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

std::string_view expression_sum_policy_name(ExpressionSumPolicy policy) noexcept {
    switch (policy) {
    case ExpressionSumPolicy::normalized: return "normalized";
    case ExpressionSumPolicy::additive: return "additive";
    }
    return "normalized";
}

core::Result<ExpressionSumPolicy> expression_sum_policy_from_name(
    std::string_view text) {
    if (text == "normalized") {
        return core::Result<ExpressionSumPolicy>::success(
            ExpressionSumPolicy::normalized);
    }
    if (text == "additive") {
        return core::Result<ExpressionSumPolicy>::success(
            ExpressionSumPolicy::additive);
    }
    return core::Result<ExpressionSumPolicy>::failure(
        invalid("unknown expression sum policy name"));
}

core::Result<void> Expression::insert_morph_term(MorphBlendTerm term) {
    if (morph_terms.size() >= kMaxMorphTerms) {
        return core::Result<void>::failure(invalid(
            "expression exceeds its morph term bound"));
    }
    if (auto result = validate_morph_term(term); !result) return result;
    return insert_sorted_unique(morph_terms, term);
}

core::Result<void> Expression::insert_control_term(ControlOffsetTerm term) {
    if (control_terms.size() >= kMaxControlTerms) {
        return core::Result<void>::failure(invalid(
            "expression exceeds its control term bound"));
    }
    if (auto result = validate_control_term(term); !result) return result;
    return insert_sorted_unique(control_terms, term);
}

core::Result<void> Expression::validate() const {
    if (schema_version != kSchemaVersion) {
        return core::Result<void>::failure(detail::version_error(
            "expression schema version is not supported"));
    }
    if (!safe_token(name, kMaxNameBytes)) {
        return core::Result<void>::failure(invalid(
            "expression name is not a bounded safe token"));
    }
    if (morph_terms.size() > kMaxMorphTerms) {
        return core::Result<void>::failure(validation(
            "expression exceeds its morph term bound"));
    }
    if (control_terms.size() > kMaxControlTerms) {
        return core::Result<void>::failure(validation(
            "expression exceeds its control term bound"));
    }
    for (const MorphBlendTerm& term : morph_terms) {
        if (auto result = validate_morph_term(term); !result) return result;
    }
    for (const ControlOffsetTerm& term : control_terms) {
        if (auto result = validate_control_term(term); !result) return result;
    }
    if (!std::is_sorted(morph_terms.begin(), morph_terms.end()) ||
        std::adjacent_find(morph_terms.begin(), morph_terms.end()) != morph_terms.end()) {
        return core::Result<void>::failure(validation(
            "expression morph terms must be sorted and unique by target"));
    }
    if (!std::is_sorted(control_terms.begin(), control_terms.end()) ||
        std::adjacent_find(control_terms.begin(), control_terms.end()) != control_terms.end()) {
        return core::Result<void>::failure(validation(
            "expression control terms must be sorted and unique by control"));
    }
    return core::Result<void>::success();
}

std::string Expression::serialize() const {
    std::ostringstream output;
    output << kExpressionMagic << ' ' << kSchemaVersion << '\n';
    output << "NAME " << detail::json_string(name) << '\n';
    output << "POLICY " << expression_sum_policy_name(sum_policy) << '\n';
    output << "MORPH_TERMS " << morph_terms.size() << '\n';
    for (const MorphBlendTerm& term : morph_terms) {
        output << "MORPH_TERM " << term.target.value << ' ';
        detail::write_double(output, term.weight);
        output << '\n';
    }
    output << "CONTROL_TERMS " << control_terms.size() << '\n';
    for (const ControlOffsetTerm& term : control_terms) {
        output << "CONTROL_TERM " << term.control.value << ' '
               << static_cast<unsigned>(term.axes_mask);
        for (const double offset : term.offsets) {
            output << ' ';
            detail::write_double(output, offset);
        }
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> Expression::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<Expression> Expression::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<Expression>::failure(parse_error(
            "expression text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = detail::expect_magic(input, kExpressionMagic, kSchemaVersion); !result) {
        return core::Result<Expression>::failure(result.error());
    }
    Expression expression;
    const auto name_tag = detail::read_token(input, "NAME");
    if (!name_tag) return core::Result<Expression>::failure(name_tag.error());
    if (name_tag.value() != "NAME") {
        return core::Result<Expression>::failure(parse_error(
            "expression framing is invalid: NAME"));
    }
    const auto name = detail::read_quoted(input, "NAME", kMaxNameBytes);
    if (!name) return core::Result<Expression>::failure(name.error());
    expression.name = name.value();
    const auto policy_tag = detail::read_token(input, "POLICY");
    if (!policy_tag) return core::Result<Expression>::failure(policy_tag.error());
    if (policy_tag.value() != "POLICY") {
        return core::Result<Expression>::failure(parse_error(
            "expression framing is invalid: POLICY"));
    }
    const auto policy_token = detail::read_token(input, "POLICY");
    if (!policy_token) return core::Result<Expression>::failure(policy_token.error());
    const auto policy = expression_sum_policy_from_name(policy_token.value());
    if (!policy) return core::Result<Expression>::failure(policy.error());
    expression.sum_policy = policy.value();
    const auto morph_tag = detail::read_token(input, "MORPH_TERMS");
    if (!morph_tag) return core::Result<Expression>::failure(morph_tag.error());
    if (morph_tag.value() != "MORPH_TERMS") {
        return core::Result<Expression>::failure(parse_error(
            "expression framing is invalid: MORPH_TERMS"));
    }
    const auto morph_count = read_count(input, "MORPH_TERMS");
    if (!morph_count) return core::Result<Expression>::failure(morph_count.error());
    if (morph_count.value() > kMaxMorphTerms) {
        return core::Result<Expression>::failure(parse_error(
            "expression morph term count exceeds its bound"));
    }
    for (std::uint64_t index = 0U; index < morph_count.value(); ++index) {
        const auto tag = detail::read_token(input, "MORPH_TERM");
        if (!tag) return core::Result<Expression>::failure(tag.error());
        if (tag.value() != "MORPH_TERM") {
            return core::Result<Expression>::failure(parse_error(
                "expression framing is invalid: MORPH_TERM"));
        }
        const auto target = read_uint(input, "MORPH_TERM target");
        if (!target) return core::Result<Expression>::failure(target.error());
        const auto weight_token = detail::read_token(input, "MORPH_TERM weight");
        if (!weight_token) {
            return core::Result<Expression>::failure(weight_token.error());
        }
        const auto weight = detail::parse_double_token(weight_token.value(), "MORPH_TERM weight");
        if (!weight) return core::Result<Expression>::failure(weight.error());
        if (auto result = expression.insert_morph_term(MorphBlendTerm{
                character::MorphTargetId{target.value()}, weight.value()});
            !result) {
            return core::Result<Expression>::failure(result.error());
        }
    }
    const auto control_tag = detail::read_token(input, "CONTROL_TERMS");
    if (!control_tag) return core::Result<Expression>::failure(control_tag.error());
    if (control_tag.value() != "CONTROL_TERMS") {
        return core::Result<Expression>::failure(parse_error(
            "expression framing is invalid: CONTROL_TERMS"));
    }
    const auto control_count = read_count(input, "CONTROL_TERMS");
    if (!control_count) return core::Result<Expression>::failure(control_count.error());
    if (control_count.value() > kMaxControlTerms) {
        return core::Result<Expression>::failure(parse_error(
            "expression control term count exceeds its bound"));
    }
    for (std::uint64_t index = 0U; index < control_count.value(); ++index) {
        const auto tag = detail::read_token(input, "CONTROL_TERM");
        if (!tag) return core::Result<Expression>::failure(tag.error());
        if (tag.value() != "CONTROL_TERM") {
            return core::Result<Expression>::failure(parse_error(
                "expression framing is invalid: CONTROL_TERM"));
        }
        const auto control = read_uint(input, "CONTROL_TERM control");
        if (!control) return core::Result<Expression>::failure(control.error());
        const auto mask = read_uint(input, "CONTROL_TERM axes mask");
        if (!mask) return core::Result<Expression>::failure(mask.error());
        if (mask.value() > 0xFFU) {
            return core::Result<Expression>::failure(parse_error(
                "expression control term axes mask exceeds one byte"));
        }
        ControlOffsetTerm term;
        term.control = character::ControlId{control.value()};
        term.axes_mask = static_cast<std::uint8_t>(mask.value());
        for (double& offset : term.offsets) {
            const auto offset_token = detail::read_token(input, "CONTROL_TERM offset");
            if (!offset_token) {
                return core::Result<Expression>::failure(offset_token.error());
            }
            const auto parsed = detail::parse_double_token(
                offset_token.value(), "CONTROL_TERM offset");
            if (!parsed) return core::Result<Expression>::failure(parsed.error());
            offset = parsed.value();
        }
        if (auto result = expression.insert_control_term(std::move(term)); !result) {
            return core::Result<Expression>::failure(result.error());
        }
    }
    if (auto result = detail::expect_end(input); !result) {
        return core::Result<Expression>::failure(result.error());
    }
    if (auto result = expression.validate(); !result) {
        return core::Result<Expression>::failure(result.error());
    }
    return core::Result<Expression>::success(std::move(expression));
}

core::Result<void> validate_expression_controls(
    const Expression& expression,
    const character::ControlGraph& controls) {
    if (auto result = expression.validate(); !result) return result;
    for (const ControlOffsetTerm& term : expression.control_terms) {
        const character::ControlDefinition* definition = controls.find(term.control);
        if (definition == nullptr) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "expression drives a control the graph does not contain"));
        }
        if ((term.axes_mask & ~definition->allowed_axes) != 0U) {
            return core::Result<void>::failure(validation(
                "expression drives axes the control does not allow"));
        }
        for (std::size_t axis = 0U; axis < character::kAxisCount; ++axis) {
            if ((term.axes_mask & (1U << axis)) == 0U) continue;
            if (term.offsets[axis] < definition->axis_minimum[axis] ||
                term.offsets[axis] > definition->axis_maximum[axis]) {
                return core::Result<void>::failure(validation(
                    "expression control offset leaves the authored axis domain"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<void> BlendedExpression::validate() const {
    double sum = 0.0;
    for (const auto& [target, weight] : morph_weights) {
        if (!target) {
            return core::Result<void>::failure(invalid(
                "blended morph weight must name a morph target identity"));
        }
        if (!std::isfinite(weight) || weight <= 0.0 ||
            weight > kMaxObservedMorphWeight) {
            return core::Result<void>::failure(validation(
                "blended morph weight is non-finite or exceeds its bound"));
        }
        sum += weight;
    }
    if (!std::isfinite(sum) || sum > kMaxObservedMorphWeight) {
        return core::Result<void>::failure(validation(
            "blended morph weight total exceeds its bound"));
    }
    if (!std::isfinite(total_morph_weight) || total_morph_weight < 0.0 ||
        total_morph_weight > kMaxObservedMorphWeight) {
        return core::Result<void>::failure(validation(
            "blended total morph weight is out of domain"));
    }
    for (const auto& [control, offsets] : control_offsets) {
        if (!control) {
            return core::Result<void>::failure(invalid(
                "blended control offsets must name a control identity"));
        }
        for (const double offset : offsets) {
            if (!std::isfinite(offset) || std::abs(offset) > kMaxControlOffsetMagnitude) {
                return core::Result<void>::failure(validation(
                    "blended control offsets are non-finite or unbounded"));
            }
        }
    }
    return core::Result<void>::success();
}

std::string BlendedExpression::serialize() const {
    std::ostringstream output;
    output << "CARTO_FACE_BLENDED_EXPRESSION 1\n";
    output << "MORPH_WEIGHTS " << morph_weights.size() << '\n';
    for (const auto& [target, weight] : morph_weights) {
        output << "MORPH_WEIGHT " << target.value << ' ';
        detail::write_double(output, weight);
        output << '\n';
    }
    output << "CONTROL_OFFSETS " << control_offsets.size() << '\n';
    for (const auto& [control, offsets] : control_offsets) {
        output << "CONTROL_OFFSET " << control.value;
        for (const double offset : offsets) {
            output << ' ';
            detail::write_double(output, offset);
        }
        output << '\n';
    }
    output << "TOTAL ";
    detail::write_double(output, total_morph_weight);
    output << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> BlendedExpression::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<BlendedExpression> blend_expressions(
    std::span<const ExpressionBlendTerm> terms) {
    BlendedExpression blended;
    for (const ExpressionBlendTerm& term : terms) {
        if (term.expression == nullptr) {
            return core::Result<BlendedExpression>::failure(invalid(
                "blend term must reference an expression"));
        }
        if (auto result = term.expression->validate(); !result) {
            return core::Result<BlendedExpression>::failure(result.error());
        }
        if (!std::isfinite(term.weight) || term.weight < 0.0 || term.weight > 1.0) {
            return core::Result<BlendedExpression>::failure(invalid(
                "blend weight must be finite and within [0, 1]"));
        }
        const Expression& expression = *term.expression;
        if (expression.sum_policy == ExpressionSumPolicy::normalized) {
            double term_sum = 0.0;
            for (const MorphBlendTerm& morph : expression.morph_terms) {
                term_sum += morph.weight;
            }
            // Every term weight is > 0 and finite, so term_sum > 0.
            const double scale = term.weight / term_sum;
            for (const MorphBlendTerm& morph : expression.morph_terms) {
                blended.morph_weights[morph.target] += morph.weight * scale;
                blended.total_morph_weight += morph.weight * scale;
            }
        } else {
            for (const MorphBlendTerm& morph : expression.morph_terms) {
                const double contribution = morph.weight * term.weight;
                blended.morph_weights[morph.target] += contribution;
                blended.total_morph_weight += contribution;
            }
        }
        for (const ControlOffsetTerm& control : expression.control_terms) {
            std::array<double, character::kAxisCount>& offsets =
                blended.control_offsets[control.control];
            for (std::size_t axis = 0U; axis < character::kAxisCount; ++axis) {
                if ((control.axes_mask & (1U << axis)) == 0U) continue;
                offsets[axis] += control.offsets[axis] * term.weight;
            }
        }
    }
    if (auto result = blended.validate(); !result) {
        return core::Result<BlendedExpression>::failure(result.error());
    }
    return core::Result<BlendedExpression>::success(std::move(blended));
}

core::Result<void> ExpressionConflictReceipt::validate() const {
    if (conflicts.size() > kMaxConflicts) {
        return core::Result<void>::failure(validation(
            "expression conflict receipt exceeds its conflict bound"));
    }
    for (const ExpressionConflict& conflict : conflicts) {
        if (!conflict.target) {
            return core::Result<void>::failure(validation(
                "expression conflict must name a morph target identity"));
        }
        if (!safe_token(conflict.first_expression, Expression::kMaxNameBytes) ||
            !safe_token(conflict.second_expression, Expression::kMaxNameBytes)) {
            return core::Result<void>::failure(validation(
                "expression conflict names are not bounded safe tokens"));
        }
    }
    if (!std::is_sorted(conflicts.begin(), conflicts.end()) ||
        std::adjacent_find(conflicts.begin(), conflicts.end()) != conflicts.end()) {
        return core::Result<void>::failure(validation(
            "expression conflicts must be sorted and unique"));
    }
    return core::Result<void>::success();
}

std::string ExpressionConflictReceipt::serialize() const {
    std::ostringstream output;
    output << "CARTO_FACE_EXPRESSION_CONFLICTS 1\n";
    output << "CONFLICTS " << conflicts.size() << '\n';
    for (const ExpressionConflict& conflict : conflicts) {
        output << "CONFLICT " << conflict.target.value << ' '
               << detail::json_string(conflict.first_expression) << ' '
               << detail::json_string(conflict.second_expression) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> ExpressionConflictReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<ExpressionConflictReceipt> detect_expression_conflicts(
    std::span<const Expression> expressions) {
    for (const Expression& expression : expressions) {
        if (auto result = expression.validate(); !result) {
            return core::Result<ExpressionConflictReceipt>::failure(result.error());
        }
    }
    std::vector<std::string_view> names;
    names.reserve(expressions.size());
    for (const Expression& expression : expressions) {
        names.push_back(expression.name);
    }
    if (!std::is_sorted(names.begin(), names.end()) ||
        std::adjacent_find(names.begin(), names.end()) != names.end()) {
        return core::Result<ExpressionConflictReceipt>::failure(invalid(
            "expressions must be sorted and unique by name"));
    }
    ExpressionConflictReceipt receipt;
    for (std::size_t first = 0U; first < expressions.size(); ++first) {
        for (std::size_t second = first + 1U; second < expressions.size(); ++second) {
            const bool order_visible =
                expressions[first].sum_policy == ExpressionSumPolicy::normalized ||
                expressions[second].sum_policy == ExpressionSumPolicy::normalized;
            if (!order_visible) continue;
            std::set<character::MorphTargetId> shared;
            for (const MorphBlendTerm& term : expressions[first].morph_terms) {
                const auto match = std::lower_bound(
                    expressions[second].morph_terms.begin(),
                    expressions[second].morph_terms.end(),
                    term,
                    [](const MorphBlendTerm& left, const MorphBlendTerm& right) {
                        return left.target < right.target;
                    });
                if (match != expressions[second].morph_terms.end() &&
                    match->target == term.target) {
                    shared.insert(term.target);
                }
            }
            for (const character::MorphTargetId target : shared) {
                if (receipt.conflicts.size() >= ExpressionConflictReceipt::kMaxConflicts) {
                    return core::Result<ExpressionConflictReceipt>::failure(
                        validation(
                            "expression conflict receipt exceeds its conflict bound"));
                }
                receipt.conflicts.push_back(ExpressionConflict{
                    target,
                    std::string(expressions[first].name),
                    std::string(expressions[second].name)});
            }
        }
    }
    std::sort(receipt.conflicts.begin(), receipt.conflicts.end());
    if (auto result = receipt.validate(); !result) {
        return core::Result<ExpressionConflictReceipt>::failure(result.error());
    }
    return core::Result<ExpressionConflictReceipt>::success(std::move(receipt));
}

} // namespace carto::face
