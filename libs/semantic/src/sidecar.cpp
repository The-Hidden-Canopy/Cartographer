#include <carto/semantic/sidecar.hpp>

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

constexpr std::string_view kSidecarMagic = "CARTOGRAPHER_SEMANTIC_SIDECAR";
constexpr std::string_view kRecordMagic = "CARTOGRAPHER_SEMANTIC_EXTENSION_RECORD";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

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

auto binding_key(const world::AuthoringBinding& binding) {
    return std::make_tuple(
        static_cast<std::uint8_t>(binding.kind), binding.identity);
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

bool read_count(
    std::istream& input, std::uint64_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_uint(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool read_binding(std::istream& input, world::AuthoringBinding& binding) {
    std::uint64_t kind = 0U;
    std::uint64_t identity = 0U;
    if (!read_uint(input, kind) || !read_uint(input, identity) ||
        kind > static_cast<std::uint64_t>(world::AuthoringBindingKind::mesh_asset)) {
        return false;
    }
    binding.kind = static_cast<world::AuthoringBindingKind>(kind);
    binding.identity = identity;
    return true;
}

core::Result<void> validate_binding_list(
    const std::vector<world::AuthoringBinding>& bindings,
    std::size_t maximum,
    std::string_view label) {
    if (bindings.size() > maximum) {
        return core::Result<void>::failure(validation(
            std::string(label) + " exceeds its bounded capacity"));
    }
    for (const auto& binding : bindings) {
        if (auto result = binding.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                std::string(label) + " entry"));
        }
    }
    if (!std::is_sorted(bindings.begin(), bindings.end(),
            [](const world::AuthoringBinding& left, const world::AuthoringBinding& right) {
                return binding_key(left) < binding_key(right);
            }) ||
        std::adjacent_find(bindings.begin(), bindings.end(),
            [](const world::AuthoringBinding& left, const world::AuthoringBinding& right) {
                return binding_key(left) == binding_key(right);
            }) != bindings.end()) {
        return core::Result<void>::failure(validation(
            std::string(label) + " must be sorted and unique"));
    }
    return core::Result<void>::success();
}

} // namespace

std::string_view field_ownership_name(FieldOwnership ownership) noexcept {
    switch (ownership) {
    case FieldOwnership::native_lowerable: return "native-lowerable";
    case FieldOwnership::game_owned: return "game-owned";
    }
    return "unknown";
}

