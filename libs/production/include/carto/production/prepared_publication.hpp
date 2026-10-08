#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/production/prepared_scene.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace carto::production {

struct PreparedProductPayload {
    std::string identity;
    std::vector<std::uint8_t> bytes;
};

struct PreparedPublicationReceipt {
    assets::Sha256Digest prepared_scene_digest;
    std::optional<assets::Sha256Digest> previous_selected_digest;
    core::Revision source_revision;
    std::filesystem::path revision_path;
    bool reused_immutable_revision = false;
};

struct SelectedPreparedScene {
    assets::Sha256Digest prepared_scene_digest;
    PreparedSceneEnvelope envelope;
    std::filesystem::path revision_path;
};

// Publishes one complete immutable revision under output_root/revisions and
// atomically selects it through output_root/current. expected_selected_digest
// is a mandatory compare-and-swap expectation: nullopt means no revision may
// currently be selected. A failed or stale attempt never changes current.
[[nodiscard]] core::Result<PreparedPublicationReceipt> publish_prepared_scene(
    const std::filesystem::path& output_root,
    const PreparedSceneEnvelope& envelope,
    const std::vector<PreparedProductPayload>& payloads,
    std::optional<assets::Sha256Digest> expected_selected_digest);

// Resolves current once, then verifies the immutable manifest and every
// included product digest. It does not follow current again during the read.
[[nodiscard]] core::Result<std::optional<SelectedPreparedScene>>
read_selected_prepared_scene(const std::filesystem::path& output_root);

} // namespace carto::production
