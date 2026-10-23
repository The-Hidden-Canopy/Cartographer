#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/lookdev_draw_package.hpp>
#include <carto/production/prepared_scene.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace carto::verification {

// Deterministic failure injection for docs 182/183: every helper models one
// named hostile condition between planning and publishing so pipelines can
// prove fail-closed behavior without engine or runtime dependencies.

// Commits one trivial, fixed-content concurrent edit so the durable source
// revision advances after a prepared scene was planned. The staged mutation
// goes through the normal journaled transaction boundary; nothing reaches the
// document without a matching journal append.
[[nodiscard]] core::Result<project::TransactionReceipt> inject_concurrent_edit(
    project::ProjectDocument& document,
    journal::Journal& journal,
    std::string actor);

// Planning-time readiness gate: a prepared envelope may enter publication
// only while its bound source revision and source digest still equal the
// durable authoring document. The source namespace is also compared whenever
// the document declares one. Any drift is stale_data and fails closed.
[[nodiscard]] core::Result<void> prepublish_source_check(
    const project::ProjectDocument& document,
    const production::PreparedSceneEnvelope& prepared);

// Forces one derived render product to lag the durable source revision,
// modeling a stale product reused after an authored edit. Fails when the
// artifact is already at revision zero.
[[nodiscard]] core::Result<void> force_stale_render_mesh(
    production::RenderMeshArtifact& artifact);

// Forces a lookdev package to lag its exact render-mesh source revision. The
// package digest is refreshed so the drifted record stays self-consistent but
// still fails against the current source snapshot.
[[nodiscard]] core::Result<void> force_stale_lookdev_package(
    production::LookdevDrawPackage& package);

// In-memory payload corruption applied before publication. Every helper
// fails closed on out-of-range offsets instead of clamping.
[[nodiscard]] core::Result<void> flip_payload_byte(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset);
[[nodiscard]] core::Result<void> zero_payload_range(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::size_t count);
[[nodiscard]] core::Result<void> truncate_payload(
    std::vector<std::uint8_t>& bytes,
    std::size_t size);

enum class PublishedCorruptionKind : std::uint8_t {
    flip_byte,
    zero_range,
    truncate_file,
    append_garbage,
};

// Post-publication corruption of one immutable product payload under
// output_root/revisions/<digest>/<relative_path>. Relative paths are rejected
// on traversal or symlink indirection before any byte is written; missing
// revisions and files fail closed.
[[nodiscard]] core::Result<void> corrupt_published_product(
    const std::filesystem::path& output_root,
    const assets::Sha256Digest& revision_digest,
    const std::string& relative_path,
    PublishedCorruptionKind kind,
    std::uint64_t offset = 0U,
    std::uint64_t length = 0U);

enum class ManifestTamperKind : std::uint8_t {
    flip_byte,
    append_garbage,
    replace_digest_text,
};

// Post-publication tampering with the immutable prepared-scene manifest.
// replace_digest_text rewrites one hex digit of the first digest token so the
// manifest may still parse but can no longer match its selected identity.
[[nodiscard]] core::Result<void> tamper_published_manifest(
    const std::filesystem::path& output_root,
    const assets::Sha256Digest& revision_digest,
    ManifestTamperKind kind);

} // namespace carto::verification
