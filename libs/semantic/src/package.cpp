#include <carto/semantic/package.hpp>

#include <algorithm>
#include <charconv>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

namespace carto::semantic {
namespace {

constexpr std::string_view kPackageMagic = "CARTOGRAPHER_EXTENSION_PACKAGE";
constexpr std::string_view kCacheKeyMagic = "CARTOGRAPHER_EXTENSION_CACHE_KEY";

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

assets::Sha256Digest digest_of(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
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

auto revision_key(const SchemaRevision& revision) {
    return std::tie(
        revision.schema.name_space, revision.schema.name, revision.version.value);
}

// Depth-first cycle check over the dependency graph whose vertices are schema
// identities and whose edges run from every provided schema revision to every
// required dependency schema, including self-edges: a package that requires
// its own provided schema is a one-node cycle.
bool dependency_graph_has_cycle(
    const std::vector<SchemaRevision>& revisions,
    const std::vector<ExtensionDependency>& dependencies) {
    std::map<SchemaIdentity, std::vector<SchemaIdentity>> edges;
    for (const auto& revision : revisions) {
        auto& targets = edges[revision.schema];
        for (const auto& dependency : dependencies) {
            targets.push_back(dependency.schema);
        }
    }
    enum class VisitState : std::uint8_t { unvisited, in_stack, done };
    std::map<SchemaIdentity, VisitState> states;
    for (const auto& revision : revisions) states[revision.schema] = VisitState::unvisited;
    for (const auto& dependency : dependencies) {
        states.emplace(dependency.schema, VisitState::unvisited);
    }
    const std::function<bool(const SchemaIdentity&)> depth_first =
        [&](const SchemaIdentity& vertex) {
            states[vertex] = VisitState::in_stack;
            const auto found = edges.find(vertex);
            if (found != edges.end()) {
                for (const auto& next : found->second) {
                    const auto state = states[next];
                    if (state == VisitState::in_stack) return true;
                    if (state == VisitState::unvisited && depth_first(next)) return true;
                }
            }
            states[vertex] = VisitState::done;
            return false;
        };
    for (const auto& [identity, state] : states) {
        if (state == VisitState::unvisited && depth_first(identity)) return true;
    }
    return false;
}

} // namespace

core::Result<void> ExtensionDependency::validate() const {
    if (auto result = schema.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "extension dependency"));
    }
    if (version.value == 0U) {
        return core::Result<void>::failure(validation(
            "extension dependency version must be non-zero"));
    }
    return core::Result<void>::success();
}

core::Result<void> ExtensionPackageManifest::validate() const {
    if (schema_revisions.size() > kMaxRevisionsPerPackage ||
        payload_digests.size() > kMaxPayloadDigestsPerPackage ||
        dependencies.size() > kMaxDependenciesPerPackage) {
        return core::Result<void>::failure(validation(
            "extension package manifest exceeds its bounded capacity"));
    }
    for (const auto& revision : schema_revisions) {
        if (auto result = revision.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "extension package schema revision"));
        }
    }
    if (!std::is_sorted(schema_revisions.begin(), schema_revisions.end(),
            [](const SchemaRevision& left, const SchemaRevision& right) {
                return revision_key(left) < revision_key(right);
            }) ||
        std::adjacent_find(schema_revisions.begin(), schema_revisions.end(),
            [](const SchemaRevision& left, const SchemaRevision& right) {
                return revision_key(left) == revision_key(right);
            }) != schema_revisions.end()) {
        return core::Result<void>::failure(validation(
            "extension package schema revisions must be sorted and unique"));
    }
    for (const auto& payload_digest : payload_digests) {
        if (payload_digest.is_zero()) {
            return core::Result<void>::failure(validation(
                "extension package payload digest must not be zero"));
        }
    }
    if (!std::is_sorted(payload_digests.begin(), payload_digests.end()) ||
        std::adjacent_find(payload_digests.begin(), payload_digests.end()) !=
            payload_digests.end()) {
        return core::Result<void>::failure(validation(
            "extension package payload digests must be sorted and unique"));
    }
    for (const auto& dependency : dependencies) {
        if (auto result = dependency.validate(); !result) return result;
    }
    if (!std::is_sorted(dependencies.begin(), dependencies.end()) ||
        std::adjacent_find(dependencies.begin(), dependencies.end()) !=
            dependencies.end()) {
        return core::Result<void>::failure(validation(
            "extension package dependencies must be sorted and unique"));
    }
    if (dependency_graph_has_cycle(schema_revisions, dependencies)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::cycle_detected,
            "extension package dependency closure contains a cycle"));
    }
    return core::Result<void>::success();
}

