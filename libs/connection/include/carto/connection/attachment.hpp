#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/connection/graph.hpp>
#include <carto/connection/port.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::connection {

// Runtime/game-owned attachment truth. PortAttachmentState records which
// opaque runtime tokens are bound to which authored port identities *right
// now*. It is explicitly outside Cartographer authoring truth: it carries no
// engine handles, no scene authority, and is never an input to port
// compatibility. Reimport preserves it selectively; see
// reconcile_attachments_after_reimport.

inline constexpr std::uint32_t kPortAttachmentStateVersion = 1U;
inline constexpr std::uint32_t kAttachmentPreservationVersion = 1U;
inline constexpr std::size_t kMaxPortAttachmentStateBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxAttachmentPreservationBytes = 1U * 1024U * 1024U;
inline constexpr std::size_t kMaxAttachmentRecords = 65'536U;
inline constexpr std::size_t kMaxAttachmentsPerPort = 64U;
inline constexpr std::size_t kMaxAttachmentTokenBytes = 256U;

class PortAttachmentState final {
public:
    [[nodiscard]] core::Result<void> attach(PortId port, std::string token);

    [[nodiscard]] const std::map<PortId, std::vector<std::string>>& attachments()
        const noexcept {
        return attachments_;
    }
    [[nodiscard]] bool empty() const noexcept { return attachments_.empty(); }

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PortAttachmentState> deserialize(
        std::string_view text);

private:
    // Each token vector is kept sorted and unique so the canonical form is
    // unambiguous without any further normalization.
    std::map<PortId, std::vector<std::string>> attachments_;
};

// Digest-bound result of a three-way reimport reconciliation: which
// runtime/game attachments survived because their authored ports survived,
// and which were dropped because their ports no longer exist.
struct AttachmentPreservationReceipt {
    assets::Sha256Digest previous_state_digest;
    assets::Sha256Digest surviving_state_digest;
    assets::Sha256Digest reimported_graph_digest;
    std::uint64_t preserved_attachments = 0U;
    std::uint64_t dropped_attachments = 0U;
    std::vector<PortId> preserved_ports;
    std::vector<PortId> dropped_ports;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<AttachmentPreservationReceipt> deserialize(
        std::string_view text);
};

// Never resurrect destroyed objects and never reset surviving persistent
// state because authored geometry changed: an attachment survives exactly
// when its port identity exists in the reimported graph. Everything else is
// dropped and reported, never silently kept.
[[nodiscard]] core::Result<AttachmentPreservationReceipt> reconcile_attachments_after_reimport(
    const PortAttachmentState& previous_state,
    const ConnectionPortGraph& reimported_graph);

} // namespace carto::connection
