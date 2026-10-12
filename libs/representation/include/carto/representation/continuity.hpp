#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/representation/family.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace carto::representation {

// Reimport continuity binding: the durable promise that one continuity
// identity inside a family keeps its meaning across reimport. It binds the
// stable family id, the continuity id, and the source revision observed at
// bind time. It grants no mutation authority and holds no runtime state.
struct RepresentationContinuityBinding {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U;

    RepresentationFamilyId family;
    RepresentationContinuityId continuity;
    core::Revision source_revision;
    assets::Sha256Digest family_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<RepresentationContinuityBinding> deserialize(
        std::string_view text);
};

// Binds continuity after a reimport. The family identity and source namespace
// must be unchanged; the source revision must not regress, and it must
// advance when the source digest changed. Every continuity identity present
// before the reimport must still exist with the same purpose, so persistent
// runtime state is never reset because authored geometry changed.
[[nodiscard]] core::Result<RepresentationContinuityBinding> bind_reimport_continuity(
    const RepresentationFamily& previous,
    const RepresentationFamily& reimported,
    RepresentationContinuityId continuity);

// Checks that a reimported family preserves every continuity identity of the
// previous family with the same purpose.
[[nodiscard]] core::Result<void> verify_reimport_continuity(
    const RepresentationFamily& previous,
    const RepresentationFamily& reimported);

} // namespace carto::representation
