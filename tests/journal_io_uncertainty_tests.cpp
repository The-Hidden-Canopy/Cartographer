// Compile the unchanged journal implementation with a test-only I/O facade.
// The facade writes real files, reports controlled output/rollback failures,
// and leaves parsing, locks, hashes, revisions and all Journal methods intact.
// No test hook or fault switch is added to the production library or ABI.
#include <carto/journal/journal.hpp>
#include <carto/core/filesystem.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#ifdef _WIN32
#    define NOMINMAX
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <sys/file.h>
#    include <unistd.h>
#endif

namespace journal_io_fault {
enum class Failure { none, partial_record, complete_record_flush };
struct Control {
    std::filesystem::path target;
    Failure failure = Failure::none;
    bool fail_rollback = false;
    bool output_failure_reported = false;
    bool rollback_failure_reported = false;
};
Control control;

class OutputStream {
public:
    OutputStream(const std::filesystem::path& path, std::ios::openmode mode)
        : stream_(path, mode), target_(path == control.target) {}
    explicit operator bool() const { return static_cast<bool>(stream_); }
    bool operator!() const { return !stream_; }
    OutputStream& operator<<(std::string_view text) {
        if (target_ && control.failure == Failure::partial_record &&
            !control.output_failure_reported) {
            stream_.write(text.data(), static_cast<std::streamsize>(std::min<std::size_t>(8U, text.size())));
            stream_.flush();
            stream_.setstate(std::ios::badbit);
            control.output_failure_reported = true;
        } else {
            stream_ << text;
        }
        return *this;
    }
    OutputStream& operator<<(char value) { stream_ << value; return *this; }
    void flush() {
        stream_.flush();
        if (target_ && control.failure == Failure::complete_record_flush) {
            // The record reached the file, but the caller received an error.
            // A failed rollback must keep that uncertainty across reopen.
            stream_.setstate(std::ios::badbit);
            control.output_failure_reported = true;
        }
    }
    void close() { stream_.close(); }
private:
    std::ofstream stream_;
    bool target_;
};
} // namespace journal_io_fault

namespace journal_io_std {
using namespace std;
using ofstream = journal_io_fault::OutputStream;
namespace filesystem {
using namespace std::filesystem;
inline void resize_file(const path& target, std::uintmax_t size, std::error_code& error) noexcept {
    if (target == journal_io_fault::control.target && journal_io_fault::control.fail_rollback) {
        error = std::make_error_code(std::errc::permission_denied);
        journal_io_fault::control.rollback_failure_reported = true;
        return;
    }
    std::filesystem::resize_file(target, size, error);
}
} // namespace filesystem
} // namespace journal_io_std

// All implementation prerequisites were included above, outside this facade.
// This translation unit supplies Journal symbols, so the normal journal object
// is not linked into this test executable a second time.
#define std journal_io_std
#include "journal.cpp"
#undef std

