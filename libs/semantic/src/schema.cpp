#include <carto/semantic/schema.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

namespace carto::semantic {
namespace {

constexpr std::string_view kRevisionMagic = "CARTOGRAPHER_SCHEMA_REVISION";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic missing_schema(std::string message) {
    return core::Diagnostic(core::ErrorCode::not_found, std::move(message));
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

bool valid_value_kind(SchemaValueKind kind) {
    switch (kind) {
    case SchemaValueKind::boolean:
    case SchemaValueKind::integer:
    case SchemaValueKind::real:
    case SchemaValueKind::string:
    case SchemaValueKind::tag:
    case SchemaValueKind::relationship:
    case SchemaValueKind::opaque:
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

bool read_digest(std::istream& input, assets::Sha256Digest& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) return false;
    value = parsed.value();
    return true;
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

} // namespace

std::string_view schema_value_kind_name(SchemaValueKind kind) noexcept {
    switch (kind) {
    case SchemaValueKind::boolean: return "boolean";
    case SchemaValueKind::integer: return "integer";
    case SchemaValueKind::real: return "real";
    case SchemaValueKind::string: return "string";
    case SchemaValueKind::tag: return "tag";
    case SchemaValueKind::relationship: return "relationship";
    case SchemaValueKind::opaque: return "opaque";
    }
    return "unknown";
}

core::Result<void> SchemaField::validate() const {
    if (!safe_text(name, kMaxFieldNameBytes)) {
        return core::Result<void>::failure(invalid(
            "schema field name is invalid"));
    }
    if (!valid_value_kind(value_kind)) {
        return core::Result<void>::failure(validation(
            "schema field value kind is invalid"));
    }
    if (static_cast<std::uint8_t>(ownership) >
        static_cast<std::uint8_t>(FieldOwnership::game_owned)) {
        return core::Result<void>::failure(validation(
            "schema field ownership is invalid"));
    }
    return core::Result<void>::success();
}

std::string SchemaRevision::declaration_text() const {
    std::ostringstream output;
    output << kRevisionMagic << ' ' << kSchemaVersion << '\n';
    output << "SCHEMA " << std::quoted(schema.name_space) << ' '
           << std::quoted(schema.name) << '\n';
    output << "VERSION " << version.value << '\n';
    output << "FIELDS " << fields.size() << '\n';
    for (const auto& field : fields) {
        output << "FIELD " << std::quoted(field.name) << ' '
               << static_cast<std::uint64_t>(field.value_kind) << ' '
               << static_cast<std::uint64_t>(field.ownership) << '\n';
    }
    return output.str();
}

std::string SchemaRevision::serialize() const {
    std::ostringstream output;
    output << declaration_text();
    output << "DIGEST " << revision_digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<void> SchemaRevision::validate() const {
    if (auto result = schema.validate(); !result) return result;
    if (version.value == 0U) {
        return core::Result<void>::failure(validation(
            "schema revision version must be non-zero"));
    }
    if (fields.size() > kMaxFieldsPerRevision) {
        return core::Result<void>::failure(validation(
            "schema revision exceeds its bounded field capacity"));
    }
    for (const auto& field : fields) {
        if (auto result = field.validate(); !result) return result;
    }
    if (!std::is_sorted(fields.begin(), fields.end(),
            [](const SchemaField& left, const SchemaField& right) {
                return left.name < right.name;
            }) ||
        std::adjacent_find(fields.begin(), fields.end(),
            [](const SchemaField& left, const SchemaField& right) {
                return left.name == right.name;
            }) != fields.end()) {
        return core::Result<void>::failure(validation(
            "schema revision fields must be sorted and unique by name"));
    }
    if (revision_digest.is_zero() || digest_of(declaration_text()) != revision_digest) {
        return core::Result<void>::failure(validation(
            "schema revision digest does not bind its field declaration"));
    }
    return core::Result<void>::success();
}

core::Result<void> SchemaRevision::refresh_digest() {
    const std::string declaration = declaration_text();
    if (declaration.size() > SemanticSidecar::kMaxSerializedBytes) {
        return core::Result<void>::failure(validation(
            "schema revision exceeds its serialized byte bound"));
    }
    revision_digest = digest_of(declaration);
    return core::Result<void>::success();
}

core::Result<SchemaRevision> SchemaRevision::deserialize(std::string_view text) {
    if (text.empty() || text.size() > SemanticSidecar::kMaxSerializedBytes) {
        return core::Result<SchemaRevision>::failure(parse_error(
            "schema revision text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || magic != kRevisionMagic || !read_uint(input, version) ||
        version != kSchemaVersion) {
        return core::Result<SchemaRevision>::failure(parse_error(
            "schema revision framing or schema version is invalid"));
    }
    SchemaRevision revision;
    std::uint64_t encoded_version = 0U;
    std::size_t field_count = 0U;
    if (!require_token(input, "SCHEMA") ||
        !(input >> std::quoted(revision.schema.name_space) >>
          std::quoted(revision.schema.name)) ||
        !require_token(input, "VERSION") || !read_uint(input, encoded_version) ||
        !require_token(input, "FIELDS") ||
        !read_count(input, SchemaRevision::kMaxFieldsPerRevision, field_count)) {
        return core::Result<SchemaRevision>::failure(parse_error(
            "schema revision header is malformed"));
    }
    revision.version = SchemaVersion{static_cast<std::uint32_t>(encoded_version)};
    revision.fields.reserve(field_count);
    for (std::size_t index = 0U; index < field_count; ++index) {
        SchemaField field;
        std::uint64_t kind = 0U;
        std::uint64_t ownership = 0U;
        if (!require_token(input, "FIELD") || !(input >> std::quoted(field.name)) ||
            !read_uint(input, kind) || !read_uint(input, ownership)) {
            return core::Result<SchemaRevision>::failure(parse_error(
                "schema revision field is malformed"));
        }
        field.value_kind = static_cast<SchemaValueKind>(kind);
        field.ownership = static_cast<FieldOwnership>(ownership);
        revision.fields.push_back(std::move(field));
    }
    if (!require_token(input, "DIGEST") || !read_digest(input, revision.revision_digest)) {
        return core::Result<SchemaRevision>::failure(parse_error(
            "schema revision digest record is malformed"));
    }
    if (!require_token(input, "END")) {
        return core::Result<SchemaRevision>::failure(parse_error(
            "schema revision is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<SchemaRevision>::failure(parse_error(
            "schema revision contains trailing unbound data"));
    }
    if (auto result = revision.validate(); !result) {
        return core::Result<SchemaRevision>::failure(result.error());
    }
    return core::Result<SchemaRevision>::success(std::move(revision));
}

core::Result<assets::Sha256Digest> SchemaRevision::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(revision_digest);
}

core::Result<void> SchemaRegistry::register_schema(const SchemaRevision& revision) {
    if (auto result = revision.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "schema registry admission"));
    }
    auto& revisions = revisions_[revision.schema];
    if (revisions.size() >= kMaxRevisionsPerNamespace) {
        return core::Result<void>::failure(validation(
            "schema registry namespace has reached its bounded revision capacity"));
    }
    const auto position = std::lower_bound(
        revisions.begin(), revisions.end(), revision.version,
        [](const SchemaRevision& existing, SchemaVersion version) {
            return existing.version < version;
        });
    if (position != revisions.end() && position->version == revision.version) {
        if (position->revision_digest != revision.revision_digest) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "schema registry already holds a different revision for this version"));
        }
        return core::Result<void>::success();
    }
    revisions.insert(position, revision);
    return core::Result<void>::success();
}

core::Result<const SchemaRevision*> SchemaRegistry::find(
    const SchemaIdentity& schema, SchemaVersion version) const noexcept {
    const auto found = revisions_.find(schema);
    if (found == revisions_.end()) {
        return core::Result<const SchemaRevision*>::failure(missing_schema(
            "unknown semantic schema namespace"));
    }
    const auto& revisions = found->second;
    const auto position = std::lower_bound(
        revisions.begin(), revisions.end(), version,
        [](const SchemaRevision& existing, SchemaVersion wanted) {
            return existing.version < wanted;
        });
    if (position == revisions.end() || position->version != version) {
        return core::Result<const SchemaRevision*>::failure(missing_schema(
            "unknown semantic schema version"));
    }
    return core::Result<const SchemaRevision*>::success(&*position);
}

bool SchemaRegistry::contains(
    const SchemaIdentity& schema, SchemaVersion version) const noexcept {
    return static_cast<bool>(find(schema, version));
}

core::Result<SchemaVersion> SchemaRegistry::latest_registered(
    const SchemaIdentity& schema) const noexcept {
    const auto found = revisions_.find(schema);
    if (found == revisions_.end() || found->second.empty()) {
        return core::Result<SchemaVersion>::failure(missing_schema(
            "unknown semantic schema namespace"));
    }
    return core::Result<SchemaVersion>::success(found->second.back().version);
}

core::Result<void> SchemaRegistry::reconcile(
    const SemanticExtensionRecord& record) const {
    if (auto result = record.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "semantic extension reconciliation"));
    }
    const auto revision = find(record.schema, SchemaVersion{record.schema_version});
    if (!revision) {
        return core::Result<void>::failure(revision.error().with_context(
            "semantic extension reconciliation"));
    }
    return core::Result<void>::success();
}

core::Result<void> SchemaRegistry::compatible(
    const SchemaIdentity& schema, SchemaVersion from, SchemaVersion to) const {
    if (from > to) {
        return core::Result<void>::failure(validation(
            "schema compatibility cannot move backwards across versions"));
    }
    const auto from_result = find(schema, from);
    if (!from_result) {
        return core::Result<void>::failure(from_result.error().with_context(
            "schema compatibility"));
    }
    const auto to_result = find(schema, to);
    if (!to_result) {
        return core::Result<void>::failure(to_result.error().with_context(
            "schema compatibility"));
    }
    const SchemaRevision* from_revision = from_result.value();
    const SchemaRevision* to_revision = to_result.value();
    for (const auto& field : from_revision->fields) {
        if (field.ownership != FieldOwnership::game_owned) continue;
        const auto position = std::lower_bound(
            to_revision->fields.begin(), to_revision->fields.end(), field.name,
            [](const SchemaField& existing, const std::string& name) {
                return existing.name < name;
            });
        const bool preserved = position != to_revision->fields.end() &&
            position->name == field.name && *position == field;
        if (!preserved) {
            return core::Result<void>::failure(validation(
                "schema compatibility dropped or re-owned a game-owned field"));
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::semantic
