#include <carto/assets/blob_store.hpp>
#include <carto/journal/checkpoint.hpp>
#include <carto/project/package.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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
    REQUIRE(!created.value().has_document_database());
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
    REQUIRE(document.create_object("concurrent-object"));
    const auto stale_commit = blocked.value().commit("scene.concurrent");
    REQUIRE(!stale_commit);
    REQUIRE(stale_commit.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(blocked.value().active());
    REQUIRE(journal.current_revision().value() == carto::core::Revision{1U});
}

} // namespace

int main() {
    try {
        blob_store_is_content_addressed_and_integrity_checked();
        journal_is_revision_bound_replayable_and_hash_chained();
        checkpoint_store_recovers_pending_entries_and_rejects_mismatch();
        package_manifest_is_human_readable_and_does_not_fabricate_database();
        project_transaction_publishes_only_after_journal_append();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
