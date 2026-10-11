#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/world/model.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::semantic {

// Field ownership separates what Cartographer may lower natively from what
// remains game/domain truth. Ownership is declared per field and never
// inferred: native-lowerable fields may become neutral prepared evidence,
// while game-owned fields are fenced out of Cartographer mutation authority
// and must survive every migration untouched.
enum class FieldOwnership : std::uint8_t {
    native_lowerable,
    game_owned,
};

[[nodiscard]] std::string_view field_ownership_name(FieldOwnership ownership) noexcept;

// Stable namespaced identity of one game/domain semantic schema. The member
// is named `name_space` because `namespace` is a language keyword; it is the
// donor schema's authority segment, not a filesystem path or runtime handle.
struct SchemaIdentity {
    std::string name_space;
    std::string name;

    [[nodiscard]] auto operator<=>(const SchemaIdentity&) const noexcept = default;
    [[nodiscard]] core::Result<void> validate() const;
};

// One neutral, versioned, digest-bound extension record. The payload is an
// opaque canonical string owned by the donor schema; Cartographer stores,
// digests, and binds it to authored identities but never parses it.
struct SemanticExtensionRecord {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxNamespaceBytes = 256U;
    static constexpr std::size_t kMaxNameBytes = 256U;
    static constexpr std::size_t kMaxPayloadBytes = 1024U * 1024U;
    static constexpr std::size_t kMaxBoundIdentities = 1024U;

    SchemaIdentity schema;
    std::uint32_t schema_version = 0U;
    std::string payload;
    assets::Sha256Digest payload_digest;
    // Provider-neutral references to authoritative Cartographer authoring
    // state. The vector must be sorted and unique so its canonical form is
    // unambiguous.
    std::vector<world::AuthoringBinding> bound_identities;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<SemanticExtensionRecord> static deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct FieldOwnershipEntry {
    std::string field;
    FieldOwnership ownership = FieldOwnership::game_owned;

    [[nodiscard]] bool operator==(const FieldOwnershipEntry&) const noexcept = default;
};

// Per-bound-identity ownership declaration: which fields of the bound record
// are native-lowerable versus game-owned for this exact authored identity.
// Entries must be sorted by field name and unique.
struct OwnershipManifest {
    static constexpr std::size_t kMaxFieldNameBytes = 256U;
    static constexpr std::size_t kMaxFieldsPerManifest = 4096U;

    world::AuthoringBinding bound_identity;
    std::vector<FieldOwnershipEntry> entries;

    [[nodiscard]] core::Result<void> validate() const;
};

// Authoritative neutral container for semantic extension records. Cartographer
// owns durability, identity, and digest binding; donor schemas own payload
// meaning; games own runtime state. The digest member binds the exact record
// set and manifests; it is never stored inside the serialized bytes and is
// recomputed fail-closed on reopen.
class SemanticSidecar final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxRecordsPerSidecar = 65'536U;
    static constexpr std::size_t kMaxManifestsPerSidecar = 65'536U;

    [[nodiscard]] core::Result<void> insert_record(SemanticExtensionRecord record);
    [[nodiscard]] core::Result<void> insert_manifest(OwnershipManifest manifest);

    [[nodiscard]] const std::vector<SemanticExtensionRecord>& records() const noexcept {
        return records_;
    }
    [[nodiscard]] const std::vector<OwnershipManifest>& manifests() const noexcept {
        return manifests_;
    }
    [[nodiscard]] const assets::Sha256Digest& digest() const noexcept { return digest_; }

    [[nodiscard]] bool empty() const noexcept {
        return records_.empty() && manifests_.empty();
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<SemanticSidecar> deserialize(std::string_view text);

private:
    [[nodiscard]] std::string serialize_body() const;

    std::vector<SemanticExtensionRecord> records_;
    std::vector<OwnershipManifest> manifests_;
    assets::Sha256Digest digest_;
};

} // namespace carto::semantic
