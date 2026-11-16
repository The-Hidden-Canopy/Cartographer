#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include <carto/application/application.hpp>
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <carto/journal/journal.hpp>
#include <carto/project/recovery.hpp>
#include <carto/project/transaction.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace carto;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void legacy_recovery(const std::filesystem::path& root) {
    auto document = project::ProjectDocument::create("legacy");
    check(bool(document), "legacy project");
    const std::string snapshot = document.value().serialize();
    const std::string payload = "CARTOGRAPHER_APPLICATION_MUTATION_V1\noperation=Legacy edit\n"
        "document_bytes=" + std::to_string(snapshot.size()) + "\n" + snapshot;
    const auto path = root / "legacy.carto";
    check(bool(document.value().save_atomic(path)), "save legacy project");
    journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const std::span<const std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    check(bool(journal.append({core::Revision{}, document.value().revision(),
                               "cartographer.application_action", bytes, 1U})), "write legacy journal");
    const auto recovered = project::recover_latest_snapshot(journal);
    check(bool(recovered) && recovered.value().document.serialize() == snapshot, "legacy V1 recovery");
    auto evidence = application::ApplicationSession::inspect_recovery(path);
    check(evidence && !evidence.value().journal_entries.back().source &&
          evidence.value().journal_entries.back().provenance_id.empty(), "V1 attribution remains unknown");
}
void human_attribution_gate(const std::filesystem::path& root) {
    using namespace application;
    const auto path = root / "new.carto";
    ApplicationSession session;
    check(bool(HumanApplicationAccess::dispatch(session, NewProjectAction{"journal"})), "new project");
    check(bool(HumanApplicationAccess::dispatch(session, SaveProjectAction{path})), "save project");
    check(bool(HumanApplicationAccess::dispatch(session, CreateBoxAction{"box", {1,1,1}})), "human action");
    journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto entries = journal.read_all();
    check(entries && !entries.value().empty(), "journal entries");
    const auto& entry = entries.value().back();
    const std::string payload(entry.payload.begin(), entry.payload.end());
    check(entry.event_type == "cartographer.application_action" &&
          payload.starts_with("CARTOGRAPHER_APPLICATION_MUTATION_V2\n") &&
          payload.find("source=human\n") != std::string::npos, "V2 human attribution");
    check(bool(project::recover_latest_snapshot(journal)), "V2 cold recovery");
    // Test-only admission access exercises the existing source/provenance path;
    // it is not a new production AI authorization.
    const std::string request = "request\nidentity";
    check(bool(session.dispatch(HumanActionAdmission{}, CreateBoxAction{"AI reviewed", {1,1,1}},
                                OperationSource::ai_proposal, request)), "AI source attribution");
    auto evidence = ApplicationSession::inspect_recovery(path);
    check(evidence && evidence.value().journal_entries.back().source == OperationSource::ai_proposal &&
          evidence.value().journal_entries.back().provenance_id == request,
          "AI provenance is exact and safely encoded");
    check(bool(HumanApplicationAccess::dispatch(session, SaveProjectAction{})), "save AI state");
    ApplicationSession reopened;
    check(bool(HumanApplicationAccess::dispatch(reopened, OpenProjectAction{path})), "cold reopen V2");
    evidence = ApplicationSession::inspect_recovery(path);
    check(evidence && evidence.value().journal_entries.back().source == OperationSource::ai_proposal &&
          evidence.value().journal_entries.back().provenance_id == request, "reopen retains AI attribution");
    const auto before = session.snapshot();
    check(!session.dispatch(HumanActionAdmission{}, CreateBoxAction{"missing provenance", {1,1,1}},
                            OperationSource::ai_proposal, {}), "empty AI identity refused");
    check(session.snapshot().project_revision == before.project_revision &&
          session.snapshot().undo_count == before.undo_count, "missing identity preserves history");
}