std::string ExtensionPackageManifest::serialize() const {
    std::ostringstream output;
    output << kPackageMagic << ' ' << kSchemaVersion << '\n';
    output << "REVISIONS " << schema_revisions.size() << '\n';
    for (const auto& revision : schema_revisions) {
        output << "REVISION " << std::quoted(revision.serialize()) << '\n';
    }
    output << "PAYLOAD_DIGESTS " << payload_digests.size() << '\n';
    for (const auto& payload_digest : payload_digests) {
        output << "PAYLOAD_DIGEST " << payload_digest.hex() << '\n';
    }
    output << "DEPENDENCIES " << dependencies.size() << '\n';
    for (const auto& dependency : dependencies) {
        output << "DEPENDENCY " << std::quoted(dependency.schema.name_space) << ' '
               << std::quoted(dependency.schema.name) << ' ' << dependency.version.value
               << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<ExtensionPackageManifest> ExtensionPackageManifest::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<ExtensionPackageManifest>::failure(parse_error(
            "extension package manifest text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || magic != kPackageMagic || !read_uint(input, version) ||
        version != kSchemaVersion) {
        return core::Result<ExtensionPackageManifest>::failure(parse_error(
            "extension package manifest framing or schema version is invalid"));
    }
    ExtensionPackageManifest manifest;
    std::size_t revision_count = 0U;
    if (!require_token(input, "REVISIONS") ||
        !read_count(input, ExtensionPackageManifest::kMaxRevisionsPerPackage,
            revision_count)) {
        return core::Result<ExtensionPackageManifest>::failure(parse_error(
            "extension package manifest revision header is malformed"));
    }
    manifest.schema_revisions.reserve(revision_count);
    for (std::size_t index = 0U; index < revision_count; ++index) {
        std::string encoded_revision;
        if (!require_token(input, "REVISION") ||
            !(input >> std::quoted(encoded_revision))) {
            return core::Result<ExtensionPackageManifest>::failure(parse_error(
                "extension package manifest revision is malformed"));
        }
        auto revision = SchemaRevision::deserialize(encoded_revision);
        if (!revision) {
            return core::Result<ExtensionPackageManifest>::failure(
                revision.error().with_context("extension package manifest revision"));
        }
        manifest.schema_revisions.push_back(std::move(revision.value()));
    }
    std::size_t payload_count = 0U;
    if (!require_token(input, "PAYLOAD_DIGESTS") ||
        !read_count(input, ExtensionPackageManifest::kMaxPayloadDigestsPerPackage,
            payload_count)) {
        return core::Result<ExtensionPackageManifest>::failure(parse_error(
            "extension package manifest payload header is malformed"));
    }
    manifest.payload_digests.reserve(payload_count);
    for (std::size_t index = 0U; index < payload_count; ++index) {
        assets::Sha256Digest payload_digest;
        if (!require_token(input, "PAYLOAD_DIGEST") || !read_digest(input, payload_digest)) {
            return core::Result<ExtensionPackageManifest>::failure(parse_error(
                "extension package manifest payload digest is malformed"));
        }
        manifest.payload_digests.push_back(payload_digest);
    }
    std::size_t dependency_count = 0U;
    if (!require_token(input, "DEPENDENCIES") ||
        !read_count(input, ExtensionPackageManifest::kMaxDependenciesPerPackage,
            dependency_count)) {
        return core::Result<ExtensionPackageManifest>::failure(parse_error(
            "extension package manifest dependency header is malformed"));
    }
    manifest.dependencies.reserve(dependency_count);
    for (std::size_t index = 0U; index < dependency_count; ++index) {
        ExtensionDependency dependency;
        std::uint64_t encoded_version = 0U;
        if (!require_token(input, "DEPENDENCY") ||
            !(input >> std::quoted(dependency.schema.name_space) >>
              std::quoted(dependency.schema.name)) ||
            !read_uint(input, encoded_version)) {
            return core::Result<ExtensionPackageManifest>::failure(parse_error(
                "extension package manifest dependency is malformed"));
        }
        dependency.version = SchemaVersion{static_cast<std::uint32_t>(encoded_version)};
        manifest.dependencies.push_back(std::move(dependency));
    }
    if (!require_token(input, "END")) {
        return core::Result<ExtensionPackageManifest>::failure(parse_error(
            "extension package manifest is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ExtensionPackageManifest>::failure(parse_error(
            "extension package manifest contains trailing unbound data"));
    }
    std::sort(manifest.schema_revisions.begin(), manifest.schema_revisions.end(),
        [](const SchemaRevision& left, const SchemaRevision& right) {
            return revision_key(left) < revision_key(right);
        });
    std::sort(manifest.payload_digests.begin(), manifest.payload_digests.end());
    std::sort(manifest.dependencies.begin(), manifest.dependencies.end());
    if (auto result = manifest.validate(); !result) {
        return core::Result<ExtensionPackageManifest>::failure(result.error());
    }
    return core::Result<ExtensionPackageManifest>::success(std::move(manifest));
}

core::Result<assets::Sha256Digest> ExtensionPackageManifest::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

core::Result<void> ExtensionCacheKey::validate() const {
    if (auto result = schema.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "extension cache key"));
    }
    if (version.value == 0U) {
        return core::Result<void>::failure(validation(
            "extension cache key version must be non-zero"));
    }
    if (manifest_digest.is_zero() || sidecar_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "extension cache key digests must not be zero"));
    }
    return core::Result<void>::success();
}

