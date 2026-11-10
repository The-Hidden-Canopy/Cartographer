#include "gltf_dependency_reader.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace carto::io::detail {
namespace {
using Bytes = std::vector<std::uint8_t>;
core::Result<Bytes> failure(std::string text) {
    return core::Result<Bytes>::failure({core::ErrorCode::validation_failed, std::move(text)});
}

bool safe_filename(std::string_view uri) {
    if (uri.empty() || uri.size() > 240U || uri == "." || uri == ".." ||
        uri.back() == '.' || uri.back() == ' ') return false;
    for (const char byte : uri) {
        const auto c = static_cast<unsigned char>(byte);
        if (c < 0x20U || c > 0x7eU || c == 0x22U ||
            std::string_view("/\\:<>|?*%#").find(static_cast<char>(c)) != std::string_view::npos)
            return false;
    }
    std::string stem(uri.substr(0, uri.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c);
    });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return false;
    if (stem.size() == 4U && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
        stem[3] >= '1' && stem[3] <= '9') return false;
    return true;
}
#ifdef _WIN32
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring final_path(HANDLE h) {
    const DWORD length = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!length || length > 32768U) return {};
    std::wstring result(length, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(h, result.data(), length, FILE_NAME_NORMALIZED);
    if (!written || written >= length) return {};
    result.resize(written);
    return result;
}
#else
struct Handle {
    int value = -1;
    ~Handle() { if (value >= 0) close(value); }
};
#endif
}

core::Result<Bytes> read_gltf_sibling(const std::filesystem::path& document,
                                      std::string_view uri, std::size_t limit) {
    if (!safe_filename(uri)) return failure("external glTF buffer must be a plain sibling filename");
    const auto parent = document.parent_path().empty() ? std::filesystem::path(".") : document.parent_path();
#ifdef _WIN32
    Handle directory{CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (directory.value == INVALID_HANDLE_VALUE) return failure("cannot open glTF source directory");
    const auto root = final_path(directory.value);
    if (root.empty()) return failure("cannot identify glTF source directory");
    const auto sibling = parent / std::filesystem::path(std::string(uri));
    Handle file{CreateFileW(sibling.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return failure("cannot open external glTF buffer");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0U ||
        GetFileType(file.value) != FILE_TYPE_DISK)
        return failure("external glTF buffer is a link or non-file");
    const auto actual = std::filesystem::path(final_path(file.value)).parent_path().wstring();
    if (actual.empty() || CompareStringOrdinal(actual.c_str(), static_cast<int>(actual.size()),
            root.c_str(), static_cast<int>(root.size()), TRUE) != CSTR_EQUAL)
        return failure("external glTF buffer escaped its source directory");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > limit)
        return failure("external glTF buffer exceeds its size limit");
    Bytes bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        DWORD read = 0U;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024U * 1024U));
        if (!ReadFile(file.value, bytes.data() + offset, chunk, &read, nullptr) || !read)
            return failure("cannot read complete external glTF buffer");
        offset += read;
    }
    std::uint8_t extra{}; DWORD read = 0U;
    if (!ReadFile(file.value, &extra, 1U, &read, nullptr) || read != 0U)
        return failure("external glTF buffer changed during capture");
    BY_HANDLE_FILE_INFORMATION after{};
    if (!GetFileInformationByHandle(file.value, &after) ||
        info.nFileSizeHigh != after.nFileSizeHigh || info.nFileSizeLow != after.nFileSizeLow ||
        CompareFileTime(&info.ftLastWriteTime, &after.ftLastWriteTime) != 0)
        return failure("external glTF buffer changed during capture");
#else
    Handle directory{open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (directory.value < 0) return failure("cannot open glTF source directory");
    Handle file{openat(directory.value, std::string(uri).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if (file.value < 0) return failure("cannot open external glTF buffer without following links");
    struct stat info{};
    if (fstat(file.value, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > limit)
        return failure("external glTF buffer is not a bounded regular file");
    Bytes bytes(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const auto count = read(file.value, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) return failure("cannot read complete external glTF buffer");
        offset += static_cast<std::size_t>(count);
    }
    std::uint8_t extra{};
    if (read(file.value, &extra, 1U) != 0) return failure("external glTF buffer changed during capture");
    struct stat after{};
    if (fstat(file.value, &after) || info.st_dev != after.st_dev || info.st_ino != after.st_ino ||
        info.st_size != after.st_size || info.st_mtime != after.st_mtime || info.st_ctime != after.st_ctime)
        return failure("external glTF buffer changed during capture");
#if defined(__linux__)
    if (info.st_mtim.tv_nsec != after.st_mtim.tv_nsec || info.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        return failure("external glTF buffer changed during capture");
#elif defined(__APPLE__)
    if (info.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
        info.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec)
        return failure("external glTF buffer changed during capture");
#endif
#endif
    return core::Result<Bytes>::success(std::move(bytes));
}
}
