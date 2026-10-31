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
#include <carto/assets/blob_store.hpp>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace carto;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void u32(std::vector<std::uint8_t>& out, std::uint32_t n) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(n >> shift));
}
std::vector<std::uint8_t> fixture() {
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"part","mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],"materials":[{"name":"red","doubleSided":true}],"buffers":[{"byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}]})";
    while (json.size() % 4U) json.push_back(' ');
    std::vector<std::uint8_t> bin, out;
    for (float f : {0.F,0.F,0.F, 1.F,0.F,0.F, 0.F,1.F,0.F}) u32(bin, std::bit_cast<std::uint32_t>(f));
    u32(out, 0x46546c67U); u32(out, 2U);
    u32(out, static_cast<std::uint32_t>(28U + json.size() + bin.size()));
    u32(out, static_cast<std::uint32_t>(json.size())); u32(out, 0x4e4f534aU);
    out.insert(out.end(), json.begin(), json.end());
    u32(out, static_cast<std::uint32_t>(bin.size())); u32(out, 0x004e4942U);
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct Probe final : editor::CheckpointableEditorCommand<Probe> {
    int* value;
    bool* source_available;
    bool applied = false;
    Probe(int& v, bool& source) : value(&v), source_available(&source) {}
    core::Result<void> execute() override {
        if (!*source_available || applied) return core::Result<void>::failure(
            {core::ErrorCode::invalid_state, "source unavailable or command applied"});
        ++*value; applied = true; return core::Result<void>::success();
    }
    core::Result<void> undo() override {
        if (!applied) return core::Result<void>::failure(
            {core::ErrorCode::invalid_state, "command not applied"});
        --*value; applied = false; return core::Result<void>::success();
    }
    std::string label() const override { return "probe"; }
};
struct UnsupportedProbe final : editor::EditorCommand {
    int* value;
    explicit UnsupportedProbe(int& v) : value(&v) {}
    core::Result<void> execute() override { ++*value; return core::Result<void>::success(); }
    core::Result<void> undo() override { --*value; return core::Result<void>::success(); }
    std::string label() const override { return "uncheckpointed probe"; }
};
void checkpoint_protocol() {
    int value = 0; bool source = true;
    editor::CommandBus bus;
    check(bool(bus.execute(std::make_unique<Probe>(value, source))), "execute probe");
    bus.finalize_last_execute();
    source = false;
    check(bool(bus.prepare_durable_undo()) && value == 0, "prepare undo on copy");
    value = 1; // ApplicationSession restores its pre-operation document on append failure.
    bus.abort_durable_history();
    check(bus.undo_count() == 1 && bus.redo_count() == 0, "abort preserves original stack");
    check(bool(bus.prepare_durable_undo()), "original command state remains applied");
    bus.commit_durable_history();
    check(!bus.prepare_durable_redo() && bus.redo_count() == 1, "real redo still checks source");
    source = true;
    check(bool(bus.prepare_durable_redo()), "redo can be prepared when source returns");
    value = 0;
    bus.abort_durable_history();
    check(bus.redo_count() == 1 && bool(bus.prepare_durable_redo()), "redo command checkpoint survived abort");
    bus.commit_durable_history();
    check(value == 1 && bus.undo_count() == 1, "redo committed exactly once");
    editor::CommandBus unsupported;
    check(bool(unsupported.execute(std::make_unique<UnsupportedProbe>(value))), "execute uncheckpointed probe");
    unsupported.finalize_last_execute();
    check(!unsupported.prepare_durable_undo() && value == 2 && unsupported.undo_count() == 1,
          "uncheckpointed command fails before mutation");
}
void private_tool_checkpoints() {
    project::ProjectDocument document;
    editor::SelectionState selection;
    editor::ToolContext context(editor::ProjectCommandAdmission{}, document, selection);
    auto loop = context.make_loop_cut_selected_edge_command();
    auto knife = context.make_knife_cut_selected_face_command({{0,0,0}, {1,0,0}});
    check(loop && knife && loop.value()->checkpoint() && knife.value()->checkpoint(),
          "private loop and knife commands support checkpointing");
}
void scenario(const std::filesystem::path& dir) {
    using namespace application;
    auto path = dir / "history.carto";
    auto journal = std::filesystem::path(path.string() + ".journal");
    auto parked_journal = dir / "journal-backup";
    ApplicationSession session;
    const auto dispatch = [&session](const ApplicationAction& action) {
        return HumanApplicationAccess::dispatch(session, action);
    };
    check(bool(dispatch(NewProjectAction{"history"})), "new project");
    check(bool(dispatch(SaveProjectAction{path})), "save project");
    assets::BlobStore store(dir / ".carto-assets" / "source-blobs");
    auto blob = store.put(fixture(), "model/gltf-binary");
    check(bool(blob), "stage source");
    const auto initial = session.snapshot();
    ImportGltfSceneAction action{initial.project_generation, initial.project_revision,
        initial.source_namespace, "scene", core::Revision{}, blob.value().digest, true, "fixture rights"};
    check(bool(dispatch(action)), "import scene");
    const auto imported = session.snapshot();
    const auto imported_document = session.document_.serialize();
    const auto source_path = store.path_for(blob.value().digest);
    const auto parked_source = dir / "source-backup";
    std::filesystem::rename(source_path, parked_source);
    const auto journal_before = read(journal);
    std::filesystem::rename(journal, parked_journal);
    std::filesystem::create_directory(journal);
    check(!dispatch(UndoAction{}), "failed journal undo rejected");
    check(session.document_.serialize() == imported_document,
          "failed undo preserves every serialized authoring field");
    const auto failed = session.snapshot();
    check(failed.project_revision == imported.project_revision &&
          failed.undo_count == imported.undo_count && failed.redo_count == imported.redo_count &&
          failed.source_assets.front().source_generation == imported.source_assets.front().source_generation &&
          failed.source_assets.front().document_products == imported.source_assets.front().document_products &&
          failed.source_asset_tombstone_count == imported.source_asset_tombstone_count &&
          failed.objects.size() == imported.objects.size(), "failed undo preserves state and stacks");
    check(read(parked_journal) == journal_before, "failed undo leaves original journal bytes unchanged");
    std::filesystem::remove(journal);
    std::filesystem::rename(parked_journal, journal);
    check(read(journal) == journal_before, "restored journal retains pre-refusal bytes");
    check(bool(dispatch(UndoAction{})), "retry undo with missing source");
    const auto undone = session.snapshot();
    const auto undone_document = session.document_.serialize();
    check(undone.source_assets.empty() && undone.source_asset_tombstone_count == 1, "undo keeps tombstone");
    check(!dispatch(RedoAction{}), "real redo verifies missing source");
    check(session.document_.serialize() == undone_document,
          "missing-source redo preserves every serialized authoring field");
    check(session.snapshot().redo_count == undone.redo_count, "failed redo keeps stack");
    std::filesystem::rename(parked_source, source_path);
    const auto before_redo_journal = read(journal);
    std::filesystem::rename(journal, parked_journal);
    std::filesystem::create_directory(journal);
    check(!dispatch(RedoAction{}), "failed journal redo rejected");
    check(session.document_.serialize() == undone_document,
          "failed redo preserves every serialized authoring field");
    const auto failed_redo = session.snapshot();
    check(failed_redo.project_revision == undone.project_revision && failed_redo.redo_count == undone.redo_count &&
          failed_redo.source_assets.empty() && failed_redo.source_asset_tombstone_count == 1,
          "failed redo preserves state and source lineage");
    check(read(parked_journal) == before_redo_journal, "failed redo leaves journal bytes unchanged");
    std::filesystem::remove(journal);
    std::filesystem::rename(parked_journal, journal);
    check(read(journal) == before_redo_journal,
          "restored redo journal retains pre-refusal bytes");
    check(bool(dispatch(RedoAction{})), "retry redo with restored source");
    check(session.snapshot().source_assets.front().source_generation == core::Revision{2},
          "failed redo did not consume a generation");
}

