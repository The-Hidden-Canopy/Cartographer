#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/semantic/sidecar.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::semantic {

// Ordered schema version. Versions order linearly; compatibility is a
// separate registry decision, not an arithmetic one.
struct SchemaVersion {
    std::uint32_t value = 0U;

    [[nodiscard]] constexpr auto operator<=>(const SchemaVersion&) const noexcept = default;
};

enum class SchemaValueKind : std::uint8_t {
    boolean,
    integer,
    real,
    string,
    tag,
    relationship,
    opaque,
};

[[nodiscard]] std::string_view schema_value_kind_name(SchemaValueKind kind) noexcept;

// One declared schema field with explicit ownership. Cartographer may lower
// native-lowerable fields into neutral prepared evidence; game-owned fields
// remain fenced out and must be preserved verbatim by every migration.
struct SchemaField {
    static constexpr std::size_t kMaxFieldNameBytes = 256U;

    std::string name;
    SchemaValueKind value_kind = SchemaValueKind::opaque;
    FieldOwnership ownership = FieldOwnership::game_owned;

    [[nodiscard]] bool operator==(const SchemaField&) const noexcept = default;
    [[nodiscard]] core::Result<void> validate() const;
};

// Registered revision of one schema: identity, ordered version, declared
// fields, and a digest binding the exact field declaration.
struct SchemaRevision {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxFieldsPerRevision = 4096U;

    SchemaIdentity schema;
    SchemaVersion version;
    // Fields must be sorted by name and unique so the canonical digest is
    // unambiguous.
    std::vector<SchemaField> fields;
    assets::Sha256Digest revision_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<SchemaRevision> deserialize(std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

private:
    [[nodiscard]] std::string declaration_text() const;
};

// Durable cross-game schema registry. Registration is the only admission
// path; lookup of an unknown namespace or version fails closed. Compatibility
// requires both revisions to be registered, `from` not to exceed `to`, and
// every game-owned field of `from` to survive into `to` unchanged.
class SchemaRegistry final {
public:
    static constexpr std::size_t kMaxRevisionsPerNamespace = 4096U;
    static constexpr std::size_t kMaxRegisteredNamespaces = 4096U;

    [[nodiscard]] core::Result<void> register_schema(const SchemaRevision& revision);

    [[nodiscard]] core::Result<const SchemaRevision*> find(
        const SchemaIdentity& schema, SchemaVersion version) const noexcept;
    [[nodiscard]] bool contains(
        const SchemaIdentity& schema, SchemaVersion version) const noexcept;
    [[nodiscard]] core::Result<SchemaVersion> latest_registered(
        const SchemaIdentity& schema) const noexcept;

    [[nodiscard]] core::Result<void> reconcile(
        const SemanticExtensionRecord& record) const;
    [[nodiscard]] core::Result<void> compatible(
        const SchemaIdentity& schema, SchemaVersion from, SchemaVersion to) const;

private:
    std::map<SchemaIdentity, std::vector<SchemaRevision>> revisions_;
};

} // namespace carto::semantic
