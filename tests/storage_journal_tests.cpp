#include "project_document_access.hpp"

#include <carto/assets/blob_store.hpp>
#include <carto/eval/graph.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/journal/checkpoint.hpp>
#include <carto/project/package.hpp>
#include <carto/project/file_lock.hpp>
#include <carto/project/recovery.hpp>
#include <carto/project/transaction.hpp>
#include <carto/core/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            throw TestFailure(std::string("requirement failed: ") + #condition);         \
        }                                                                                \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-storage-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void project_file_lock_serializes_writers_in_process() {
    TempDirectory temp;
    const auto target = temp.path() / "shared.carto";
    std::atomic<bool> started{false};
    std::atomic<bool> acquired{false};
    std::thread waiter;
    bool first_acquired_ok = false;
    bool started_seen = false;
    bool blocked_while_held = false;
    {
        carto::project::FileLock first(target);
        first_acquired_ok = static_cast<bool>(first.acquire());
        waiter = std::thread([&] {
            started.store(true, std::memory_order_release);
            carto::project::FileLock second(target);
            acquired.store(static_cast<bool>(second.acquire()), std::memory_order_release);
        });
        for (unsigned attempt = 0U;
             attempt < 1000U && !started.load(std::memory_order_acquire); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        started_seen = started.load(std::memory_order_acquire);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        blocked_while_held = !acquired.load(std::memory_order_acquire);
    }
    waiter.join();
    REQUIRE(first_acquired_ok);
    REQUIRE(started_seen);
    REQUIRE(blocked_while_held);
    REQUIRE(acquired.load(std::memory_order_acquire));
}

void blob_store_is_content_addressed_and_integrity_checked() {
    TempDirectory temp;
    carto::assets::BlobStore store(temp.path() / "blobs");
    const auto stored = store.put("hello", "text/plain");
    REQUIRE(stored);
    REQUIRE(stored.value().bytes == 5U);
    REQUIRE(stored.value().digest.hex() ==
            "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
    REQUIRE(std::filesystem::is_regular_file(store.path_for(stored.value().digest)));
    const auto read = store.read(stored.value().digest);
    REQUIRE(read);
    REQUIRE(std::string(read.value().begin(), read.value().end()) == "hello");
    REQUIRE(store.verify(stored.value().digest));

    std::ofstream tamper(store.path_for(stored.value().digest), std::ios::binary | std::ios::trunc);
    tamper << "tampered";
    tamper.close();
    REQUIRE(!store.verify(stored.value().digest));
    REQUIRE(store.verify(stored.value().digest).error().code ==
            carto::core::ErrorCode::validation_failed);
    REQUIRE(!store.put("hello", "text/plain"));
}

void json_boundary_decodes_unicode_and_rejects_unpaired_surrogates() {
    const auto parsed = carto::core::json::parse_object(
        R"({"name":"Caf\u00e9 \ud83c\udf0d","\u006eame2":"ok"})");
    REQUIRE(parsed);
    const auto* name = carto::core::json::find_member(parsed.value(), "name");
    REQUIRE(name != nullptr);
    const auto decoded = carto::core::json::decode_string(name->raw_value);
    REQUIRE(decoded);
    REQUIRE(decoded.value() == std::string("Caf\xc3\xa9 \xf0\x9f\x8c\x8d"));

    const auto duplicate = carto::core::json::parse_object(
        R"({"name":"one","\u006eame":"two"})");
    REQUIRE(!duplicate);

    const auto unpaired_high = carto::core::json::parse_object(R"({"name":"\ud83c"})");
    REQUIRE(unpaired_high);
    REQUIRE(!carto::core::json::decode_string(
        carto::core::json::find_member(unpaired_high.value(), "name")->raw_value));

    const auto unpaired_low = carto::core::json::parse_object(R"({"name":"\udf0d"})");
    REQUIRE(unpaired_low);
    REQUIRE(!carto::core::json::decode_string(
        carto::core::json::find_member(unpaired_low.value(), "name")->raw_value));
}

void journal_is_revision_bound_replayable_and_hash_chained() {
    TempDirectory temp;
    carto::journal::Journal journal(temp.path() / "recovery" / "journal.log");
    const std::string first_payload = "patch-one";
    const auto first = journal.append(carto::journal::JournalAppend{
        carto::core::Revision{}, carto::core::Revision{1U}, "mesh.patch",
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(first_payload.data()),
                                      first_payload.size()), 100U});
    REQUIRE(first);
    const std::string second_payload = "patch-two";
    const auto second = journal.append(carto::journal::JournalAppend{
        carto::core::Revision{1U}, carto::core::Revision{2U}, "scene.transform",
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(second_payload.data()),
                                      second_payload.size()), 200U});
    REQUIRE(second);
    REQUIRE(journal.verify());
    REQUIRE(journal.current_revision().value() == carto::core::Revision{2U});

    const auto stale = journal.append(carto::journal::JournalAppend{
        carto::core::Revision{1U}, carto::core::Revision{3U}, "stale", {}, 300U});
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);

    std::vector<std::string> replayed;
    REQUIRE(journal.replay_from(carto::core::Revision{1U},
        [&replayed](const carto::journal::JournalEntry& entry) {
            replayed.push_back(entry.event_type);
            return carto::core::Result<void>::success();
        }));
    REQUIRE(replayed.size() == 1U);
    REQUIRE(replayed.front() == "scene.transform");

    std::ofstream tamper(journal.path(), std::ios::binary | std::ios::app);
    tamper << "truncated";
    tamper.close();
    REQUIRE(!journal.verify());
    REQUIRE(journal.verify().error().code == carto::core::ErrorCode::validation_failed);
}

