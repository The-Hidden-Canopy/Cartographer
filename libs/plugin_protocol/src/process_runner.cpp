#include <carto/plugin_protocol/process_runner.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace carto::plugin_protocol {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic failed(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

core::Diagnostic over_budget(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

} // namespace

#if defined(_WIN32)
namespace {

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }
    void reset(HANDLE replacement = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_ = nullptr;
};

class AttributeList {
public:
    AttributeList() {
        SIZE_T bytes = 0U;
        (void)InitializeProcThreadAttributeList(nullptr, 1U, 0U, &bytes);
        storage_.resize(bytes);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        initialized_ = InitializeProcThreadAttributeList(list_, 1U, 0U, &bytes) != 0;
    }
    ~AttributeList() {
        if (initialized_) DeleteProcThreadAttributeList(list_);
    }
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }

private:
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
    bool initialized_ = false;
};

bool utf8_to_wide(std::string_view input, std::wstring& output) {
    if (input.find('\0') != std::string_view::npos ||
        input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (required <= 0) return false;
    output.resize(static_cast<std::size_t>(required));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), output.data(), required) == required;
}

std::wstring quote_argument(std::wstring_view argument) {
    const bool needs_quotes = argument.empty() ||
        argument.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) return std::wstring(argument);

    std::wstring result;
    result.push_back(L'"');
    std::size_t backslashes = 0U;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2U + 1U, L'\\');
            result.push_back(L'"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0U;
    }
    result.append(backslashes * 2U, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring minimal_environment_block() {
    std::wstring windows_directory(32768U, L'\0');
    const UINT length = GetWindowsDirectoryW(windows_directory.data(),
        static_cast<UINT>(windows_directory.size()));
    if (length == 0U || length >= windows_directory.size()) return {};
    windows_directory.resize(length);

    std::wstring block = L"SystemRoot=" + windows_directory;
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

std::string win32_error(const char* operation, DWORD code) {
    return std::string(operation) + " failed (Windows error " +
        std::to_string(code) + ")";
}

} // namespace

core::Result<ProcessOutput> run_isolated_process(const ProcessRequest& request) {
    if (auto budget = Protocol::validate_execution_budget(request.budget); !budget) {
        return core::Result<ProcessOutput>::failure(budget.error());
    }
    if (request.input.size() > Protocol::kMaxFrameBytes ||
        request.executable.empty() || !request.executable.is_absolute()) {
        return core::Result<ProcessOutput>::failure(invalid(
            "isolated provider request has an invalid executable or input size"));
    }
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(request.executable, filesystem_error) ||
        filesystem_error) {
        return core::Result<ProcessOutput>::failure(invalid(
            "isolated provider executable is not a regular file"));
    }

    std::wstring command_line = quote_argument(request.executable.native());
    for (const auto& argument : request.arguments) {
        std::wstring wide_argument;
        if (!utf8_to_wide(argument, wide_argument)) {
            return core::Result<ProcessOutput>::failure(invalid(
                "isolated provider argument is not valid UTF-8"));
        }
        command_line.push_back(L' ');
        command_line += quote_argument(wide_argument);
    }

    std::vector<std::uint8_t> output;
    try {
        output.reserve(request.budget.max_output_bytes);
    } catch (...) {
        return core::Result<ProcessOutput>::failure(failed(
            "could not allocate the bounded provider output buffer"));
    }

    const std::wstring environment = minimal_environment_block();
    if (environment.empty()) {
        return core::Result<ProcessOutput>::failure(failed(
            "could not create the provider's minimal environment"));
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE stdout_read_raw = nullptr;
    HANDLE stdout_write_raw = nullptr;
    if (CreatePipe(&stdout_read_raw, &stdout_write_raw, &inheritable, 0U) == 0) {
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("CreatePipe(stdout)", GetLastError())));
    }
    UniqueHandle stdout_read(stdout_read_raw);
    UniqueHandle stdout_write(stdout_write_raw);
    if (SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0U) == 0) {
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("SetHandleInformation(stdout)", GetLastError())));
    }

    HANDLE stdin_read_raw = nullptr;
    HANDLE stdin_write_raw = nullptr;
    if (CreatePipe(&stdin_read_raw, &stdin_write_raw, &inheritable, 0U) == 0) {
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("CreatePipe(stdin)", GetLastError())));
    }
    UniqueHandle stdin_read(stdin_read_raw);
    UniqueHandle stdin_write(stdin_write_raw);
    if (SetHandleInformation(stdin_write.get(), HANDLE_FLAG_INHERIT, 0U) == 0) {
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("SetHandleInformation(stdin)", GetLastError())));
    }

    AttributeList attributes;
    if (!attributes.initialized()) {
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("InitializeProcThreadAttributeList", GetLastError())));
    }
    const HANDLE inherited_handles[]{stdin_read.get(), stdout_write.get()};
    if (UpdateProcThreadAttribute(attributes.get(), 0U,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(inherited_handles), sizeof(inherited_handles),
            nullptr, nullptr) == 0) {
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("UpdateProcThreadAttribute", GetLastError())));
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_read.get();
    startup.StartupInfo.hStdOutput = stdout_write.get();
    startup.StartupInfo.hStdError = stdout_write.get();
    startup.lpAttributeList = attributes.get();

    PROCESS_INFORMATION process_info{};
    const auto working_directory = request.executable.parent_path().native();
    if (CreateProcessW(request.executable.c_str(), command_line.data(), nullptr,
            nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED |
                EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            const_cast<wchar_t*>(environment.c_str()), working_directory.c_str(),
            &startup.StartupInfo, &process_info) == 0) {
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("CreateProcessW", GetLastError())));
    }
    UniqueHandle process(process_info.hProcess);
    UniqueHandle thread(process_info.hThread);
    stdout_write.reset();
    stdin_read.reset();

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (job.get() == nullptr) {
        const DWORD error = GetLastError();
        (void)TerminateProcess(process.get(), error);
        (void)WaitForSingleObject(process.get(), INFINITE);
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("CreateJobObjectW", error)));
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_TIME;
    limits.BasicLimitInformation.PerJobUserTimeLimit.QuadPart =
        static_cast<LONGLONG>(request.budget.max_cpu_time_ms) * 10'000LL;
    if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits)) == 0 ||
        AssignProcessToJobObject(job.get(), process.get()) == 0) {
        const DWORD error = GetLastError();
        (void)TerminateProcess(process.get(), error);
        (void)WaitForSingleObject(process.get(), INFINITE);
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("assign provider process to bounded job", error)));
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        (void)TerminateJobObject(job.get(), error);
        (void)WaitForSingleObject(process.get(), INFINITE);
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("ResumeThread(provider)", error)));
    }
    thread.reset();

    std::atomic_bool output_exceeded{false};
    std::atomic_bool output_io_failed{false};
    std::atomic_bool input_io_failed{false};
    std::thread output_reader;
    std::thread input_writer;
    try {
        output_reader = std::thread([&]() noexcept {
            std::array<std::uint8_t, 8192U> buffer{};
            for (;;) {
                DWORD read = 0U;
                if (ReadFile(stdout_read.get(), buffer.data(),
                        static_cast<DWORD>(buffer.size()), &read, nullptr) == 0) {
                    if (GetLastError() != ERROR_BROKEN_PIPE) output_io_failed.store(true);
                    break;
                }
                if (read == 0U) break;
                if (static_cast<std::size_t>(read) >
                    request.budget.max_output_bytes - output.size()) {
                    output_exceeded.store(true);
                    (void)TerminateJobObject(job.get(), ERROR_BUFFER_OVERFLOW);
                    continue;
                }
                try {
                    output.insert(output.end(), buffer.begin(),
                        buffer.begin() + static_cast<std::ptrdiff_t>(read));
                } catch (...) {
                    output_io_failed.store(true);
                    (void)TerminateJobObject(job.get(), ERROR_NOT_ENOUGH_MEMORY);
                    break;
                }
            }
        });
        input_writer = std::thread([&]() noexcept {
            std::size_t offset = 0U;
            while (offset < request.input.size()) {
                const auto remaining = request.input.size() - offset;
                const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                    remaining, std::numeric_limits<DWORD>::max()));
                DWORD written = 0U;
                if (WriteFile(stdin_write.get(), request.input.data() + offset,
                        chunk, &written, nullptr) == 0 || written == 0U) {
                    input_io_failed.store(true);
                    break;
                }
                offset += written;
            }
            stdin_write.reset();
        });
    } catch (...) {
        (void)TerminateJobObject(job.get(), ERROR_NOT_ENOUGH_MEMORY);
        if (output_reader.joinable()) output_reader.join();
        if (input_writer.joinable()) input_writer.join();
        return core::Result<ProcessOutput>::failure(failed(
            "could not start provider pipe workers"));
    }

    const DWORD wait_result = WaitForSingleObject(process.get(),
        static_cast<DWORD>(request.budget.max_wall_time_ms));
    const bool wall_time_exceeded = wait_result == WAIT_TIMEOUT;
    if (wall_time_exceeded || wait_result == WAIT_FAILED) {
        const DWORD termination_code = wall_time_exceeded ? ERROR_TIMEOUT : GetLastError();
        (void)TerminateJobObject(job.get(), termination_code);
        (void)WaitForSingleObject(process.get(), INFINITE);
    }

    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    const bool accounting_read = QueryInformationJobObject(job.get(),
        JobObjectBasicAccountingInformation, &accounting, sizeof(accounting),
        nullptr) != 0;
    if (accounting_read && accounting.ActiveProcesses > 0U) {
        // Do not leave provider descendants alive or holding the result pipe open.
        (void)TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED);
    }
    if (output_reader.joinable()) output_reader.join();
    if (input_writer.joinable()) input_writer.join();

    if (wall_time_exceeded) {
        return core::Result<ProcessOutput>::failure(over_budget(
            "provider exceeded its wall-time budget"));
    }
    if (wait_result == WAIT_FAILED) {
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("WaitForSingleObject(provider)", GetLastError())));
    }
    if (output_exceeded.load()) {
        return core::Result<ProcessOutput>::failure(over_budget(
            "provider exceeded its combined stdout/stderr budget"));
    }
    if (accounting_read && accounting.TotalUserTime.QuadPart >=
            static_cast<LONGLONG>(request.budget.max_cpu_time_ms) * 10'000LL) {
        return core::Result<ProcessOutput>::failure(over_budget(
            "provider exceeded its aggregate job CPU-time budget"));
    }
    if (output_io_failed.load() || input_io_failed.load()) {
        return core::Result<ProcessOutput>::failure(failed(
            "provider process pipe I/O failed"));
    }

    DWORD exit_code = 0U;
    if (GetExitCodeProcess(process.get(), &exit_code) == 0) {
        return core::Result<ProcessOutput>::failure(failed(
            win32_error("GetExitCodeProcess(provider)", GetLastError())));
    }
    if (exit_code != 0U) {
        return core::Result<ProcessOutput>::failure(failed(
            "provider process exited unsuccessfully (Windows exit code " +
            std::to_string(exit_code) + ")"));
    }
    return core::Result<ProcessOutput>::success(ProcessOutput{
        std::move(output), static_cast<std::uint32_t>(exit_code)});
}

#else

core::Result<ProcessOutput> run_isolated_process(const ProcessRequest&) {
    return core::Result<ProcessOutput>::failure(failed(
        "isolated provider process execution is unavailable on this platform"));
}

#endif

} // namespace carto::plugin_protocol
