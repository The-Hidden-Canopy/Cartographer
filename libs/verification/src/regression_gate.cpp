#include <carto/verification/regression_gate.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

namespace carto::verification {
namespace {

constexpr std::string_view kSpecMagic = "CARTOGRAPHER_REGRESSION_GATE_SPEC";
constexpr std::string_view kOutcomeMagic = "CARTOGRAPHER_REGRESSION_GATE_OUTCOME";

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool valid_evidence(production::PreparedCapabilityEvidence evidence) {
    return evidence == production::PreparedCapabilityEvidence::preserved_source ||
        evidence == production::PreparedCapabilityEvidence::derived_product ||
        evidence == production::PreparedCapabilityEvidence::native_realization ||
        evidence == production::PreparedCapabilityEvidence::runtime_validated ||
        evidence == production::PreparedCapabilityEvidence::unsupported;
}

bool valid_source_kind(production::PreparedSourceEntityKind kind) {
    return static_cast<unsigned>(kind) <=
        static_cast<unsigned>(production::PreparedSourceEntityKind::instance_field);
}

bool valid_prepared_kind(production::PreparedEntityKind kind) {
    return static_cast<unsigned>(kind) <=
        static_cast<unsigned>(production::PreparedEntityKind::instance_field);
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool read_count(std::istream& input, std::size_t value_maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > value_maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool read_bool(std::istream& input, bool& value) {
    unsigned encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

core::Result<assets::Sha256Digest> parse_digest(std::string_view encoded) {
    const auto digest = assets::Sha256Digest::from_hex(encoded);
    if (!digest || digest.value().is_zero()) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("regression gate digest is invalid"));
    }
    return digest;
}

bool expectation_ordered(
    const CapabilityExpectation& left,
    const CapabilityExpectation& right) {
    return std::tuple{
        left.feature, static_cast<unsigned>(left.evidence), left.required} <
        std::tuple{right.feature, static_cast<unsigned>(right.evidence), right.required};
}

bool correspondence_ordered(
    const CorrespondenceCountExpectation& left,
    const CorrespondenceCountExpectation& right) {
    return std::tuple{
        static_cast<unsigned>(left.source_kind),
        static_cast<unsigned>(left.prepared_kind), left.expected_count} <
        std::tuple{
            static_cast<unsigned>(right.source_kind),
            static_cast<unsigned>(right.prepared_kind), right.expected_count};
}

} // namespace

core::Result<void> CapabilityExpectation::validate() const {
    if (!safe_text(feature, RegressionGateSpec::kMaxFeatureBytes)) {
        return core::Result<void>::failure(validation(
            "capability expectation feature is empty, oversized, or not canonical text"));
    }
    if (!valid_evidence(evidence)) {
        return core::Result<void>::failure(validation(
            "capability expectation evidence is not a known evidence class"));
    }
    return core::Result<void>::success();
}

bool CapabilityExpectation::matches(
    const production::PreparedCapabilityResult& result) const noexcept {
    return result.feature == feature && result.evidence == evidence &&
        result.required == required;
}

core::Result<void> CorrespondenceCountExpectation::validate() const {
    if (!valid_source_kind(source_kind)) {
        return core::Result<void>::failure(validation(
            "correspondence expectation source kind is unknown"));
    }
    if (!valid_prepared_kind(prepared_kind)) {
        return core::Result<void>::failure(validation(
            "correspondence expectation prepared kind is unknown"));
    }
    return core::Result<void>::success();
}

core::Result<void> RegressionGateSpec::validate() const {
    if (expected_prepared_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "regression gate requires a non-zero expected prepared digest"));
    }
    if (expected_capabilities.size() > kMaxExpectations ||
        expected_correspondence_counts.size() > kMaxExpectations) {
        return core::Result<void>::failure(validation(
            "regression gate expectations exceed their bounded capacity"));
    }
    for (const auto& expectation : expected_capabilities) {
        if (auto result = expectation.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < expected_capabilities.size(); ++index) {
        const auto& previous = expected_capabilities[index - 1U];
        const auto& current = expected_capabilities[index];
        if (previous.feature == current.feature) {
            return core::Result<void>::failure(validation(
                "regression gate contains a duplicated capability feature"));
        }
        if (!expectation_ordered(previous, current)) {
            return core::Result<void>::failure(validation(
                "regression gate capabilities are not in canonical order"));
        }
    }
    for (const auto& correspondence : expected_correspondence_counts) {
        if (auto result = correspondence.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < expected_correspondence_counts.size(); ++index) {
        const auto& previous = expected_correspondence_counts[index - 1U];
        const auto& current = expected_correspondence_counts[index];
        if (previous.source_kind == current.source_kind &&
            previous.prepared_kind == current.prepared_kind) {
            return core::Result<void>::failure(validation(
                "regression gate contains a duplicated correspondence key"));
        }
        if (!correspondence_ordered(previous, current)) {
            return core::Result<void>::failure(validation(
                "regression gate correspondence counts are not in canonical order"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> RegressionGateSpec::normalize() {
    std::sort(expected_capabilities.begin(), expected_capabilities.end(),
        [](const auto& left, const auto& right) {
            return left.feature < right.feature;
        });
    std::sort(expected_correspondence_counts.begin(), expected_correspondence_counts.end(),
        [](const auto& left, const auto& right) {
            return std::tuple{
                static_cast<unsigned>(left.source_kind),
                static_cast<unsigned>(left.prepared_kind)} <
                std::tuple{
                    static_cast<unsigned>(right.source_kind),
                    static_cast<unsigned>(right.prepared_kind)};
        });
    return validate();
}

std::string RegressionGateSpec::serialize() const {
    std::ostringstream output;
    output << kSpecMagic << ' ' << kSchemaVersion << '\n';
    output << "DIGEST " << expected_prepared_digest.hex() << '\n';
    output << "CAPABILITIES " << expected_capabilities.size() << '\n';
    for (const auto& expectation : expected_capabilities) {
        output << "CAPABILITY " << std::quoted(expectation.feature) << ' '
               << static_cast<unsigned>(expectation.evidence) << ' '
               << (expectation.required ? 1U : 0U) << '\n';
    }
    output << "CORRESPONDENCES " << expected_correspondence_counts.size() << '\n';
    for (const auto& correspondence : expected_correspondence_counts) {
        output << "CORRESPONDENCE "
               << static_cast<unsigned>(correspondence.source_kind) << ' '
               << static_cast<unsigned>(correspondence.prepared_kind) << ' '
               << correspondence.expected_count << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<RegressionGateSpec> RegressionGateSpec::deserialize(
    std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<RegressionGateSpec>::failure(validation(
            "regression gate spec exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kSpecMagic) || !read_unsigned(input, version)) {
        return core::Result<RegressionGateSpec>::failure(validation(
            "regression gate spec header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<RegressionGateSpec>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "regression gate spec schema version is unsupported"));
    }
    RegressionGateSpec spec;
    std::string encoded_digest;
    if (!require_token(input, "DIGEST") || !(input >> encoded_digest)) {
        return core::Result<RegressionGateSpec>::failure(validation(
            "regression gate spec digest is missing"));
    }
    const auto digest = parse_digest(encoded_digest);
    if (!digest) {
        return core::Result<RegressionGateSpec>::failure(
            digest.error().with_context("regression gate spec digest"));
    }
    spec.expected_prepared_digest = digest.value();

    std::size_t count = 0U;
    if (!require_token(input, "CAPABILITIES") ||
        !read_count(input, kMaxExpectations, count)) {
        return core::Result<RegressionGateSpec>::failure(validation(
            "regression gate capability count is invalid"));
    }
    spec.expected_capabilities.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        CapabilityExpectation expectation;
        unsigned evidence = 0U;
        if (!require_token(input, "CAPABILITY") ||
            !(input >> std::quoted(expectation.feature)) ||
            !read_unsigned(input, evidence) || evidence > 255U ||
            !read_bool(input, expectation.required)) {
            return core::Result<RegressionGateSpec>::failure(validation(
                "regression gate capability expectation is invalid"));
        }
        expectation.evidence =
            static_cast<production::PreparedCapabilityEvidence>(evidence);
        spec.expected_capabilities.push_back(std::move(expectation));
    }
    if (!require_token(input, "CORRESPONDENCES") ||
        !read_count(input, kMaxExpectations, count)) {
        return core::Result<RegressionGateSpec>::failure(validation(
            "regression gate correspondence count is invalid"));
    }
    spec.expected_correspondence_counts.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        CorrespondenceCountExpectation correspondence;
        unsigned source_kind = 0U;
        unsigned prepared_kind = 0U;
        if (!require_token(input, "CORRESPONDENCE") ||
            !read_unsigned(input, source_kind) ||
            !read_unsigned(input, prepared_kind) ||
            !read_unsigned(input, correspondence.expected_count)) {
            return core::Result<RegressionGateSpec>::failure(validation(
                "regression gate correspondence expectation is invalid"));
        }
        correspondence.source_kind =
            static_cast<production::PreparedSourceEntityKind>(source_kind);
        correspondence.prepared_kind =
            static_cast<production::PreparedEntityKind>(prepared_kind);
        spec.expected_correspondence_counts.push_back(std::move(correspondence));
    }
    if (!require_token(input, "END")) {
        return core::Result<RegressionGateSpec>::failure(validation(
            "regression gate spec is missing its end marker"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<RegressionGateSpec>::failure(validation(
            "regression gate spec contains trailing data"));
    }
    if (auto result = spec.validate(); !result) {
        return core::Result<RegressionGateSpec>::failure(result.error());
    }
    return core::Result<RegressionGateSpec>::success(std::move(spec));
}

core::Result<void> RegressionGateOutcome::validate() const {
    if (failures.size() > kMaxFailures) {
        return core::Result<void>::failure(validation(
            "regression gate outcome exceeds its bounded failure capacity"));
    }
    for (const auto& failure : failures) {
        if (!safe_text(failure, kMaxFailureBytes)) {
            return core::Result<void>::failure(validation(
                "regression gate failure reason is empty, oversized, or not canonical text"));
        }
    }
    if (passed && !failures.empty()) {
        return core::Result<void>::failure(validation(
            "a passing regression gate cannot carry failure reasons"));
    }
    if (!passed && failures.empty()) {
        return core::Result<void>::failure(validation(
            "a failing regression gate requires at least one failure reason"));
    }
    return core::Result<void>::success();
}

std::string RegressionGateOutcome::canonical() const {
    std::ostringstream output;
    output << kOutcomeMagic << '\n';
    output << "PASSED " << (passed ? 1U : 0U) << '\n';
    output << "DIGEST " << observed_prepared_digest.hex() << '\n';
    output << "FAILURES " << failures.size() << '\n';
    for (const auto& failure : failures) {
        output << "FAILURE " << std::quoted(failure) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<RegressionGateOutcome> RegressionGateOutcome::deserialize(
    std::string_view text) {
    std::istringstream input{std::string(text)};
    if (!require_token(input, kOutcomeMagic)) {
        return core::Result<RegressionGateOutcome>::failure(validation(
            "regression gate outcome header is invalid"));
    }
    RegressionGateOutcome outcome;
    if (!require_token(input, "PASSED") || !read_bool(input, outcome.passed)) {
        return core::Result<RegressionGateOutcome>::failure(validation(
            "regression gate outcome verdict is invalid"));
    }
    std::string encoded_digest;
    if (!require_token(input, "DIGEST") || !(input >> encoded_digest)) {
        return core::Result<RegressionGateOutcome>::failure(validation(
            "regression gate outcome digest is missing"));
    }
    const auto digest = parse_digest(encoded_digest);
    if (!digest) {
        return core::Result<RegressionGateOutcome>::failure(
            digest.error().with_context("regression gate outcome digest"));
    }
    outcome.observed_prepared_digest = digest.value();
    std::size_t count = 0U;
    if (!require_token(input, "FAILURES") ||
        !read_count(input, kMaxFailures, count)) {
        return core::Result<RegressionGateOutcome>::failure(validation(
            "regression gate outcome failure count is invalid"));
    }
    outcome.failures.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        std::string failure;
        if (!require_token(input, "FAILURE") || !(input >> std::quoted(failure))) {
            return core::Result<RegressionGateOutcome>::failure(validation(
                "regression gate outcome failure reason is invalid"));
        }
        outcome.failures.push_back(std::move(failure));
    }
    if (!require_token(input, "END")) {
        return core::Result<RegressionGateOutcome>::failure(validation(
            "regression gate outcome is missing its end marker"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<RegressionGateOutcome>::failure(validation(
            "regression gate outcome contains trailing data"));
    }
    if (auto result = outcome.validate(); !result) {
        return core::Result<RegressionGateOutcome>::failure(result.error());
    }
    return core::Result<RegressionGateOutcome>::success(std::move(outcome));
}

core::Result<RegressionGateOutcome> run_regression_gate(
    const RegressionGateSpec& spec,
    const production::PreparedSceneEnvelope& prepared,
    const production::PreparedPublicationReceipt* publication) {
    if (auto result = spec.validate(); !result) {
        return core::Result<RegressionGateOutcome>::failure(
            result.error().with_context("regression gate spec"));
    }
    if (auto result = prepared.validate(); !result) {
        return core::Result<RegressionGateOutcome>::failure(
            result.error().with_context("regression gate prepared scene"));
    }
    const auto observed_digest = prepared.canonical_digest();
    if (!observed_digest) {
        return core::Result<RegressionGateOutcome>::failure(observed_digest.error());
    }

    RegressionGateOutcome outcome;
    outcome.observed_prepared_digest = observed_digest.value();
    const auto fail = [&outcome](std::string reason) {
        if (outcome.failures.size() < RegressionGateOutcome::kMaxFailures) {
            outcome.failures.push_back(std::move(reason));
        }
    };

    if (observed_digest.value() != spec.expected_prepared_digest) {
        fail("prepared digest " + observed_digest.value().hex() +
            " does not match the expected regression digest " +
            spec.expected_prepared_digest.hex());
    }

    std::vector<CapabilityExpectation> observed_capabilities;
    observed_capabilities.reserve(prepared.capabilities.size());
    for (const auto& capability : prepared.capabilities) {
        observed_capabilities.push_back(CapabilityExpectation{
            capability.feature, capability.evidence, capability.required});
    }
    std::sort(observed_capabilities.begin(), observed_capabilities.end(),
        [](const auto& left, const auto& right) {
            return left.feature < right.feature;
        });
    if (observed_capabilities.size() != spec.expected_capabilities.size()) {
        fail("capability count drifted: expected " +
            std::to_string(spec.expected_capabilities.size()) + " observed " +
            std::to_string(observed_capabilities.size()));
    } else {
        for (std::size_t index = 0U; index < observed_capabilities.size(); ++index) {
            const auto& expected = spec.expected_capabilities[index];
            const auto& observed = observed_capabilities[index];
            if (expected.feature != observed.feature) {
                fail("capability set drifted: expected feature " + expected.feature +
                    " observed " + observed.feature);
                break;
            }
            if (expected.evidence != observed.evidence ||
                expected.required != observed.required) {
                fail("capability " + expected.feature +
                    " drifted from its expected evidence or requirement class");
            }
        }
    }

    std::map<std::pair<unsigned, unsigned>, std::uint64_t> observed_counts;
    for (const auto& correspondence : prepared.correspondences) {
        const auto key = std::pair{
            static_cast<unsigned>(correspondence.source_kind),
            static_cast<unsigned>(correspondence.prepared_kind)};
        if (observed_counts[key] ==
            std::numeric_limits<std::uint64_t>::max()) {
            return core::Result<RegressionGateOutcome>::failure(validation(
                "prepared correspondence count exceeds its numeric bound"));
        }
        ++observed_counts[key];
    }
    std::uint64_t expected_total = 0U;
    for (const auto& correspondence : spec.expected_correspondence_counts) {
        expected_total += correspondence.expected_count;
        const auto key = std::pair{
            static_cast<unsigned>(correspondence.source_kind),
            static_cast<unsigned>(correspondence.prepared_kind)};
        const auto observed = observed_counts.find(key);
        if (observed == observed_counts.end() ||
            observed->second != correspondence.expected_count) {
            fail("correspondence count drifted for source kind " +
                std::to_string(key.first) + " prepared kind " +
                std::to_string(key.second));
        }
    }
    if (expected_total != prepared.correspondences.size()) {
        fail("total correspondence count drifted: expected " +
            std::to_string(expected_total) + " observed " +
            std::to_string(prepared.correspondences.size()));
    }

    if (publication != nullptr) {
        if (publication->prepared_scene_digest != observed_digest.value()) {
            fail("publication receipt digest does not match the observed prepared digest");
        }
        if (publication->source_revision != prepared.source_revision) {
            fail("publication receipt source revision does not match the prepared scene");
        }
    }

    outcome.passed = outcome.failures.empty();
    return core::Result<RegressionGateOutcome>::success(std::move(outcome));
}

} // namespace carto::verification
