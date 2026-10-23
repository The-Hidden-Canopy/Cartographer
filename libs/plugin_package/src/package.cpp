#include <carto/plugin_package/package.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace carto::plugin_package {

namespace {

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool read_u16(std::span<const std::uint8_t> bytes, std::size_t offset, std::uint16_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) return false;
    value = static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(bytes[offset + 1U] << 8U);
    return true;
}

bool read_u32(std::span<const std::uint8_t> bytes, std::size_t offset, std::uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) return false;
    value = static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
    return true;
}

bool safe_archive_path(std::string_view path) {
    if (path.empty() || path.size() > 512U || path.front() == '/' ||
        path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) return false;
    std::size_t cursor = 0U;
    while (cursor < path.size()) {
        const std::size_t separator = path.find('/', cursor);
        const std::size_t end = separator == std::string_view::npos ? path.size() : separator;
        const auto component = path.substr(cursor, end - cursor);
        if (component.empty() || component == "." || component == "..") return false;
        cursor = separator == std::string_view::npos ? path.size() : separator + 1U;
    }
    return true;
}

bool has_symlink_attributes(std::uint16_t made_by, std::uint32_t external_attributes) {
    const auto operating_system = static_cast<std::uint8_t>(made_by >> 8U);
    if (operating_system == 3U) {
        const std::uint32_t mode = external_attributes >> 16U;
        return (mode & 0170000U) == 0120000U;
    }
    return false;
}

} // namespace

