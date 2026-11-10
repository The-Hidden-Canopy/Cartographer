#pragma once

#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace carto::core {

inline std::filesystem::path native_io_path(const std::filesystem::path& path, std::error_code& error) {
    error.clear();
    if (path.empty()) {
        error = std::make_error_code(std::errc::invalid_argument);
        return {};
    }
#ifdef _WIN32
    if (path.native().starts_with(LR"(\\?\)")) {
        auto result = path;
        result.make_preferred();
        return result;
    }
    auto absolute = std::filesystem::absolute(path, error).lexically_normal().native();
    if (error) return {};
    absolute = absolute.starts_with(LR"(\\)")
        ? LR"(\\?\UNC\)" + absolute.substr(2) : LR"(\\?\)" + absolute;
    auto result = std::filesystem::path(std::move(absolute));
    result.make_preferred();
    return result;
#else
    error.clear();
    return path;
#endif
}

// Some Windows standard libraries cannot recursively create an extended-length
// path because they parse its device prefix as a normal directory component.
inline bool create_directories(const std::filesystem::path& path, std::error_code& error) {
#ifdef _WIN32
    if (path.native().starts_with(LR"(\\?\)")) {
        std::vector<std::filesystem::path> missing;
        auto cursor = path;
        cursor.make_preferred();
        for (;;) {
            const auto status = std::filesystem::status(cursor, error);
            if (!error && std::filesystem::is_directory(status)) break;
            if (!error && std::filesystem::exists(status)) {
                error = std::make_error_code(std::errc::not_a_directory);
                return false;
            }
            if (error && error != std::errc::no_such_file_or_directory) return false;
            error.clear();
            const auto parent = cursor.parent_path();
            if (parent.empty() || parent == cursor) {
                error = std::make_error_code(std::errc::no_such_file_or_directory);
                return false;
            }
            missing.push_back(cursor);
            cursor = parent;
        }
        bool created = false;
        for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
            created = std::filesystem::create_directory(*it, error) || created;
            if (error) return false;
        }
        return created;
    }
#endif
    return std::filesystem::create_directories(path, error);
}

} // namespace carto::core