void journal_serializes_concurrent_appends_in_process() {
    TempDirectory temp;
    const auto path = temp.path() / "concurrent" / "journal.log";
    carto::journal::Journal first(path);
    carto::journal::Journal second(path);
    std::atomic_int successes{0};
    const auto append = [&successes](carto::journal::Journal& journal, const char* payload) {
        const std::string value(payload);
        const auto result = journal.append(carto::journal::JournalAppend{
            carto::core::Revision{}, carto::core::Revision{1U}, "concurrent.event",
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(value.data()), value.size()), 150U});
        if (result) {
            successes.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread left(append, std::ref(first), "left");
    std::thread right(append, std::ref(second), "right");
    left.join();
    right.join();

    REQUIRE(successes.load(std::memory_order_relaxed) == 1);
    const auto entries = first.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 1U);
    REQUIRE(first.verify());
}

std::string shell_argument(const std::string& value) {
#ifdef _WIN32
    return "\"" + value + "\"";
#else
    std::string quoted = "'";
    for (const char byte : value) {
        if (byte == '\'') quoted += "'\\''";
        else quoted.push_back(byte);
    }
    quoted.push_back('\'');
    return quoted;
#endif
}

int journal_append_worker(int argc, char** argv) {
    if (argc != 6) return 2;
    const std::filesystem::path journal_path = argv[2];
    const std::filesystem::path ready_path = argv[4];
    const std::filesystem::path gate_path = argv[5];
    std::error_code error;
    std::filesystem::create_directories(ready_path.parent_path(), error);
    if (error) return 3;
    std::ofstream ready(ready_path, std::ios::binary | std::ios::trunc);
    if (!ready) return 4;
    ready << "ready\n";
    ready.close();
    if (!ready) return 5;
    for (unsigned attempt = 0U; attempt < 10'000U; ++attempt) {
        if (std::filesystem::exists(gate_path, error)) break;
        if (error) return 6;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!std::filesystem::exists(gate_path, error) || error) return 7;
    const std::string payload = argv[3];
    carto::journal::Journal journal(journal_path);
    const auto result = journal.append(carto::journal::JournalAppend{
        carto::core::Revision{}, carto::core::Revision{1U}, "cross_process.event",
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()), 175U});
    return result ? 0 : 1;
}