core::Result<std::vector<PackageEntry>> read_cartoplug(
    std::span<const std::uint8_t> archive,
    const PackageLimits& limits) {
    if (archive.empty() || archive.size() > limits.max_archive_bytes) {
        return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive is empty or exceeds its size limit"));
    }
    if (archive.size() < 22U) {
        return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive is smaller than a ZIP end record"));
    }
    const std::size_t search_start = archive.size() > 65557U ? archive.size() - 65557U : 0U;
    std::size_t eocd = std::numeric_limits<std::size_t>::max();
    for (std::size_t offset = archive.size() - 4U;; --offset) {
        std::uint32_t signature = 0U;
        if (read_u32(archive, offset, signature) && signature == 0x06054b50U) {
            eocd = offset;
            break;
        }
        if (offset == search_start) break;
    }
    if (eocd == std::numeric_limits<std::size_t>::max()) {
        return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive has no supported ZIP end record"));
    }
    std::uint16_t disk = 0U;
    std::uint16_t central_disk = 0U;
    std::uint16_t disk_entries = 0U;
    std::uint16_t entries_count = 0U;
    std::uint16_t archive_comment_length = 0U;
    std::uint32_t central_bytes = 0U;
    std::uint32_t central_offset = 0U;
    if (!read_u16(archive, eocd + 4U, disk) || !read_u16(archive, eocd + 6U, central_disk) ||
        !read_u16(archive, eocd + 8U, disk_entries) || !read_u16(archive, eocd + 10U, entries_count) ||
        !read_u32(archive, eocd + 12U, central_bytes) || !read_u32(archive, eocd + 16U, central_offset) ||
        !read_u16(archive, eocd + 20U, archive_comment_length)) {
        return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive end record is truncated"));
    }
    if (disk != 0U || central_disk != 0U || disk_entries != entries_count || entries_count == 0U ||
        entries_count > limits.max_entries || central_offset > archive.size() ||
        central_bytes > archive.size() - central_offset ||
        static_cast<std::size_t>(central_offset) + central_bytes > eocd ||
        eocd > archive.size() - 22U || archive_comment_length > archive.size() - eocd - 22U ||
        eocd + 22U + archive_comment_length != archive.size()) {
        return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive has unsupported or inconsistent ZIP directory metadata"));
    }
    std::vector<PackageEntry> result;
    result.reserve(entries_count);
    std::set<std::string> paths;
    std::size_t cursor = central_offset;
    std::uint64_t total_uncompressed = 0U;
    for (std::uint16_t entry_index = 0U; entry_index < entries_count; ++entry_index) {
        std::uint32_t signature = 0U;
        if (!read_u32(archive, cursor, signature) || signature != 0x02014b50U ||
            archive.size() - cursor < 46U) {
            return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive central directory entry is invalid"));
        }
        std::uint16_t made_by = 0U;
        std::uint16_t flags = 0U;
        std::uint16_t method = 0U;
        std::uint32_t compressed_size = 0U;
        std::uint32_t uncompressed_size = 0U;
        std::uint16_t name_length = 0U;
        std::uint16_t extra_length = 0U;
        std::uint16_t comment_length = 0U;
        std::uint32_t external_attributes = 0U;
        std::uint32_t local_offset = 0U;
        if (!read_u16(archive, cursor + 4U, made_by) || !read_u16(archive, cursor + 8U, flags) ||
            !read_u16(archive, cursor + 10U, method) || !read_u32(archive, cursor + 20U, compressed_size) ||
            !read_u32(archive, cursor + 24U, uncompressed_size) || !read_u16(archive, cursor + 28U, name_length) ||
            !read_u16(archive, cursor + 30U, extra_length) || !read_u16(archive, cursor + 32U, comment_length) ||
            !read_u32(archive, cursor + 38U, external_attributes) || !read_u32(archive, cursor + 42U, local_offset)) {
            return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive central directory entry fields are invalid"));
        }
        const std::size_t record_bytes = 46U + name_length + extra_length + comment_length;
        if (record_bytes > archive.size() - cursor || (flags & static_cast<std::uint16_t>(~0x0800U)) != 0U || method != 0U ||
            compressed_size != uncompressed_size || uncompressed_size > limits.max_single_file ||
            has_symlink_attributes(made_by, external_attributes)) {
            return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive uses unsupported compression, descriptors, links, or file sizes"));
        }
        const std::string path(reinterpret_cast<const char*>(archive.data() + cursor + 46U), name_length);
        if (!safe_archive_path(path) || !paths.insert(path).second) {
            return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive contains an unsafe or duplicate path"));
        }
        if (static_cast<std::uint64_t>(uncompressed_size) > limits.max_total_uncompressed -
            std::min(limits.max_total_uncompressed, total_uncompressed)) {
            return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive exceeds its total uncompressed size limit"));
        }
        total_uncompressed += uncompressed_size;
        if (path == "manifest.json" && uncompressed_size > limits.max_manifest_bytes) {
            return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug manifest exceeds its size limit"));
        }
        if (local_offset > archive.size() || archive.size() - local_offset < 30U) {
            return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive local entry is out of bounds"));
        }
        std::uint32_t local_signature = 0U;
        std::uint16_t local_flags = 0U;
        std::uint16_t local_method = 0U;
        std::uint16_t local_name_length = 0U;
        std::uint16_t local_extra_length = 0U;
        if (!read_u32(archive, local_offset, local_signature) || local_signature != 0x04034b50U ||
            !read_u16(archive, local_offset + 6U, local_flags) || !read_u16(archive, local_offset + 8U, local_method) ||
            !read_u16(archive, local_offset + 26U, local_name_length) || !read_u16(archive, local_offset + 28U, local_extra_length) ||
            local_flags != flags || local_method != method || local_name_length != name_length ||
            local_extra_length > archive.size() - local_offset - 30U) {
            return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive local and central entries disagree"));
        }
        if (local_name_length != 0U &&
            std::string_view(reinterpret_cast<const char*>(archive.data() + local_offset + 30U), local_name_length) != path) {
            return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive local path differs from central path"));
        }
        const std::size_t data_offset = local_offset + 30U + local_name_length + local_extra_length;
        if (data_offset > archive.size() || compressed_size > archive.size() - data_offset ||
            data_offset > static_cast<std::size_t>(central_offset) ||
            compressed_size > static_cast<std::size_t>(central_offset) - data_offset) {
            return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive file data is out of bounds"));
        }
        PackageEntry package_entry;
        package_entry.path = path;
        package_entry.bytes.assign(archive.begin() + static_cast<std::ptrdiff_t>(data_offset),
                                   archive.begin() + static_cast<std::ptrdiff_t>(data_offset + compressed_size));
        result.push_back(std::move(package_entry));
        cursor += record_bytes;
    }
    if (cursor != static_cast<std::size_t>(central_offset) + central_bytes) {
        return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive central directory length does not match its entries"));
    }
    if (std::none_of(result.begin(), result.end(), [](const PackageEntry& entry) {
            return entry.path == "manifest.json";
        })) {
        return core::Result<std::vector<PackageEntry>>::failure(validation("cartoplug archive has no manifest.json"));
    }
    return core::Result<std::vector<PackageEntry>>::success(std::move(result));
}

} // namespace carto::plugin_package
