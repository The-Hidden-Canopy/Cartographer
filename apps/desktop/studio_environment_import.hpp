#pragma once

#include <carto/project/presentation_catalog.hpp>

#include <utility>

namespace carto::studio {

// Prepare a replacement without mutating the live catalog. Reimporting the
// same source keeps the author's lighting controls while refreshing its grid.
[[nodiscard]] inline core::Result<project::PresentationCatalog> prepare_environment_import(
    const project::PresentationCatalog& current, project::AuthoredEnvironment source) {
    auto catalog = current;
    const bool same_source = current.environment() &&
        current.environment()->identity == source.identity;
    if (same_source) {
        const auto& previous = *current.environment();
        source.enabled = previous.enabled;
        source.rotation_radians = previous.rotation_radians;
        source.intensity = previous.intensity;
        source.revision = previous.revision;
    }
    auto admitted = catalog.set_environment(source);
    if (!admitted) return core::Result<project::PresentationCatalog>::failure(admitted.error());
    if (same_source && catalog.serialize() != current.serialize()) {
        if (source.revision.exhausted()) {
            return core::Result<project::PresentationCatalog>::failure(core::Diagnostic(
                core::ErrorCode::stale_data, "Environment revision is exhausted; existing lighting retained."));
        }
        source.revision = source.revision.next();
        admitted = catalog.set_environment(std::move(source));
        if (!admitted) return core::Result<project::PresentationCatalog>::failure(admitted.error());
    }
    return core::Result<project::PresentationCatalog>::success(std::move(catalog));
}

} // namespace carto::studio