void journal_serializes_concurrent_appends_across_processes(const std::filesystem::path& executable) {
    TempDirectory temp;
    const auto path = temp.path() / "cross-process" / "journal.log";
    const auto coordination = temp.path() / "coordination";
    const auto gate = coordination / "start";
    constexpr std::size_t worker_count = 4U;
    std::vector<std::filesystem::path> ready_paths;
    std::vector<std::thread> workers;
    ready_paths.reserve(worker_count);
    workers.reserve(worker_count);
    for (std::size_t index = 0U; index < worker_count; ++index) {
        ready_paths.push_back(coordination / ("ready-" + std::to_string(index)));
#ifdef _WIN32
        const std::string command_prefix = "start \"\" /wait ";
#else
        const std::string command_prefix;
#endif
        const std::string command = command_prefix + shell_argument(executable.string()) +
            " --journal-append-worker " + shell_argument(path.string()) +
            " payload-" + std::to_string(index) + " " +
            shell_argument(ready_paths.back().string()) + " " + shell_argument(gate.string());
        workers.emplace_back([command] {
            static_cast<void>(std::system(command.c_str()));
        });
    }

    bool all_ready = false;
    for (unsigned attempt = 0U; attempt < 10'000U && !all_ready; ++attempt) {
        all_ready = true;
        for (const auto& ready_path : ready_paths) {
            if (!std::filesystem::exists(ready_path)) {
                all_ready = false;
                break;
            }
        }
        if (!all_ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::filesystem::create_directories(coordination);
    bool gate_opened = false;
    std::ofstream start(gate, std::ios::binary | std::ios::trunc);
    if (start) {
        start << "go\n";
        start.close();
        gate_opened = static_cast<bool>(start);
    }
    for (auto& worker : workers) worker.join();
    REQUIRE(all_ready);
    REQUIRE(gate_opened);

    carto::journal::Journal journal(path);
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 1U);
    REQUIRE(journal.verify());
}

void checkpoint_store_recovers_pending_entries_and_rejects_mismatch() {
    TempDirectory temp;
    carto::journal::Journal journal(temp.path() / "recovery" / "journal.log");
    const std::string first_payload = "first";
    REQUIRE(journal.append(carto::journal::JournalAppend{
        carto::core::Revision{}, carto::core::Revision{1U}, "first",
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(first_payload.data()),
                                      first_payload.size()), 100U}));

    carto::journal::CheckpointStore checkpoints(temp.path() / "recovery" / "checkpoints");
    const auto asset_manifest = carto::assets::sha256(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>("assets"), 6U));
    const std::string snapshot_text = "snapshot at revision one";
    const auto saved = checkpoints.save(
        journal, carto::core::Revision{1U},
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(snapshot_text.data()),
                                      snapshot_text.size()),
        asset_manifest);
    REQUIRE(saved);
    REQUIRE(saved.value().journal_entries == 1U);

    std::error_code copy_error;
    std::filesystem::copy_file(
        temp.path() / "recovery" / "checkpoints" / "checkpoint-1.meta",
        temp.path() / "recovery" / "checkpoints" / "checkpoint-99.meta",
        std::filesystem::copy_options::overwrite_existing,
        copy_error);
    REQUIRE(!copy_error);
    const auto rejected_filename_mismatch = checkpoints.recover(journal);
    REQUIRE(!rejected_filename_mismatch);
    REQUIRE(rejected_filename_mismatch.error().code == carto::core::ErrorCode::validation_failed);
    std::filesystem::remove(
        temp.path() / "recovery" / "checkpoints" / "checkpoint-99.meta");

    const auto duplicate = checkpoints.save(
        journal, carto::core::Revision{1U},
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(snapshot_text.data()),
                                      snapshot_text.size()),
        asset_manifest);
    REQUIRE(!duplicate);
    REQUIRE(duplicate.error().code == carto::core::ErrorCode::invalid_state);

    const std::string second_payload = "second";
    REQUIRE(journal.append(carto::journal::JournalAppend{
        carto::core::Revision{1U}, carto::core::Revision{2U}, "second",
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(second_payload.data()),
                                      second_payload.size()), 200U}));
    const auto recovered = checkpoints.recover(journal);
    REQUIRE(recovered);
    REQUIRE(std::string(recovered.value().snapshot.begin(), recovered.value().snapshot.end()) == snapshot_text);
    REQUIRE(recovered.value().pending_entries.size() == 1U);
    REQUIRE(recovered.value().pending_entries.front().event_type == "second");

    std::ofstream corrupted(
        temp.path() / "recovery" / "checkpoints" / "checkpoint-99.meta",
        std::ios::binary | std::ios::trunc);
    corrupted << "CARTOGRAPHER_CHECKPOINT_V1\nrevision=99\n";
    corrupted.close();
    const auto rejected_corruption = checkpoints.recover(journal);
    REQUIRE(!rejected_corruption);
    REQUIRE(rejected_corruption.error().code == carto::core::ErrorCode::validation_failed);

    const auto stale_checkpoint = checkpoints.save(
        journal, carto::core::Revision{1U},
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(snapshot_text.data()),
                                      snapshot_text.size()),
        asset_manifest);
    REQUIRE(!stale_checkpoint);
    REQUIRE(stale_checkpoint.error().code == carto::core::ErrorCode::stale_data);
}

