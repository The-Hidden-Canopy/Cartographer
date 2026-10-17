#include <carto/engineering/cad_provider.hpp>

#include <algorithm>
#include <cctype>

namespace carto::engineering {
namespace {

constexpr std::size_t kMaximumHeaderScanBytes = 64U * 1024U;

std::string uppercase_ascii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char byte) {
        return static_cast<char>(std::toupper(byte));
    });
    return result;
}

bool skip_step_space_and_comments(
    std::string_view text, std::size_t& cursor, std::size_t limit) {
    while (cursor < limit) {
        if (std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
            continue;
        }
        if (text.substr(cursor, 2U) != "/*") return true;
        const auto comment_end = text.find("*/", cursor + 2U);
        if (comment_end == std::string_view::npos || comment_end + 2U > limit) return false;
        cursor = comment_end + 2U;
    }
    return true;
}

bool is_step_identifier_byte(unsigned char byte) {
    return std::isalnum(byte) != 0 || byte == '_';
}

bool find_unquoted_step_token(
    std::string_view text,
    std::string_view token,
    std::size_t begin,
    std::size_t limit,
    std::size_t& found) {
    bool in_string = false;
    char quote = '\0';
    for (std::size_t cursor = begin; cursor + token.size() <= limit;) {
        if (text.substr(cursor, 2U) == "/*") {
            const auto comment_end = text.find("*/", cursor + 2U);
            if (comment_end == std::string_view::npos || comment_end + 2U > limit) return false;
            cursor = comment_end + 2U;
            continue;
        }
        const char byte = text[cursor];
        if (in_string) {
            if (byte == quote) {
                if (cursor + 1U < limit && text[cursor + 1U] == quote) {
                    cursor += 2U;
                    continue;
                }
                in_string = false;
            }
            ++cursor;
            continue;
        }
        if (byte == '\'' || byte == '"') {
            quote = byte;
            in_string = true;
            ++cursor;
            continue;
        }
        if (text.substr(cursor, token.size()) == token &&
            (cursor == begin || !is_step_identifier_byte(
                static_cast<unsigned char>(text[cursor - 1U]))) &&
            (cursor + token.size() == limit || !is_step_identifier_byte(
                static_cast<unsigned char>(text[cursor + token.size()])))) {
            found = cursor;
            return true;
        }
        ++cursor;
    }
    return false;
}

// This bounded probe recognizes only the exchange header's declared schema;
// it does not parse STEP entities or synthesize engineering geometry.
bool probe_step_schema_profile(std::string_view header, std::string& schema_profile) {
    constexpr std::string_view kPart21Marker = "ISO-10303-21;";
    const auto normalized = uppercase_ascii(header);
    if (!normalized.starts_with(kPart21Marker)) return false;

    std::size_t cursor = kPart21Marker.size();
    if (!skip_step_space_and_comments(normalized, cursor, normalized.size()) ||
        normalized.substr(cursor, 7U) != "HEADER;") {
        return false;
    }
    const auto header_content = cursor + std::string_view("HEADER;").size();
    std::size_t header_end = 0U;
    if (!find_unquoted_step_token(
            normalized, "ENDSEC;", header_content, normalized.size(), header_end)) {
        return false;
    }
    std::size_t schema_record = 0U;
    if (!find_unquoted_step_token(
            normalized, "FILE_SCHEMA", header_content, header_end, schema_record)) {
        return false;
    }

    cursor = schema_record + std::string_view("FILE_SCHEMA").size();
    if (!skip_step_space_and_comments(normalized, cursor, header_end) ||
        cursor >= header_end || normalized[cursor] != '(') {
        return false;
    }
    ++cursor;
    if (!skip_step_space_and_comments(normalized, cursor, header_end) ||
        cursor >= header_end || normalized[cursor] != '(') {
        return false;
    }
    ++cursor;
    if (!skip_step_space_and_comments(header, cursor, header_end) || cursor >= header_end ||
        (header[cursor] != '\'' && header[cursor] != '"')) {
        return false;
    }
    const char quote = header[cursor++];
    const auto value_begin = cursor;
    const auto quote_end = header.find(quote, cursor);
    if (quote_end == std::string_view::npos || quote_end == value_begin ||
        quote_end - value_begin > 128U) {
        return false;
    }
    schema_profile.assign(header.substr(value_begin, quote_end - value_begin));
    if (!std::all_of(schema_profile.begin(), schema_profile.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU;
        })) {
        return false;
    }
    cursor = quote_end + 1U;
    if (!skip_step_space_and_comments(normalized, cursor, header_end) ||
        cursor >= header_end || normalized[cursor++] != ')' ||
        !skip_step_space_and_comments(normalized, cursor, header_end) ||
        cursor >= header_end || normalized[cursor++] != ')' ||
        !skip_step_space_and_comments(normalized, cursor, header_end) ||
        cursor >= header_end || normalized[cursor] != ';') {
        return false;
    }
    return true;
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

} // namespace

