#include <carto/character/manifest.hpp>

#include <carto/core/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <span>
#include <system_error>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic version_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

bool safe_token(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

void write_json_string(std::ostringstream& output, std::string_view value) {
    output << '"';
    for (const char raw : value) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(byte) << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(byte);
            }
        }
    }
    output << '"';
}

std::string json_string(std::string_view value) {
    std::ostringstream output;
    write_json_string(output, value);
    return output.str();
}

// Exact 64-char lowercase hex admission for manifest digests. The blob store
// hex parser also accepts uppercase; interchange digests do not.
bool is_lowercase_hex_64(std::string_view value) noexcept {
    if (value.size() != 64U) return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f');
    });
}

core::Result<assets::Sha256Digest> parse_digest(std::string_view token, const char* label) {
    if (!is_lowercase_hex_64(token)) {
        return core::Result<assets::Sha256Digest>::failure(parse_error(
            std::string(label) + " digest is not exact 64-char lowercase hex"));
    }
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) {
        return core::Result<assets::Sha256Digest>::failure(parse_error(
            std::string(label) + " digest is malformed"));
    }
    return parsed;
}

core::Result<std::string> parse_string_member(
    const std::vector<core::json::Member>& members,
    std::string_view key,
    std::size_t maximum) {
    const core::json::Member* member = core::json::find_member(members, key);
    if (member == nullptr) {
        return core::Result<std::string>::failure(parse_error(
            std::string("manifest member is missing: ") + std::string(key)));
    }
    const auto decoded = core::json::decode_string(member->raw_value);
    if (!decoded || !safe_token(decoded.value(), maximum)) {
        return core::Result<std::string>::failure(parse_error(
            std::string("manifest member is not a bounded safe string: ") + std::string(key)));
    }
    return core::Result<std::string>::success(decoded.value());
}

core::Result<std::uint64_t> parse_uint_member(
    const std::vector<core::json::Member>& members,
    std::string_view key) {
    const core::json::Member* member = core::json::find_member(members, key);
    if (member == nullptr) {
        return core::Result<std::uint64_t>::failure(parse_error(
            std::string("manifest member is missing: ") + std::string(key)));
    }
    const auto parsed = core::json::parse_uint(member->raw_value);
    if (!parsed) {
        return core::Result<std::uint64_t>::failure(parse_error(
            std::string("manifest member is not an unsigned integer: ") + std::string(key)));
    }
    return parsed;
}

core::Result<double> parse_double_token(std::string_view token, const char* label) {
    if (token.empty()) {
        return core::Result<double>::failure(parse_error(
            std::string(label) + " number is empty"));
    }
    double parsed = 0.0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
        !std::isfinite(parsed)) {
        return core::Result<double>::failure(parse_error(
            std::string(label) + " number is invalid"));
    }
    return core::Result<double>::success(parsed);
}

core::Result<std::vector<std::string_view>> split_json_array(std::string_view raw) {
    if (raw.size() < 2U || raw.front() != '[' || raw.back() != ']') {
        return core::Result<std::vector<std::string_view>>::failure(parse_error(
            "manifest member is not a JSON array"));
    }
    std::vector<std::string_view> elements;
    std::size_t cursor = 1U;
    const std::size_t end = raw.size() - 1U;
    while (cursor < end) {
        while (cursor < end && (raw[cursor] == ' ' || raw[cursor] == '\n' ||
                                raw[cursor] == '\r' || raw[cursor] == '\t')) {
            ++cursor;
        }
        if (cursor >= end) break;
        const std::size_t start = cursor;
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        while (cursor < end) {
            const char byte = raw[cursor];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (byte == '\\') {
                    escaped = true;
                } else if (byte == '"') {
                    in_string = false;
                }
            } else if (byte == '"') {
                in_string = true;
            } else if (byte == '{' || byte == '[') {
                ++depth;
            } else if (byte == '}' || byte == ']') {
                --depth;
            } else if (byte == ',' && depth == 0) {
                break;
            }
            ++cursor;
        }
        if (in_string || depth != 0) {
            return core::Result<std::vector<std::string_view>>::failure(parse_error(
                "manifest array element is malformed"));
        }
        std::size_t element_end = cursor;
        while (element_end > start &&
               (raw[element_end - 1U] == ' ' || raw[element_end - 1U] == '\n' ||
                raw[element_end - 1U] == '\r' || raw[element_end - 1U] == '\t')) {
            --element_end;
        }
        if (element_end == start) {
            return core::Result<std::vector<std::string_view>>::failure(parse_error(
                "manifest array contains an empty element"));
        }
        elements.push_back(raw.substr(start, element_end - start));
        if (cursor < end) {
            if (raw[cursor] != ',') {
                return core::Result<std::vector<std::string_view>>::failure(parse_error(
                    "manifest array separator is invalid"));
            }
            ++cursor;
        }
    }
    return core::Result<std::vector<std::string_view>>::success(std::move(elements));
}

