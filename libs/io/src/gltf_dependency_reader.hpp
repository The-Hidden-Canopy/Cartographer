#pragma once

#include <carto/core/result.hpp>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace carto::io::detail {
// Opens a single sibling of an already-selected document without following a
// final link. Paths, URL syntax and platform device names are not admitted.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> read_gltf_sibling(
    const std::filesystem::path& document, std::string_view uri, std::size_t limit);
}
