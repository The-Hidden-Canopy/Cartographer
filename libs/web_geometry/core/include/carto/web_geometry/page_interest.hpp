#pragma once

#include <carto/web_geometry/overlay.hpp>

#include <string>
#include <vector>

namespace carto::web_geometry {

// A deterministic request description for consumers that may later manage
// runtime page residency. It contains identities only; it performs no I/O,
// allocation in a device, admission, or residency mutation.
struct RuntimePageInterestPlan {
    std::string package_digest;
    core::Revision source_revision;
    std::vector<RuntimePageId> pages;
    std::vector<ClusterId> visible_clusters;

    [[nodiscard]] core::Result<void> validate() const;
};

// Derives page/cluster interest from an already projected overlay snapshot.
// Package identity, cluster metadata, and diagnostic receipt identity are
// checked again so a stale or forged snapshot cannot drive a later loader.
[[nodiscard]] core::Result<RuntimePageInterestPlan> plan_runtime_page_interest(
    const WebGeometryPackage& package,
    const ClusterOverlaySnapshot& overlays);

} // namespace carto::web_geometry