void package_manifest_is_human_readable_and_does_not_fabricate_database() {
    TempDirectory temp;
    const auto path = temp.path() / "Example.carto";
    const auto created = carto::project::ProjectPackage::create(path, {
        "example-project", "Example \"Project\"", "meter", "z",
    });
    REQUIRE(created);
    REQUIRE(created.value().validate());
    const auto database_status = created.value().has_document_database();
    REQUIRE(database_status);
    REQUIRE(!database_status.value());
    REQUIRE(std::filesystem::is_regular_file(created.value().manifest_path()));
    REQUIRE(std::filesystem::is_directory(path / "blobs" / "sha256"));

    const auto blob = created.value().blob_store().put("package-bytes", "application/octet-stream");
    REQUIRE(blob);
    const auto opened = carto::project::ProjectPackage::open(path);
    REQUIRE(opened);
    REQUIRE(opened.value().manifest().project_id == "example-project");
    REQUIRE(opened.value().manifest().name == "Example \"Project\"");
    REQUIRE(opened.value().blob_store().verify(blob.value().digest));

    std::filesystem::remove(path / "assets");
    const auto incomplete = carto::project::ProjectPackage::open(path);
    REQUIRE(!incomplete);
    REQUIRE(incomplete.error().code == carto::core::ErrorCode::validation_failed);
    std::filesystem::create_directories(path / "assets");

    std::ofstream malformed(created.value().manifest_path(), std::ios::binary | std::ios::trunc);
    malformed << "{\"format\":\"cartographer-project\",\"format_version\":2,"
                 "\"project_id\":\"example-project\",\"name\":\"Example\","
                 "\"units\":\"meter\",\"up_axis\":\"z\","
                 "\"document_database\":\"document.db\",\"blob_root\":\"blobs/sha256\"} trailing";
    malformed.close();
    const auto rejected_malformed = carto::project::ProjectPackage::open(path);
    REQUIRE(!rejected_malformed);
    REQUIRE(rejected_malformed.error().code == carto::core::ErrorCode::validation_failed);

    std::ofstream nested_field(created.value().manifest_path(), std::ios::binary | std::ios::trunc);
    nested_field << "{\"format\":\"cartographer-project\","
                    "\"project_id\":\"example-project\",\"name\":\"Example\","
                    "\"units\":\"meter\",\"up_axis\":\"z\","
                    "\"coordinate_system\":{\"format_version\":2},"
                    "\"document_database\":\"document.db\","
                    "\"blob_root\":\"blobs/sha256\"}";
    nested_field.close();
    const auto rejected_nested_field = carto::project::ProjectPackage::open(path);
    REQUIRE(!rejected_nested_field);
    REQUIRE(rejected_nested_field.error().code == carto::core::ErrorCode::validation_failed);

    std::ofstream future(created.value().manifest_path(), std::ios::binary | std::ios::trunc);
    future << "{\"format\":\"cartographer-project\",\"format_version\":999,"
              "\"project_id\":\"future\",\"name\":\"Future\",\"units\":\"meter\","
              "\"up_axis\":\"z\",\"document_database\":\"document.db\","
              "\"blob_root\":\"blobs/sha256\"}";
    future.close();
    const auto rejected = carto::project::ProjectPackage::open(path);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::version_mismatch);
}