std::string ExtensionCacheKey::serialize() const {
    std::ostringstream output;
    output << kCacheKeyMagic << ' ' << 1U << '\n';
    output << "SCHEMA " << std::quoted(schema.name_space) << ' '
           << std::quoted(schema.name) << '\n';
    output << "VERSION " << version.value << '\n';
    output << "MANIFEST_DIGEST " << manifest_digest.hex() << '\n';
    output << "SIDECAR_DIGEST " << sidecar_digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<ExtensionCacheKey> ExtensionCacheKey::deserialize(std::string_view text) {
    if (text.empty() || text.size() > SemanticSidecar::kMaxSerializedBytes) {
        return core::Result<ExtensionCacheKey>::failure(parse_error(
            "extension cache key text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || magic != kCacheKeyMagic || !read_uint(input, version) ||
        version != 1U) {
        return core::Result<ExtensionCacheKey>::failure(parse_error(
            "extension cache key framing or version is invalid"));
    }
    ExtensionCacheKey key;
    std::uint64_t encoded_version = 0U;
    if (!require_token(input, "SCHEMA") ||
        !(input >> std::quoted(key.schema.name_space) >>
          std::quoted(key.schema.name)) ||
        !require_token(input, "VERSION") ||
        !read_uint(input, encoded_version) ||
        !require_token(input, "MANIFEST_DIGEST") ||
        !read_digest(input, key.manifest_digest) ||
        !require_token(input, "SIDECAR_DIGEST") ||
        !read_digest(input, key.sidecar_digest) ||
        !require_token(input, "END")) {
        return core::Result<ExtensionCacheKey>::failure(parse_error(
            "extension cache key body is malformed"));
    }
    key.version = SchemaVersion{static_cast<std::uint32_t>(encoded_version)};
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ExtensionCacheKey>::failure(parse_error(
            "extension cache key contains trailing unbound data"));
    }
    if (auto result = key.validate(); !result) {
        return core::Result<ExtensionCacheKey>::failure(result.error());
    }
    return core::Result<ExtensionCacheKey>::success(std::move(key));
}

core::Result<assets::Sha256Digest> ExtensionCacheKey::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

} // namespace carto::semantic
