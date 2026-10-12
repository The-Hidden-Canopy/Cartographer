#pragma once

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#ifdef _WIN32
#    define NOMINMAX
#    include <windows.h>
#    include <winioctl.h>
#endif

namespace carto::test_support {

#ifdef _WIN32
inline bool create_directory_junction(
    const std::filesystem::path& target,
    const std::filesystem::path& link,
    std::error_code& error) {
    struct MountPointData {
        DWORD tag;
        WORD data_length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
        WCHAR path_buffer[1];
    };

    const auto absolute_target = std::filesystem::absolute(target, error);
    if (error) return false;
    if (!CreateDirectoryW(link.wstring().c_str(), nullptr)) {
        error = std::error_code(
            static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    const HANDLE handle = CreateFileW(
        link.wstring().c_str(), GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = std::error_code(
            static_cast<int>(GetLastError()), std::system_category());
        std::error_code ignored;
        std::filesystem::remove(link, ignored);
        return false;
    }

    const std::wstring substitute = L"\\??\\" + absolute_target.wstring();
    const std::wstring printable = absolute_target.wstring();
    const std::size_t substitute_bytes = substitute.size() * sizeof(WCHAR);
    const std::size_t printable_bytes = printable.size() * sizeof(WCHAR);
    const std::size_t path_bytes = substitute_bytes + sizeof(WCHAR) +
        printable_bytes + sizeof(WCHAR);
    std::vector<std::byte> storage(
        offsetof(MountPointData, path_buffer) + path_bytes);
    auto* data = reinterpret_cast<MountPointData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->data_length = static_cast<WORD>(
        substitute_bytes + printable_bytes + 12U);
    data->reserved = 0U;
    data->substitute_offset = 0U;
    data->substitute_length = static_cast<WORD>(substitute_bytes);
    data->print_offset = static_cast<WORD>(substitute_bytes + sizeof(WCHAR));
    data->print_length = static_cast<WORD>(printable_bytes);
    std::memcpy(
        data->path_buffer, substitute.c_str(), substitute_bytes + sizeof(WCHAR));
    std::memcpy(
        reinterpret_cast<std::byte*>(data->path_buffer) + data->print_offset,
        printable.c_str(), printable_bytes + sizeof(WCHAR));

    DWORD returned = 0U;
    const BOOL created = DeviceIoControl(
        handle, FSCTL_SET_REPARSE_POINT, data,
        static_cast<DWORD>(data->data_length + 8U), nullptr, 0U, &returned, nullptr);
    if (!created) {
        error = std::error_code(
            static_cast<int>(GetLastError()), std::system_category());
    } else {
        error.clear();
    }
    CloseHandle(handle);
    if (!created) {
        std::error_code ignored;
        std::filesystem::remove(link, ignored);
    }
    return created != FALSE;
}
#endif

inline bool create_directory_link(
    const std::filesystem::path& target,
    const std::filesystem::path& link,
    std::error_code& error) {
    std::filesystem::create_directory_symlink(target, link, error);
    if (!error) return true;
#ifdef _WIN32
    error.clear();
    return create_directory_junction(target, link, error);
#else
    return false;
#endif
}

} // namespace carto::test_support
