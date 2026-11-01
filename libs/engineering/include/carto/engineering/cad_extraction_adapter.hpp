#pragma once

#include <carto/engineering/cad_provider.hpp>
#include <carto/engineering/topology_correspondence.hpp>

namespace carto::engineering {

// Caller supplies identities; no project IDs, source revisions, units, mesh
// handles, or provider claims are inferred from a STEP header.
enum class CadImportMode : std::uint8_t { unspecified = 0U, first_import, reimport };

struct CadExtractionContext {
    std::string project_namespace;
    std::string source_namespace;
    Id source_id = 0U;
    std::string locator;
    std::string revision;
    assets::Sha256Digest import_settings_digest;
    // Exact caller-owned settings bytes; callers own their canonical format.
    // The adapter verifies the digest and supplies these bytes to the backend.
    std::string import_settings_bytes;
    std::string import_run_id;
    CadImportMode mode = CadImportMode::unspecified;
};

// Provider-authored evidence for a first face-only bridge. Source and settings
// digests bind the exact request, but do not authenticate provider execution
// or prove the backend used the supplied settings. The provider must explicitly attest that units
// and unchanged-source topology identities were obtained, and that unsupported
// geometry/external references were not silently omitted. Mesh handles, mesh
// digests and triangle correspondence remain provider assertions until the
// owning project validates actual mesh content.
struct CadKernelExtraction {
    assets::Sha256Digest source_digest;
    assets::Sha256Digest settings_digest;
    std::string units;
    // Units of mesh coordinates and occurrence translations; this narrow
    // bridge refuses conversion and requires an exact match to source units.
    std::string coordinate_units;
    bool source_units_known = false;
    bool unchanged_source_identity_stable = false;
    bool unsupported_geometry = true;
    bool unresolved_external_references = true;
    std::vector<std::string> diagnostics;
    std::vector<EngineeringPart> parts;
    std::vector<EngineeringOccurrence> occurrences;
    std::vector<TopologyBinding> topology;
    TopologyCorrespondenceMap correspondence;
};

// The backend must enforce these budgets while extracting, before allocating
// unbounded provider-owned payloads. The adapter rechecks returned counts; this
// in-process interface is not a sandbox against a misbehaving backend.
struct CadExtractionLimits {
    std::size_t max_settings_bytes = 65536U;
    std::size_t max_parts = 4096U;
    std::size_t max_occurrences = 4096U;
    std::size_t max_faces = 4096U;
    std::size_t max_display_triangles = 65536U;
    std::size_t max_correspondence_text_bytes = 4U * 1024U * 1024U;
};

class CadExtractionBackend {
  public:
    virtual ~CadExtractionBackend() = default;
    [[nodiscard]] virtual std::string_view identity() const noexcept = 0;
    [[nodiscard]] virtual std::string_view version() const noexcept = 0;
    [[nodiscard]] virtual core::Result<CadKernelExtraction>
    extract_step(std::string_view source_bytes, const CadExtractionContext &context,
                 const CadExtractionLimits &limits) const = 0;
};

struct CadExtractionCandidate {
    AuthoringDocument authoring;
    TopologyCorrespondenceMap correspondence;
    // This bridge does not inspect a project display mesh. A future owner must
    // validate actual mesh content and triangle ownership before acceptance.
    bool display_mesh_content_verified = false;
};

// Synchronous, provider-neutral wiring; no backend ships with this API. The
// first-import vs reimport is explicit; reimport requires the prior map. The
// owning caller must establish that a declared first import is actually new.
// Reimport compares face correspondence, but is refused even if that matches:
// a map alone cannot reconcile prior part, occurrence, or numeric topology IDs.
// Full authoring reconciliation and replacement remain with the owner. Every
// candidate remains explicitly lossy because this face-only
// seam does not import PMI or verify actual display mesh content. Existing
// consumer admission rejects lossy sources; raw package creation alone does
// not enforce that gate, so callers must not publish the candidate.
[[nodiscard]] core::Result<CadExtractionCandidate>
extract_step_with_backend(const CadExtractionBackend &backend, const CadExtractionContext &context,
                          std::string_view source_bytes,
                          const TopologyCorrespondenceMap *previous = nullptr);

} // namespace carto::engineering
