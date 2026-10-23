#include <carto/project/file_lock.hpp>

#include <mutex>
#include <utility>

#ifdef _WIN32
#    define NOMINMAX
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <sys/file.h>
#    include <unistd.h>
#endif

namespace carto::project {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

std::mutex g_file_lock_mutex;

} // namespace

struct FileLock::State {
    std::unique_lock<std::mutex> process_lock;
    bool acquired = false;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int descriptor = -1;
#endif

    void release() noexcept {
#ifdef _WIN32
        if (handle == INVALID_HANDLE_VALUE) return;
        if (acquired) {
            OVERLAPPED overlapped{};
            static_cast<void>(UnlockFileEx(handle, 0U, MAXDWORD, MAXDWORD, &overlapped));
        }
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
#else
        if (descriptor < 0) return;
        if (acquired) static_cast<void>(::flock(descriptor, LOCK_UN));
        static_cast<void>(::close(descriptor));
        descriptor = -1;
#endif
        acquired = false;
        if (process_lock.owns_lock()) process_lock.unlock();
    }

    ~State() { release(); }
};

FileLock::FileLock(std::filesystem::path target)
    : target_(std::move(target)), state_(std::make_unique<State>()) {}

FileLock::~FileLock() = default;

FileLock::FileLock(FileLock&&) noexcept = default;
FileLock& FileLock::operator=(FileLock&&) noexcept = default;

core::Result<void> FileLock::acquire() {
    if (target_.empty() || target_.filename().empty()) {
        return core::Result<void>::failure(invalid("file lock target must name a path"));
    }
    if (state_->acquired || state_->process_lock.owns_lock()) {
        return core::Result<void>::failure(invalid("file lock is already acquired"));
    }

    const std::filesystem::path lock_path = target_.string() + ".lock";
    const auto parent = lock_path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        if (!std::filesystem::is_directory(parent, error) || error) {
            return core::Result<void>::failure(io_error(
                "file lock parent directory is unavailable"));
        }
    }

    state_->process_lock = std::unique_lock<std::mutex>(g_file_lock_mutex);
#ifdef _WIN32
    state_->handle = CreateFileW(
        lock_path.wstring().c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (state_->handle == INVALID_HANDLE_VALUE) {
        state_->process_lock.unlock();
        return core::Result<void>::failure(io_error("unable to open file lock"));
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(state_->handle, LOCKFILE_EXCLUSIVE_LOCK, 0U, MAXDWORD, MAXDWORD, &overlapped)) {
        CloseHandle(state_->handle);
        state_->handle = INVALID_HANDLE_VALUE;
        state_->process_lock.unlock();
        return core::Result<void>::failure(io_error("unable to acquire file lock"));
    }
#else
    state_->descriptor = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
    if (state_->descriptor < 0) {
        state_->process_lock.unlock();
        return core::Result<void>::failure(io_error("unable to open file lock"));
    }
    if (::flock(state_->descriptor, LOCK_EX) != 0) {
        static_cast<void>(::close(state_->descriptor));
        state_->descriptor = -1;
        state_->process_lock.unlock();
        return core::Result<void>::failure(io_error("unable to acquire file lock"));
    }
#endif
    state_->acquired = true;
    return core::Result<void>::success();
}

} // namespace carto::project