core::Result<void> ImportProbe::validate() const {
    if (provider_identity.empty() || provider_version.empty() ||
        (recognized && schema_profile.empty()) || diagnostics.size() > 256U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed, "CAD import probe is malformed"));
    }
    return core::Result<void>::success();
}

std::string_view StepAp242Provider::identity() const noexcept {
    return "cartographer.step-ap242.boundary";
}

std::string_view StepAp242Provider::version() const noexcept {
    return "1.0.0";
}

core::Result<ImportProbe> StepAp242Provider::probe(std::string_view source_bytes) const {
    ImportProbe result;
    result.provider_identity = std::string(identity());
    result.provider_version = std::string(version());
    const auto header = source_bytes.substr(0U,
        std::min(source_bytes.size(), kMaximumHeaderScanBytes));
    const auto normalized_header = uppercase_ascii(header);
    constexpr std::string_view kPart21Marker = "ISO-10303-21;";
    const bool has_step_marker = normalized_header.starts_with(kPart21Marker);
    std::string schema_profile;
    const bool has_schema = has_step_marker &&
        probe_step_schema_profile(header, schema_profile);
    result.recognized = has_step_marker && has_schema;
    result.supported = false;
    result.schema_profile = has_schema ? std::move(schema_profile) : "unknown";
    result.units.clear();
    const bool is_ap242 = has_schema &&
        uppercase_ascii(result.schema_profile).starts_with("AP242");
    if (!result.recognized) {
        result.diagnostics.push_back(
            "source does not contain a bounded STEP Part 21 HEADER with FILE_SCHEMA");
        result.diagnostics.push_back(
            "marker text outside the exchange header is not treated as CAD source identity");
    } else if (!is_ap242) {
        result.diagnostics.push_back(
            "STEP header recognized, but its declared schema is outside the AP242 profile");
        result.diagnostics.push_back(
            "no topology, tessellation, PMI, or units were synthesized from the probe");
    } else {
        result.diagnostics.push_back(
            "STEP/AP242 header recognized, but no public CAD kernel is connected in this build");
        result.diagnostics.push_back(
            "no topology, tessellation, PMI, or units were synthesized from the probe");
    }
    if (auto validation = result.validate(); !validation) {
        return core::Result<ImportProbe>::failure(validation.error());
    }
    return core::Result<ImportProbe>::success(std::move(result));
}

core::Result<AuthoringDocument> StepAp242Provider::read_assembly(
    std::string_view source_bytes, std::string locator, std::string revision,
    assets::Sha256Digest import_settings_digest) const {
    static_cast<void>(source_bytes);
    static_cast<void>(locator);
    static_cast<void>(revision);
    static_cast<void>(import_settings_digest);
    return core::Result<AuthoringDocument>::failure(unsupported(
        "STEP/AP242 import requires an external CAD kernel/provider; probe results are not an assembly"));
}

} // namespace carto::engineering