namespace {
using carto::core::Revision;
using carto::journal::Journal;
using carto::journal::JournalAppend;
using journal_io_fault::Failure;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TempDirectory {
public:
    TempDirectory() {
        static unsigned count = 0U;
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-journal-io-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++count));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        journal_io_fault::control = {};
        std::error_code ignored;
        const auto native = carto::core::native_io_path(path_, ignored);
        if (!ignored) std::filesystem::remove_all(native, ignored);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

std::string bytes(const std::filesystem::path& path) {
    std::error_code error;
    const auto native = carto::core::native_io_path(path, error);
    require(!error, "resolve evidence file");
    std::ifstream input(native, std::ios::binary);
    require(static_cast<bool>(input), "read evidence file");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

auto append(Journal& journal, std::uint64_t before, std::uint64_t after) {
    const std::string payload = "pressure-payload";
    return journal.append(JournalAppend{Revision{before}, Revision{after}, "test.mutation",
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()),
        100U + after});
}

void seed(Journal& journal) {
    journal_io_fault::control = {};
    require(static_cast<bool>(append(journal, 0U, 1U)), "seed append");
    require(static_cast<bool>(journal.verify()), "seed verification");
}

void fail_append(Journal& journal, Failure failure, bool rollback_failure) {
    std::error_code error;
    const auto native = carto::core::native_io_path(journal.path(), error);
    require(!error, "resolve fault target");
    journal_io_fault::control = {native, failure, rollback_failure, false, false};
    const auto failed = append(journal, 1U, 2U);
    require(!failed, "injected append must report failure");
    require(journal_io_fault::control.output_failure_reported, "actual output fault reached");
    require(journal_io_fault::control.rollback_failure_reported == rollback_failure, "actual rollback fault reached");
    journal_io_fault::control = {};
}

bool blocked_without_mutation(const std::filesystem::path& path) {
    const auto before = bytes(path);
    Journal fresh(path);
    bool callback_called = false;
    const auto read = fresh.read_all();
    const auto verified = fresh.verify();
    const auto revision = fresh.current_revision();
    const auto replayed = fresh.replay_from(Revision{}, [&](const auto&) {
        callback_called = true;
        return carto::core::Result<void>::success();
    });
    const auto refused = append(fresh, 2U, 3U);
    const bool unchanged = bytes(path) == before;
    std::cout << "uncertainty observation: read=" << static_cast<bool>(read)
              << " verify=" << static_cast<bool>(verified)
              << " revision=" << static_cast<bool>(revision)
              << " replay=" << static_cast<bool>(replayed)
              << " callback=" << callback_called
              << " append=" << static_cast<bool>(refused)
              << " unchanged=" << unchanged << '\n';
    return !read && !verified && !revision && !replayed && !callback_called && !refused && unchanged;
}

void happy_path_and_invalid_transition() {
    TempDirectory temp;
    Journal journal(temp.path() / "journal.log");
    seed(journal);
    const auto before = bytes(journal.path());
    require(!append(journal, 0U, 2U), "stale transition refused");
    require(!append(journal, 1U, 1U), "nonadvancing transition refused");
    require(bytes(journal.path()) == before, "invalid transition changes no journal bytes");
    require(static_cast<bool>(append(journal, 1U, 2U)), "second happy append");
    Journal fresh(journal.path());
    const auto entries = fresh.read_all();
    require(entries && entries.value().size() == 2U, "two audited happy records");
    require(entries.value()[1].previous_entry_hash == entries.value()[0].entry_hash, "hash chain preserved");
}

void successful_rollback_allows_later_append() {
    TempDirectory temp;
    Journal journal(temp.path() / "journal.log");
    seed(journal);
    const auto before = bytes(journal.path());
    fail_append(journal, Failure::complete_record_flush, false);
    require(bytes(journal.path()) == before, "successful rollback restores exact bytes");
    Journal fresh(journal.path());
    require(fresh.current_revision() && fresh.current_revision().value() == Revision{1U}, "rollback retains old revision");
    require(static_cast<bool>(append(fresh, 1U, 2U)), "later append after certain rollback");
    require(static_cast<bool>(fresh.verify()), "later record verifies");
}

void partial_record_failed_rollback_blocks_reopen() {
    TempDirectory temp;
    Journal journal(temp.path() / "journal.log");
    seed(journal);
    fail_append(journal, Failure::partial_record, true);
    require(blocked_without_mutation(journal.path()), "torn failed append must block every fresh-reader path");
}

void complete_record_failed_rollback_blocks_reopen() {
    TempDirectory temp;
    Journal journal(temp.path() / "journal.log");
    seed(journal);
    fail_append(journal, Failure::complete_record_flush, true);
    require(blocked_without_mutation(journal.path()), "complete but failed append must block every fresh-reader path");
}

std::string shell_argument(const std::string& value) {
    require(value.find('"') == std::string::npos, "worker path must not contain shell quotes");
    return '"' + value + '"';
}

void complete_record_failed_rollback_blocks_fresh_process(const std::filesystem::path& executable) {
    TempDirectory temp;
    Journal journal(temp.path() / "journal.log");
    seed(journal);
    fail_append(journal, Failure::complete_record_flush, true);
    const auto before = bytes(journal.path());
    const std::string invocation = shell_argument(executable.string()) + " --expect-blocked " + shell_argument(journal.path().string());
#ifdef _WIN32
    const std::string command = '"' + invocation + '"';
#else
    const std::string command = invocation;
#endif
    const int result = std::system(command.c_str());
    require(result == 0, "fresh process must refuse the uncertain journal");
    require(bytes(journal.path()) == before, "fresh process does not rewrite evidence");
}

void uncertain_journal_does_not_block_independent_journal() {
    TempDirectory temp;
    Journal uncertain(temp.path() / "uncertain.log");
    seed(uncertain);
    fail_append(uncertain, Failure::complete_record_flush, true);
    Journal independent(temp.path() / "independent.log");
    seed(independent);
    require(static_cast<bool>(append(independent, 1U, 2U)), "unrelated journal remains usable");
    require(static_cast<bool>(independent.verify()), "unrelated journal remains verified");
}

void long_path_preserves_rollback_and_uncertainty() {
    TempDirectory temp;
    const auto directory = temp.path() / std::string(160U, 'l') / std::string(80U, 'j');
    std::error_code error;
    const auto native_directory = carto::core::native_io_path(directory, error);
    require(!error, "resolve long journal directory");
    require(carto::core::create_directories(native_directory, error) && !error,
        "create long journal directory");
    const auto path = directory / "journal.log";
#ifdef _WIN32
    require(path.native().size() > 260U, "journal fixture exceeds Windows legacy path limit");
#endif
    Journal journal(path);
    seed(journal);
    const auto before = bytes(path);
    fail_append(journal, Failure::complete_record_flush, false);
    require(bytes(path) == before, "long-path successful rollback restores exact bytes");
    Journal reopened(path);
    const auto revision = reopened.current_revision();
    require(revision && revision.value() == Revision{1U}, "long-path reopen retains revision");
    fail_append(reopened, Failure::partial_record, true);
    require(blocked_without_mutation(path), "long-path uncertainty blocks fresh readers and writers");
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--expect-blocked") {
        return blocked_without_mutation(argv[2]) ? 0 : 1;
    }
    unsigned failed = 0U;
    const auto run = [&](const char* name, const auto& test) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    };
    run("happy path and invalid transition", happy_path_and_invalid_transition);
    run("successful rollback permits later append", successful_rollback_allows_later_append);
    run("partial record failed rollback blocks reopen", partial_record_failed_rollback_blocks_reopen);
    run("complete record failed rollback blocks reopen", complete_record_failed_rollback_blocks_reopen);
    run("complete record failed rollback blocks fresh process", [&] {
        complete_record_failed_rollback_blocks_fresh_process(std::filesystem::absolute(argv[0]));
    });
    run("uncertainty is scoped to its own journal", uncertain_journal_does_not_block_independent_journal);
    run("long-path rollback and uncertainty", long_path_preserves_rollback_and_uncertainty);
    return failed == 0U ? 0 : 1;
}