core::Result<void> SchemaIdentity::validate() const {
    if (!safe_text(name_space, SemanticExtensionRecord::kMaxNamespaceBytes) ||
        !safe_text(name, SemanticExtensionRecord::kMaxNameBytes)) {
        return core::Result<void>::failure(invalid(
            "semantic schema identity namespace or name is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> SemanticExtensionRecord::validate() const {
    if (auto result = schema.validate(); !result) return result;
    if (schema_version == 0U) {
        return core::Result<void>::failure(validation(
            "semantic extension record schema version must be non-zero"));
    }
    if (!safe_text(payload, kMaxPayloadBytes)) {
        return core::Result<void>::failure(invalid(
            "semantic extension record payload is empty, unsafe, or too large"));
    }
    if (payload_digest.is_zero() || digest_of(payload) != payload_digest) {
        return core::Result<void>::failure(validation(
            "semantic extension record payload digest does not bind its payload"));
    }
    return validate_binding_list(bound_identities, kMaxBoundIdentities, "bound identities");
}

std::string SemanticExtensionRecord::serialize() const {
    std::ostringstream output;
    output << kRecordMagic << ' ' << kSchemaVersion << '\n';
    output << "SCHEMA " << std::quoted(schema.name_space) << ' '
           << std::quoted(schema.name) << '\n';
    output << "VERSION " << schema_version << '\n';
    output << "PAYLOAD_DIGEST " << payload_digest.hex() << '\n';
    output << "PAYLOAD " << std::quoted(payload) << '\n';
    output << "BOUND_IDENTITIES " << bound_identities.size() << '\n';
    for (const auto& binding : bound_identities) {
        output << "BOUND_IDENTITY " << static_cast<std::uint64_t>(binding.kind) << ' '
               << binding.identity << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<SemanticExtensionRecord> SemanticExtensionRecord::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > SemanticSidecar::kMaxSerializedBytes) {
        return core::Result<SemanticExtensionRecord>::failure(parse_error(
            "semantic extension record text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || magic != kRecordMagic || !read_uint(input, version) ||
        version != kSchemaVersion) {
        return core::Result<SemanticExtensionRecord>::failure(parse_error(
            "semantic extension record framing or schema version is invalid"));
    }
    SemanticExtensionRecord record;
    std::uint64_t encoded_version = 0U;
    std::size_t bound_count = 0U;
    if (!require_token(input, "SCHEMA") ||
        !(input >> std::quoted(record.schema.name_space) >>
          std::quoted(record.schema.name)) ||
        !require_token(input, "VERSION") ||
        !read_uint(input, encoded_version) ||
        !require_token(input, "PAYLOAD_DIGEST") ||
        !read_digest(input, record.payload_digest) ||
        !require_token(input, "PAYLOAD") ||
        !(input >> std::quoted(record.payload))) {
        return core::Result<SemanticExtensionRecord>::failure(parse_error(
            "semantic extension record body is malformed"));
    }
    record.schema_version = static_cast<std::uint32_t>(encoded_version);
    if (!require_token(input, "BOUND_IDENTITIES") ||
        !read_count(input, SemanticExtensionRecord::kMaxBoundIdentities, bound_count)) {
        return core::Result<SemanticExtensionRecord>::failure(parse_error(
            "semantic extension record bound-identity header is malformed"));
    }
    record.bound_identities.reserve(bound_count);
    for (std::size_t index = 0U; index < bound_count; ++index) {
        world::AuthoringBinding binding;
        if (!require_token(input, "BOUND_IDENTITY") || !read_binding(input, binding)) {
            return core::Result<SemanticExtensionRecord>::failure(parse_error(
                "semantic extension record bound identity is malformed"));
        }
        record.bound_identities.push_back(binding);
    }
    if (!require_token(input, "END")) {
        return core::Result<SemanticExtensionRecord>::failure(parse_error(
            "semantic extension record is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<SemanticExtensionRecord>::failure(parse_error(
            "semantic extension record contains trailing unbound data"));
    }
    if (auto result = record.validate(); !result) {
        return core::Result<SemanticExtensionRecord>::failure(result.error());
    }
    return core::Result<SemanticExtensionRecord>::success(std::move(record));
}

core::Result<assets::Sha256Digest> SemanticExtensionRecord::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

core::Result<void> OwnershipManifest::validate() const {
    if (auto result = bound_identity.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "ownership manifest bound identity"));
    }
    if (entries.size() > kMaxFieldsPerManifest) {
        return core::Result<void>::failure(validation(
            "ownership manifest exceeds its bounded field capacity"));
    }
    for (const auto& entry : entries) {
        if (!safe_text(entry.field, kMaxFieldNameBytes)) {
            return core::Result<void>::failure(invalid(
                "ownership manifest field name is invalid"));
        }
        if (static_cast<std::uint8_t>(entry.ownership) >
            static_cast<std::uint8_t>(FieldOwnership::game_owned)) {
            return core::Result<void>::failure(validation(
                "ownership manifest field ownership is invalid"));
        }
    }
    if (!std::is_sorted(entries.begin(), entries.end(),
            [](const FieldOwnershipEntry& left, const FieldOwnershipEntry& right) {
                return left.field < right.field;
            }) ||
        std::adjacent_find(entries.begin(), entries.end(),
            [](const FieldOwnershipEntry& left, const FieldOwnershipEntry& right) {
                return left.field == right.field;
            }) != entries.end()) {
        return core::Result<void>::failure(validation(
            "ownership manifest entries must be sorted and unique by field"));
    }
    return core::Result<void>::success();
}

core::Result<void> SemanticSidecar::insert_record(SemanticExtensionRecord record) {
    if (records_.size() >= kMaxRecordsPerSidecar) {
        return core::Result<void>::failure(validation(
            "semantic sidecar record collection has reached its bounded capacity"));
    }
    if (auto result = record.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "semantic sidecar record admission"));
    }
    const auto position = std::lower_bound(
        records_.begin(), records_.end(), record.schema,
        [](const SemanticExtensionRecord& existing, const SchemaIdentity& identity) {
            return existing.schema < identity;
        });
    if (position != records_.end() && position->schema == record.schema) {
        return core::Result<void>::failure(validation(
            "semantic sidecar already contains this schema record"));
    }
    records_.insert(position, std::move(record));
    return core::Result<void>::success();
}

core::Result<void> SemanticSidecar::insert_manifest(OwnershipManifest manifest) {
    if (manifests_.size() >= kMaxManifestsPerSidecar) {
        return core::Result<void>::failure(validation(
            "semantic sidecar manifest collection has reached its bounded capacity"));
    }
    if (auto result = manifest.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "semantic sidecar manifest admission"));
    }
    const auto key = binding_key(manifest.bound_identity);
    const auto position = std::lower_bound(
        manifests_.begin(), manifests_.end(), key,
        [](const OwnershipManifest& existing, const auto& wanted) {
            return binding_key(existing.bound_identity) < wanted;
        });
    if (position != manifests_.end() &&
        binding_key(position->bound_identity) == key) {
        return core::Result<void>::failure(validation(
            "semantic sidecar already contains this bound-identity manifest"));
    }
    manifests_.insert(position, std::move(manifest));
    return core::Result<void>::success();
}

