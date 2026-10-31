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
}
int main() {
    const auto dir = std::filesystem::temp_directory_path() /
        ("carto-provenance-sprint2-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try { std::filesystem::create_directories(dir); legacy_recovery(dir); human_attribution_gate(dir); malformed_v2(dir); std::filesystem::remove_all(dir); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "unknown provenance test failure\n"; return 1; }
    return 0;
}
