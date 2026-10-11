#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/semantic/schema.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::semantic {

// One dependency edge of an extension package: the exact registered schema
// revision the package requires.
struct ExtensionDependency {
    SchemaIdentity schema;
    SchemaVersion version;

    [[nodiscard]] auto operator<=>(const ExtensionDependency&) const noexcept = default;
    [[nodiscard]] core::Result<void> validate() const;
};

// Neutral package manifest for one compiled extension package. It declares
// the schema revisions it was compiled against, the payload digests it
// carries, and its full dependency closure. Dependency cycles are rejected:
// an extension may never transitively require itself.
struct ExtensionPackageManifest {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxRevisionsPerPackage = 4096U;
    static constexpr std::size_t kMaxPayloadDigestsPerPackage = 65'536U;
    static constexpr std::size_t kMaxDependenciesPerPackage = 4096U;

    std::vector<SchemaRevision> schema_revisions;
    std::vector<assets::Sha256Digest> payload_digests;
    std::vector<ExtensionDependency> dependencies;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<ExtensionPackageManifest> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Content-addressed cache key for compiled extension products. Hot reload
// admits a rebuilt product only when every input identity that shaped it is
// unchanged; any drift produces a different key and a cold admission.
struct ExtensionCacheKey {
    SchemaIdentity schema;
    SchemaVersion version;
    assets::Sha256Digest manifest_digest;
    assets::Sha256Digest sidecar_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<ExtensionCacheKey> deserialize(std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

} // namespace carto::semantic
