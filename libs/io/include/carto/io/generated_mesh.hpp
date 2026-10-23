#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/mesh.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace carto::io {

struct GeneratedMeshImport final {
    geometry::EditableMesh mesh;
    std::string source_identity;
    core::Revision source_revision;
    assets::Sha256Digest artifact_digest;
};

// Admits only the bounded, embedded-buffer glTF profile emitted for a single
// Cartographer botanical job. No external URI, transform hierarchy, extension,
// or arbitrary importer behavior is accepted.
[[nodiscard]] core::Result<GeneratedMeshImport> import_generated_botanical_gltf(
    std::span<const std::uint8_t> bytes,
    std::string_view expected_source_identity,
    core::Revision expected_source_revision,
    assets::Sha256Digest expected_artifact_digest);

// Admits the same bounded botanical profile from a GLB 2.0 container. The
// artifact digest is checked against the complete GLB bytes, and the JSON and
// optional BIN chunks must follow the canonical container order.
[[nodiscard]] core::Result<GeneratedMeshImport> import_generated_botanical_glb(
    std::span<const std::uint8_t> bytes,
    std::string_view expected_source_identity,
    core::Revision expected_source_revision,
    assets::Sha256Digest expected_artifact_digest);

} // namespace carto::io
