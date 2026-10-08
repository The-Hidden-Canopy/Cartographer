#pragma once

#include <carto/web_geometry/types.hpp>

#include <string>
#include <string_view>

namespace carto::web_geometry {

// The package format is a bounded, deterministic sidecar representation. It
// carries derived geometry only; it cannot mutate or stand in for .carto
// authoring truth.
[[nodiscard]] core::Result<std::string> serialize_package(
    const WebGeometryPackage& package);
[[nodiscard]] core::Result<WebGeometryPackage> deserialize_package(
    std::string_view text);

} // namespace carto::web_geometry