void malformed_v2(const std::filesystem::path& root) {
    auto document = project::ProjectDocument::create("malformed");
    check(bool(document), "malformed fixture project");
    const auto snapshot = document.value().serialize();
    const std::vector<std::string> metadata{
        "operation=edit\nsource=unknown\nprovenance_hex=\n",
        "operation=edit\nsource=ai_proposal\nprovenance_hex=\n",
        "operation=edit\nsource=human\nprovenance_hex=0z\n",
        "operation=edit\nsource=human\nsource=human\nprovenance_hex=\n",
        "operation=edit\nsource=human\nprovenance_hex=0\n",
        "operation=edit\nsource=human\nprovenance_hex=\nextra=bad\n"};
    for (std::size_t i = 0; i < metadata.size(); ++i) {
        journal::Journal journal(root / ("bad-" + std::to_string(i) + ".journal"));
        const std::string payload = "CARTOGRAPHER_APPLICATION_MUTATION_V2\n" + metadata[i] +
            "document_bytes=" + std::to_string(snapshot.size()) + "\n" + snapshot;
        const std::span<const std::uint8_t> bytes(
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
        check(bool(journal.append({core::Revision{}, document.value().revision(),
                                   "cartographer.application_action", bytes, 1U})), "write malformed evidence");
        check(!project::recover_latest_snapshot(journal), "malformed V2 metadata refused");
    }
}

void transaction_journal_open(const std::filesystem::path& root) {
    using namespace application;
    const auto path = root / "transaction.carto";
    auto document = project::ProjectDocument::create("transaction");
    check(bool(document), "transaction project");
    journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto initial = document.value().serialize();
    const auto checkpoint = "CARTOGRAPHER_PROJECT_SNAPSHOT_V1\ndocument_bytes=" +
        std::to_string(initial.size()) + "\n" + initial;
    check(bool(journal.append({{}, document.value().revision(), "cartographer.snapshot",
        {reinterpret_cast<const std::uint8_t*>(checkpoint.data()), checkpoint.size()}, {}})), "initial journal");
    auto tx = project::ProjectTransaction::begin(document.value(), journal, "artist");
    check(bool(tx) && bool(tx.value().create_object("fur")), "transaction edit");
    check(bool(tx.value().commit("study.fur")), "transaction commit");
    check(bool(document.value().save_atomic(path)), "transaction save");
    const auto before = journal.read_all();
    check(before && before.value().back().event_type == "study.fur", "transaction event retained");
    ApplicationSession session;
    check(bool(HumanApplicationAccess::dispatch(session, OpenProjectAction{path})), "open transaction journal");
    check(bool(HumanApplicationAccess::dispatch(session, CreateBoxAction{"continued", {1,1,1}})),
        "edit after transaction open");
    const auto after = journal.read_all();
    check(after && after.value().size() == before.value().size()+1 &&
        after.value()[before.value().size()-1].payload == before.value().back().payload &&
        after.value().back().event_type == "cartographer.application_action", "journal lineage retained");
    check(bool(HumanApplicationAccess::dispatch(session, SaveProjectAction{})), "save continued edit");
    ApplicationSession reopened;
    check(bool(HumanApplicationAccess::dispatch(reopened, OpenProjectAction{path})), "reopen continued edit");
    check(reopened.snapshot().project_revision == session.snapshot().project_revision,
        "continued project revision preserved");
}

void malformed_transaction_open(const std::filesystem::path& root) {
    using namespace application;
    const std::vector<std::pair<std::string,std::string>> cases{
        {"actor=\noperation=study.fur\n", "study.fur"},
        {"actor=bad\tactor\noperation=study.fur\n", "study.fur"},
        {"actor=" + std::string(257,'a') + "\noperation=study.fur\n", "study.fur"},
        {"actor=artist\noperation=other\n", "study.fur"},
        {"actor=artist\noperation=study.fur\nextra=bad\n", "study.fur"},
        {"actor=artist\noperation=study.fur\n", "cartographer.application_action"}};
    for (std::size_t i=0; i<cases.size(); ++i) {
        auto document = project::ProjectDocument::create("invalid transaction");
        check(bool(document), "invalid transaction project");
        const auto path = root / ("invalid-transaction-"+std::to_string(i)+".carto");
        check(bool(document.value().save_atomic(path)), "save invalid transaction fixture");
        const auto snapshot = document.value().serialize();
        const auto payload = "CARTOGRAPHER_PROJECT_TRANSACTION_V1\n"+cases[i].first+
            "document_bytes="+std::to_string(snapshot.size())+"\n"+snapshot;
        journal::Journal journal(std::filesystem::path(path.string()+".journal"));
        check(bool(journal.append({{},document.value().revision(),cases[i].second,
            {reinterpret_cast<const std::uint8_t*>(payload.data()),payload.size()}, {}})), "append malformed transaction");
        ApplicationSession session;
        check(!HumanApplicationAccess::dispatch(session,OpenProjectAction{path}), "invalid transaction refused");
    }
    auto saved = project::ProjectDocument::create("saved");
    auto other = project::ProjectDocument::create("different same-revision document");
    check(saved && other && saved.value().revision() == other.value().revision(), "stale fixture");
    const auto path = root / "stale-transaction.carto";
    check(bool(saved.value().save_atomic(path)), "save stale fixture");
    const auto snapshot = other.value().serialize();
    const auto payload = "CARTOGRAPHER_PROJECT_TRANSACTION_V1\nactor=artist\noperation=study.fur\n"
        "document_bytes=" + std::to_string(snapshot.size()) + "\n" + snapshot;
    journal::Journal journal(std::filesystem::path(path.string()+".journal"));
    check(bool(journal.append({{},saved.value().revision(),"study.fur",
        {reinterpret_cast<const std::uint8_t*>(payload.data()),payload.size()}, {}})), "append stale snapshot");
    ApplicationSession session;
    check(!HumanApplicationAccess::dispatch(session,OpenProjectAction{path}), "same-revision different snapshot refused");
}
}
int main() {
    const auto dir = std::filesystem::temp_directory_path() /
        ("carto-provenance-sprint2-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try { std::filesystem::create_directories(dir); legacy_recovery(dir); human_attribution_gate(dir); malformed_v2(dir); transaction_journal_open(dir); malformed_transaction_open(dir); std::filesystem::remove_all(dir); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "unknown provenance test failure\n"; return 1; }
    return 0;
}