void owner_replay_boundary(const std::filesystem::path& root, bool face) {
    using namespace application;
    auto path = root / (face ? "face.carto" : "vertex.carto");
    auto journal = std::filesystem::path(path.string() + ".journal");
    ApplicationSession session;
    const auto dispatch = [&session](const ApplicationAction& action) {
        return HumanApplicationAccess::dispatch(session, action);
    };
    check(bool(dispatch(NewProjectAction{"owner"})) && bool(dispatch(SaveProjectAction{path})) &&
          bool(dispatch(CreateBoxAction{"box", {2,2,2}})), "owner fixture");
    const auto owner = session.snapshot().objects.front().object.id;
    check(bool(dispatch(SetSelectionModeAction{face ? editor::SelectionMode::face : editor::SelectionMode::vertex})), "selection mode");
    if (face) check(bool(dispatch(SelectFaceAction{owner, geometry::FaceId{1}})), "face selection");
    else check(bool(dispatch(SelectVertexAction{owner, geometry::VertexId{1}})), "vertex selection");
    editor::ToolArguments arguments;
    arguments.distance = 0.25; arguments.position = {0.125,0.125,0.125};
    check(bool(dispatch(InvokeToolAction{face ? "mesh.extrude-face" : "mesh.set-vertex-position", arguments})), "unlocked edit");
    auto* node = const_cast<scene::SceneObject*>(session.document_.scene().find(owner));
    const auto asset = node->mesh_asset;
    node->mesh_asset.reset(); // Deliberate test-only stale binding injection.
    auto before = session.document_.serialize();
    auto journal_before = read(journal);
    auto history = session.snapshot();
    check(!dispatch(UndoAction{}) && session.document_.serialize() == before &&
          session.snapshot().undo_count == history.undo_count && read(journal) == journal_before,
          "rebound owner refuses undo atomically");
    node = const_cast<scene::SceneObject*>(session.document_.scene().find(owner));
    node->mesh_asset = asset;
    check(bool(dispatch(UndoAction{})), "undo after restoring binding");
    const auto saved_node = *session.document_.scene().find(owner);
    check(bool(session.document_.scene_.remove_object(owner)), "inject missing owner");
    before = session.document_.serialize(); journal_before = read(journal); history = session.snapshot();
    check(!dispatch(RedoAction{}) && session.document_.serialize() == before &&
          session.snapshot().redo_count == history.redo_count && read(journal) == journal_before,
          "removed owner refuses redo atomically");
    check(bool(session.document_.scene_.insert_object(saved_node)), "restore owner");
    check(bool(dispatch(RedoAction{})), "redo after restoring owner");
}
}
int main() {
    const auto dir = std::filesystem::temp_directory_path() /
        ("carto-history-sprint2-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try { checkpoint_protocol(); private_tool_checkpoints(); std::filesystem::create_directories(dir); scenario(dir); owner_replay_boundary(dir, false); owner_replay_boundary(dir, true); std::filesystem::remove_all(dir); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "unknown history test failure\n"; return 1; }
    return 0;
}
