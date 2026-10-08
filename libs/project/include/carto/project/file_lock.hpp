#pragma once

#include <carto/core/result.hpp>

#include <filesystem>
#include <memory>

namespace carto::project {

// Coordinates writers for one project or package path across threads and
// cooperating Cartographer processes. The lock file is retained on disk; the
// operating-system lock owns the lifetime of the writer lease.
class FileLock final {
public:
    explicit FileLock(std::filesystem::path target);
    ~FileLock();

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&&) noexcept;
    FileLock& operator=(FileLock&&) noexcept;

    [[nodiscard]] core::Result<void> acquire();

private:
    struct State;

    std::filesystem::path target_;
    std::unique_ptr<State> state_;
};

} // namespace carto::project
