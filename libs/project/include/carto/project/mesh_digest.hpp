#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/geometry/mesh.hpp>

namespace carto::project {

// Runtime-independent binary content fingerprint for derived-mesh caching:
// source IDs/revision, geometry, typed attributes and UV-set descriptors.
// Edge/corner payloads additionally bind their source topology identities.
// Equivalent reordered inputs may miss the cache, never admit stale data.
// This does not replace persisted mesh-patch digests or validate/admit a mesh;
// callers must validate the owning source separately. No paths/runtime data.
[[nodiscard]] assets::Sha256Digest
canonical_mesh_source_digest(const geometry::EditableMesh& mesh);

} // namespace carto::project
