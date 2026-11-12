#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace carto::application {
struct LinkedSceneIdentity final {
    std::filesystem::path source_project;
    assets::Sha256Digest scene_digest;
    assets::Sha256Digest binding_digest;
    assets::Sha256Digest condition_journal_digest;
};
struct LinkedScenePublishRequest final {
    LinkedSceneIdentity source;
    std::filesystem::path new_output_directory;
    std::string canonical_document;
    std::filesystem::path application_journal;
    std::string human_reason;
    std::stop_token stop;
};
struct LinkedScenePublished final {
    LinkedSceneIdentity identity;
    std::filesystem::path application_journal;
    core::Revision revision;
};
// Geometry and authored appearance of one externally linked scene part, as
// verified by the provider. Vertex and face IDs belong to the linked source
// asset's mesh, never to this project; the part has no project mesh asset.
struct LinkedViewportPart final {
    std::uint64_t scene_object = 0U;
    std::uint64_t source_asset = 0U;
    std::uint64_t source_component = 0U;
    std::uint64_t source_mesh = 0U;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::string material_identity;
    std::shared_ptr<const geometry::CompiledMesh> mesh;
    // One linear RGBA per triangle, sampled from the authored base-color
    // texture at the triangle's UV centroid and multiplied by the material
    // factor and authored vertex color. Empty only when the provider refuses.
    std::vector<core::Vec4d> triangle_base_color;
};
struct LinkedViewportScene final {
    assets::Sha256Digest resolution_digest;
    std::vector<LinkedViewportPart> parts;
};
// Provider installation belongs to the trusted host, not to project data.
// Implementations validate a baseline and publish a fresh immutable generation
// without changing the caller's document, journal or source directory.
class LinkedSceneProvider {
public:
    virtual ~LinkedSceneProvider() = default;
    [[nodiscard]] virtual core::Result<LinkedSceneIdentity> inspect(
        const std::filesystem::path& source_project) const = 0;
    [[nodiscard]] virtual core::Result<LinkedScenePublished> publish(
        const LinkedScenePublishRequest& request) const = 0;
    // Resolves the verified linked parts for display. A provider that cannot
    // resolve geometry and authored appearance must fail, not return blanks.
    [[nodiscard]] virtual core::Result<LinkedViewportScene> resolve_viewport(
        const LinkedSceneIdentity& identity) const {
        static_cast<void>(identity);
        return core::Result<LinkedViewportScene>::failure(core::Diagnostic(
            core::ErrorCode::unsupported, "linked-scene provider does not resolve viewport geometry"));
    }
};
} // namespace carto::application