core::Result<void> SemanticSidecar::validate() const {
    if (records_.size() > kMaxRecordsPerSidecar ||
        manifests_.size() > kMaxManifestsPerSidecar) {
        return core::Result<void>::failure(validation(
            "semantic sidecar exceeds its bounded collection capacity"));
    }
    if (!std::is_sorted(records_.begin(), records_.end(),
            [](const SemanticExtensionRecord& left, const SemanticExtensionRecord& right) {
                return left.schema < right.schema;
            }) ||
        std::adjacent_find(records_.begin(), records_.end(),
            [](const SemanticExtensionRecord& left, const SemanticExtensionRecord& right) {
                return left.schema == right.schema;
            }) != records_.end()) {
        return core::Result<void>::failure(validation(
            "semantic sidecar records must be sorted and unique by schema identity"));
    }
    for (const auto& record : records_) {
        if (auto result = record.validate(); !result) return result;
    }
    if (!std::is_sorted(manifests_.begin(), manifests_.end(),
            [](const OwnershipManifest& left, const OwnershipManifest& right) {
                return binding_key(left.bound_identity) < binding_key(right.bound_identity);
            }) ||
        std::adjacent_find(manifests_.begin(), manifests_.end(),
            [](const OwnershipManifest& left, const OwnershipManifest& right) {
                return binding_key(left.bound_identity) == binding_key(right.bound_identity);
            }) != manifests_.end()) {
        return core::Result<void>::failure(validation(
            "semantic sidecar manifests must be sorted and unique by bound identity"));
    }
    for (const auto& manifest : manifests_) {
        if (auto result = manifest.validate(); !result) return result;
    }
    if (digest_.is_zero() || digest_ != digest_of(serialize())) {
        return core::Result<void>::failure(validation(
            "semantic sidecar digest does not bind its exact record set"));
    }
    return core::Result<void>::success();
}

std::string SemanticSidecar::serialize_body() const {
    std::ostringstream output;
    output << kSidecarMagic << ' ' << kSchemaVersion << '\n';
    output << "RECORDS " << records_.size() << '\n';
    for (const auto& record : records_) {
        output << "RECORD " << std::quoted(record.schema.name_space) << ' '
               << std::quoted(record.schema.name) << ' ' << record.schema_version << ' '
               << record.payload_digest.hex() << ' ' << std::quoted(record.payload) << ' '
               << record.bound_identities.size();
        for (const auto& binding : record.bound_identities) {
            output << ' ' << static_cast<std::uint64_t>(binding.kind) << ' '
                   << binding.identity;
        }
        output << '\n';
    }
    output << "MANIFESTS " << manifests_.size() << '\n';
    for (const auto& manifest : manifests_) {
        output << "MANIFEST " << static_cast<std::uint64_t>(manifest.bound_identity.kind)
               << ' ' << manifest.bound_identity.identity << ' ' << manifest.entries.size();
        for (const auto& entry : manifest.entries) {
            output << ' ' << std::quoted(entry.field) << ' '
                   << static_cast<std::uint64_t>(entry.ownership);
        }
        output << '\n';
    }
    return output.str();
}

core::Result<void> SemanticSidecar::refresh_digest() {
    const std::string canonical = serialize();
    if (canonical.size() > kMaxSerializedBytes) {
        return core::Result<void>::failure(validation(
            "semantic sidecar exceeds its serialized byte bound"));
    }
    digest_ = digest_of(canonical);
    return core::Result<void>::success();
}

std::string SemanticSidecar::serialize() const {
    std::ostringstream output;
    output << serialize_body();
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> SemanticSidecar::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_);
}