void package_persists_evaluation_graph_as_verified_content_addressed_state() {
    TempDirectory temp;
    const auto path = temp.path() / "Graph.carto";
    auto package = carto::project::ProjectPackage::create(path, {
        "graph-project", "Graph Project", "meter", "z",
    });
    REQUIRE(package);

    carto::eval::EvaluationGraph graph;
    REQUIRE(graph.register_type(carto::eval::NodeTypeDefinition{
        carto::eval::NodeTypeId{"source"},
        {carto::eval::PortDescriptor{
            carto::eval::PortId{"mesh"},
            carto::eval::PortDirection::output,
            carto::eval::ValueKind::editable_mesh}},
    }));
    const auto node = graph.create_node(carto::eval::NodeTypeId{"source"}, "source-v1");
    REQUIRE(node);
    const auto stored = package.value().store_evaluation_graph(graph);
    REQUIRE(stored);
    REQUIRE(package.value().manifest().evaluation_graph_blob.has_value());
    REQUIRE(package.value().manifest().evaluation_graph_blob.value() == stored.value().digest);

    carto::project::ProjectDocument bound_document;
    carto::journal::Journal binding_journal(temp.path() / "binding" / "journal.log");
    auto binding = carto::project::ProjectTransaction::begin(
        bound_document, binding_journal, "graph-binding");
    REQUIRE(binding);
    REQUIRE(binding.value().set_evaluation_graph_digest(stored.value().digest));
    REQUIRE(binding.value().commit("graph.bind"));
    REQUIRE(package.value().validate_document_binding(bound_document));
    REQUIRE(!package.value().validate_document_binding(carto::project::ProjectDocument{}));

    const auto opened = carto::project::ProjectPackage::open(path);
    REQUIRE(opened);
    const auto loaded = opened.value().load_evaluation_graph();
    REQUIRE(loaded);
    REQUIRE(loaded.value().content_digest() == graph.content_digest());

    const auto graph_blob = opened.value().manifest().evaluation_graph_blob.value();
    const auto graph_path = opened.value().blob_store().path_for(graph_blob);
    std::ofstream tampered(graph_path, std::ios::binary | std::ios::trunc);
    tampered << "tampered";
    tampered.close();
    const auto rejected = carto::project::ProjectPackage::open(path);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::validation_failed);
}

void package_concurrent_graph_writers_fail_closed_on_stale_manifests() {
    TempDirectory temp;
    const auto path = temp.path() / "Concurrent.carto";
    auto created = carto::project::ProjectPackage::create(path, {
        "concurrent-project", "Concurrent Project", "meter", "z",
    });
    REQUIRE(created);
    auto first = carto::project::ProjectPackage::open(path);
    auto second = carto::project::ProjectPackage::open(path);
    REQUIRE(first && second);

    carto::eval::EvaluationGraph graph_a;
    REQUIRE(graph_a.register_type(carto::eval::NodeTypeDefinition{
        carto::eval::NodeTypeId{"source-a"},
        {carto::eval::PortDescriptor{
            carto::eval::PortId{"mesh"}, carto::eval::PortDirection::output,
            carto::eval::ValueKind::editable_mesh}},
    }));
    carto::eval::EvaluationGraph graph_b;
    REQUIRE(graph_b.register_type(carto::eval::NodeTypeDefinition{
        carto::eval::NodeTypeId{"source-b"},
        {carto::eval::PortDescriptor{
            carto::eval::PortId{"mesh"}, carto::eval::PortDirection::output,
            carto::eval::ValueKind::editable_mesh}},
    }));

    std::atomic<std::uint32_t> ready{0U};
    std::atomic<bool> first_succeeded{false};
    std::atomic<bool> second_succeeded{false};
    std::thread first_writer([&] {
        ready.fetch_add(1U, std::memory_order_release);
        while (ready.load(std::memory_order_acquire) != 2U) std::this_thread::yield();
        first_succeeded.store(
            static_cast<bool>(first.value().store_evaluation_graph(graph_a)),
            std::memory_order_release);
    });
    std::thread second_writer([&] {
        ready.fetch_add(1U, std::memory_order_release);
        while (ready.load(std::memory_order_acquire) != 2U) std::this_thread::yield();
        second_succeeded.store(
            static_cast<bool>(second.value().store_evaluation_graph(graph_b)),
            std::memory_order_release);
    });
    first_writer.join();
    second_writer.join();

    REQUIRE(first_succeeded.load(std::memory_order_acquire) !=
            second_succeeded.load(std::memory_order_acquire));
    const auto reopened = carto::project::ProjectPackage::open(path);
    REQUIRE(reopened);
    const auto loaded = reopened.value().load_evaluation_graph();
    REQUIRE(loaded);
    REQUIRE(loaded.value().content_digest() == graph_a.content_digest() ||
            loaded.value().content_digest() == graph_b.content_digest());
}

