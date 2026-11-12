#pragma once

#include <carto/core/result.hpp>
#include <filesystem>
#include <system_error>

namespace carto::project {
// A companion marker is only a reason to prevent a destructive base-only
// operation. Its presence does not validate the binding or resolve assets.
[[nodiscard]] inline core::Result<bool> has_condition_link_companions(
    const std::filesystem::path& project_path) {
    std::error_code error;
    auto root = project_path.parent_path();
    if (root.empty()) root = ".";
    const bool binding = std::filesystem::exists(root / "natural-scene.binding.txt", error);
    if (error) return core::Result<bool>::failure(core::Diagnostic(
        core::ErrorCode::io_error, "unable to inspect linked-scene binding"));
    const bool assets = std::filesystem::exists(root / "natural-assets", error);
    if (error) return core::Result<bool>::failure(core::Diagnostic(
        core::ErrorCode::io_error, "unable to inspect linked-scene assets"));
    return core::Result<bool>::success(binding || assets);
}
} // namespace carto::project
