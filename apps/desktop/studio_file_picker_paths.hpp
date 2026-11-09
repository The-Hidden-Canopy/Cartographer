#pragma once

#include "studio_file_picker_rules.hpp"

#include <windows.h>
#include <shlwapi.h>
#include <filesystem>
#include <string>
#include <string_view>

namespace carto::studio::file_picker_detail {

enum class SelectionStatus { selected, enter_directory, confirm_replace, invalid_name,
    missing_file, missing_parent, unavailable, use_system_browser };

struct SelectionPlan {
    SelectionStatus status = SelectionStatus::unavailable;
    std::filesystem::path path;
};

[[nodiscard]] inline bool matches_filter(const std::wstring& name, const std::wstring& patterns) {
    return patterns == L"*.*" || patterns == L"*" || PathMatchSpecW(name.c_str(), patterns.c_str());
}

[[nodiscard]] inline bool remote_location(const std::filesystem::path& path) {
    const auto& native = path.native();
    if (native.size() >= 2U && native[0] == L'\\' && native[1] == L'\\') return true;
    const auto root = path.root_path();
    return !root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
}

[[nodiscard]] inline SelectionPlan plan_selection(const std::filesystem::path& directory,
    const std::filesystem::path& entered, bool save, std::wstring_view default_extension,
    bool confirm_overwrite) {
    if (entered.empty() || (!entered.is_absolute() &&
        (entered.has_root_name() || entered.has_root_directory())))
        return {SelectionStatus::invalid_name, {}};
    auto candidate = entered.is_absolute() ? entered : directory / entered;
    if (remote_location(candidate)) return {SelectionStatus::use_system_browser, {}};
    std::error_code ec;
    if (std::filesystem::is_directory(candidate, ec) && !ec)
        return {SelectionStatus::enter_directory, candidate.lexically_normal()};
    if (!save) {
        ec.clear();
        if (!std::filesystem::is_regular_file(candidate, ec) || ec)
            return {SelectionStatus::missing_file, {}};
        return {SelectionStatus::selected, candidate.lexically_normal()};
    }
    if (!candidate.has_extension() && !default_extension.empty())
        candidate += L"." + std::wstring(default_extension);
    if (invalid_save_filename(candidate.filename().wstring()))
        return {SelectionStatus::invalid_name, {}};
    ec.clear();
    if (!std::filesystem::is_directory(candidate.parent_path(), ec) || ec)
        return {SelectionStatus::missing_parent, {}};
    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    if (attributes == INVALID_FILE_ATTRIBUTES && error != ERROR_FILE_NOT_FOUND &&
        error != ERROR_PATH_NOT_FOUND) return {SelectionStatus::unavailable, {}};
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return {SelectionStatus::invalid_name, {}};
    const auto status = attributes != INVALID_FILE_ATTRIBUTES && confirm_overwrite
        ? SelectionStatus::confirm_replace : SelectionStatus::selected;
    return {status, candidate.lexically_normal()};
}

} // namespace carto::studio::file_picker_detail