void project_transaction_publishes_only_after_journal_append() {
    TempDirectory temp;
    carto::project::ProjectDocument document;
    carto::journal::Journal journal(temp.path() / "recovery" / "journal.log");

    const auto rejected_actor = carto::project::ProjectTransaction::begin(
        document, journal, "bad\nactor");
    REQUIRE(!rejected_actor);
    REQUIRE(rejected_actor.error().code == carto::core::ErrorCode::invalid_argument);

    auto transaction = carto::project::ProjectTransaction::begin(document, journal, "editor-1");
    REQUIRE(transaction);
    REQUIRE(transaction.value().create_object("durable-object"));
    const auto committed = transaction.value().commit("scene.create");
    REQUIRE(committed);
    REQUIRE(committed.value().revision_before == carto::core::Revision{});
    REQUIRE(committed.value().revision_after == carto::core::Revision{1U});
    REQUIRE(committed.value().actor == "editor-1");
    REQUIRE(!transaction.value().active());
    REQUIRE(document.scene().size() == 1U);
    REQUIRE(journal.current_revision().value() == carto::core::Revision{1U});

    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 1U);
    REQUIRE(entries.value().front().event_type == "scene.create");
    const std::string payload(entries.value().front().payload.begin(), entries.value().front().payload.end());
    REQUIRE(payload.find("actor=editor-1") != std::string::npos);
    REQUIRE(payload.find("CARTOGRAPHER_PROJECT_TRANSACTION_V1") == 0U);

    const auto after_commit = transaction.value().create_object("must-not-publish");
    REQUIRE(!after_commit);
    REQUIRE(after_commit.error().code == carto::core::ErrorCode::invalid_state);

    auto blocked = carto::project::ProjectTransaction::begin(document, journal, "editor-2");
    REQUIRE(blocked);
    REQUIRE(blocked.value().create_object("blocked-object"));
    REQUIRE(carto::project::testing::access(document).create_object("concurrent-object"));
    const auto stale_commit = blocked.value().commit("scene.concurrent");
    REQUIRE(!stale_commit);
    REQUIRE(stale_commit.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(blocked.value().active());
    REQUIRE(journal.current_revision().value() == carto::core::Revision{1U});
}

void project_transaction_journals_evaluation_graph_reference_with_document_state() {
    TempDirectory temp;
    carto::project::ProjectDocument document;
    carto::journal::Journal journal(temp.path() / "recovery" / "journal.log");
    carto::journal::CheckpointStore checkpoints(temp.path() / "recovery" / "checkpoints");
    const std::string baseline = document.serialize();
    const auto asset_manifest = carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>("assets"), 6U});
    REQUIRE(checkpoints.save(
        journal, carto::core::Revision{},
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(baseline.data()), baseline.size()),
        asset_manifest));

    const auto digest = carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>("graph-snapshot"), 14U});
    auto transaction = carto::project::ProjectTransaction::begin(document, journal, "graph-editor");
    REQUIRE(transaction);
    REQUIRE(transaction.value().create_object("graph-bound"));
    REQUIRE(transaction.value().set_evaluation_graph_digest(digest));
    const auto committed = transaction.value().commit("graph.bind");
    REQUIRE(committed);
    REQUIRE(document.evaluation_graph_digest().has_value());
    REQUIRE(document.evaluation_graph_digest().value() == digest);

    const auto entries = journal.read_all();
    REQUIRE(entries);
    const std::string payload(entries.value().front().payload.begin(), entries.value().front().payload.end());
    REQUIRE(payload.find("EVALUATION_GRAPH_DIGEST " + digest.hex()) != std::string::npos);

    const auto checkpoint_text = document.serialize();
    const auto restored = carto::project::ProjectDocument::deserialize(checkpoint_text);
    REQUIRE(restored);
    REQUIRE(restored.value().evaluation_graph_digest().has_value());
    REQUIRE(restored.value().evaluation_graph_digest().value() == digest);

    const auto recovered = carto::project::recover_from_journal(checkpoints, journal);
    REQUIRE(recovered);
    REQUIRE(recovered.value().document.evaluation_graph_digest().has_value());
    REQUIRE(recovered.value().document.evaluation_graph_digest().value() == digest);

    REQUIRE(!carto::project::ProjectDocument::deserialize(
        checkpoint_text.substr(0, checkpoint_text.find("EVALUATION_GRAPH_DIGEST")) +
        "EVALUATION_GRAPH_DIGEST 0000000000000000000000000000000000000000000000000000000000000000\nEND\n"));
    std::string legacy_graph = checkpoint_text;
    const auto current_header = legacy_graph.find("CARTOGRAPHER_PROJECT 4");
    REQUIRE(current_header != std::string::npos);
    legacy_graph.replace(current_header, std::string("CARTOGRAPHER_PROJECT 4").size(),
                         "CARTOGRAPHER_PROJECT 2");
    const auto rejected_legacy_graph = carto::project::ProjectDocument::deserialize(legacy_graph);
    REQUIRE(!rejected_legacy_graph);
    REQUIRE(rejected_legacy_graph.error().code == carto::core::ErrorCode::version_mismatch);
}

