#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/production/prepared_scene.hpp>
#include <carto/project/project.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production {

struct PreparedProductPayload {
    std::string identity;
    std::vector<std::uint8_t> bytes;
};

struct PreparedPublicationReceipt {
    assets::Sha256Digest prepared_scene_digest;
    assets::Sha256Digest dependency_closure_digest;
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

// Canonical, deterministic hash of the envelope's resolved dependency set.
// Dependency records are already canonically sorted and unique in a valid
// envelope; hashing their canonical text binds the receipt to the exact
// resolved closure rather than to manifest bytes.
[[nodiscard]] core::Result<assets::Sha256Digest> prepared_dependency_closure_digest(
    const PreparedSceneEnvelope& envelope);

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

// Relationship between a selected publication and one saved source document.
// `current` requires the same source namespace, revision and canonical source
// digest; anything else means the publication no longer describes the work.
enum class PreparedPublicationFreshness : std::uint8_t {
    current,
    stale_source,
    foreign_source,
};

[[nodiscard]] std::string_view prepared_publication_freshness_name(
    PreparedPublicationFreshness freshness) noexcept;

[[nodiscard]] PreparedPublicationFreshness prepared_publication_freshness(
    const PreparedSceneEnvelope& envelope, const project::ProjectDocument& document);

// The exact identities a destination runtime pins before admitting the
// selected revision, as canonical `key=value` lines in a fixed order. This is
// descriptive evidence only; it grants no destination authority.
[[nodiscard]] std::string prepared_publication_admission_pins(
    const SelectedPreparedScene& selected);

struct PreparedPublicationStatus {
    // Empty when the publication root does not exist or selects no revision.
    std::optional<SelectedPreparedScene> selected;
    std::optional<PreparedPublicationFreshness> freshness;
};

// Reads and verifies the selected revision under output_root (a missing root
// is an empty status, not an error) and compares it with the saved document.
// Tampered, partial or ambiguous publications fail.
[[nodiscard]] core::Result<PreparedPublicationStatus> inspect_prepared_publication(
    const std::filesystem::path& output_root, const project::ProjectDocument& document);

} // namespace carto::production