std::string_view handedness_token(Handedness handedness) noexcept {
    return handedness == Handedness::right ? "RH" : "LH";
}

std::string_view axis_token(Axis axis) noexcept {
    switch (axis) {
    case Axis::positive_x: return "+X";
    case Axis::negative_x: return "-X";
    case Axis::positive_y: return "+Y";
    case Axis::negative_y: return "-Y";
    case Axis::positive_z: return "+Z";
    case Axis::negative_z: return "-Z";
    }
    return "?";
}

core::Result<Handedness> parse_handedness(std::string_view token) {
    if (token == "RH") return core::Result<Handedness>::success(Handedness::right);
    if (token == "LH") return core::Result<Handedness>::success(Handedness::left);
    return core::Result<Handedness>::failure(parse_error(
        "manifest coordinate handedness is invalid"));
}

core::Result<Axis> parse_axis(std::string_view token) {
    for (const Axis axis : {Axis::positive_x, Axis::negative_x, Axis::positive_y,
                            Axis::negative_y, Axis::positive_z, Axis::negative_z}) {
        if (token == axis_token(axis)) {
            return core::Result<Axis>::success(axis);
        }
    }
    return core::Result<Axis>::failure(parse_error(
        "manifest coordinate axis is invalid"));
}

} // namespace

bool is_safe_product_path(std::string_view path) noexcept {
    if (path.empty() || path.size() > CharacterPreparedManifest::kMaxProductPathBytes ||
        path.front() == '/' || path.front() == '\\') {
        return false;
    }
    bool component_start = true;
    for (const char byte : path) {
        const unsigned char value = static_cast<unsigned char>(byte);
        if (value < 0x20U || value == 0x7fU) return false;
        if (byte == '\\') return false;
        if (byte == '/') {
            component_start = true;
            continue;
        }
        if (component_start) {
            if (byte == '.') return false; // covers "." and ".." components
            component_start = false;
        }
    }
    return true;
}