void project_transaction_persists_topology_receipts_through_recovery() {
    TempDirectory temp;
    carto::project::ProjectDocument document;
    carto::journal::Journal journal(temp.path() / "recovery" / "journal.log");
    carto::journal::CheckpointStore checkpoints(temp.path() / "recovery" / "checkpoints");
    const std::string baseline = document.serialize();
    const auto asset_manifest = carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>("assets"), 6U});
    REQUIRE(checkpoints.save(
        journal, carto::core::Revision{},
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(baseline.data()), baseline.size()),
        asset_manifest));

    auto transaction = carto::project::ProjectTransaction::begin(
        document, journal, "topology-editor");
    REQUIRE(transaction);
    auto box = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(box);
    const auto mesh_asset = transaction.value().add_mesh(std::move(box.value()));
    REQUIRE(mesh_asset);
    const auto before = transaction.value().staged_document().meshes().at(mesh_asset.value());
    auto after = before;
    const auto face = after.faces_sorted().front().id;
    const auto inset = after.inset_face(face, 0.1);
    REQUIRE(inset);
    auto forged_after = before;
    auto forged_receipt = inset.value();
    forged_receipt.revision_before = forged_receipt.revision_before.next();
    forged_receipt.revision_after = forged_receipt.revision_after.next();
    const auto staged_revision_before_rejection = transaction.value().staged_revision();
    const auto forged = transaction.value().replace_mesh(
        mesh_asset.value(), std::move(forged_after), forged_receipt);
    REQUIRE(!forged);
    REQUIRE(forged.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(transaction.value().staged_revision() == staged_revision_before_rejection);
    REQUIRE(transaction.value().replace_mesh(mesh_asset.value(), std::move(after), inset.value()));
    REQUIRE(transaction.value().commit("mesh.inset"));

    REQUIRE(document.topology_receipts().size() == 1U);
    const auto entries = journal.read_all();
    REQUIRE(entries);
    REQUIRE(entries.value().size() == 1U);
    const std::string payload(entries.value().front().payload.begin(), entries.value().front().payload.end());
    REQUIRE(payload.find("TOPOLOGY_RECEIPTS 1") != std::string::npos);

    const auto recovered = carto::project::recover_from_journal(checkpoints, journal);
    REQUIRE(recovered);
    REQUIRE(recovered.value().document.serialize() == document.serialize());
    REQUIRE(recovered.value().document.topology_receipts().size() == 1U);
    REQUIRE(recovered.value().document.topology_receipts().front().mesh_asset == mesh_asset.value());
}

