#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::representation {

template <typename Tag>
struct Id {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const Id&) const noexcept = default;
};

struct RepresentationFamilyTag;
struct RepresentationVariantTag;
struct RepresentationContinuityTag;

using RepresentationFamilyId = Id<RepresentationFamilyTag>;
using RepresentationVariantId = Id<RepresentationVariantTag>;
using RepresentationContinuityId = Id<RepresentationContinuityTag>;

// One authored source identity binds the whole family. Runtime handles, paths,
// and display names are never identity; the source digest is.
enum class RepresentationPurpose : std::uint8_t {
    render,
    collision,
    navigation,
    sensor,
    shadow,
};

[[nodiscard]] std::string_view representation_purpose_name(
    RepresentationPurpose purpose) noexcept;
[[nodiscard]] core::Result<RepresentationPurpose> representation_purpose_from_name(
    std::string_view name);

struct AuthoredSourceIdentity {
    std::string source_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const AuthoredSourceIdentity&) const noexcept = default;
};

// A neutral, serialization-ready variant record. It references its error
// envelope by digest and carries no engine type, runtime handle, or payload.
struct RepresentationVariant {
    RepresentationVariantId id;
    RepresentationPurpose purpose = RepresentationPurpose::render;
    assets::Sha256Digest error_envelope_digest;
    std::uint64_t resident_bytes = 0U;
    // Continuity identity survives variant replacement: a new byte product
    // binds to the same continuity id so runtime consumers never lose track.
    RepresentationContinuityId continuity;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const RepresentationVariant&) const noexcept = default;
};

// Stable family identity binding exactly one authored source identity. Variant
// replacement never changes the family id; continuity ids are preserved by
// upsert_variant and unique within the family.
class RepresentationFamily final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;
    static constexpr std::size_t kMaxVariantsPerFamily = 256U;
    static constexpr std::size_t kMaxNamespaceBytes = 256U;

    [[nodiscard]] core::Result<void> upsert_variant(RepresentationVariant variant);
    [[nodiscard]] const RepresentationVariant* find_variant(
        RepresentationVariantId id) const noexcept;
    [[nodiscard]] const RepresentationVariant* find_continuity(
        RepresentationContinuityId continuity) const noexcept;

    [[nodiscard]] RepresentationFamilyId id() const noexcept { return id_; }
    void set_id(RepresentationFamilyId id) noexcept { id_ = id; }
    [[nodiscard]] const AuthoredSourceIdentity& source() const noexcept { return source_; }
    void set_source(AuthoredSourceIdentity source) { source_ = std::move(source); }
    [[nodiscard]] const std::vector<RepresentationVariant>& variants() const noexcept {
        return variants_;
    }

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<RepresentationFamily> deserialize(
        std::string_view text);

private:
    RepresentationFamilyId id_;
    AuthoredSourceIdentity source_;
    // Kept sorted by variant id so the canonical form is unambiguous.
    std::vector<RepresentationVariant> variants_;
};

// Binds one variant to the digest of the selection evidence that admitted it.
// The certification is only meaningful while the family source revision it
// records still matches the family it certifies.
struct RepresentationCertification {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U;
    static constexpr std::size_t kMaxAlgorithmBytes = 128U;

    RepresentationFamilyId family;
    RepresentationVariantId variant;
    core::Revision certified_source_revision;
    assets::Sha256Digest selection_evidence_digest;
    std::string algorithm = "cartographer-representation-certification/0.1";
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<RepresentationCertification> deserialize(
        std::string_view text);
};

} // namespace carto::representation
