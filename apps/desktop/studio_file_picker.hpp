#pragma once

#include <windows.h>
#include <commdlg.h>

#include <filesystem>
#include <optional>

namespace carto::studio {

enum class FilePickerTheme { dark, light, high_contrast };
enum class FilePickerOutcome { selected, cancelled, system_browser };

struct FilePickerResult {
    FilePickerOutcome outcome = FilePickerOutcome::cancelled;
    std::optional<std::filesystem::path> path;
};

// Selection never mutates the filesystem. The caller opens or saves the file.
[[nodiscard]] FilePickerResult choose_studio_file(
    HWND owner, const OPENFILENAMEW& options, FilePickerTheme theme, bool save);

} // namespace carto::studio
