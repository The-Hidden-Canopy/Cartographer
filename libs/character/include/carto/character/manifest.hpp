#pragma once

#include <carto/character/coordinate.hpp>
#include <carto/character/ids.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::character {

// Portable product path admission: relative POSIX-style paths only. Absolute
// paths, ".." segments, backslashes, empty components and control bytes are
// rejected so a manifest can never address outside its package root.
[[nodiscard]] bool is_safe_product_path(std::string_view path) noexcept;

struct ManifestSource {
    std::string project_namespace;
    std::uint64_t revision = 0U;
    assets::Sha256Digest project_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

// Interchange coordinate summary in the reduced envelope form (handedness,
// up/forward axes, meters per unit). The full convention incl. winding,
// rotation representation and inverse-bind space lives in CoordinatePolicy;
// this summary is what a target profile admits against.
struct ManifestCoordinate {
    Handedness handedness = Handedness::right;
    Axis up = Axis::positive_y;
    Axis forward = Axis::positive_z;
    double meters_per_unit = 1.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const ManifestCoordinate&) const noexcept = default;
};

struct TargetProfileRef {
    std::string id;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ManifestProduct {
    std::string kind;
    std::string path;
    std::uint32_t schema = 0U;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const ManifestProduct&) const noexcept = default;
    [[nodiscard]] bool operator<(const ManifestProduct& other) const noexcept {
        return path < other.path;
    }
};

// The prepared character manifest: the exact closure envelope a target
// profile admits against. Serialization is a readable deterministic JSON
// reference codec in the Chapter 04 envelope shape, with fixed member order,
// exact 64-char lowercase hex digests, sorted capability and product tables,
// and no float formatting beyond what std::setprecision(17) guarantees.
// Determinism is a hard acceptance gate: equal manifests serialize to equal
// bytes, and equal bytes digest equally.
class CharacterPreparedManifest final {
public:
    static constexpr std::string_view kFormat = "carto-character-prepared";
    static constexpr std::uint32_t kSchemaMajor = 1U;
    static constexpr std::uint32_t kSchemaMinor = 0U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;
    static constexpr std::size_t kMaxNamespaceBytes = 256U;
    static constexpr std::size_t kMaxProfileIdBytes = 256U;
    static constexpr std::size_t kMaxCapabilities = 256U;
    static constexpr std::size_t kMaxCapabilityBytes = 128U;
    static constexpr std::size_t kMaxProducts = 4'096U;
    static constexpr std::size_t kMaxProductKindBytes = 128U;
    static constexpr std::size_t kMaxProductPathBytes = 1'024U;

    std::uint32_t schema_major = kSchemaMajor;
    std::uint32_t schema_minor = kSchemaMinor;
    ManifestSource source;
    ManifestCoordinate coordinate;
    CharacterId character_id;
    TargetProfileRef target_profile;
    // Sorted and unique capability strings.
    std::vector<std::string> required_capabilities;
    // Sorted and unique by product path; paths must be safe and distinct.
    std::vector<ManifestProduct> products;

    [[nodiscard]] core::Result<void> validate() const;

    // Closure verification: every declared product's payload must be present
    // with the exact declared digest, and no extra payloads are admitted.
    [[nodiscard]] core::Result<void> verify_closure(
        const std::map<std::string, assets::Sha256Digest>& payload_digests) const;

    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<CharacterPreparedManifest> deserialize(
        std::string_view text);
};

} // namespace carto::character
