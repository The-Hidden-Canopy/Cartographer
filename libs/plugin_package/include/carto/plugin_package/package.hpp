#pragma once

#include <carto/core/result.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace carto::plugin_package {

struct PackageEntry {
    std::string path;
    std::vector<std::uint8_t> bytes;
};

struct PackageLimits {
    std::uint64_t max_archive_bytes = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_uncompressed = 512ULL * 1024ULL * 1024ULL;
    std::uint32_t max_entries = 4096U;
    std::uint64_t max_single_file = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t max_manifest_bytes = 65536ULL;
};

[[nodiscard]] core::Result<std::vector<PackageEntry>> read_cartoplug(
    std::span<const std::uint8_t> archive,
    const PackageLimits& limits = {});

} // namespace carto::plugin_package
