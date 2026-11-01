#include <carto/project/accepted_autorig.hpp>
#include <carto/project/transaction.hpp>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>

using namespace carto;
using namespace carto::character;
namespace {
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
assets::Sha256Digest hash(std::string_view s) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}
AutorigConstraintProvenance human(std::string id) {
    return {std::move(id), "artist", "reviewed", "", AutorigConstraintOrigin::human_authored};
}
}
int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("carto-autorig-project-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    project::ProjectDocument doc;
    journal::Journal journal(path / "events.log");
    geometry::EditableMesh mesh;
    auto a = mesh.add_vertex({0,0,0}); auto b = mesh.add_vertex({0,2,0}); auto c = mesh.add_vertex({1,0,0});
    require(a && b && c && mesh.add_face({a.value(), b.value(), c.value()}), "mesh fixture");
    auto setup = project::ProjectTransaction::begin(doc, journal, "test");
    require(setup && setup.value().insert_mesh(51, mesh) && setup.value().commit("mesh"), "insert staged mesh");
    AutorigAnalysisRequest request{{51, hash("source")}, mesh.revision(), AutorigAxis::x, 0, 1e-7, {}, {}};
    auto analysis = analyze_autorig_features(mesh, request);
    require(static_cast<bool>(analysis), "analyze mesh");
    auto geometry_pin = autorig_mesh_geometry_digest(mesh);
    require(geometry_pin && geometry_pin.value() == analysis.value().mesh_geometry_digest,
            "geometry-only digest matches review analysis");
    AutorigConstraintSet constraints{request.source, mesh.revision(), analysis.value().analysis_digest,
        {{"root", "root", std::nullopt, AutorigPartSide::center},
         {"child", "child", std::string("root"), AutorigPartSide::center}},
        {{"a", "root", "root", AutorigPartSide::center, a.value(), human("root")},
         {"b", "child", "child", AutorigPartSide::center, b.value(), human("child")}}, {}, {}};
    auto opened = AutorigCandidateReview::begin(mesh, request, constraints);
    require(static_cast<bool>(opened), "review opens");
    auto review = std::move(opened).value();
    auto set = review.candidates();
    require(static_cast<bool>(review.select_candidate(set.ranked_candidates.front().digest)), "select");
    auto acceptance = review.accept_review(human("decision"));
    require(static_cast<bool>(acceptance), "accept");
    std::vector<AutorigFinalNode> nodes;
    std::uint64_t id = 100;
    for (const auto& joint : set.ranked_candidates.front().joints) {
        AutorigFinalNode node; node.preview_joint = joint.id; node.durable_joint = {id++};
        node.local_rest.translation = joint.parent ? core::Vec3d{0,2,0} : core::Vec3d{};
        node.node_class = kinematic::NodeClass::skinned; node.semantic_type = joint.semantic_role;
        nodes.push_back(node);
    }
    auto prepared = prepare_accepted_autorig_graph(mesh, request, set, acceptance.value(), nodes);
    require(static_cast<bool>(prepared), "prepare");
    kinematic::JointLimitSet limits;
    RigDefinition rig; rig.id = RigId{7}; rig.kinematic_graph_digest = prepared.value().graph_digest;
    rig.joint_limits.digest = limits.canonical_digest().value();
    kinematic::SkinBinding source; source.mesh_asset = 4; source.mesh_digest = hash("old mesh");
    source.graph_digest = prepared.value().graph_digest;
    source.influences.emplace(0, std::vector<kinematic::JointInfluence>{{nodes.front().durable_joint, 1.0}});
    SkinWeightLayer layer; layer.layer_id = RigLayerId{1}; layer.mesh_topology_digest = source.mesh_digest;
    layer.joint_graph_digest = source.graph_digest; layer.influenced_vertex_count = 1;
    layer.total_influence_count = 1; layer.provenance_digest = hash("authored");
    weights::WeightCorrespondence correspondence;
    require(correspondence.map(0, {{0,1}}) && correspondence.map(1, {{0,1}}) && correspondence.map(2, {{0,1}}), "correspondence");
    weights::WeightTransferOptions opts; opts.output_layer_id = RigLayerId{2};
    opts.output_mesh_asset = 51; opts.output_mesh_digest = request.source.content_digest;
    const auto before = doc.serialize();
    auto prepared_bundle = project::prepare_accepted_autorig_bundle(doc, request, set,
        acceptance.value(), nodes, rig, limits, source, layer, correspondence, {}, opts);
    require(static_cast<bool>(prepared_bundle) && doc.serialize() == before,
        "pure preparation validates without changing the published project");
    auto tx = project::ProjectTransaction::begin(doc, journal, "artist");
    auto accepted = tx.value().accept_autorig("body", doc.source_namespace(), doc.revision(), {}, request, set, acceptance.value(), nodes, rig, limits,
                                              source, layer, correspondence, {}, opts);
    require(accepted && accepted.value() == core::Revision{1}, "stage complete acceptance");
    require(doc.accepted_autorigs().empty(), "staging leaves published document unchanged");
    require(static_cast<bool>(tx.value().commit("accept.autorig")), "journal commit");
    const auto entry = doc.accepted_autorigs().at("body");
    require(entry.bytes == prepared_bundle.value(), "transaction uses the same prepared bundle");
    auto decoded = project::decode_accepted_autorig_bundle(entry.bytes);
    require(decoded && decoded.value().weights.output.binding.influences.size() == 3 &&
            decoded.value().weights.output.binding.influences.at(2).front().joint == nodes.front().durable_joint &&
            decoded.value().reviewed.acceptance.digest == acceptance.value().digest &&
            decoded.value().rig.kinematic_graph_digest == prepared.value().graph_digest,
            "decoded bundle retains rig, review and per-vertex weights");
    require(static_cast<bool>(project::validate_accepted_autorig_current(doc, decoded.value())), "current mesh passes readiness");
    auto stale = project::ProjectTransaction::begin(doc, journal, "artist");
    require(!stale.value().accept_autorig("body", doc.source_namespace(), doc.revision(), {}, request, set, acceptance.value(), nodes, rig, limits,
                                         source, layer, correspondence, {}, opts), "stale CAS refused");
    require(stale.value().staged_document().accepted_autorigs().at("body") == entry, "CAS failure atomic");
    auto wrong = opts; wrong.output_mesh_asset = 52;
    require(!project::prepare_accepted_autorig_bundle(doc, request, set, acceptance.value(), nodes,
        rig, limits, source, layer, correspondence, {}, wrong) && doc.accepted_autorigs().at("body") == entry,
        "pure preparation refuses mismatched target without mutation");
    require(!stale.value().accept_autorig("body", doc.source_namespace(), doc.revision(), core::Revision{1}, request, set, acceptance.value(), nodes, rig, limits,
                                         source, layer, correspondence, {}, wrong), "wrong target refused");
    weights::WeightCorrespondence out_of_range;
    require(static_cast<bool>(out_of_range.map(3, {{0,1}})), "out of bounds fixture");
    require(!stale.value().accept_autorig("body", doc.source_namespace(), doc.revision(), core::Revision{1}, request, set, acceptance.value(), nodes, rig, limits,
                                         source, layer, out_of_range, {}, opts), "invalid target index refused");
    auto changed_request = request; changed_request.expected_mesh_revision = core::Revision{1};
    require(!stale.value().accept_autorig("body", doc.source_namespace(), doc.revision(), core::Revision{1}, changed_request, set, acceptance.value(), nodes, rig, limits,
                                         source, layer, correspondence, {}, opts), "stale source revision refused");
    require(static_cast<bool>(stale.value().rollback()), "rollback refusals");
    const auto existing_events = journal.read_all();
    require(static_cast<bool>(existing_events), "read prior journal events");
    journal::JournalLimits tiny_limits;
    tiny_limits.max_payload_bytes = 0;
    for (const auto& event : existing_events.value())
        tiny_limits.max_payload_bytes = std::max(tiny_limits.max_payload_bytes,
            static_cast<std::uint64_t>(event.payload.size()));
    journal::Journal blocked_journal(path / "events.log", tiny_limits);
    auto blocked = project::ProjectTransaction::begin(doc, blocked_journal, "artist");
    require(static_cast<bool>(blocked), "begin before journal failure");
    require(static_cast<bool>(blocked.value().accept_autorig("other", doc.source_namespace(), doc.revision(), {}, request, set, acceptance.value(), nodes, rig, limits,
                                                    source, layer, correspondence, {}, opts)), "stage before journal failure");
    require(!blocked.value().commit("accept.autorig") && doc.accepted_autorigs().size() == 1,
            "journal failure does not publish staged bundle");
    auto saved = doc.save_atomic(path / "project.carto");
    auto reopened = project::ProjectDocument::load(path / "project.carto");
    require(saved && reopened && reopened.value().accepted_autorigs().at("body") == entry, "save/reopen");
    auto populated_v36 = reopened.value().serialize();
    require(populated_v36.starts_with("CARTOGRAPHER_PROJECT 37\n"), "current project header");
    populated_v36.replace(0U, std::string("CARTOGRAPHER_PROJECT 37\n").size(),
                          "CARTOGRAPHER_PROJECT 36\n");
    const auto migrated_v36 = project::ProjectDocument::deserialize(populated_v36);
    require(migrated_v36 && migrated_v36.value().accepted_autorigs().at("body") == entry &&
            migrated_v36.value().serialize().starts_with("CARTOGRAPHER_PROJECT 37\n"),
            "v36 project with a populated v1 bundle migrates without changing it");
    require(project::ProjectDocument::deserialize(before).value().accepted_autorigs().empty(), "pre-commit snapshot restores without bundle");
    auto remove = project::ProjectTransaction::begin(doc, journal, "artist");
    require(remove && remove.value().remove_mesh(51) && remove.value().commit("remove.mesh"), "remove source mesh");
    require(!project::validate_accepted_autorig_current(doc, decoded.value()), "removed source fails readiness");
    auto text = reopened.value().serialize(); auto at = text.find(entry.digest.hex());
    require(at != std::string::npos, "digest in project"); text[at] = text[at] == 'a' ? 'b' : 'a';
    require(!project::ProjectDocument::deserialize(text), "tampered bundle digest refused");
    auto old = project::ProjectDocument{}.serialize();
    old.replace(0, std::string("CARTOGRAPHER_PROJECT 37\n").size(), "CARTOGRAPHER_PROJECT 35\n");
    auto table = old.find("ACCEPTED_AUTORIGS 0\n"); require(table != std::string::npos, "empty table fixture");
    old.erase(table, std::string("ACCEPTED_AUTORIGS 0\n").size());
    auto migrated = project::ProjectDocument::deserialize(old);
    require(migrated && migrated.value().accepted_autorigs().empty() && migrated.value().schema_version() == 37,
            "v35 migration to empty v37 table");
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
}