core::Result<void> ManifestSource::validate() const {
    if (!safe_token(project_namespace, CharacterPreparedManifest::kMaxNamespaceBytes) ||
        revision == 0U || project_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "manifest source namespace, revision, or digest is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> ManifestCoordinate::validate() const {
    if (!std::isfinite(meters_per_unit) || meters_per_unit <= 0.0) {
        return core::Result<void>::failure(invalid(
            "manifest coordinate meters-per-unit must be positive and finite"));
    }
    const auto magnitude = [](Axis axis) {
        switch (axis) {
        case Axis::positive_x: case Axis::negative_x: return 0;
        case Axis::positive_y: case Axis::negative_y: return 1;
        case Axis::positive_z: case Axis::negative_z: return 2;
        }
        return -1;
    };
    if (magnitude(up) < 0 || magnitude(forward) < 0 || magnitude(up) == magnitude(forward)) {
        return core::Result<void>::failure(validation(
            "manifest coordinate up and forward axes must be distinct"));
    }
    return core::Result<void>::success();
}

core::Result<void> TargetProfileRef::validate() const {
    if (!safe_token(id, CharacterPreparedManifest::kMaxProfileIdBytes) ||
        digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "manifest target profile id or digest is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> ManifestProduct::validate() const {
    if (!safe_token(kind, CharacterPreparedManifest::kMaxProductKindBytes) ||
        !is_safe_product_path(path) || schema == 0U || digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "manifest product kind, path, schema, or digest is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> CharacterPreparedManifest::validate() const {
    if (schema_major != kSchemaMajor || schema_minor != kSchemaMinor) {
        return core::Result<void>::failure(version_error(
            "manifest schema version is not supported by this codec"));
    }
    if (required_capabilities.size() > kMaxCapabilities ||
        products.size() > kMaxProducts || products.empty() ||
        !character_id) {
        return core::Result<void>::failure(invalid(
            "manifest identity or bounded collection size is invalid"));
    }
    if (auto result = source.validate(); !result) return result;
    if (auto result = coordinate.validate(); !result) return result;
    if (auto result = target_profile.validate(); !result) return result;
    if (!std::is_sorted(required_capabilities.begin(), required_capabilities.end()) ||
        std::adjacent_find(required_capabilities.begin(), required_capabilities.end()) !=
            required_capabilities.end() ||
        std::any_of(required_capabilities.begin(), required_capabilities.end(),
            [](const std::string& capability) {
                return !safe_token(capability, kMaxCapabilityBytes);
            })) {
        return core::Result<void>::failure(validation(
            "manifest required capabilities must be sorted, unique, bounded safe tokens"));
    }
    if (!std::is_sorted(products.begin(), products.end()) ||
        std::adjacent_find(products.begin(), products.end()) != products.end()) {
        return core::Result<void>::failure(validation(
            "manifest products must be sorted and unique by path"));
    }
    std::set<std::string> paths;
    for (const ManifestProduct& product : products) {
        if (auto result = product.validate(); !result) return result;
        if (!paths.insert(product.path).second) {
            return core::Result<void>::failure(validation(
                "manifest product paths must be distinct"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> CharacterPreparedManifest::verify_closure(
    const std::map<std::string, assets::Sha256Digest>& payload_digests) const {
    if (auto result = validate(); !result) return result;
    if (payload_digests.size() != products.size()) {
        return core::Result<void>::failure(validation(
            "manifest closure does not match the payload table size"));
    }
    for (const ManifestProduct& product : products) {
        const auto payload = payload_digests.find(product.path);
        if (payload == payload_digests.end()) {
            return core::Result<void>::failure(validation(
                "manifest product payload is missing: " + product.path));
        }
        if (payload->second != product.digest) {
            return core::Result<void>::failure(validation(
                "manifest product payload digest disagrees with the declaration: " +
                product.path));
        }
    }
    return core::Result<void>::success();
}

std::string CharacterPreparedManifest::serialize() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n";
    output << "  \"format\": " << json_string(std::string(kFormat)) << ",\n";
    output << "  \"schema_major\": " << schema_major << ",\n";
    output << "  \"schema_minor\": " << schema_minor << ",\n";
    output << "  \"source\": {\n";
    output << "    \"project_namespace\": " << json_string(source.project_namespace) << ",\n";
    output << "    \"revision\": " << source.revision << ",\n";
    output << "    \"project_sha256\": " << json_string(source.project_digest.hex()) << "\n";
    output << "  },\n";
    output << "  \"coordinate\": {\n";
    output << "    \"handedness\": " << json_string(std::string(handedness_token(coordinate.handedness))) << ",\n";
    output << "    \"up\": " << json_string(std::string(axis_token(coordinate.up))) << ",\n";
    output << "    \"forward\": " << json_string(std::string(axis_token(coordinate.forward))) << ",\n";
    output << "    \"meters_per_unit\": " << coordinate.meters_per_unit << "\n";
    output << "  },\n";
    output << "  \"character_id\": " << json_string(id_string(character_id)) << ",\n";
    output << "  \"target_profile\": {\n";
    output << "    \"id\": " << json_string(target_profile.id) << ",\n";
    output << "    \"digest\": " << json_string(target_profile.digest.hex()) << "\n";
    output << "  },\n";
    output << "  \"required_capabilities\": [";
    for (std::size_t index = 0U; index < required_capabilities.size(); ++index) {
        output << (index == 0U ? "" : ", ") << json_string(required_capabilities[index]);
    }
    output << "],\n";
    output << "  \"products\": [";
    for (std::size_t index = 0U; index < products.size(); ++index) {
        const ManifestProduct& product = products[index];
        output << (index == 0U ? "" : ",") << "\n    {\n";
        output << "      \"kind\": " << json_string(product.kind) << ",\n";
        output << "      \"path\": " << json_string(product.path) << ",\n";
        output << "      \"schema\": " << product.schema << ",\n";
        output << "      \"sha256\": " << json_string(product.digest.hex()) << "\n";
        output << "    }";
    }
    output << (products.empty() ? "" : "\n  ") << "]\n";
    output << "}\n";
    return output.str();
}

core::Result<assets::Sha256Digest> CharacterPreparedManifest::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<CharacterPreparedManifest> CharacterPreparedManifest::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<CharacterPreparedManifest>::failure(parse_error(
            "manifest text is empty or exceeds its byte bound"));
    }
    const auto top = core::json::parse_object(text);
    if (!top) {
        return core::Result<CharacterPreparedManifest>::failure(top.error());
    }
    CharacterPreparedManifest manifest;

    const auto format = parse_string_member(top.value(), "format", 64U);
    if (!format) return core::Result<CharacterPreparedManifest>::failure(format.error());
    if (format.value() != kFormat) {
        return core::Result<CharacterPreparedManifest>::failure(parse_error(
            "manifest format string is not carto-character-prepared"));
    }
    const auto schema_major = parse_uint_member(top.value(), "schema_major");
    if (!schema_major) return core::Result<CharacterPreparedManifest>::failure(schema_major.error());
    const auto schema_minor = parse_uint_member(top.value(), "schema_minor");
    if (!schema_minor) return core::Result<CharacterPreparedManifest>::failure(schema_minor.error());
    if (schema_major.value() != kSchemaMajor || schema_minor.value() > kSchemaMinor) {
        return core::Result<CharacterPreparedManifest>::failure(version_error(
            "manifest schema version is not supported by this codec"));
    }
    manifest.schema_major = static_cast<std::uint32_t>(schema_major.value());
    manifest.schema_minor = static_cast<std::uint32_t>(schema_minor.value());

    const core::json::Member* source = core::json::find_member(top.value(), "source");
    const core::json::Member* coordinate = core::json::find_member(top.value(), "coordinate");
    const core::json::Member* character_id = core::json::find_member(top.value(), "character_id");
    const core::json::Member* profile = core::json::find_member(top.value(), "target_profile");
    const core::json::Member* capabilities = core::json::find_member(top.value(), "required_capabilities");
    const core::json::Member* products = core::json::find_member(top.value(), "products");
    if (source == nullptr || coordinate == nullptr || character_id == nullptr ||
        profile == nullptr || capabilities == nullptr || products == nullptr) {
        return core::Result<CharacterPreparedManifest>::failure(parse_error(
            "manifest is missing a required envelope member"));
    }
    // Strict envelope: unknown top-level members are refused, never dropped.
    if (top.value().size() != 9U) {
        return core::Result<CharacterPreparedManifest>::failure(parse_error(
            "manifest contains unknown top-level members"));
    }

    const auto source_members = core::json::parse_object(source->raw_value);
    if (!source_members) {
        return core::Result<CharacterPreparedManifest>::failure(source_members.error());
    }
    if (source_members.value().size() != 3U) {
        return core::Result<CharacterPreparedManifest>::failure(parse_error(
            "manifest source object has unknown members"));
    }
    const auto source_namespace = parse_string_member(
        source_members.value(), "project_namespace", kMaxNamespaceBytes);
    if (!source_namespace) {
        return core::Result<CharacterPreparedManifest>::failure(source_namespace.error());
    }
    const auto revision = parse_uint_member(source_members.value(), "revision");
    if (!revision) return core::Result<CharacterPreparedManifest>::failure(revision.error());
    const auto source_digest_raw = parse_string_member(
        source_members.value(), "project_sha256", 64U);
    if (!source_digest_raw) {
        return core::Result<CharacterPreparedManifest>::failure(source_digest_raw.error());
    }
    const auto source_digest = parse_digest(source_digest_raw.value(), "source");
    if (!source_digest) {
        return core::Result<CharacterPreparedManifest>::failure(source_digest.error());
    }
    manifest.source = ManifestSource{
        source_namespace.value(), revision.value(), source_digest.value()};

    const auto coordinate_members = core::json::parse_object(coordinate->raw_value);
    if (!coordinate_members) {
        return core::Result<CharacterPreparedManifest>::failure(coordinate_members.error());
    }
    if (coordinate_members.value().size() != 4U) {
        return core::Result<CharacterPreparedManifest>::failure(parse_error(
            "manifest coordinate object has unknown members"));
    }
    const auto handedness = parse_string_member(coordinate_members.value(), "handedness", 8U);
    if (!handedness) {
        return core::Result<CharacterPreparedManifest>::failure(handedness.error());
    }
    const auto up = parse_string_member(coordinate_members.value(), "up", 8U);
    if (!up) return core::Result<CharacterPreparedManifest>::failure(up.error());
    const auto forward = parse_string_member(coordinate_members.value(), "forward", 8U);
    if (!forward) return core::Result<CharacterPreparedManifest>::failure(forward.error());
    const core::json::Member* mpu = core::json::find_member(coordinate_members.value(), "meters_per_unit");
    if (mpu == nullptr) {
        return core::Result<CharacterPreparedManifest>::failure(parse_error(
            "manifest coordinate meters_per_unit is missing"));
    }
    const auto meters = parse_double_token(mpu->raw_value, "meters_per_unit");
    if (!meters) return core::Result<CharacterPreparedManifest>::failure(meters.error());
    const auto parsed_handedness = parse_handedness(handedness.value());
    if (!parsed_handedness) {
        return core::Result<CharacterPreparedManifest>::failure(parsed_handedness.error());
    }
    const auto parsed_up = parse_axis(up.value());
    if (!parsed_up) return core::Result<CharacterPreparedManifest>::failure(parsed_up.error());
    const auto parsed_forward = parse_axis(forward.value());
    if (!parsed_forward) {
        return core::Result<CharacterPreparedManifest>::failure(parsed_forward.error());
    }
    manifest.coordinate = ManifestCoordinate{
        parsed_handedness.value(), parsed_up.value(), parsed_forward.value(),
        meters.value()};

    const auto identity = parse_string_member(top.value(), "character_id", 64U);
    if (!identity) return core::Result<CharacterPreparedManifest>::failure(identity.error());
    {
        constexpr std::string_view kPrefix = "character:";
        const std::string_view value = identity.value();
        if (value.size() <= kPrefix.size() || value.substr(0, kPrefix.size()) != kPrefix ||
            !std::all_of(value.begin() + static_cast<std::ptrdiff_t>(kPrefix.size()), value.end(),
                [](char byte) { return byte >= '0' && byte <= '9'; })) {
            return core::Result<CharacterPreparedManifest>::failure(parse_error(
                "manifest character_id is not a stable character identity string"));
        }
        const std::string_view digits = value.substr(kPrefix.size());
        std::uint64_t parsed = 0U;
        const auto converted = std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
        if (converted.ec != std::errc{} || parsed == 0U) {
            return core::Result<CharacterPreparedManifest>::failure(parse_error(
                "manifest character_id is not a stable character identity string"));
        }
        manifest.character_id = CharacterId{parsed};
    }

    const auto profile_members = core::json::parse_object(profile->raw_value);
    if (!profile_members) {
        return core::Result<CharacterPreparedManifest>::failure(profile_members.error());
    }
    if (profile_members.value().size() != 2U) {
        return core::Result<CharacterPreparedManifest>::failure(parse_error(
            "manifest target profile object has unknown members"));
    }
    const auto profile_id = parse_string_member(profile_members.value(), "id", kMaxProfileIdBytes);
    if (!profile_id) return core::Result<CharacterPreparedManifest>::failure(profile_id.error());
    const auto profile_digest_raw = parse_string_member(profile_members.value(), "digest", 64U);
    if (!profile_digest_raw) {
        return core::Result<CharacterPreparedManifest>::failure(profile_digest_raw.error());
    }
    const auto profile_digest = parse_digest(profile_digest_raw.value(), "target profile");
    if (!profile_digest) {
        return core::Result<CharacterPreparedManifest>::failure(profile_digest.error());
    }
    manifest.target_profile =
        TargetProfileRef{profile_id.value(), profile_digest.value()};

    const auto capability_elements = split_json_array(capabilities->raw_value);
    if (!capability_elements) {
        return core::Result<CharacterPreparedManifest>::failure(capability_elements.error());
    }
    for (const std::string_view element : capability_elements.value()) {
        if (element.size() < 2U || element.front() != '"' || element.back() != '"') {
            return core::Result<CharacterPreparedManifest>::failure(parse_error(
                "manifest capability is not a string"));
        }
        const auto decoded = core::json::decode_string(element);
        if (!decoded) {
            return core::Result<CharacterPreparedManifest>::failure(decoded.error());
        }
        manifest.required_capabilities.push_back(decoded.value());
    }

    const auto product_elements = split_json_array(products->raw_value);
    if (!product_elements) {
        return core::Result<CharacterPreparedManifest>::failure(product_elements.error());
    }
    for (const std::string_view element : product_elements.value()) {
        const auto product_members = core::json::parse_object(element);
        if (!product_members) {
            return core::Result<CharacterPreparedManifest>::failure(product_members.error());
        }
        if (product_members.value().size() != 4U) {
            return core::Result<CharacterPreparedManifest>::failure(parse_error(
                "manifest product object has unknown members"));
        }
        const auto kind = parse_string_member(product_members.value(), "kind", kMaxProductKindBytes);
        if (!kind) return core::Result<CharacterPreparedManifest>::failure(kind.error());
        const auto path = parse_string_member(product_members.value(), "path", kMaxProductPathBytes);
        if (!path) return core::Result<CharacterPreparedManifest>::failure(path.error());
        const auto schema = parse_uint_member(product_members.value(), "schema");
        if (!schema) return core::Result<CharacterPreparedManifest>::failure(schema.error());
        const auto digest_raw = parse_string_member(product_members.value(), "sha256", 64U);
        if (!digest_raw) return core::Result<CharacterPreparedManifest>::failure(digest_raw.error());
        const auto digest = parse_digest(digest_raw.value(), "product");
        if (!digest) return core::Result<CharacterPreparedManifest>::failure(digest.error());
        manifest.products.push_back(ManifestProduct{
            kind.value(), path.value(), static_cast<std::uint32_t>(schema.value()),
            digest.value()});
    }

    std::sort(manifest.required_capabilities.begin(), manifest.required_capabilities.end());
    std::sort(manifest.products.begin(), manifest.products.end());
    if (auto result = manifest.validate(); !result) {
        return core::Result<CharacterPreparedManifest>::failure(result.error());
    }
    return core::Result<CharacterPreparedManifest>::success(std::move(manifest));
}

} // namespace carto::character