void project_recovery_replays_only_verified_snapshot_envelopes() {
    TempDirectory temp;
    carto::project::ProjectDocument document;
    carto::journal::Journal journal(temp.path() / "recovery" / "journal.log");

    auto first = carto::project::ProjectTransaction::begin(document, journal, "recovery-test");
    REQUIRE(first);
    REQUIRE(first.value().create_object("checkpointed"));
    REQUIRE(first.value().commit("scene.create"));

    carto::journal::CheckpointStore checkpoints(temp.path() / "recovery" / "checkpoints");
    const std::string checkpoint_text = document.serialize();
    const auto asset_manifest = carto::assets::sha256(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>("assets"), 6U));
    REQUIRE(checkpoints.save(
        journal,
        document.revision(),
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(checkpoint_text.data()), checkpoint_text.size()),
        asset_manifest));

    auto second = carto::project::ProjectTransaction::begin(document, journal, "recovery-test");
    REQUIRE(second);
    REQUIRE(second.value().create_object("replayed"));
    REQUIRE(second.value().commit("scene.create"));

    const auto recovered = carto::project::recover_from_journal(checkpoints, journal);
    REQUIRE(recovered);
    REQUIRE(recovered.value().checkpoint_revision == carto::core::Revision{1U});
    REQUIRE(recovered.value().recovered_revision == carto::core::Revision{2U});
    REQUIRE(recovered.value().replayed_entries == 1U);
    REQUIRE(recovered.value().document.scene().size() == 2U);

    carto::journal::Journal malformed_journal(temp.path() / "malformed" / "journal.log");
    const std::string baseline = carto::project::ProjectDocument{}.serialize();
    carto::journal::CheckpointStore malformed_checkpoints(temp.path() / "malformed" / "checkpoints");
    REQUIRE(malformed_checkpoints.save(
        malformed_journal,
        carto::core::Revision{},
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(baseline.data()), baseline.size()),
        asset_manifest));
    const std::string unsupported_payload = "not-a-project-snapshot\n";
    REQUIRE(malformed_journal.append(carto::journal::JournalAppend{
        carto::core::Revision{}, carto::core::Revision{1U}, "unsupported.event",
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(unsupported_payload.data()),
            unsupported_payload.size()),
        300U}));
    const auto rejected = carto::project::recover_from_journal(
        malformed_checkpoints, malformed_journal);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == carto::core::ErrorCode::unsupported);

    carto::journal::Journal metadata_journal(temp.path() / "metadata" / "journal.log");
    carto::journal::CheckpointStore metadata_checkpoints(temp.path() / "metadata" / "checkpoints");
    REQUIRE(metadata_checkpoints.save(
        metadata_journal,
        carto::core::Revision{},
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(baseline.data()), baseline.size()),
        asset_manifest));
    const std::string malformed_metadata =
        "CARTOGRAPHER_APPLICATION_MUTATION_V1\n"
        "actor=unexpected\n"
        "document_bytes=0\n";
    REQUIRE(metadata_journal.append(carto::journal::JournalAppend{
        carto::core::Revision{}, carto::core::Revision{1U}, "malformed.metadata",
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(malformed_metadata.data()),
            malformed_metadata.size()),
        301U}));
    const auto rejected_metadata = carto::project::recover_from_journal(
        metadata_checkpoints, metadata_journal);
    REQUIRE(!rejected_metadata);
    REQUIRE(rejected_metadata.error().code == carto::core::ErrorCode::validation_failed);
}

void project_recovery_can_materialize_latest_snapshot_without_checkpoint() {
    TempDirectory temp;
    carto::project::ProjectDocument document;
    carto::journal::Journal journal(temp.path() / "latest" / "journal.log");

    auto first = carto::project::ProjectTransaction::begin(document, journal, "latest-test");
    REQUIRE(first);
    REQUIRE(first.value().create_object("latest"));
    REQUIRE(first.value().commit("scene.create"));

    auto second = carto::project::ProjectTransaction::begin(document, journal, "latest-test");
    REQUIRE(second);
    REQUIRE(second.value().create_object("newer"));
    REQUIRE(second.value().commit("scene.create"));

    const auto recovered = carto::project::recover_latest_snapshot(journal);
    REQUIRE(recovered);
    REQUIRE(recovered.value().recovered_revision == carto::core::Revision{2U});
    REQUIRE(recovered.value().source_entries == 2U);
    REQUIRE(recovered.value().document.scene().size() == 2U);

    const auto empty = carto::project::recover_latest_snapshot(
        carto::journal::Journal(temp.path() / "missing" / "journal.log"));
    REQUIRE(!empty);
    REQUIRE(empty.error().code == carto::core::ErrorCode::not_found);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--journal-append-worker") {
        return journal_append_worker(argc, argv);
    }
    try {
        project_file_lock_serializes_writers_in_process();
        json_boundary_decodes_unicode_and_rejects_unpaired_surrogates();
        blob_store_is_content_addressed_and_integrity_checked();
        journal_is_revision_bound_replayable_and_hash_chained();
        journal_serializes_concurrent_appends_in_process();
        journal_serializes_concurrent_appends_across_processes(
            std::filesystem::absolute(std::filesystem::path(argv[0])));
        checkpoint_store_recovers_pending_entries_and_rejects_mismatch();
        package_manifest_is_human_readable_and_does_not_fabricate_database();
        package_persists_evaluation_graph_as_verified_content_addressed_state();
        package_concurrent_graph_writers_fail_closed_on_stale_manifests();
        project_transaction_publishes_only_after_journal_append();
        project_transaction_journals_evaluation_graph_reference_with_document_state();
        project_transaction_persists_topology_receipts_through_recovery();
        project_recovery_replays_only_verified_snapshot_envelopes();
        project_recovery_can_materialize_latest_snapshot_without_checkpoint();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