core::Result<SemanticSidecar> SemanticSidecar::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<SemanticSidecar>::failure(parse_error(
            "semantic sidecar text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || magic != kSidecarMagic || !read_uint(input, version) ||
        version != kSchemaVersion) {
        return core::Result<SemanticSidecar>::failure(parse_error(
            "semantic sidecar framing or schema version is invalid"));
    }
    SemanticSidecar sidecar;
    std::size_t record_count = 0U;
    if (!require_token(input, "RECORDS") ||
        !read_count(input, kMaxRecordsPerSidecar, record_count)) {
        return core::Result<SemanticSidecar>::failure(parse_error(
            "semantic sidecar record header is malformed"));
    }
    for (std::size_t index = 0U; index < record_count; ++index) {
        SemanticExtensionRecord record;
        std::uint64_t encoded_version = 0U;
        std::size_t bound_count = 0U;
        if (!require_token(input, "RECORD") ||
            !(input >> std::quoted(record.schema.name_space) >>
              std::quoted(record.schema.name)) ||
            !read_uint(input, encoded_version) ||
            !read_digest(input, record.payload_digest) ||
            !(input >> std::quoted(record.payload)) ||
            !read_count(input, SemanticExtensionRecord::kMaxBoundIdentities, bound_count)) {
            return core::Result<SemanticSidecar>::failure(parse_error(
                "semantic sidecar record is malformed"));
        }
        record.schema_version = static_cast<std::uint32_t>(encoded_version);
        record.bound_identities.reserve(bound_count);
        for (std::size_t bound_index = 0U; bound_index < bound_count; ++bound_index) {
            world::AuthoringBinding binding;
            if (!read_binding(input, binding)) {
                return core::Result<SemanticSidecar>::failure(parse_error(
                    "semantic sidecar bound identity is malformed"));
            }
            record.bound_identities.push_back(binding);
        }
        if (auto result = sidecar.insert_record(std::move(record)); !result) {
            return core::Result<SemanticSidecar>::failure(result.error());
        }
    }
    std::size_t manifest_count = 0U;
    if (!require_token(input, "MANIFESTS") ||
        !read_count(input, kMaxManifestsPerSidecar, manifest_count)) {
        return core::Result<SemanticSidecar>::failure(parse_error(
            "semantic sidecar manifest header is malformed"));
    }
    for (std::size_t index = 0U; index < manifest_count; ++index) {
        OwnershipManifest manifest;
        std::uint64_t kind = 0U;
        std::size_t entry_count = 0U;
        if (!require_token(input, "MANIFEST") || !read_uint(input, kind) ||
            !read_uint(input, manifest.bound_identity.identity) ||
            !read_count(input, OwnershipManifest::kMaxFieldsPerManifest, entry_count) ||
            kind > static_cast<std::uint64_t>(world::AuthoringBindingKind::mesh_asset)) {
            return core::Result<SemanticSidecar>::failure(parse_error(
                "semantic sidecar manifest is malformed"));
        }
        manifest.bound_identity.kind = static_cast<world::AuthoringBindingKind>(kind);
        manifest.entries.reserve(entry_count);
        for (std::size_t entry_index = 0U; entry_index < entry_count; ++entry_index) {
            FieldOwnershipEntry entry;
            std::uint64_t ownership = 0U;
            if (!(input >> std::quoted(entry.field)) || !read_uint(input, ownership) ||
                ownership > static_cast<std::uint64_t>(FieldOwnership::game_owned)) {
                return core::Result<SemanticSidecar>::failure(parse_error(
                    "semantic sidecar manifest entry is malformed"));
            }
            entry.ownership = static_cast<FieldOwnership>(ownership);
            manifest.entries.push_back(std::move(entry));
        }
        if (auto result = sidecar.insert_manifest(std::move(manifest)); !result) {
            return core::Result<SemanticSidecar>::failure(result.error());
        }
    }
    if (!require_token(input, "END")) {
        return core::Result<SemanticSidecar>::failure(parse_error(
            "semantic sidecar is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<SemanticSidecar>::failure(parse_error(
            "semantic sidecar contains trailing unbound data"));
    }
    // The digest is never stored inside the bytes: reopening recomputes it
    // fail-closed over the exact canonical serialization that was parsed.
    sidecar.digest_ = digest_of(sidecar.serialize());
    if (auto result = sidecar.validate(); !result) {
        return core::Result<SemanticSidecar>::failure(result.error());
    }
    return core::Result<SemanticSidecar>::success(std::move(sidecar));
}

} // namespace carto::semantic
