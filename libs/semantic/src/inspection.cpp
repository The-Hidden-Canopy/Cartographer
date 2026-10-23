#include <carto/semantic/inspection.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

namespace carto::semantic {
namespace {

constexpr std::string_view kReportMagic = "CARTOGRAPHER_SEMANTIC_INSPECTION";

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

assets::Sha256Digest digest_of(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

bool valid_tier(VerificationTier tier) {
    switch (tier) {
    case VerificationTier::structural:
    case VerificationTier::reference:
    case VerificationTier::native:
    case VerificationTier::full_game:
        return true;
    }
    return false;
}

bool read_uint(std::istream& input, std::uint64_t& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

bool read_count(std::istream& input, std::uint64_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_uint(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool sorted_unique(const std::vector<std::string>& values) {
    return std::is_sorted(values.begin(), values.end()) &&
        std::adjacent_find(values.begin(), values.end()) == values.end();
}

} // namespace

std::string_view verification_tier_name(VerificationTier tier) noexcept {
    switch (tier) {
    case VerificationTier::structural: return "structural";
    case VerificationTier::reference: return "reference";
    case VerificationTier::native: return "native";
    case VerificationTier::full_game: return "full-game";
    }
    return "unknown";
}

core::Result<void> SemanticInspectionReport::validate() const {
    if (!valid_tier(tier)) {
        return core::Result<void>::failure(validation(
            "inspection report verification tier is invalid"));
    }
    if (binding_resolutions.size() > kMaxResolutions ||
        dropped_tags.size() > kMaxTags || lowered_tags.size() > kMaxTags ||
        schema_mismatches.size() > kMaxMismatches) {
        return core::Result<void>::failure(validation(
            "inspection report exceeds its bounded capacity"));
    }
    for (const auto& resolution : binding_resolutions) {
        if (auto result = resolution.binding.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "inspection report binding resolution"));
        }
        if (!safe_text(resolution.detail, 4096U, true)) {
            return core::Result<void>::failure(validation(
                "inspection report resolution detail is invalid"));
        }
    }
    if (!sorted_unique(dropped_tags) || !sorted_unique(lowered_tags)) {
        return core::Result<void>::failure(validation(
            "inspection report tags must be sorted and unique"));
    }
    for (const auto& tag : dropped_tags) {
        if (!safe_text(tag, LoweredTagBinding::kMaxTagBytes)) {
            return core::Result<void>::failure(validation(
                "inspection report tag is invalid"));
        }
    }
    for (const auto& tag : lowered_tags) {
        if (!safe_text(tag, LoweredTagBinding::kMaxTagBytes)) {
            return core::Result<void>::failure(validation(
                "inspection report tag is invalid"));
        }
    }
    if (!std::is_sorted(schema_mismatches.begin(), schema_mismatches.end()) ||
        std::adjacent_find(schema_mismatches.begin(), schema_mismatches.end()) !=
            schema_mismatches.end()) {
        return core::Result<void>::failure(validation(
            "inspection report schema mismatches must be sorted and unique"));
    }
    for (const auto& mismatch : schema_mismatches) {
        if (auto result = mismatch.schema.validate(); !result) return result;
        if (!safe_text(mismatch.detail, 4096U, true)) {
            return core::Result<void>::failure(validation(
                "inspection report mismatch detail is invalid"));
        }
    }
    return core::Result<void>::success();
}

std::string SemanticInspectionReport::serialize() const {
    std::ostringstream output;
    output << kReportMagic << ' ' << kSchemaVersion << '\n';
    output << "TIER " << static_cast<std::uint64_t>(tier) << '\n';
    output << "RESOLUTIONS " << binding_resolutions.size() << '\n';
    for (const auto& resolution : binding_resolutions) {
        output << "RESOLUTION "
               << static_cast<std::uint64_t>(resolution.binding.kind) << ' '
               << resolution.binding.identity << ' ' << (resolution.resolved ? 1 : 0)
               << ' ' << std::quoted(resolution.detail) << '\n';
    }
    output << "DROPPED_TAGS " << dropped_tags.size() << '\n';
    for (const auto& tag : dropped_tags) {
        output << "DROPPED_TAG " << std::quoted(tag) << '\n';
    }
    output << "LOWERED_TAGS " << lowered_tags.size() << '\n';
    for (const auto& tag : lowered_tags) {
        output << "LOWERED_TAG " << std::quoted(tag) << '\n';
    }
    output << "MISMATCHES " << schema_mismatches.size() << '\n';
    for (const auto& mismatch : schema_mismatches) {
        output << "MISMATCH " << std::quoted(mismatch.schema.name_space) << ' '
               << std::quoted(mismatch.schema.name) << ' ' << mismatch.expected.value
               << ' ' << mismatch.found.value << ' ' << std::quoted(mismatch.detail)
               << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<SemanticInspectionReport> SemanticInspectionReport::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<SemanticInspectionReport>::failure(parse_error(
            "inspection report text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || magic != kReportMagic || !read_uint(input, version) ||
        version != kSchemaVersion) {
        return core::Result<SemanticInspectionReport>::failure(parse_error(
            "inspection report framing or schema version is invalid"));
    }
    SemanticInspectionReport report;
    std::uint64_t tier = 0U;
    std::size_t resolution_count = 0U;
    if (!require_token(input, "TIER") || !read_uint(input, tier) ||
        tier > static_cast<std::uint64_t>(VerificationTier::full_game) ||
        !require_token(input, "RESOLUTIONS") ||
        !read_count(input, SemanticInspectionReport::kMaxResolutions, resolution_count)) {
        return core::Result<SemanticInspectionReport>::failure(parse_error(
            "inspection report header is malformed"));
    }
    report.tier = static_cast<VerificationTier>(tier);
    report.binding_resolutions.reserve(resolution_count);
    for (std::size_t index = 0U; index < resolution_count; ++index) {
        BindingResolution resolution;
        std::uint64_t kind = 0U;
        std::uint64_t resolved = 0U;
        if (!require_token(input, "RESOLUTION") || !read_uint(input, kind) ||
            !read_uint(input, resolution.binding.identity) ||
            !read_uint(input, resolved) ||
            !(input >> std::quoted(resolution.detail)) ||
            kind > static_cast<std::uint64_t>(world::AuthoringBindingKind::mesh_asset) ||
            resolved > 1U) {
            return core::Result<SemanticInspectionReport>::failure(parse_error(
                "inspection report resolution is malformed"));
        }
        resolution.binding.kind = static_cast<world::AuthoringBindingKind>(kind);
        resolution.resolved = resolved != 0U;
        report.binding_resolutions.push_back(std::move(resolution));
    }
    std::size_t dropped_count = 0U;
    if (!require_token(input, "DROPPED_TAGS") ||
        !read_count(input, SemanticInspectionReport::kMaxTags, dropped_count)) {
        return core::Result<SemanticInspectionReport>::failure(parse_error(
            "inspection report dropped-tag header is malformed"));
    }
    report.dropped_tags.reserve(dropped_count);
    for (std::size_t index = 0U; index < dropped_count; ++index) {
        std::string tag;
        if (!require_token(input, "DROPPED_TAG") || !(input >> std::quoted(tag))) {
            return core::Result<SemanticInspectionReport>::failure(parse_error(
                "inspection report dropped tag is malformed"));
        }
        report.dropped_tags.push_back(std::move(tag));
    }
    std::size_t lowered_count = 0U;
    if (!require_token(input, "LOWERED_TAGS") ||
        !read_count(input, SemanticInspectionReport::kMaxTags, lowered_count)) {
        return core::Result<SemanticInspectionReport>::failure(parse_error(
            "inspection report lowered-tag header is malformed"));
    }
    report.lowered_tags.reserve(lowered_count);
    for (std::size_t index = 0U; index < lowered_count; ++index) {
        std::string tag;
        if (!require_token(input, "LOWERED_TAG") || !(input >> std::quoted(tag))) {
            return core::Result<SemanticInspectionReport>::failure(parse_error(
                "inspection report lowered tag is malformed"));
        }
        report.lowered_tags.push_back(std::move(tag));
    }
    std::size_t mismatch_count = 0U;
    if (!require_token(input, "MISMATCHES") ||
        !read_count(input, SemanticInspectionReport::kMaxMismatches, mismatch_count)) {
        return core::Result<SemanticInspectionReport>::failure(parse_error(
            "inspection report mismatch header is malformed"));
    }
    report.schema_mismatches.reserve(mismatch_count);
    for (std::size_t index = 0U; index < mismatch_count; ++index) {
        SchemaMismatch mismatch;
        std::uint64_t expected = 0U;
        std::uint64_t found = 0U;
        if (!require_token(input, "MISMATCH") ||
            !(input >> std::quoted(mismatch.schema.name_space) >>
              std::quoted(mismatch.schema.name)) ||
            !read_uint(input, expected) || !read_uint(input, found) ||
            !(input >> std::quoted(mismatch.detail))) {
            return core::Result<SemanticInspectionReport>::failure(parse_error(
                "inspection report schema mismatch is malformed"));
        }
        mismatch.expected = SchemaVersion{static_cast<std::uint32_t>(expected)};
        mismatch.found = SchemaVersion{static_cast<std::uint32_t>(found)};
        report.schema_mismatches.push_back(std::move(mismatch));
    }
    if (!require_token(input, "END")) {
        return core::Result<SemanticInspectionReport>::failure(parse_error(
            "inspection report is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<SemanticInspectionReport>::failure(parse_error(
            "inspection report contains trailing unbound data"));
    }
    if (auto result = report.validate(); !result) {
        return core::Result<SemanticInspectionReport>::failure(result.error());
    }
    return core::Result<SemanticInspectionReport>::success(std::move(report));
}

core::Result<assets::Sha256Digest> SemanticInspectionReport::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

SemanticInspectionReport inspect_semantic_bindings(
    const world::WorldModel& model,
    const SemanticSidecar& sidecar,
    const SchemaRegistry& registry,
    const TagLoweringMap* lowering,
    VerificationTier tier,
    const std::function<bool(const world::AuthoringBinding&)>& resolve_binding) {
    SemanticInspectionReport report;
    report.tier = tier;
    for (const auto& record : sidecar.records()) {
        if (auto result = registry.reconcile(record); !result) {
            const auto latest = registry.latest_registered(record.schema);
            report.schema_mismatches.push_back(SchemaMismatch{
                record.schema,
                latest ? latest.value() : SchemaVersion{0U},
                SchemaVersion{record.schema_version},
                result.error().message,
            });
        }
        for (const auto& binding : record.bound_identities) {
            const bool resolved = resolve_binding(binding);
            report.binding_resolutions.push_back(BindingResolution{
                binding,
                resolved,
                std::string(world::authoring_binding_kind_name(binding.kind)) + ":" +
                    std::to_string(binding.identity),
            });
        }
    }
    std::set<std::string> authored_tags;
    for (const auto& [identity, area] : model.areas()) {
        static_cast<void>(identity);
        authored_tags.insert(area.tags.begin(), area.tags.end());
    }
    for (const auto& tag : authored_tags) {
        if (lowering != nullptr && lowering->find(tag) != nullptr) {
            report.lowered_tags.push_back(tag);
        } else {
            report.dropped_tags.push_back(tag);
        }
    }
    std::sort(report.schema_mismatches.begin(), report.schema_mismatches.end());
    report.schema_mismatches.erase(
        std::unique(report.schema_mismatches.begin(), report.schema_mismatches.end()),
        report.schema_mismatches.end());
    return report;
}

} // namespace carto::semantic
