#include <carto/application/application.hpp>
#include <carto/project/recovery.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <memory>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace carto::application {

assets::Sha256Digest canonical_mesh_geometry_digest(
    const geometry::EditableMesh& mesh) {
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << "CARTOGRAPHER_MESH_GEOMETRY 1\n";
    const auto vertices = mesh.vertices_sorted();
    canonical << "VERTICES " << vertices.size() << '\n' << std::hex << std::setfill('0');
    for (const auto& vertex : vertices) {
        canonical << vertex.id.value << ' '
            << std::setw(16) << std::bit_cast<std::uint64_t>(vertex.position.x) << ' '
            << std::setw(16) << std::bit_cast<std::uint64_t>(vertex.position.y) << ' '
            << std::setw(16) << std::bit_cast<std::uint64_t>(vertex.position.z) << '\n';
    }
    const auto faces = mesh.faces_sorted();
    canonical << "FACES " << std::dec << faces.size() << '\n';
    for (const auto& face : faces) {
        canonical << face.id.value << ' ' << face.vertices.size();
        for (const auto vertex : face.vertices) canonical << ' ' << vertex.value;
        canonical << '\n';
    }
    const std::string bytes = canonical.str();
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

namespace {

constexpr std::size_t kMaxNativeStructureMeshElements = 100'000U;
constexpr std::size_t kMaxNativeStructureMatches = 4096U;

struct NativeStructureSignatureIndex {
    assets::Sha256Digest object;
    std::map<geometry::FaceId, assets::Sha256Digest> faces;
    std::map<geometry::VertexId, assets::Sha256Digest> vertices;
};

core::Diagnostic invalid_state(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

assets::Sha256Digest structure_digest(std::string_view bytes) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

core::Result<NativeStructureSignatureIndex> build_native_structure_signature_index(
    const geometry::EditableMesh& mesh,
    bool include_face_signatures,
    bool include_vertex_signatures,
    std::optional<geometry::FaceId> only_face = std::nullopt,
    std::optional<geometry::VertexId> only_vertex = std::nullopt) {
    if (mesh.vertex_count() > kMaxNativeStructureMeshElements ||
        mesh.face_count() > kMaxNativeStructureMeshElements ||
        mesh.edge_count() > kMaxNativeStructureMeshElements) {
        return core::Result<NativeStructureSignatureIndex>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "structural correspondence exceeds the bounded mesh element limit"));
    }
    if (auto valid = mesh.validate(); !valid) {
        return core::Result<NativeStructureSignatureIndex>::failure(valid.error().with_context(
            "structural correspondence source mesh"));
    }

    const auto vertices = mesh.vertices_sorted();
    const auto faces = mesh.faces_sorted();
    std::map<geometry::VertexId, std::vector<std::size_t>> incident_face_arities;
    std::map<geometry::FaceId, std::vector<std::size_t>> neighboring_face_arities;
    std::map<std::pair<geometry::VertexId, geometry::VertexId>, std::vector<geometry::FaceId>>
        edge_faces;
    std::map<geometry::VertexId, std::vector<geometry::VertexId>> neighbors;
    std::vector<std::size_t> face_arities;
    face_arities.reserve(faces.size());
    for (const auto& face : faces) {
        face_arities.push_back(face.vertices.size());
        for (const auto vertex : face.vertices) {
            incident_face_arities[vertex].push_back(face.vertices.size());
        }
        for (std::size_t index = 0U; index < face.vertices.size(); ++index) {
            auto first = face.vertices[index];
            auto second = face.vertices[(index + 1U) % face.vertices.size()];
            if (second < first) std::swap(first, second);
            edge_faces[{first, second}].push_back(face.id);
            neighbors[first].push_back(second);
            neighbors[second].push_back(first);
        }
    }

    std::size_t boundary_edge_count = 0U;
    std::vector<std::size_t> edge_face_counts;
    edge_face_counts.reserve(edge_faces.size());
    for (auto& [edge, attached_faces] : edge_faces) {
        static_cast<void>(edge);
        std::sort(attached_faces.begin(), attached_faces.end());
        attached_faces.erase(std::unique(attached_faces.begin(), attached_faces.end()),
            attached_faces.end());
        if (attached_faces.empty() || attached_faces.size() > 2U) {
            return core::Result<NativeStructureSignatureIndex>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "structural correspondence requires manifold edge incidence"));
        }
        edge_face_counts.push_back(attached_faces.size());
        if (attached_faces.size() == 1U) {
            ++boundary_edge_count;
        } else {
            const auto first = mesh.find_face(attached_faces[0]);
            const auto second = mesh.find_face(attached_faces[1]);
            if (first == nullptr || second == nullptr) {
                return core::Result<NativeStructureSignatureIndex>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "structural correspondence found a missing adjacent face"));
            }
            neighboring_face_arities[first->id].push_back(second->vertices.size());
            neighboring_face_arities[second->id].push_back(first->vertices.size());
        }
    }

    for (auto& [vertex, arities] : incident_face_arities) {
        static_cast<void>(vertex);
        std::sort(arities.begin(), arities.end());
    }
    for (auto& [face, arities] : neighboring_face_arities) {
        static_cast<void>(face);
        std::sort(arities.begin(), arities.end());
    }
    for (auto& [vertex, adjacent] : neighbors) {
        static_cast<void>(vertex);
        std::sort(adjacent.begin(), adjacent.end());
        adjacent.erase(std::unique(adjacent.begin(), adjacent.end()), adjacent.end());
    }
    std::sort(face_arities.begin(), face_arities.end());
    std::sort(edge_face_counts.begin(), edge_face_counts.end());

    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << "CARTOGRAPHER_NATIVE_STRUCTURE 1\n"
        << "M " << vertices.size() << ' ' << faces.size() << ' '
        << edge_faces.size() << ' ' << boundary_edge_count << '\n';
    const auto append_values = [&canonical](const auto& values) {
        canonical << values.size();
        for (const auto value : values) canonical << ' ' << value;
        canonical << '\n';
    };
    append_values(face_arities);
    append_values(edge_face_counts);
    std::vector<std::size_t> vertex_valences;
    vertex_valences.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        const auto found = incident_face_arities.find(vertex.id);
        vertex_valences.push_back(found == incident_face_arities.end() ? 0U : found->second.size());
    }
    std::sort(vertex_valences.begin(), vertex_valences.end());
    append_values(vertex_valences);

    NativeStructureSignatureIndex index;
    index.object = structure_digest(canonical.str());
    if (include_face_signatures) {
        for (const auto& face : faces) {
            if (only_face.has_value() && face.id != *only_face) continue;
            std::vector<std::size_t> local_vertex_valences;
            local_vertex_valences.reserve(face.vertices.size());
            std::size_t local_boundary_edges = 0U;
            for (std::size_t boundary_index = 0U;
                 boundary_index < face.vertices.size(); ++boundary_index) {
                const auto vertex = face.vertices[boundary_index];
                const auto incident = incident_face_arities.find(vertex);
                local_vertex_valences.push_back(
                    incident == incident_face_arities.end() ? 0U : incident->second.size());
                auto first = vertex;
                auto second = face.vertices[(boundary_index + 1U) % face.vertices.size()];
                if (second < first) std::swap(first, second);
                const auto edge = edge_faces.find({first, second});
                if (edge == edge_faces.end()) {
                    return core::Result<NativeStructureSignatureIndex>::failure(core::Diagnostic(
                        core::ErrorCode::stale_data,
                        "structural correspondence face edge is missing"));
                }
                if (edge->second.size() == 1U) ++local_boundary_edges;
            }
            std::sort(local_vertex_valences.begin(), local_vertex_valences.end());
            std::ostringstream local;
            local.imbue(std::locale::classic());
            local << "CARTOGRAPHER_NATIVE_COMPONENT 1 FACE " << index.object.hex() << '\n'
                << face.vertices.size() << ' ' << local_boundary_edges << '\n';
            const auto append_local_values = [&local](const auto& values) {
                local << values.size();
                for (const auto value : values) local << ' ' << value;
                local << '\n';
            };
            append_local_values(local_vertex_valences);
            const auto adjacent = neighboring_face_arities.find(face.id);
            if (adjacent == neighboring_face_arities.end()) {
                append_local_values(std::vector<std::size_t>{});
            } else {
                append_local_values(adjacent->second);
            }
            index.faces.emplace(face.id, structure_digest(local.str()));
        }
    }

    if (include_vertex_signatures) {
        for (const auto& vertex : vertices) {
            if (only_vertex.has_value() && vertex.id != *only_vertex) continue;
            const auto incident = incident_face_arities.find(vertex.id);
            const auto adjacent = neighbors.find(vertex.id);
            std::vector<std::size_t> neighbor_valences;
            std::size_t local_boundary_edges = 0U;
            if (adjacent != neighbors.end()) {
                neighbor_valences.reserve(adjacent->second.size());
                for (const auto neighbor : adjacent->second) {
                    const auto neighbor_incident = incident_face_arities.find(neighbor);
                    neighbor_valences.push_back(neighbor_incident == incident_face_arities.end()
                        ? 0U : neighbor_incident->second.size());
                    auto first = vertex.id;
                    auto second = neighbor;
                    if (second < first) std::swap(first, second);
                    const auto edge = edge_faces.find({first, second});
                    if (edge == edge_faces.end()) {
                        return core::Result<NativeStructureSignatureIndex>::failure(core::Diagnostic(
                            core::ErrorCode::stale_data,
                            "structural correspondence vertex edge is missing"));
                    }
                    if (edge->second.size() == 1U) ++local_boundary_edges;
                }
            }
            std::sort(neighbor_valences.begin(), neighbor_valences.end());
            std::ostringstream local;
            local.imbue(std::locale::classic());
            local << "CARTOGRAPHER_NATIVE_COMPONENT 1 VERTEX " << index.object.hex() << '\n'
                << (incident == incident_face_arities.end() ? 0U : incident->second.size())
                << ' ' << local_boundary_edges << '\n';
            const auto append_local_values = [&local](const auto& values) {
                local << values.size();
                for (const auto value : values) local << ' ' << value;
                local << '\n';
            };
            if (incident == incident_face_arities.end()) {
                append_local_values(std::vector<std::size_t>{});
            } else {
                append_local_values(incident->second);
            }
            append_local_values(neighbor_valences);
            index.vertices.emplace(vertex.id, structure_digest(local.str()));
        }
    }
    return core::Result<NativeStructureSignatureIndex>::success(std::move(index));
}

core::Result<assets::Sha256Digest> structure_signature_for_mesh(
    const geometry::EditableMesh& mesh,
    const std::variant<std::monostate, geometry::FaceId, geometry::VertexId>& component) {
    const bool include_faces = std::holds_alternative<geometry::FaceId>(component);
    const bool include_vertices = std::holds_alternative<geometry::VertexId>(component);
    auto index = build_native_structure_signature_index(
        mesh, include_faces, include_vertices,
        include_faces ? std::optional<geometry::FaceId>{
            std::get<geometry::FaceId>(component)} : std::nullopt,
        include_vertices ? std::optional<geometry::VertexId>{
            std::get<geometry::VertexId>(component)} : std::nullopt);
    if (!index) return core::Result<assets::Sha256Digest>::failure(index.error());
    if (std::holds_alternative<std::monostate>(component)) {
        return core::Result<assets::Sha256Digest>::success(index.value().object);
    }
    if (std::holds_alternative<geometry::FaceId>(component)) {
        const auto found = index.value().faces.find(std::get<geometry::FaceId>(component));
        if (found != index.value().faces.end()) {
            return core::Result<assets::Sha256Digest>::success(found->second);
        }
    } else if (std::holds_alternative<geometry::VertexId>(component)) {
        const auto found = index.value().vertices.find(std::get<geometry::VertexId>(component));
        if (found != index.value().vertices.end()) {
            return core::Result<assets::Sha256Digest>::success(found->second);
        }
    }
    return core::Result<assets::Sha256Digest>::failure(core::Diagnostic(
        core::ErrorCode::stale_data,
        "structural correspondence target is missing from its source mesh"));
}

core::Result<void> verify_source_blob(
    const std::filesystem::path& project_path,
    const assets::SourceImportRecord& record) {
    if (record.kind != assets::SourceAssetKind::mesh ||
        record.importer_version != 1U ||
        (record.importer_identity != "carto.import.obj" &&
         record.importer_identity != "carto.import.ply" &&
         record.importer_identity != "carto.import.stl")) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "source-backed mesh import requires a supported built-in importer"));
    }
    constexpr std::uintmax_t kMaxMeshSourceBytes = 128ULL * 1024ULL * 1024ULL;
    std::error_code error;
    const auto requested_root = project_path.parent_path().empty()
        ? std::filesystem::current_path(error)
        : project_path.parent_path();
    if (error) return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::io_error, "unable to resolve the saved project directory"));
    const auto project_root = std::filesystem::weakly_canonical(requested_root, error);
    if (error || project_root.empty()) return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::io_error, "unable to resolve the saved project directory"));

    assets::BlobStore store(project_root / ".carto-assets" / "source-blobs");
    const auto expected_path = store.path_for(record.source_digest);
    const auto stored_path = std::filesystem::canonical(expected_path, error);
    if (error) return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::not_found, "source-backed mesh bytes are missing"));
    const auto relative_path = stored_path.lexically_relative(project_root);
    if (relative_path.empty() || relative_path.is_absolute() ||
        relative_path.generic_string() != record.relative_source_path ||
        std::any_of(relative_path.begin(), relative_path.end(), [](const auto& component) {
            return component == "..";
        })) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "source-backed mesh path is not the project-local content-addressed blob"));
    }
    const auto size = std::filesystem::file_size(stored_path, error);
    if (error || size > kMaxMeshSourceBytes) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "source-backed mesh blob is unavailable or exceeds the importer limit"));
    }
    return store.verify(record.source_digest);
}

constexpr std::size_t kMaxProblems = 128U;
// ProjectDocument bounds serialized projects at 128 MiB. Keep the durable
// snapshot envelope capable of carrying that governed project state so large
// imported anatomy meshes do not require an unjournaled mutation path.
constexpr std::size_t kMaxJournalPayloadBytes = 128U * 1024U * 1024U;
constexpr std::string_view kSnapshotEvent = "cartographer.snapshot";
constexpr std::string_view kActionEvent = "cartographer.application_action";

journal::JournalLimits application_journal_limits() {
    journal::JournalLimits limits;
    limits.max_payload_bytes = kMaxJournalPayloadBytes;
    limits.max_record_bytes = 256ULL * 1024ULL * 1024ULL;
    limits.max_file_bytes = 1024ULL * 1024ULL * 1024ULL;
    return limits;
}

std::filesystem::path journal_path_for(const std::filesystem::path& project_path) {
    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    return journal_path;
}

std::string document_snapshot_payload(
    std::string_view header,
    std::string_view operation,
    std::string_view serialized_document) {
    return std::string(header) + "\n" +
        (operation.empty() ? std::string{} : "operation=" + std::string(operation) + "\n") +
        "document_bytes=" + std::to_string(serialized_document.size()) + "\n" +
        std::string(serialized_document);
}

bool journal_entry_matches_document(
    const journal::JournalEntry& entry,
    std::string_view serialized_document) {
    const std::string payload(entry.payload.begin(), entry.payload.end());
    const bool is_snapshot = entry.event_type == kSnapshotEvent;
    const bool is_mutation = entry.event_type == kActionEvent;
    if (!is_snapshot && !is_mutation) return false;

    std::size_t cursor = 0U;
    const auto next_line = [&payload, &cursor]() -> std::optional<std::string_view> {
        const std::size_t end = payload.find('\n', cursor);
        if (end == std::string::npos) return std::nullopt;
        const std::string_view line(payload.data() + cursor, end - cursor);
        cursor = end + 1U;
        return line;
    };
    const auto header = next_line();
    if (!header.has_value() || *header !=
            (is_snapshot ? "CARTOGRAPHER_PROJECT_SNAPSHOT_V1"
                         : "CARTOGRAPHER_APPLICATION_MUTATION_V1")) {
        return false;
    }
    if (is_mutation) {
        const auto operation = next_line();
        if (!operation.has_value() || !operation->starts_with("operation=") ||
            operation->size() == std::string_view("operation=").size() ||
            operation->find_first_of("\r\n") != std::string_view::npos) {
            return false;
        }
    }
    const auto document_bytes = next_line();
    constexpr std::string_view kDocumentBytes = "document_bytes=";
    if (!document_bytes.has_value() || !document_bytes->starts_with(kDocumentBytes)) {
        return false;
    }
    const std::string_view value = document_bytes->substr(kDocumentBytes.size());
    if (value.empty()) return false;
    std::uint64_t declared_bytes = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), declared_bytes);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        declared_bytes != serialized_document.size()) {
        return false;
    }
    return std::string_view(payload).substr(cursor) == serialized_document;
}

} // namespace

core::Result<DispatchReceipt> HumanApplicationAccess::dispatch(
    ApplicationSession& session,
    const ApplicationAction& action) {
    return session.dispatch(HumanActionAdmission{}, action);
}

core::Result<DispatchReceipt> HumanApplicationAccess::commit_preview(
    ApplicationSession& session,
    editor::AuthoringPreview& preview) {
    return session.commit_preview(HumanActionAdmission{}, preview);
}

core::Result<DispatchReceipt> HumanApplicationAccess::commit_preview(
    ApplicationSession& session,
    editor::AuthoringPreview& preview,
    OperationSource source,
    std::string_view provenance_id) {
    return session.commit_preview(HumanActionAdmission{}, preview, source, provenance_id);
}

core::Result<void> WorkspaceState::validate() const {
    if (visible_panes.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "workspace must expose at least one pane"));
    }
    std::set<Pane> unique_panes;
    for (const Pane pane : visible_panes) {
        const bool known_pane = pane == Pane::outliner || pane == Pane::viewport ||
            pane == Pane::inspector || pane == Pane::problems || pane == Pane::history;
        if (!known_pane) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "workspace contains an unknown pane"));
        }
        if (!unique_panes.insert(pane).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "workspace cannot contain duplicate panes"));
        }
    }
    if (!unique_panes.contains(Pane::viewport)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "workspace must keep the viewport pane visible"));
    }
    return core::Result<void>::success();
}

core::Result<void> RecoveryInspection::validate() const {
    if (project_path.empty() || project_path.filename().empty() ||
        journal_path.empty() || journal_path.filename().empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "recovery inspection paths must name files"));
    }
    if (journal_entry_count != journal_entries.size()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "recovery inspection entry count does not match its entries"));
    }
    if (journal_entries.empty()) {
        if (durable_revision != core::Revision{}) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "empty recovery inspection cannot have a durable revision"));
        }
    } else if (durable_revision != journal_entries.back().revision_after) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "recovery inspection durable revision does not match its journal tail"));
    }
    if (state == RecoveryInspectionState::blocked && !diagnostic.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "blocked recovery inspection requires a diagnostic"));
    }
    if (state != RecoveryInspectionState::blocked && diagnostic.has_value() &&
        state == RecoveryInspectionState::clean) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "clean recovery inspection cannot carry a blocking diagnostic"));
    }
    for (const auto& entry : journal_entries) {
        if (entry.event_type.empty() || entry.revision_after <= entry.revision_before) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "recovery inspection contains an invalid journal summary"));
        }
    }
    return core::Result<void>::success();
}

ApplicationSession::ApplicationSession() {
    static_cast<void>(tools_.register_builtin_tools());
    saved_revision_ = document_.revision();
}

ApplicationSession::ApplicationSession(
    const ApplicationSession& source,
    SpeculativeCopyTag)
    : document_(source.document_),
      saved_revision_(source.document_.revision()),
      project_generation_(source.project_generation_),
      selection_(source.selection_),
      workspace_(source.workspace_) {
    static_cast<void>(tools_.register_builtin_tools());
}

core::Result<RecoveryInspection> ApplicationSession::inspect_recovery(
    const std::filesystem::path& project_path) {
    if (project_path.empty() || project_path.filename().empty()) {
        return core::Result<RecoveryInspection>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "recovery inspection requires a project file path"));
    }

    RecoveryInspection inspection;
    inspection.project_path = project_path;
    inspection.journal_path = journal_path_for(project_path);

    std::error_code error;
    const bool project_exists = std::filesystem::exists(project_path, error);
    if (error) {
        return core::Result<RecoveryInspection>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect the project path"));
    }
    const bool journal_exists = std::filesystem::exists(inspection.journal_path, error);
    if (error) {
        return core::Result<RecoveryInspection>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect the project journal path"));
    }

    // Inspect the project and its journal under the same writer lease used by
    // save, mutation publication, and recovery. Without this lease a writer
    // could replace the project between the project load and journal read,
    // producing a recovery classification for two different revisions.
    std::optional<project::FileLock> project_lock;
    if (project_exists || journal_exists) {
        project_lock.emplace(project_path);
        if (auto result = project_lock->acquire(); !result) {
            return core::Result<RecoveryInspection>::failure(result.error().with_context(
                "recovery inspection lock"));
        }
    }

    if (project_exists) {
        const auto document = project::ProjectDocument::load(project_path);
        if (!document) {
            inspection.state = RecoveryInspectionState::blocked;
            inspection.diagnostic = document.error().with_context(
                "recovery inspection project load");
        } else {
            inspection.project_revision = document.value().revision();
        }
    }

    if (!journal_exists) {
        if (inspection.state != RecoveryInspectionState::blocked) {
            inspection.state = RecoveryInspectionState::unavailable;
        }
        if (auto result = inspection.validate(); !result) {
            return core::Result<RecoveryInspection>::failure(result.error());
        }
        return core::Result<RecoveryInspection>::success(std::move(inspection));
    }

    journal::Journal journal(inspection.journal_path, application_journal_limits());
    const auto entries = journal.read_all();
    if (!entries) {
        inspection.state = RecoveryInspectionState::blocked;
        inspection.diagnostic = entries.error().with_context(
            "recovery inspection journal verification");
    } else {
        inspection.journal_entry_count = entries.value().size();
        inspection.journal_entries.reserve(entries.value().size());
        for (const auto& entry : entries.value()) {
            inspection.journal_entries.push_back(RecoveryJournalEntry{
                entry.revision_before, entry.revision_after, entry.event_type});
        }
        if (!entries.value().empty()) {
            inspection.durable_revision = entries.value().back().revision_after;
        }

        if (inspection.state != RecoveryInspectionState::blocked) {
            if (!project_exists) {
                inspection.state = RecoveryInspectionState::pending;
                inspection.diagnostic = core::Diagnostic(
                    core::ErrorCode::not_found,
                    "durable journal exists but the project file is missing");
            } else if (entries.value().empty()) {
                if (inspection.project_revision.value_or(core::Revision{}) == core::Revision{}) {
                    inspection.state = RecoveryInspectionState::clean;
                } else {
                    inspection.state = RecoveryInspectionState::blocked;
                    inspection.diagnostic = core::Diagnostic(
                        core::ErrorCode::stale_data,
                        "project has a revision but its durable journal is empty");
                }
            } else if (inspection.project_revision.value() < inspection.durable_revision) {
                inspection.state = RecoveryInspectionState::pending;
                inspection.diagnostic = core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "durable journal is ahead of the saved project");
            } else if (inspection.project_revision.value() > inspection.durable_revision) {
                inspection.state = RecoveryInspectionState::blocked;
                inspection.diagnostic = core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "saved project is ahead of its durable journal");
            } else {
                const auto document = project::ProjectDocument::load(project_path);
                if (!document || !journal_entry_matches_document(
                        entries.value().back(), document.value().serialize())) {
                    inspection.state = RecoveryInspectionState::blocked;
                    inspection.diagnostic = core::Diagnostic(
                        core::ErrorCode::validation_failed,
                        "durable journal tail does not match the saved project");
                } else {
                    inspection.state = RecoveryInspectionState::clean;
                }
            }
        }
    }

    if (auto result = inspection.validate(); !result) {
        return core::Result<RecoveryInspection>::failure(result.error());
    }
    return core::Result<RecoveryInspection>::success(std::move(inspection));
}

core::Result<ApplicationSession::PreparedJournal> ApplicationSession::prepare_journal(
    const std::filesystem::path& project_path,
    const project::ProjectDocument& document) const {
    if (project_path.empty() || project_path.filename().empty()) {
        return core::Result<PreparedJournal>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "journal binding requires a project file path"));
    }

    const auto path = journal_path_for(project_path);
    std::error_code exists_error;
    const bool existed = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        return core::Result<PreparedJournal>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect the project journal path"));
    }
    std::uintmax_t original_bytes = 0U;
    if (existed) {
        std::error_code size_error;
        original_bytes = std::filesystem::file_size(path, size_error);
        if (size_error) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::io_error,
                "unable to inspect the project journal size"));
        }
    }

    journal::Journal candidate(path, application_journal_limits());
    const auto entries = candidate.read_all();
    if (!entries) {
        return core::Result<PreparedJournal>::failure(entries.error().with_context(
            "project journal verification"));
    }

    if (!entries.value().empty()) {
        const core::Revision tail = entries.value().back().revision_after;
        if (tail != document.revision()) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project revision does not match the durable journal; recovery is required"));
        }
        const std::string serialized = document.serialize();
        if (!journal_entry_matches_document(entries.value().back(), serialized)) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "project contents do not match the latest durable journal snapshot"));
        }
        return core::Result<PreparedJournal>::success(
            PreparedJournal{std::move(candidate), false, false, original_bytes});
    }

    if (document.revision().value() != 0U) {
        const std::string snapshot = document.serialize();
        if (snapshot.size() > kMaxJournalPayloadBytes) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "project snapshot exceeds the durable journal payload limit"));
        }
        const std::string payload_text = document_snapshot_payload(
            "CARTOGRAPHER_PROJECT_SNAPSHOT_V1", {}, snapshot);
        if (payload_text.size() > kMaxJournalPayloadBytes) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "project snapshot journal envelope exceeds the payload limit"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload_text.data());
        const std::span<const std::uint8_t> payload(bytes, payload_text.size());
        const auto baseline = candidate.append(journal::JournalAppend{
            core::Revision{},
            document.revision(),
            std::string(kSnapshotEvent),
            payload,
            std::nullopt,
        });
        if (!baseline) {
            return core::Result<PreparedJournal>::failure(baseline.error().with_context(
                "project journal baseline"));
        }
        return core::Result<PreparedJournal>::success(
            PreparedJournal{std::move(candidate), !existed, true, original_bytes});
    }

    return core::Result<PreparedJournal>::success(
        PreparedJournal{std::move(candidate), !existed, false, original_bytes});
}

core::Result<void> ApplicationSession::append_mutation_event(
    std::string_view action,
    core::Revision revision_before,
    core::Revision revision_after) {
    if (!journal_.has_value() || revision_before == revision_after) {
        return core::Result<void>::success();
    }
    const std::string serialized_document = document_.serialize();
    const std::string payload_text = document_snapshot_payload(
        "CARTOGRAPHER_APPLICATION_MUTATION_V1", action, serialized_document);
    if (payload_text.size() > kMaxJournalPayloadBytes) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "application mutation snapshot exceeds the durable journal payload limit"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload_text.data());
    const std::span<const std::uint8_t> payload(bytes, payload_text.size());
    const auto append = [&]() -> core::Result<void> {
        const auto appended = journal_->append(journal::JournalAppend{
            revision_before,
            revision_after,
            std::string(kActionEvent),
            payload,
            std::nullopt,
        });
        if (!appended) {
            return core::Result<void>::failure(appended.error().with_context(
                "durable application journal append"));
        }
        return core::Result<void>::success();
    };
    if (project_path_.has_value()) {
        project::FileLock project_lock(*project_path_);
        if (auto result = project_lock.acquire(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project mutation lock"));
        }
        return append();
    }
    return append();
}

core::Result<DispatchReceipt> ApplicationSession::accept_speculative_result(
    const project::ProjectDocument& candidate,
    core::Revision expected_revision,
    std::string_view task_id,
    std::string_view objective_digest,
    std::string_view diff_digest) {
    const auto parsed_objective_digest = assets::Sha256Digest::from_hex(objective_digest);
    const auto parsed_digest = assets::Sha256Digest::from_hex(diff_digest);
    if (active_source_ != OperationSource::human || task_id.empty() ||
        task_id.size() > 64U || task_id.find_first_of("\r\n") != std::string_view::npos ||
        !parsed_objective_digest || !parsed_digest) {
        return core::Result<DispatchReceipt>::failure(invalid_state(
            "speculative result acceptance requires a valid human task boundary"));
    }
    if (document_.revision() != expected_revision) {
        return core::Result<DispatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project changed after the speculative task began"));
    }
    if (expected_revision.exhausted() || candidate.revision() <= expected_revision) {
        return core::Result<DispatchReceipt>::failure(invalid_state(
            "speculative result is empty or its base revision cannot advance"));
    }
    if (auto result = candidate.validate(); !result) {
        return core::Result<DispatchReceipt>::failure(
            result.error().with_context("speculative result acceptance"));
    }

    editor::CommandBus prior_history = std::move(history_);
    history_ = editor::CommandBus{};
    editor::ProjectCommandAdmission admission;
    const OperationSource previous_source = active_source_;
    std::string previous_provenance = std::move(active_provenance_id_);
    active_source_ = OperationSource::ai_proposal;
    active_provenance_id_ = std::string(task_id);
    const std::string operation = "Accept Agent Speculative Result " +
        std::string(task_id) + " objective=" + std::string(objective_digest) +
        " diff=" + std::string(diff_digest);
    auto result = execute_command(
        std::make_unique<editor::ReplaceProjectDocumentCommand>(
            admission, document_, candidate),
        operation);
    active_source_ = previous_source;
    active_provenance_id_ = std::move(previous_provenance);
    if (!result) {
        history_ = std::move(prior_history);
        return result;
    }

    selection_.clear();
    last_tool_action_.reset();
    last_tool_context_.reset();
    viewport_mesh_cache_.clear();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0U;
    } else {
        ++project_generation_;
    }
    return result;
}

core::Result<DispatchReceipt> ApplicationSession::install_agent_rebase_branch(
    const project::ProjectDocument& candidate,
    core::Revision expected_revision,
    std::string_view checkpoint_task_id,
    std::string_view plan_context_digest,
    std::string_view checkpoint_source_digest) {
    const auto parsed_plan_digest = assets::Sha256Digest::from_hex(plan_context_digest);
    const auto parsed_source_digest = assets::Sha256Digest::from_hex(checkpoint_source_digest);
    if (active_source_ != OperationSource::human || checkpoint_task_id.empty() ||
        checkpoint_task_id.size() > 64U ||
        checkpoint_task_id.find_first_of("\r\n") != std::string_view::npos ||
        !parsed_plan_digest || !parsed_source_digest) {
        return core::Result<DispatchReceipt>::failure(invalid_state(
            "agent branch rebase requires a validated checkpoint lineage"));
    }
    if (document_.revision() != expected_revision) {
        return core::Result<DispatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "speculative source changed before the rebased branch was installed"));
    }
    if (expected_revision.exhausted() || candidate.revision() <= expected_revision) {
        return core::Result<DispatchReceipt>::failure(invalid_state(
            "rebased speculative branch must advance its exact base revision"));
    }
    if (auto result = candidate.validate(); !result) {
        return core::Result<DispatchReceipt>::failure(
            result.error().with_context("agent speculative rebase branch"));
    }

    editor::CommandBus prior_history = std::move(history_);
    history_ = editor::CommandBus{};
    editor::ProjectCommandAdmission admission;
    const OperationSource previous_source = active_source_;
    std::string previous_provenance = std::move(active_provenance_id_);
    active_source_ = OperationSource::ai_proposal;
    active_provenance_id_ = std::string(checkpoint_task_id);
    const std::string operation = "Rebase Agent Speculative Branch " +
        std::string(checkpoint_task_id) + " plan=" + parsed_plan_digest.value().hex() +
        " source=" + parsed_source_digest.value().hex();
    auto result = execute_command(
        std::make_unique<editor::ReplaceProjectDocumentCommand>(
            admission, document_, candidate),
        operation);
    active_source_ = previous_source;
    active_provenance_id_ = std::move(previous_provenance);
    if (!result) {
        history_ = std::move(prior_history);
        return result;
    }

    selection_.clear();
    last_tool_action_.reset();
    last_tool_context_.reset();
    viewport_mesh_cache_.clear();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0U;
    } else {
        ++project_generation_;
    }
    return result;
}

core::Result<project::ProjectDocument> ApplicationSession::merge_agent_rebase_document(
    const project::ProjectDocument& base,
    const project::ProjectDocument& speculative,
    const std::vector<scene::ObjectId>& read_objects,
    const std::vector<std::uint64_t>& read_mesh_assets,
    const std::vector<scene::ObjectId>& write_objects,
    const std::vector<std::uint64_t>& write_mesh_assets) const {
    return project::ProjectDocument::merge_disjoint_scene_mesh_changes(
        base, document_, speculative, read_objects, read_mesh_assets,
        write_objects, write_mesh_assets);
}

core::Result<project::ProjectDocument> ApplicationSession::merge_agent_rebase_step_document(
    const project::ProjectDocument& base,
    const project::ProjectDocument& speculative,
    const std::vector<scene::ObjectId>& write_objects,
    const std::vector<std::uint64_t>& write_mesh_assets,
    std::uint64_t revision_delta) const {
    auto selected = project::ProjectDocument::select_agent_rebase_step_changes(
        base, speculative, write_objects, write_mesh_assets, revision_delta);
    if (!selected) return selected;
    return project::ProjectDocument::merge_disjoint_scene_mesh_changes(
        base, document_, selected.value(), write_objects, write_mesh_assets,
        write_objects, write_mesh_assets);
}

core::Result<void> ApplicationSession::install_agent_checkpoint_source(
    project::ProjectDocument checkpoint_source) {
    if (auto result = checkpoint_source.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("historical agent checkpoint source"));
    }
    document_.swap(checkpoint_source);
    saved_revision_ = document_.revision();
    reset_editor_state();
    viewport_mesh_cache_.clear();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0U;
    } else {
        ++project_generation_;
    }
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::dispatch(
    HumanActionAdmission,
    const ApplicationAction& action,
    OperationSource source,
    std::string_view provenance_id) {
    const OperationSource previous_source = active_source_;
    std::string previous_provenance = std::move(active_provenance_id_);
    active_source_ = source;
    active_provenance_id_ = std::string(provenance_id);
    struct SourceRestore final {
        ApplicationSession* session;
        OperationSource source;
        std::string provenance;
        ~SourceRestore() {
            session->active_source_ = source;
            session->active_provenance_id_ = std::move(provenance);
        }
    } restore{this, previous_source, std::move(previous_provenance)};
    return std::visit(
        [this](const auto& value) -> core::Result<DispatchReceipt> {
            using Action = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Action, NewProjectAction>) {
                return new_project(value);
            } else if constexpr (std::is_same_v<Action, OpenProjectAction>) {
                return open_project(value);
            } else if constexpr (std::is_same_v<Action, RecoverProjectAction>) {
                return recover_project(value);
            } else if constexpr (std::is_same_v<Action, SaveProjectAction>) {
                return save_project(value);
            } else if constexpr (std::is_same_v<Action, SetWorkspaceAction>) {
                const core::Revision before = document_.revision();
                if (auto result = set_workspace(value.workspace); !result) {
                    return failure(result.error().with_context("workspace update"));
                }
                return accepted("Set Workspace", before);
            } else if constexpr (std::is_same_v<Action, SetSelectionModeAction>) {
                const core::Revision before = document_.revision();
                if (auto result = selection_.set_mode(value.mode); !result) {
                    return failure(result.error().with_context("selection mode"));
                }
                return accepted("Set Selection Mode", before);
            } else if constexpr (std::is_same_v<Action, ConvertSelectionAction>) {
                return convert_selection(value);
            } else if constexpr (std::is_same_v<Action, ExpandSelectionAction>) {
                return expand_selection(value);
            } else if constexpr (std::is_same_v<Action, SelectShortestPathAction>) {
                return select_shortest_path(value);
            } else if constexpr (std::is_same_v<Action, SelectObjectAction>) {
                return select_object(value);
            } else if constexpr (std::is_same_v<Action, SelectVertexAction>) {
                return select_vertex(value);
            } else if constexpr (std::is_same_v<Action, SelectFaceAction>) {
                return select_face(value);
            } else if constexpr (std::is_same_v<Action, SelectEdgeAction>) {
                return select_edge(value);
            } else if constexpr (std::is_same_v<Action, SetObjectTransformAction>) {
                if (value.expected_project_revision.has_value() &&
                    *value.expected_project_revision != document_.revision()) {
                    return failure(core::Diagnostic(
                        core::ErrorCode::stale_data,
                        "object transform was authored against a stale project revision"));
                }
                const auto* object = document_.scene().find(value.object);
                if (!object) {
                    return failure(core::Diagnostic(
                        core::ErrorCode::not_found,
                        "cannot transform a missing scene object"));
                }
                if (object->locked) {
                    return failure(invalid_state("locked scene objects cannot be transformed"));
                }
                const auto before_transform = object->local_transform;
                const bool is_root_object = !object->parent.has_value();
                std::optional<assets::Sha256Digest> structure_signature;
                if (object->mesh_asset.has_value()) {
                    const auto mesh = document_.meshes().find(*object->mesh_asset);
                    if (mesh != document_.meshes().end()) {
                        const auto signature = structure_signature_for_mesh(
                            mesh->second, std::monostate{});
                        if (signature) structure_signature = signature.value();
                    }
                }
                editor::ProjectCommandAdmission admission;
                auto result = execute_command(
                    std::make_unique<editor::SetProjectObjectTransformCommand>(
                        admission,
                        document_, value.object, before_transform, value.transform),
                    "Set Object Transform",
                    AffectedSet{{value.object}, {}, {}, {}},
                    ParameterPayload{
                        value.transform, std::nullopt, std::nullopt, std::nullopt,
                        std::nullopt, std::nullopt, std::nullopt});
                const bool translation_only =
                    before_transform.rotation.x == value.transform.rotation.x &&
                    before_transform.rotation.y == value.transform.rotation.y &&
                    before_transform.rotation.z == value.transform.rotation.z &&
                    before_transform.rotation.w == value.transform.rotation.w &&
                    before_transform.scale.x == value.transform.scale.x &&
                    before_transform.scale.y == value.transform.scale.y &&
                    before_transform.scale.z == value.transform.scale.z &&
                    is_root_object;
                if (result && result.value().document_changed) {
                    const auto displacement = value.transform.translation -
                        before_transform.translation;
                    const double magnitude = displacement.length();
                    if (active_source_ == OperationSource::human && translation_only &&
                        displacement.finite() && std::isfinite(magnitude) && magnitude > 0.0) {
                        std::optional<assets::Sha256Digest> result_object_signature;
                        const auto* result_object = document_.scene().find(value.object);
                        if (result_object != nullptr && result_object->mesh_asset.has_value()) {
                            const auto result_mesh = document_.meshes().find(
                                *result_object->mesh_asset);
                            if (result_mesh != document_.meshes().end()) {
                                const auto signature = structure_signature_for_mesh(
                                    result_mesh->second, std::monostate{});
                                if (signature) result_object_signature = signature.value();
                            }
                        }
                        append_native_operation_trace(NativeOperationTraceEntry{
                            result.value().operation_id,
                            "object.translate",
                            result.value().source,
                            result.value().revision_before,
                            result.value().revision_after,
                            value.object,
                            std::monostate{},
                            NativeTraceParameterization::relative_displacement,
                            0.0,
                            displacement,
                            structure_signature,
                            result_object_signature});
                    } else {
                        native_operation_trace_.clear();
                    }
                }
                return result;
            } else if constexpr (std::is_same_v<Action, InvokeToolAction>) {
                return invoke_tool(value);
            } else if constexpr (std::is_same_v<Action, RepeatLastToolAction>) {
                return repeat_last_tool();
            } else if constexpr (std::is_same_v<Action, AdjustLastToolAction>) {
                return adjust_last_tool(value);
            } else if constexpr (std::is_same_v<Action, CreateBoxAction>) {
                return create_box(value);
            } else if constexpr (std::is_same_v<Action, CreatePlaneAction>) {
                return create_plane(value);
            } else if constexpr (std::is_same_v<Action, CreateMeshObjectAction>) {
                return create_mesh_object(value);
            } else if constexpr (std::is_same_v<Action, AppendSculptStrokeAction>) {
                return append_sculpt_stroke(value);
            } else if constexpr (std::is_same_v<Action, AppendSculptSurfaceStrokeAction>) {
                return append_sculpt_surface_stroke(value);
            } else if constexpr (std::is_same_v<Action, SetSculptLayerVisibilityAction>) {
                return set_sculpt_layer_visibility(value);
            } else if constexpr (std::is_same_v<Action, DuplicateMeshInstanceAction>) {
                return duplicate_mesh_instance(value);
            } else if constexpr (std::is_same_v<Action, ImportEngineeringFixtureAction>) {
                return import_engineering_fixture();
            } else if constexpr (std::is_same_v<Action, ImportEngineeringAuthoringAction>) {
                return import_engineering_authoring(value);
            } else if constexpr (std::is_same_v<Action, InvalidateEngineeringSourceAction>) {
                return invalidate_engineering_source(value);
            } else if constexpr (std::is_same_v<Action, ImportConditionFixtureAction>) {
                return import_condition_fixture();
            } else if constexpr (std::is_same_v<Action, ImportConditionAuthoringAction>) {
                return import_condition_authoring(value);
            } else if constexpr (std::is_same_v<Action, BindConditionAssetObjectAction>) {
                return bind_condition_asset_object(value);
            } else if constexpr (std::is_same_v<Action, InvalidateConditionSourceAction>) {
                return invalidate_condition_source(value);
            } else if constexpr (std::is_same_v<Action, ApplySourceReimportPlanAction>) {
                return apply_source_reimport_plan(value);
            } else if constexpr (std::is_same_v<Action, CreateSpatialFieldAction>) {
                return create_spatial_field(value);
            } else if constexpr (std::is_same_v<Action, PaintSpatialFieldAction>) {
                return paint_spatial_field(value);
            } else if constexpr (std::is_same_v<Action, CreateMaterialAction>) {
                return create_material(value);
            } else if constexpr (std::is_same_v<Action, UpdateMaterialAction>) {
                return update_material(value);
            } else if constexpr (std::is_same_v<Action, SetMaterialRegionAssignmentAction>) {
                return set_material_region_assignment(value);
            } else if constexpr (std::is_same_v<Action, SetMaterialGraphAction>) {
                return set_material_graph(value);
            } else if constexpr (std::is_same_v<Action, SetMaterialGraphBindingAction>) {
                return set_material_graph_binding(value);
            } else if constexpr (std::is_same_v<Action, SetRoadAuthoringAction>) {
                return set_road_authoring(value);
            } else if constexpr (std::is_same_v<Action, SetSweepSourceAction>) {
                return set_sweep_source(value);
            } else if constexpr (std::is_same_v<Action, UndoAction>) {
                return undo();
            } else {
                return redo();
            }
        },
        action);
}

core::Result<editor::AuthoringContext> ApplicationSession::authoring_context() const {
    auto context = editor::AuthoringContext::from_selection(document_.revision(), selection_);
    if (!context) {
        return context;
    }

    if (selection_.mode() == editor::SelectionMode::object) {
        if (auto result = selection_.validate(document_.scene(), nullptr); !result) {
            return core::Result<editor::AuthoringContext>::failure(result.error().with_context(
                "authoring selection"));
        }
        return context;
    }

    const auto owner = selection_.component_object();
    if (!owner.has_value()) {
        return core::Result<editor::AuthoringContext>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "component authoring context has no owning scene object"));
    }
    const auto* object = document_.scene().find(*owner);
    if (!object) {
        return core::Result<editor::AuthoringContext>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "component authoring context owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<editor::AuthoringContext>::failure(invalid_state(
            "component authoring context owner has no mesh asset"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return core::Result<editor::AuthoringContext>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "component authoring context references a missing mesh asset"));
    }
    if (auto result = selection_.validate(document_.scene(), &mesh->second); !result) {
        return core::Result<editor::AuthoringContext>::failure(result.error().with_context(
            "authoring selection"));
    }
    return context;
}

core::Result<engineering::AuthoringDocument> ApplicationSession::engineering_authoring() const {
    if (!document_.engineering_authoring().has_value()) {
        return core::Result<engineering::AuthoringDocument>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "the project has no engineering authoring document"));
    }
    const auto decoded = engineering::AuthoringDocument::deserialize(
        document_.engineering_authoring()->bytes);
    if (!decoded) {
        return core::Result<engineering::AuthoringDocument>::failure(decoded.error().with_context(
            "project engineering authoring"));
    }
    return decoded;
}

core::Result<condition::AuthoringDocument> ApplicationSession::condition_authoring() const {
    if (!document_.condition_authoring().has_value()) {
        return core::Result<condition::AuthoringDocument>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "the project has no CONDITION source-authoring document"));
    }
    const auto decoded = condition::AuthoringDocument::deserialize(
        document_.condition_authoring()->bytes);
    if (!decoded) {
        return core::Result<condition::AuthoringDocument>::failure(decoded.error().with_context(
            "project CONDITION authoring"));
    }
    return decoded;
}

core::Result<assets::SourceReimportPlan> ApplicationSession::preview_source_reimport(
    std::vector<assets::SourceImportRecord> incoming) const {
    return assets::plan_source_reimport(document_.source_asset_registry(), std::move(incoming));
}

core::Result<condition::EffectPreviewResult> ApplicationSession::condition_effect_preview(
    condition::Id effect_source_id,
    std::string_view operating_state,
    double elapsed_seconds) const {
    const auto authoring = condition_authoring();
    if (!authoring) {
        return core::Result<condition::EffectPreviewResult>::failure(
            authoring.error().with_context("CONDITION effect preview"));
    }
    return condition::preview_effect(authoring.value(), document_.spatial_fields(),
        effect_source_id, operating_state, elapsed_seconds);
}

core::Result<condition::VegetationPreviewResult>
ApplicationSession::condition_vegetation_preview(condition::Id recipe_id) const {
    const auto authoring = condition_authoring();
    if (!authoring) {
        return core::Result<condition::VegetationPreviewResult>::failure(
            authoring.error().with_context("CONDITION vegetation preview"));
    }
    return condition::preview_vegetation(authoring.value(), document_.spatial_fields(),
        recipe_id);
}

core::Result<condition::StateComparisonResult>
ApplicationSession::condition_state_comparison(
    condition::Id left_branch_id,
    condition::Id right_branch_id) const {
    const auto authoring = condition_authoring();
    if (!authoring) {
        return core::Result<condition::StateComparisonResult>::failure(
            authoring.error().with_context("CONDITION state comparison"));
    }
    return condition::compare_state_branches(
        authoring.value(), left_branch_id, right_branch_id);
}

core::Result<SpatialFieldPreview> ApplicationSession::spatial_field_preview(
    std::string_view identity) const {
    const auto found = document_.spatial_fields().find(std::string(identity));
    if (found == document_.spatial_fields().end()) {
        return core::Result<SpatialFieldPreview>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "spatial field preview requires a field present in this project"));
    }
    const auto& field = found->second;
    SpatialFieldPreview preview;
    preview.identity = field.identity();
    preview.semantic = field.semantic();
    preview.source_revision = field.source_revision();
    preview.origin_uv = field.origin_uv();
    preview.cell_size = field.cell_size();
    preview.minimum_value = field.minimum_value();
    preview.maximum_value = field.maximum_value();
    preview.source_width = field.width();
    preview.source_height = field.height();
    preview.preview_width = std::min(
        field.width(), SpatialFieldPreview::kMaxPreviewAxis);
    preview.preview_height = std::min(
        field.height(), SpatialFieldPreview::kMaxPreviewAxis);
    preview.values.reserve(static_cast<std::size_t>(preview.preview_width) *
                           preview.preview_height);
    for (std::uint32_t v = 0U; v < preview.preview_height; ++v) {
        const std::uint32_t source_v = preview.preview_height == 1U
            ? 0U
            : static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(v) * (field.height() - 1U)) /
                (preview.preview_height - 1U));
        for (std::uint32_t u = 0U; u < preview.preview_width; ++u) {
            const std::uint32_t source_u = preview.preview_width == 1U
                ? 0U
                : static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(u) * (field.width() - 1U)) /
                    (preview.preview_width - 1U));
            const std::size_t index = static_cast<std::size_t>(source_v) *
                field.width() + source_u;
            if (index >= field.values().size()) {
                return core::Result<SpatialFieldPreview>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "spatial field preview index exceeds the validated source"));
            }
            preview.values.push_back(field.values()[index]);
        }
    }
    return core::Result<SpatialFieldPreview>::success(std::move(preview));
}

core::Result<spatial_field::VectorFieldPreview>
ApplicationSession::spatial_vector_field_preview(
    std::string_view u_component_identity,
    std::string_view v_component_identity,
    std::uint32_t max_preview_axis) const {
    const auto& fields = document_.spatial_fields();
    const auto u_component = fields.find(std::string(u_component_identity));
    const auto v_component = fields.find(std::string(v_component_identity));
    if (u_component == fields.end() || v_component == fields.end()) {
        return core::Result<spatial_field::VectorFieldPreview>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "vector preview requires both component sources in this project"));
    }
    return spatial_field::build_vector_field_preview(
        u_component->second, v_component->second, max_preview_axis);
}

core::Result<terrain::TerrainTileProduct> ApplicationSession::terrain_tile_preview(
    std::string_view identity,
    std::uint32_t lod_step,
    double max_navigation_slope_degrees,
    std::optional<double> water_level) const {
    const auto found = document_.spatial_fields().find(std::string(identity));
    if (found == document_.spatial_fields().end()) {
        return core::Result<terrain::TerrainTileProduct>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "terrain preview requires a source field present in this project"));
    }
    const auto key = terrain::parse_terrain_tile_identity(identity);
    if (!key) return core::Result<terrain::TerrainTileProduct>::failure(key.error());
    return terrain::build_terrain_tile(found->second, key.value(), lod_step,
        max_navigation_slope_degrees, water_level);
}

core::Result<geometry::CurvePath> ApplicationSession::curve_path_source(
    std::string_view identity) const {
    const auto found = document_.curve_paths().find(std::string(identity));
    if (found == document_.curve_paths().end()) {
        return core::Result<geometry::CurvePath>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "curve source is not present in this project"));
    }
    return core::Result<geometry::CurvePath>::success(found->second);
}

core::Result<project::MaterialGraph> ApplicationSession::material_graph_source(
    std::string_view identity) const {
    const auto found = document_.material_catalog().material_graphs().find(
        std::string(identity));
    if (found == document_.material_catalog().material_graphs().end()) {
        return core::Result<project::MaterialGraph>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "material graph source is not present in this project"));
    }
    return core::Result<project::MaterialGraph>::success(found->second);
}

core::Result<RoadSurfacePreview> ApplicationSession::road_surface_preview(
    std::string_view identity,
    std::uint32_t samples_per_segment) const {
    const auto road = document_.road_sources().find(std::string(identity));
    if (road == document_.road_sources().end()) {
        return core::Result<RoadSurfacePreview>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "road surface preview requires a road source present in this project"));
    }
    if (auto result = road->second.validate(); !result) {
        return core::Result<RoadSurfacePreview>::failure(
            result.error().with_context("road surface preview source"));
    }
    const auto centerline = document_.curve_paths().find(road->second.centerline_identity);
    if (centerline == document_.curve_paths().end()) {
        return core::Result<RoadSurfacePreview>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road surface preview centerline is no longer present"));
    }
    if (centerline->second.source_identity != road->second.centerline_identity ||
        centerline->second.source_revision != road->second.centerline_revision) {
        return core::Result<RoadSurfacePreview>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road surface preview requires the exact authored centerline revision"));
    }
    const auto surface = geometry::build_road_surface(centerline->second,
        road->second.width_meters, road->second.shoulder_meters,
        road->second.maximum_grade_ratio, road->second.minimum_curve_radius_meters,
        samples_per_segment);
    if (!surface) {
        return core::Result<RoadSurfacePreview>::failure(
            surface.error().with_context("road surface design preview"));
    }
    const std::string encoded_centerline = centerline->second.serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded_centerline.data());
    const auto centerline_digest = assets::sha256(
        std::span<const std::uint8_t>(bytes, encoded_centerline.size()));
    return core::Result<RoadSurfacePreview>::success(RoadSurfacePreview{
        road->first, road->second.source_revision, project_generation_,
        road->second.source_digest(), centerline_digest,
        road->second.surface_class, std::move(surface).value()});
}

core::Result<geometry::SweptMeshProduct> ApplicationSession::sweep_source_preview(
    std::string_view identity) const {
    const auto source = document_.sweep_sources().find(std::string(identity));
    if (source == document_.sweep_sources().end()) {
        return core::Result<geometry::SweptMeshProduct>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "sweep preview requires a source present in this project"));
    }
    const auto centerline = document_.curve_paths().find(source->second.centerline_identity);
    if (centerline == document_.curve_paths().end() ||
        centerline->second.source_revision != source->second.centerline_revision) {
        return core::Result<geometry::SweptMeshProduct>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sweep preview requires the exact authored centerline revision"));
    }
    return geometry::build_sweep_mesh(source->second, centerline->second);
}

core::Result<core::Revision> ApplicationSession::next_curve_path_revision(
    std::string_view identity) const {
    const auto lineage = document_.curve_path_lineage_.find(std::string(identity));
    if (lineage == document_.curve_path_lineage_.end()) {
        return core::Result<core::Revision>::success(core::Revision{1U});
    }
    if (lineage->second.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "curve source revision space is exhausted"));
    }
    return core::Result<core::Revision>::success(lineage->second.next());
}

core::Result<core::Revision> ApplicationSession::next_road_source_revision(
    std::string_view identity) const {
    const auto lineage = document_.road_source_lineage_.find(std::string(identity));
    if (lineage == document_.road_source_lineage_.end()) {
        return core::Result<core::Revision>::success(core::Revision{1U});
    }
    if (lineage->second.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "road source revision space is exhausted"));
    }
    return core::Result<core::Revision>::success(lineage->second.next());
}

core::Result<core::Revision> ApplicationSession::next_sweep_source_revision(
    std::string_view identity) const {
    const auto lineage = document_.sweep_source_lineage_.find(std::string(identity));
    if (lineage == document_.sweep_source_lineage_.end()) {
        return core::Result<core::Revision>::success(core::Revision{1U});
    }
    if (lineage->second.exhausted()) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "sweep source revision space is exhausted"));
    }
    return core::Result<core::Revision>::success(lineage->second.next());
}

core::Result<editor::AuthoringPreview> ApplicationSession::begin_preview(
    editor::PreviewKind kind) const {
    auto context = authoring_context();
    if (!context) {
        return core::Result<editor::AuthoringPreview>::failure(context.error());
    }
    return editor::AuthoringPreview::begin(document_.revision(), context.value(), kind);
}

core::Result<ApplicationAction> ApplicationSession::preview_action(
    const editor::AuthoringPreview& preview) const {
    const auto& context = preview.context();
    const auto& parameters = preview.parameters();
    if (preview.kind() == editor::PreviewKind::object_transform) {
        if (!parameters.transform.has_value() || context.objects.size() != 1U) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "object transform preview has no complete action payload"));
        }
        return core::Result<ApplicationAction>::success(SetObjectTransformAction{
            context.objects.front(), *parameters.transform, context.project_revision});
    }
    if (preview.kind() == editor::PreviewKind::object_translation_delta) {
        if (!parameters.position.has_value() || context.objects.size() != 1U) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "object translation preview has no complete action payload"));
        }
        const auto* object = document_.scene().find(context.objects.front());
        if (object == nullptr) {
            return core::Result<ApplicationAction>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "object translation preview target no longer exists"));
        }
        core::Transform translated = object->local_transform;
        translated.translation = translated.translation + *parameters.position;
        if (!translated.finite()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "object translation preview produced a non-finite transform"));
        }
        return core::Result<ApplicationAction>::success(SetObjectTransformAction{
            context.objects.front(), translated, context.project_revision});
    }
    if (preview.kind() == editor::PreviewKind::extrude_face) {
        if (!parameters.distance.has_value()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "face extrusion preview has no distance"));
        }
        return core::Result<ApplicationAction>::success(InvokeToolAction{
            "mesh.extrude-face", editor::ToolArguments{
                *parameters.distance, {}, true, 0.5, std::nullopt}});
    }
    if (preview.kind() == editor::PreviewKind::inset_face) {
        if (!parameters.distance.has_value()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "face inset preview has no distance"));
        }
        return core::Result<ApplicationAction>::success(InvokeToolAction{
            "mesh.inset-face", editor::ToolArguments{
                *parameters.distance, {}, true, 0.5, std::nullopt}});
    }
    if (preview.kind() == editor::PreviewKind::poke_face) {
        if (parameters.transform.has_value() || parameters.distance.has_value() ||
            parameters.position.has_value()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "face poke preview contains unexpected parameters"));
        }
        return core::Result<ApplicationAction>::success(InvokeToolAction{
            "mesh.poke-face", editor::ToolArguments{}});
    }
    if (preview.kind() == editor::PreviewKind::set_material_region_assignment) {
        if (context.objects.size() != 1U ||
            !parameters.material_region_id.has_value() ||
            !parameters.material_id.has_value()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "material assignment preview has no complete object and ID payload"));
        }
        const auto* object = document_.scene().find(context.objects.front());
        if (object == nullptr || !object->mesh_asset.has_value()) {
            return core::Result<ApplicationAction>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "material assignment preview target no longer has an authored mesh"));
        }
        std::optional<project::MaterialVariantId> variant;
        if (parameters.material_variant_id.has_value()) {
            variant = static_cast<project::MaterialVariantId>(
                *parameters.material_variant_id);
        }
        return core::Result<ApplicationAction>::success(SetMaterialRegionAssignmentAction{
            context.project_revision,
            {*object->mesh_asset, *parameters.material_region_id},
            static_cast<render::MaterialId>(*parameters.material_id), variant});
    }
    if (preview.kind() != editor::PreviewKind::set_vertex_position) {
        return core::Result<ApplicationAction>::failure(invalid_state(
            "preview kind has no native application action"));
    }
    if (!parameters.position.has_value()) {
        return core::Result<ApplicationAction>::failure(invalid_state(
            "vertex position preview has no position"));
    }
    return core::Result<ApplicationAction>::success(InvokeToolAction{
        "mesh.set-vertex-position", editor::ToolArguments{
            0.0, *parameters.position, true, 0.5, std::nullopt}});
}

core::Result<DispatchReceipt> ApplicationSession::commit_preview(
    HumanActionAdmission admission,
    editor::AuthoringPreview& preview,
    OperationSource source,
    std::string_view provenance_id) {
    if (auto result = preview.validate_commit(document_.revision()); !result) {
        return failure(result.error().with_context("preview commit"));
    }
    auto current_context = authoring_context();
    if (!current_context) {
        return failure(current_context.error().with_context("preview commit context"));
    }
    if (current_context.value() != preview.context()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "authoring preview context no longer matches the current selection"));
    }
    auto action = preview_action(preview);
    if (!action) {
        return failure(action.error().with_context("preview commit action"));
    }
    auto result = dispatch(admission, action.value(), source, provenance_id);
    if (result) {
        preview.mark_committed();
    }
    return result;
}

core::Result<void> ApplicationSession::set_workspace(WorkspaceState workspace) {
    if (auto result = workspace.validate(); !result) {
        return result;
    }
    workspace_ = std::move(workspace);
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::new_project(const NewProjectAction& action) {
    if (auto result = require_discard_confirmation(action.discard_dirty); !result) {
        return failure(result.error());
    }
    auto created = project::ProjectDocument::create(action.name);
    if (!created) {
        return failure(created.error());
    }
    const core::Revision before = document_.revision();
    document_ = std::move(created.value());
    viewport_mesh_cache_.clear();
    project_path_.reset();
    journal_.reset();
    reset_editor_state();
    saved_revision_ = document_.revision();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0;
    } else {
        ++project_generation_;
    }
    return accepted("New Project", before);
}

core::Result<DispatchReceipt> ApplicationSession::open_project(const OpenProjectAction& action) {
    if (auto result = require_discard_confirmation(action.discard_dirty); !result) {
        return failure(result.error());
    }
    project::FileLock project_lock(action.path);
    if (auto result = project_lock.acquire(); !result) {
        return failure(result.error().with_context("project read lock"));
    }
    auto loaded = project::ProjectDocument::load(action.path);
    if (!loaded) {
        return failure(loaded.error().with_context("open project"));
    }
    auto prepared_journal = prepare_journal(action.path, loaded.value());
    if (!prepared_journal) {
        return failure(prepared_journal.error().with_context("open project journal"));
    }
    const core::Revision before = document_.revision();
    document_ = std::move(loaded.value());
    viewport_mesh_cache_.clear();
    project_path_ = action.path;
    journal_ = std::move(prepared_journal.value().journal);
    reset_editor_state();
    saved_revision_ = document_.revision();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0;
    } else {
        ++project_generation_;
    }
    return accepted("Open Project", before);
}

core::Result<DispatchReceipt> ApplicationSession::recover_project(
    const RecoverProjectAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(invalid_state(
            "journal recovery requires an explicit human application action"));
    }
    if (auto result = require_discard_confirmation(action.discard_dirty); !result) {
        return failure(result.error());
    }
    if (action.path.empty() || action.path.filename().empty()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "recovery requires a project file path"));
    }

    project::FileLock project_lock(action.path);
    if (auto result = project_lock.acquire(); !result) {
        return failure(result.error().with_context("project recovery lock"));
    }
    journal::Journal journal(journal_path_for(action.path), application_journal_limits());
    const auto recovered = project::recover_latest_snapshot(journal);
    if (!recovered) {
        return failure(recovered.error().with_context("recover project"));
    }
    auto prepared_journal = prepare_journal(action.path, recovered.value().document);
    if (!prepared_journal) {
        return failure(prepared_journal.error().with_context("recover project journal"));
    }
    const auto saved = recovered.value().document.save_atomic_unlocked(action.path);
    if (!saved) {
        return failure(saved.error().with_context("recover project save"));
    }

    const core::Revision before = document_.revision();
    document_ = recovered.value().document;
    viewport_mesh_cache_.clear();
    project_path_ = action.path;
    journal_ = std::move(prepared_journal.value().journal);
    reset_editor_state();
    saved_revision_ = document_.revision();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0;
    } else {
        ++project_generation_;
    }
    const OperationSource previous_source = active_source_;
    active_source_ = OperationSource::recovery;
    auto receipt = accepted("Recover Project", before);
    active_source_ = previous_source;
    if (receipt) {
        // Recovery can restore a revision already present in the active
        // session while still replacing the complete document state.
        receipt.value().document_changed = true;
    }
    return receipt;
}

core::Result<void> ApplicationSession::can_close(bool discard_dirty) const {
    return require_discard_confirmation(discard_dirty);
}

core::Result<void> ApplicationSession::require_discard_confirmation(bool discard_dirty) const {
    if (!discard_dirty && document_.revision() != saved_revision_) {
        return core::Result<void>::failure(invalid_state(
            "unsaved project changes require explicit discard confirmation"));
    }
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::save_project(const SaveProjectAction& action) {
    const std::filesystem::path* selected = nullptr;
    if (action.path.has_value()) {
        selected = &action.path.value();
    } else if (project_path_.has_value()) {
        selected = &project_path_.value();
    }
    if (!selected) {
        return failure(invalid_state("save requires a project path"));
    }
    project::FileLock project_lock(*selected);
    if (auto result = project_lock.acquire(); !result) {
        return failure(result.error().with_context("project write lock"));
    }
    auto prepared_journal = prepare_journal(*selected, document_);
    if (!prepared_journal) {
        return failure(prepared_journal.error().with_context("save project journal"));
    }
    const auto saved = document_.save_atomic_unlocked(*selected);
    if (!saved) {
        std::error_code cleanup_error;
        if (prepared_journal.value().created_file) {
            std::filesystem::remove(prepared_journal.value().journal.path(), cleanup_error);
        } else if (prepared_journal.value().appended_baseline) {
            std::filesystem::resize_file(
                prepared_journal.value().journal.path(),
                prepared_journal.value().original_bytes,
                cleanup_error);
        }
        if (cleanup_error) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project save failed and durable journal rollback failed: " +
                    cleanup_error.message()));
        }
        return failure(saved.error().with_context("save project"));
    }
    const core::Revision before = document_.revision();
    project_path_ = *selected;
    journal_ = std::move(prepared_journal.value().journal);
    saved_revision_ = document_.revision();
    return accepted("Save Project", before);
}

core::Result<DispatchReceipt> ApplicationSession::execute_command(
    std::unique_ptr<editor::EditorCommand> command,
    std::string action,
    AffectedSet affected,
    std::optional<ParameterPayload> parameters) {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    if (auto result = history_.execute(std::move(command)); !result) {
        return failure(result.error().with_context(action));
    }
    return accept_command_mutation(
        std::move(action), before, std::move(before_document), std::move(affected),
        std::move(parameters));
}

core::Result<DispatchReceipt> ApplicationSession::accept_command_mutation(
    std::string action,
    core::Revision revision_before,
    project::ProjectDocument before_document,
    AffectedSet affected,
    std::optional<ParameterPayload> parameters) {
    const core::Revision revision_after = document_.revision();
    if (auto result = append_mutation_event(action, revision_before, revision_after); !result) {
        const auto rollback = history_.rollback_last_execute();
        document_ = std::move(before_document);
        viewport_mesh_cache_.clear();
        if (!rollback) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "durable journal append failed and command rollback failed: " +
                    rollback.error().message));
        }
        return failure(result.error().with_context("mutation rolled back"));
    }
    return accepted(
        std::move(action), revision_before, std::move(affected), std::move(parameters));
}

core::Result<DispatchReceipt> ApplicationSession::select_object(const SelectObjectAction& action) {
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_object(document_.scene(), action.object, action.operation); !result) {
        return failure(result.error().with_context("object selection"));
    }
    return accepted("Select Object", before);
}

core::Result<DispatchReceipt> ApplicationSession::select_vertex(const SelectVertexAction& action) {
    const auto* object = document_.scene().find(action.object);
    if (!object) {
        return failure(core::Diagnostic(core::ErrorCode::not_found, "cannot select on a missing object"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state("cannot select a vertex on an object without a mesh"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data, "object references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_vertex(
            document_.scene(), action.object, mesh->second, action.vertex, action.operation);
        !result) {
        return failure(result.error().with_context("vertex selection"));
    }
    return accepted("Select Vertex", before);
}

core::Result<DispatchReceipt> ApplicationSession::select_face(const SelectFaceAction& action) {
    const auto* object = document_.scene().find(action.object);
    if (!object) {
        return failure(core::Diagnostic(core::ErrorCode::not_found, "cannot select on a missing object"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state("cannot select a face on an object without a mesh"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data, "object references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_face(
            document_.scene(), action.object, mesh->second, action.face, action.operation);
        !result) {
        return failure(result.error().with_context("face selection"));
    }
    return accepted("Select Face", before);
}

core::Result<DispatchReceipt> ApplicationSession::select_edge(const SelectEdgeAction& action) {
    const auto* object = document_.scene().find(action.object);
    if (!object) {
        return failure(core::Diagnostic(core::ErrorCode::not_found, "cannot select on a missing object"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state("cannot select an edge on an object without a mesh"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data, "object references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_edge(
            document_.scene(), action.object, mesh->second, action.edge, action.operation);
        !result) {
        return failure(result.error().with_context("edge selection"));
    }
    return accepted("Select Edge", before);
}

core::Result<DispatchReceipt> ApplicationSession::convert_selection(
    const ConvertSelectionAction& action) {
    const auto owner = selection_.component_object();
    if (!owner.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "selection conversion requires an object-bound component selection"));
    }
    const auto* object = document_.scene().find(*owner);
    if (!object) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selection conversion owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state(
            "selection conversion owner has no mesh asset"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selection conversion owner references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.convert_mode(action.mode, mesh->second); !result) {
        return failure(result.error().with_context("selection conversion"));
    }
    return accepted("Convert Selection", before);
}

core::Result<DispatchReceipt> ApplicationSession::expand_selection(
    const ExpandSelectionAction& action) {
    const auto owner = selection_.component_object();
    if (!owner.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "selection expansion requires an object-bound component selection"));
    }
    const auto* object = document_.scene().find(*owner);
    if (!object) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selection expansion owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state(
            "selection expansion owner has no mesh asset"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selection expansion owner references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.expand(action.expansion, mesh->second); !result) {
        return failure(result.error().with_context("selection expansion"));
    }
    return accepted("Expand Selection", before);
}

core::Result<DispatchReceipt> ApplicationSession::select_shortest_path(
    const SelectShortestPathAction& action) {
    const auto owner = selection_.component_object();
    if (!owner.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "shortest-path selection requires an object-bound component selection"));
    }
    const auto* object = document_.scene().find(*owner);
    if (!object) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "shortest-path selection owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state(
            "shortest-path selection owner has no mesh asset"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "shortest-path selection owner references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_shortest_path(action.goal, mesh->second); !result) {
        return failure(result.error().with_context("shortest-path selection"));
    }
    return accepted("Select Shortest Path", before);
}

core::Result<DispatchReceipt> ApplicationSession::invoke_tool(const InvokeToolAction& action) {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    editor::ProjectCommandAdmission admission;
    editor::ToolContext context(admission, document_, selection_);
    AffectedSet affected;
    std::optional<ParameterPayload> parameters;
    std::optional<editor::AuthoringContext> invocation_context;
    if (auto authoring = authoring_context(); authoring) {
        invocation_context = authoring.value();
        affected.objects = authoring.value().objects;
        affected.vertices = authoring.value().vertices;
        affected.edges = authoring.value().edges;
        affected.faces = authoring.value().faces;
        if (authoring.value().component_object.has_value()) {
            affected.objects.push_back(*authoring.value().component_object);
        }
        if (action.tool_id == "mesh.extrude-face") {
            parameters = ParameterPayload{
                std::nullopt,
                action.arguments.distance,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt};
        } else if (action.tool_id == "mesh.inset-face") {
            parameters = ParameterPayload{
                std::nullopt,
                action.arguments.distance,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt};
        } else if (action.tool_id == "mesh.bevel-edge") {
            parameters = ParameterPayload{
                std::nullopt,
                action.arguments.distance,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.segments};
        } else if (action.tool_id == "mesh.bevel-vertex") {
            parameters = ParameterPayload{
                std::nullopt,
                action.arguments.distance,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.segments};
        } else if (action.tool_id == "mesh.remove-face") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.remove_orphaned_vertices,
                std::nullopt,
                std::nullopt,
                std::nullopt};
        } else if (action.tool_id == "mesh.set-vertex-position") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                action.arguments.position,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt};
        } else if (action.tool_id == "mesh.split-edge") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.factor,
                std::nullopt,
                std::nullopt};
        } else if (action.tool_id == "mesh.slide-vertex") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.factor,
                action.arguments.support_edge,
                std::nullopt};
            if (action.arguments.support_edge.has_value()) {
                affected.edges.push_back(*action.arguments.support_edge);
            }
        }
    }
    std::optional<NativeOperationTraceEntry> trace_candidate;
    if (active_source_ == OperationSource::human && invocation_context.has_value() &&
        invocation_context->component_object.has_value()) {
        const scene::ObjectId target_object = *invocation_context->component_object;
        const auto* object = before_document.scene().find(target_object);
        if (object != nullptr && object->mesh_asset.has_value()) {
            const auto mesh = before_document.meshes().find(*object->mesh_asset);
            if (mesh != before_document.meshes().end() && action.tool_id == "mesh.inset-face" &&
                invocation_context->faces.size() == 1U) {
                const auto face_id = invocation_context->faces.front();
                const auto* face = mesh->second.find_face(face_id);
                double longest_edge = 0.0;
                if (face != nullptr && face->vertices.size() >= 3U) {
                    for (std::size_t index = 0U; index < face->vertices.size(); ++index) {
                        const auto* first = mesh->second.find_vertex(face->vertices[index]);
                        const auto* second = mesh->second.find_vertex(
                            face->vertices[(index + 1U) % face->vertices.size()]);
                        if (first == nullptr || second == nullptr) {
                            longest_edge = 0.0;
                            break;
                        }
                        longest_edge = std::max(longest_edge,
                            (second->position - first->position).length());
                    }
                }
                const double normalized_distance = longest_edge > 0.0
                    ? action.arguments.distance / longest_edge : 0.0;
                std::optional<assets::Sha256Digest> structure_signature;
                const auto descriptor = structure_signature_for_mesh(mesh->second, face_id);
                if (descriptor) structure_signature = descriptor.value();
                if (std::isfinite(normalized_distance) && normalized_distance > 0.0) {
                    trace_candidate = NativeOperationTraceEntry{
                        0U, action.tool_id, OperationSource::human, before, {}, target_object, face_id,
                        NativeTraceParameterization::normalized_face_distance,
                        normalized_distance, {}, structure_signature, std::nullopt};
                }
            } else if (mesh != before_document.meshes().end() &&
                action.tool_id == "mesh.extrude-face" &&
                invocation_context->faces.size() == 1U) {
                const auto face_id = invocation_context->faces.front();
                const auto* face = mesh->second.find_face(face_id);
                double longest_edge = 0.0;
                if (face != nullptr && face->vertices.size() >= 3U) {
                    for (std::size_t index = 0U; index < face->vertices.size(); ++index) {
                        const auto* first = mesh->second.find_vertex(face->vertices[index]);
                        const auto* second = mesh->second.find_vertex(
                            face->vertices[(index + 1U) % face->vertices.size()]);
                        if (first == nullptr || second == nullptr) {
                            longest_edge = 0.0;
                            break;
                        }
                        longest_edge = std::max(longest_edge,
                            (second->position - first->position).length());
                    }
                }
                const double normalized_distance = longest_edge > 0.0
                    ? action.arguments.distance / longest_edge : 0.0;
                std::optional<assets::Sha256Digest> structure_signature;
                const auto descriptor = structure_signature_for_mesh(mesh->second, face_id);
                if (descriptor) structure_signature = descriptor.value();
                if (std::isfinite(normalized_distance) && normalized_distance > 0.0) {
                    trace_candidate = NativeOperationTraceEntry{
                        0U, action.tool_id, OperationSource::human, before, {}, target_object, face_id,
                        NativeTraceParameterization::normalized_face_distance,
                        normalized_distance, {}, structure_signature, std::nullopt};
                }
            } else if (mesh != before_document.meshes().end() &&
                action.tool_id == "mesh.poke-face" &&
                invocation_context->faces.size() == 1U) {
                const auto face_id = invocation_context->faces.front();
                std::optional<assets::Sha256Digest> structure_signature;
                const auto descriptor = structure_signature_for_mesh(mesh->second, face_id);
                if (descriptor) structure_signature = descriptor.value();
                trace_candidate = NativeOperationTraceEntry{
                    0U, action.tool_id, OperationSource::human, before, {}, target_object, face_id,
                    NativeTraceParameterization::parameterless,
                    0.0, {}, structure_signature, std::nullopt};
            } else if (action.tool_id == "mesh.set-vertex-position" &&
                invocation_context->vertices.size() == 1U) {
                const auto vertex_id = invocation_context->vertices.front();
                const auto* vertex = mesh->second.find_vertex(vertex_id);
                if (vertex != nullptr) {
                    const auto displacement = action.arguments.position - vertex->position;
                    const double magnitude = displacement.length();
                    std::optional<assets::Sha256Digest> structure_signature;
                    const auto descriptor = structure_signature_for_mesh(
                        mesh->second, vertex_id);
                    if (descriptor) structure_signature = descriptor.value();
                    if (displacement.finite() && std::isfinite(magnitude) && magnitude > 0.0) {
                        trace_candidate = NativeOperationTraceEntry{
                            0U, action.tool_id, OperationSource::human, before, {}, target_object, vertex_id,
                            NativeTraceParameterization::relative_displacement,
                            0.0, displacement, structure_signature, std::nullopt};
                    }
                }
            }
        }
    }
    if (auto result = tools_.invoke(action.tool_id, context, action.arguments, history_); !result) {
        return failure(result.error().with_context("tool invocation"));
    }
    auto result = accept_command_mutation(
        "Invoke Tool: " + action.tool_id,
        before,
        std::move(before_document),
        std::move(affected),
        std::move(parameters));
    if (result && result.value().document_changed) {
        if (trace_candidate.has_value() && result.value().source == OperationSource::human) {
            const auto* result_object = document_.scene().find(trace_candidate->target_object);
            if (result_object != nullptr && result_object->mesh_asset.has_value()) {
                const auto result_mesh = document_.meshes().find(*result_object->mesh_asset);
                if (result_mesh != document_.meshes().end()) {
                    const auto signature = structure_signature_for_mesh(
                        result_mesh->second, std::monostate{});
                    if (signature) {
                        trace_candidate->result_object_structure_signature = signature.value();
                    }
                }
            }
            trace_candidate->operation_id = result.value().operation_id;
            trace_candidate->revision_before = result.value().revision_before;
            trace_candidate->revision_after = result.value().revision_after;
            trace_candidate->source = result.value().source;
            append_native_operation_trace(std::move(*trace_candidate));
        } else {
            native_operation_trace_.clear();
        }
    }
    if (result && (action.tool_id == "mesh.inset-face" ||
                   action.tool_id == "mesh.poke-face" ||
                   action.tool_id == "mesh.remove-face" ||
                   action.tool_id == "mesh.split-edge" ||
                   action.tool_id == "mesh.bevel-edge" ||
                   action.tool_id == "mesh.bevel-vertex" ||
                   action.tool_id == "mesh.dissolve-edge" ||
                   action.tool_id == "mesh.tri-to-quad" ||
                   action.tool_id == "mesh.merge-vertices")) {
        selection_.clear();
    }
    if (result) {
        last_tool_action_ = action;
        last_tool_context_ = std::move(invocation_context);
    }
    return result;
}

core::Result<void> ApplicationSession::rebind_last_tool_selection() {
    if (!last_tool_action_.has_value() || !last_tool_context_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "repeat-last requires a previously accepted tool invocation"));
    }

    const auto& saved = *last_tool_context_;
    if (auto result = saved.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "repeat-last authoring context"));
    }

    selection_.clear();
    if (auto result = selection_.set_mode(saved.selection_mode); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "repeat-last selection mode"));
    }

    if (saved.selection_mode == editor::SelectionMode::object) {
        for (const scene::ObjectId object : saved.objects) {
                if (auto result = selection_.select_object(
                    document_.scene(), object,
                    selection_.empty()
                        ? editor::SelectionOperation::replace
                    : editor::SelectionOperation::add);
                !result) {
                return core::Result<void>::failure(result.error().with_context(
                    "repeat-last object selection"));
            }
        }
    } else {
        if (!saved.component_object.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "repeat-last component context has no owning scene object"));
        }
        const auto* object = document_.scene().find(*saved.component_object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "repeat-last owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(invalid_state(
                "repeat-last owner object has no mesh asset"));
        }
        const auto mesh = document_.meshes().find(*object->mesh_asset);
        if (mesh == document_.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "repeat-last owner object references a missing mesh"));
        }
        const auto bind = [&](auto&& ids, auto&& select) -> core::Result<void> {
            bool first = true;
            for (const auto id : ids) {
                if (auto result = select(
                        id,
                        first ? editor::SelectionOperation::replace
                              : editor::SelectionOperation::add);
                    !result) {
                    return result;
                }
                first = false;
            }
            return core::Result<void>::success();
        };
        core::Result<void> rebound = core::Result<void>::success();
        if (saved.selection_mode == editor::SelectionMode::vertex) {
            rebound = bind(saved.vertices, [&](geometry::VertexId id, editor::SelectionOperation op) {
                return selection_.select_vertex(document_.scene(), *saved.component_object,
                                                mesh->second, id, op);
            });
        } else if (saved.selection_mode == editor::SelectionMode::edge) {
            rebound = bind(saved.edges, [&](geometry::EdgeId id, editor::SelectionOperation op) {
                return selection_.select_edge(document_.scene(), *saved.component_object,
                                              mesh->second, id, op);
            });
        } else {
            rebound = bind(saved.faces, [&](geometry::FaceId id, editor::SelectionOperation op) {
                return selection_.select_face(document_.scene(), *saved.component_object,
                                              mesh->second, id, op);
            });
        }
        if (!rebound) {
            return core::Result<void>::failure(rebound.error().with_context(
                "repeat-last selection rebind"));
        }
    }
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::repeat_last_tool() {
    const editor::SelectionState before_selection = selection_;
    if (auto result = rebind_last_tool_selection(); !result) {
        selection_ = before_selection;
        return failure(result.error());
    }
    auto result = invoke_tool(*last_tool_action_);
    if (!result) selection_ = before_selection;
    return result;
}

core::Result<DispatchReceipt> ApplicationSession::adjust_last_tool(
    const AdjustLastToolAction& action) {
    if (!last_tool_action_.has_value() || !last_tool_context_.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "adjust-last requires a previously accepted tool invocation"));
    }
    const editor::SelectionState before_selection = selection_;
    const InvokeToolAction previous_action = *last_tool_action_;
    const editor::AuthoringContext previous_context = *last_tool_context_;
    last_tool_action_->arguments = action.arguments;
    if (auto result = rebind_last_tool_selection(); !result) {
        last_tool_action_ = previous_action;
        last_tool_context_ = previous_context;
        selection_ = before_selection;
        return failure(result.error().with_context("adjust-last selection"));
    }
    auto result = invoke_tool(*last_tool_action_);
    if (!result) {
        last_tool_action_ = previous_action;
        last_tool_context_ = previous_context;
        selection_ = before_selection;
    }
    return result;
}

core::Result<DispatchReceipt> ApplicationSession::create_box(const CreateBoxAction& action) {
    auto mesh = geometry::make_box(action.size);
    if (!mesh) {
        return failure(mesh.error().with_context("create box"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission,
            document_, action.object_name, std::move(mesh.value())),
        "Create Box");
}

core::Result<DispatchReceipt> ApplicationSession::create_plane(const CreatePlaneAction& action) {
    auto mesh = geometry::make_plane(action.width, action.depth);
    if (!mesh) {
        return failure(mesh.error().with_context("create plane"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission,
            document_, action.object_name, std::move(mesh.value())),
        "Create Plane");
}

core::Result<DispatchReceipt> ApplicationSession::create_mesh_object(
    const CreateMeshObjectAction& action) {
    if (action.source_record.has_value()) {
        if (active_source_ != OperationSource::human) {
            return failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "source-backed mesh import requires explicit human review"));
        }
        if (auto result = action.source_record->validate(); !result) {
            return failure(result.error().with_context("source-backed mesh import"));
        }
        if (!project_path_.has_value()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "source-linked mesh imports require a saved project"));
        }
        if (auto result = verify_source_blob(*project_path_, *action.source_record); !result) {
            return failure(result.error().with_context("source-backed mesh bytes"));
        }
        if (auto result = action.mesh.validate(); !result) {
            return failure(result.error().with_context("source-backed mesh geometry"));
        }
        if (canonical_mesh_geometry_digest(action.mesh) !=
            action.source_record->artifact_digest) {
            return failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "source mesh artifact digest does not match the imported geometry"));
        }
        const auto active = document_.source_asset_registry().records().find(
            action.source_record->identity);
        if (active != document_.source_asset_registry().records().end()) {
            if (active->second.kind != assets::SourceAssetKind::mesh ||
                active->second.artifact_identity != action.source_record->artifact_identity) {
                return failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "source reimport cannot change its bound mesh artifact"));
            }
            auto candidate = *action.source_record;
            candidate.source_generation = active->second.source_generation;
            if (candidate.serialize() == active->second.serialize()) {
                return accepted("Import Source Mesh · No Changes", document_.revision());
            }
        }
        editor::ProjectCommandAdmission admission;
        return execute_command(
            std::make_unique<editor::ImportSourceMeshCommand>(
                admission,
                document_,
                action.object_name,
                action.mesh,
                *action.source_record),
            "Import Source Mesh");
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission,
            document_, action.object_name, action.mesh),
        "Create Mesh Object");
}

core::Result<DispatchReceipt> ApplicationSession::append_sculpt_stroke(
    const AppendSculptStrokeAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "SculptStack authoring requires the human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack authoring snapshot no longer matches the project revision"));
    }
    if (selection_.mode() != editor::SelectionMode::vertex ||
        selection_.selected_vertices().size() != 1U ||
        !selection_.component_object().has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "SculptStack point strokes require exactly one selected mesh vertex"));
    }

    const scene::ObjectId object_id = *selection_.component_object();
    const scene::SceneObject* object = document_.scene().find(object_id);
    if (!object || !object->mesh_asset.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack selection owner or mesh asset is missing"));
    }
    if (object->locked || !object->visible) {
        return failure(invalid_state(
            "hidden or locked scene objects cannot receive sculpt strokes"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh = document_.meshes().find(mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack selection references a missing mesh asset"));
    }
    if (auto result = selection_.validate(document_.scene(), &mesh->second); !result) {
        return failure(result.error().with_context("SculptStack selection"));
    }
    const geometry::VertexId vertex_id = selection_.selected_vertices().front();
    const geometry::Vertex* vertex = mesh->second.find_vertex(vertex_id);
    if (!vertex) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack selected vertex is no longer present in the source mesh"));
    }

    production::SculptStroke stroke;
    stroke.mode = action.mode;
    stroke.radius = action.radius;
    stroke.strength = action.strength;
    stroke.samples.push_back(production::SculptStrokeSample{vertex->position, 1.0});
    return commit_sculpt_stroke(
        mesh_asset,
        object_id,
        std::move(stroke),
        "Add Sculpt Stroke",
        AffectedSet{{object_id}, {vertex_id}, {}, {}},
        ParameterPayload{
            std::nullopt, action.radius, vertex->position, std::nullopt,
            action.strength, std::nullopt, std::nullopt});
}

core::Result<DispatchReceipt> ApplicationSession::append_sculpt_surface_stroke(
    const AppendSculptSurfaceStrokeAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "Sculpt surface authoring requires the human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "Sculpt surface snapshot no longer matches the project revision"));
    }
    const scene::SceneObject* object = document_.scene().find(action.object);
    if (object == nullptr || !object->mesh_asset.has_value() ||
        *object->mesh_asset != action.mesh_asset) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "Sculpt surface target no longer matches its authored mesh"));
    }
    if (object->locked || !object->visible) {
        return failure(invalid_state(
            "hidden or locked scene objects cannot receive sculpt surface strokes"));
    }
    const auto mesh = document_.meshes().find(action.mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "Sculpt surface target mesh is missing"));
    }
    if (action.samples.empty() ||
        action.samples.size() > production::kMaxSculptSamplesPerStroke) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "Sculpt surface stroke sample count is outside its supported bound"));
    }

    production::SculptStroke stroke;
    stroke.mode = action.mode;
    stroke.radius = action.radius;
    stroke.strength = action.strength;
    stroke.samples = action.samples;
    std::vector<geometry::VertexId> affected_vertices;
    std::vector<geometry::FaceId> affected_faces;
    affected_vertices.reserve(action.samples.size() * 3U);
    affected_faces.reserve(action.samples.size());
    for (const production::SculptStrokeSample& sample : action.samples) {
        if (!sample.surface_anchor.has_value()) {
            return failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "Sculpt surface stroke samples must retain stable source anchors"));
        }
        const production::SculptSurfaceAnchor& anchor = *sample.surface_anchor;
        if (auto result = anchor.validate(); !result) {
            return failure(result.error().with_context("Sculpt surface stroke anchor"));
        }
        const auto resolved = production::resolve_sculpt_surface_anchor(mesh->second, anchor);
        if (!resolved) {
            return failure(resolved.error().with_context("Sculpt surface stroke source"));
        }
        if (sample.position.x != resolved.value().x ||
            sample.position.y != resolved.value().y ||
            sample.position.z != resolved.value().z) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "Sculpt surface sample position no longer matches its source anchor"));
        }
        affected_faces.push_back(anchor.face);
        affected_vertices.insert(
            affected_vertices.end(), anchor.vertices.begin(), anchor.vertices.end());
    }
    std::sort(affected_vertices.begin(), affected_vertices.end(),
        [](geometry::VertexId left, geometry::VertexId right) {
            return left.value < right.value;
        });
    affected_vertices.erase(
        std::unique(affected_vertices.begin(), affected_vertices.end()),
        affected_vertices.end());
    std::sort(affected_faces.begin(), affected_faces.end(),
        [](geometry::FaceId left, geometry::FaceId right) {
            return left.value < right.value;
        });
    affected_faces.erase(std::unique(affected_faces.begin(), affected_faces.end()),
        affected_faces.end());

    return commit_sculpt_stroke(
        action.mesh_asset,
        action.object,
        std::move(stroke),
        "Add Sculpt Surface Stroke",
        AffectedSet{{action.object}, std::move(affected_vertices), {}, std::move(affected_faces)},
        ParameterPayload{
            std::nullopt, action.radius, action.samples.front().position, std::nullopt,
            action.strength, std::nullopt, std::nullopt});
}

core::Result<DispatchReceipt> ApplicationSession::commit_sculpt_stroke(
    std::uint64_t mesh_asset,
    scene::ObjectId object_id,
    production::SculptStroke stroke,
    std::string action_name,
    AffectedSet affected,
    std::optional<ParameterPayload> parameters) {
    const scene::SceneObject* object = document_.scene().find(object_id);
    if (object == nullptr || !object->mesh_asset.has_value() ||
        *object->mesh_asset != mesh_asset) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "Sculpt stroke owner no longer matches its mesh asset"));
    }
    const auto mesh = document_.meshes().find(mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "Sculpt stroke mesh asset is missing"));
    }

    std::optional<project::SerializedSculptStack> before;
    production::SculptStack stack;
    const auto current = document_.sculpt_stacks().find(mesh_asset);
    if (current == document_.sculpt_stacks().end()) {
        auto created = production::make_sculpt_stack(document_, mesh_asset);
        if (!created) return failure(created.error().with_context("create SculptStack"));
        stack = std::move(created.value());
    } else {
        before = current->second;
        auto decoded = production::SculptStack::deserialize(current->second.bytes);
        if (!decoded) return failure(decoded.error().with_context("read existing SculptStack"));
        stack = std::move(decoded.value());
    }
    stack.format_version = production::kSculptStackVersion;
    for (production::SculptLayer& layer : stack.layers) {
        for (production::SculptStroke& existing : layer.strokes) {
            if (existing.symmetry != production::SculptSymmetryMode::mirror_x) {
                existing.symmetry_plane_x = 0.0;
            }
        }
    }

    production::SculptLayer* target_layer = nullptr;
    for (auto layer = stack.layers.rbegin(); layer != stack.layers.rend(); ++layer) {
        if (layer->visible && layer->weight > 0.0) {
            target_layer = &*layer;
            break;
        }
    }
    if (target_layer == nullptr) {
        std::set<std::string> used_layer_ids;
        for (const production::SculptLayer& layer : stack.layers) {
            used_layer_ids.insert(layer.identity);
        }
        const std::string revision_tag = std::to_string(document_.revision().value());
        std::string layer_id = "ui.layer.rev" + revision_tag;
        for (std::uint32_t suffix = 1U; used_layer_ids.contains(layer_id); ++suffix) {
            layer_id = "ui.layer.rev" + revision_tag + "." + std::to_string(suffix);
        }
        stack.layers.push_back(production::SculptLayer{
            std::move(layer_id), "Sculpt", true, 1.0, {}});
        target_layer = &stack.layers.back();
    }

    const std::string revision_tag = std::to_string(document_.revision().value());
    stroke.identity = "ui.stroke.rev" + revision_tag;
    const auto stroke_identity_exists = [&stack](std::string_view identity) {
        for (const production::SculptLayer& layer : stack.layers) {
            for (const production::SculptStroke& existing : layer.strokes) {
                if (existing.identity == identity) return true;
            }
        }
        return false;
    };
    for (std::uint32_t suffix = 1U; stroke_identity_exists(stroke.identity); ++suffix) {
        stroke.identity = "ui.stroke.rev" + revision_tag + "." + std::to_string(suffix);
    }
    target_layer->strokes.push_back(std::move(stroke));

    if (auto result = stack.validate(); !result) {
        return failure(result.error().with_context("SculptStack point stroke"));
    }
    const auto evaluation = production::compile_sculpt_stack(stack, mesh->second);
    if (!evaluation) {
        return failure(evaluation.error().with_context("SculptStack reference evaluation"));
    }
    const std::string serialized = stack.serialize();
    const auto digest = stack.digest();
    if (serialized.empty() || !digest) {
        return failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "SculptStack stroke did not produce canonical source bytes"));
    }

    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectSculptStackCommand>(
            admission, document_, mesh_asset, std::move(before),
            project::SerializedSculptStack{serialized, digest.value()}),
        std::move(action_name),
        std::move(affected),
        std::move(parameters));
}

core::Result<DispatchReceipt> ApplicationSession::set_sculpt_layer_visibility(
    const SetSculptLayerVisibilityAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "SculptStack layer visibility requires the human command boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack layer visibility snapshot no longer matches the project revision"));
    }
    if (!selection_.component_object().has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "SculptStack layer visibility requires a selected mesh component"));
    }
    const scene::ObjectId object_id = *selection_.component_object();
    const scene::SceneObject* object = document_.scene().find(object_id);
    if (!object || !object->mesh_asset.has_value() ||
        *object->mesh_asset != action.mesh_asset) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack layer visibility target no longer matches the selected mesh"));
    }
    if (object->locked || !object->visible) {
        return failure(invalid_state(
            "hidden or locked scene objects cannot edit SculptStack layers"));
    }
    const auto mesh = document_.meshes().find(action.mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "SculptStack layer visibility target mesh is missing"));
    }
    if (auto result = selection_.validate(document_.scene(), &mesh->second); !result) {
        return failure(result.error().with_context("SculptStack layer selection"));
    }
    const auto current = document_.sculpt_stacks().find(action.mesh_asset);
    if (current == document_.sculpt_stacks().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "selected mesh has no authored SculptStack layers"));
    }
    auto decoded = production::SculptStack::deserialize(current->second.bytes);
    if (!decoded) {
        return failure(decoded.error().with_context("read SculptStack layers"));
    }
    production::SculptStack stack = std::move(decoded.value());
    const auto layer = std::find_if(
        stack.layers.begin(), stack.layers.end(), [&action](const production::SculptLayer& item) {
            return item.identity == action.layer_identity;
        });
    if (layer == stack.layers.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "SculptStack layer identity is not present on the selected mesh"));
    }
    if (layer->visible == action.visible) {
        return accepted("Set Sculpt Layer Visibility", document_.revision());
    }

    layer->visible = action.visible;
    if (auto result = stack.validate(); !result) {
        return failure(result.error().with_context("SculptStack layer visibility"));
    }
    const auto evaluation = production::compile_sculpt_stack(stack, mesh->second);
    if (!evaluation) {
        return failure(evaluation.error().with_context("SculptStack layer evaluation"));
    }
    const std::string serialized = stack.serialize();
    const auto digest = stack.digest();
    if (serialized.empty() || !digest) {
        return failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "SculptStack layer visibility did not produce canonical source bytes"));
    }

    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectSculptStackCommand>(
            admission,
            document_,
            action.mesh_asset,
            current->second,
            project::SerializedSculptStack{serialized, digest.value()}),
        "Set Sculpt Layer Visibility",
        AffectedSet{{object_id}, {}, {}, {}});
}

core::Result<DispatchReceipt> ApplicationSession::duplicate_mesh_instance(
    const DuplicateMeshInstanceAction& action) {
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::DuplicateProjectMeshInstanceCommand>(
            admission,
            document_,
            action.source,
            action.object_name,
            action.local_translation_offset),
        "Duplicate Mesh Instance",
        AffectedSet{{action.source}, {}, {}, {}},
        ParameterPayload{
            std::nullopt,
            std::nullopt,
            action.local_translation_offset,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt});
}

core::Result<DispatchReceipt> ApplicationSession::import_engineering_fixture() {
    auto fixture = engineering::make_dv012_fixture();
    if (!fixture) return failure(fixture.error().with_context("DV012 engineering fixture"));
    const auto serialized = fixture.value().serialize();
    const auto digest = fixture.value().canonical_digest();
    if (!digest) {
        return failure(digest.error().with_context("DV012 engineering fixture digest"));
    }
    const auto current = document_.engineering_authoring();
    if (current.has_value() && current->bytes == serialized && current->digest == digest.value()) {
        return accepted("Import DV012 Engineering Authoring", document_.revision());
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            current,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Import DV012 Engineering Authoring");
}

core::Result<DispatchReceipt> ApplicationSession::import_engineering_authoring(
    const ImportEngineeringAuthoringAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "engineering authoring import requires the human command boundary"));
    }
    const auto decoded = engineering::AuthoringDocument::deserialize(action.serialized_document);
    if (!decoded) return failure(decoded.error().with_context("engineering authoring import"));
    const std::string serialized = decoded.value().serialize();
    const auto digest = decoded.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context("engineering authoring import digest"));
    const auto current = document_.engineering_authoring();
    if (current.has_value() && current->bytes == serialized && current->digest == digest.value()) {
        return accepted("Import Engineering Authoring Document", document_.revision());
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            current,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Import Engineering Authoring Document");
}

core::Result<DispatchReceipt> ApplicationSession::invalidate_engineering_source(
    const InvalidateEngineeringSourceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "engineering source invalidation requires the human command boundary"));
    }
    if (action.source_id == 0U || action.new_revision.empty() || action.new_digest.is_zero()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "engineering source invalidation requires a source, revision, and digest"));
    }
    const auto current = engineering_authoring();
    if (!current) return failure(current.error());
    auto updated = current.value();
    if (auto result = updated.invalidate_source(
            action.source_id, action.new_revision, action.new_digest); !result) {
        return failure(result.error().with_context("engineering source invalidation"));
    }
    const auto serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) {
        return failure(digest.error().with_context("engineering invalidation digest"));
    }
    const auto before = document_.engineering_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectEngineeringAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedEngineeringAuthoring{serialized, digest.value()}),
        "Invalidate Engineering Source");
}

core::Result<DispatchReceipt> ApplicationSession::import_condition_fixture() {
    auto fixture = condition::make_condition_fixture();
    if (!fixture) return failure(fixture.error().with_context("CONDITION source fixture"));
    const auto serialized = fixture.value().serialize();
    const auto digest = fixture.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context("CONDITION source fixture digest"));
    const auto current = document_.condition_authoring();
    if (current.has_value() && current->bytes == serialized && current->digest == digest.value()) {
        return accepted("Import CONDITION Source Fixture", document_.revision());
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission,
            document_,
            current,
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Import CONDITION Source Fixture");
}

core::Result<DispatchReceipt> ApplicationSession::import_condition_authoring(
    const ImportConditionAuthoringAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION authoring import requires the human command boundary"));
    }
    const auto decoded = condition::AuthoringDocument::deserialize(action.serialized_document);
    if (!decoded) return failure(decoded.error().with_context("CONDITION authoring import"));
    const std::string serialized = decoded.value().serialize();
    const auto digest = decoded.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context("CONDITION authoring import digest"));
    const auto current = document_.condition_authoring();
    if (current.has_value() && current->bytes != serialized) {
        const auto current_authoring = condition::AuthoringDocument::deserialize(current->bytes);
        if (!current_authoring) {
            return failure(current_authoring.error().with_context(
                "current CONDITION authoring revision cannot be verified"));
        }
        if (current_authoring.value().project_namespace == decoded.value().project_namespace &&
            decoded.value().project_revision != current_authoring.value().project_revision.next()) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "CONDITION authoring replacement must advance the current namespace by exactly one revision"));
        }
    }
    if (current.has_value() && current->bytes == serialized && current->digest == digest.value()) {
        return accepted("Import CONDITION Authoring Document", document_.revision());
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission,
            document_,
            current,
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Import CONDITION Authoring Document");
}

core::Result<DispatchReceipt> ApplicationSession::bind_condition_asset_object(
    const BindConditionAssetObjectAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION scene-object binding requires the explicit human authoring boundary"));
    }
    if (action.expected_project_revision != document_.revision()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION scene-object binding was based on a stale project revision"));
    }
    if (action.asset_id == 0U || action.object_id.value == 0U) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION scene-object binding requires an asset and object identity"));
    }
    if (document_.scene().find(action.object_id) == nullptr) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "CONDITION scene-object binding target does not exist in this project"));
    }
    auto authoring = condition_authoring();
    if (!authoring) return failure(authoring.error().with_context(
        "CONDITION scene-object binding"));
    if (authoring.value().project_revision != action.expected_authoring_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION scene-object binding was based on a stale authoring revision"));
    }
    if (authoring.value().project_revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "CONDITION authoring revision space is exhausted"));
    }
    const auto asset = std::find_if(authoring.value().assets.begin(),
        authoring.value().assets.end(), [&action](const auto& candidate) {
            return candidate.id == action.asset_id;
        });
    if (asset == authoring.value().assets.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "CONDITION scene-object binding asset does not exist"));
    }
    if (asset->authored_object_id == action.object_id.value) {
        return accepted("Bind CONDITION Asset · No Changes", document_.revision());
    }
    asset->authored_object_id = action.object_id.value;
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid = authoring.value().validate(); !valid) {
        return failure(valid.error().with_context("CONDITION scene-object binding"));
    }
    const std::string serialized = authoring.value().serialize();
    const auto digest = authoring.value().canonical_digest();
    if (!digest) return failure(digest.error().with_context(
        "CONDITION scene-object binding digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission,
            document_,
            document_.condition_authoring(),
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Bind CONDITION Asset to Scene Object");
}

core::Result<DispatchReceipt> ApplicationSession::invalidate_condition_source(
    const InvalidateConditionSourceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "CONDITION source invalidation requires the human command boundary"));
    }
    if (action.source_id == 0U || action.new_revision.empty() || action.new_digest.is_zero()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION source invalidation requires a source, revision, and digest"));
    }
    const auto current = condition_authoring();
    if (!current) return failure(current.error());
    auto updated = current.value();
    if (auto result = updated.invalidate_source(
            action.source_id, action.new_revision, action.new_digest); !result) {
        return failure(result.error().with_context("CONDITION source invalidation"));
    }
    const auto serialized = updated.serialize();
    const auto digest = updated.canonical_digest();
    if (!digest) return failure(digest.error().with_context("CONDITION invalidation digest"));
    const auto before = document_.condition_authoring();
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectConditionAuthoringCommand>(
            admission,
            document_,
            before,
            project::SerializedConditionAuthoring{serialized, digest.value()}),
        "Invalidate CONDITION Source");
}

core::Result<DispatchReceipt> ApplicationSession::apply_source_reimport_plan(
    const ApplySourceReimportPlanAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "source reimport adoption requires explicit human review"));
    }
    if (auto result = action.plan.validate(); !result) {
        return failure(result.error().with_context("source reimport review"));
    }
    const bool has_changes = std::any_of(
        action.plan.entries.begin(), action.plan.entries.end(), [](const auto& entry) {
            return entry.change != assets::SourceReimportChange::unchanged;
        });
    if (!has_changes) {
        const auto verified = assets::apply_source_reimport_plan(
            document_.source_asset_registry(), action.plan);
        if (!verified) return failure(verified.error().with_context(
            "source reimport no-change review"));
        return accepted("Review Source Reimport · No Changes", document_.revision());
    }
    const auto digest = action.plan.canonical_digest();
    if (!digest) return failure(digest.error().with_context("source reimport review digest"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::ApplySourceReimportPlanCommand>(
            admission, document_, action.plan),
        "Apply Source Reimport " + digest.value().hex().substr(0U, 16U));
}

core::Result<DispatchReceipt> ApplicationSession::create_spatial_field(
    const CreateSpatialFieldAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "spatial field creation requires the human authoring boundary"));
    }
    if (document_.spatial_fields().contains(action.identity)) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "spatial field identity is already active"));
    }
    auto field = spatial_field::ScalarField2D::create(
        action.identity, action.semantic, action.origin_uv, action.cell_size,
        action.width, action.height, action.minimum_value, action.maximum_value,
        action.initial_value);
    if (!field) return failure(field.error().with_context("create spatial field"));
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectSpatialFieldCommand>(
            admission, document_, std::nullopt, std::move(field.value())),
        "Create Spatial Field");
}

core::Result<DispatchReceipt> ApplicationSession::paint_spatial_field(
    const PaintSpatialFieldAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "spatial field paint requires the human authoring boundary"));
    }
    const auto found = document_.spatial_fields().find(action.identity);
    if (found == document_.spatial_fields().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "spatial field identity is not present in this project"));
    }
    auto updated = found->second;
    auto receipt = updated.paint(action.brush);
    if (!receipt) return failure(receipt.error().with_context("paint spatial field"));
    if (!receipt.value().changed) {
        return accepted("Paint Spatial Field (No Change)", document_.revision());
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectSpatialFieldCommand>(
            admission, document_, found->second, std::move(updated)),
        "Paint Spatial Field");
}

core::Result<DispatchReceipt> ApplicationSession::create_material(
    const CreateMaterialAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material creation requires the human authoring boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material creation snapshot no longer matches the project revision"));
    }
    if (action.name.empty() || action.name.size() > 128U ||
        action.name.find('\0') != std::string::npos) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "material name is empty, too long, or contains a null byte"));
    }
    if (auto result = action.definition.validate(); !result) {
        return failure(result.error().with_context("material definition"));
    }
    const auto& existing = document_.material_catalog().materials();
    if (existing.size() >= project::MaterialCatalog::kMaxRecordsPerKind) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported, "project material catalog reached its record limit"));
    }
    render::MaterialId next_id = 1U;
    if (!existing.empty()) {
        const render::MaterialId last_id = existing.rbegin()->first;
        if (last_id == std::numeric_limits<render::MaterialId>::max()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state, "material identity space is exhausted"));
        }
        next_id = last_id + 1U;
    }
    render::MaterialAsset material;
    material.id = next_id;
    material.name = action.name;
    material.definition = action.definition;
    material.revision = core::Revision{1U};
    auto catalog = document_.material_catalog();
    if (auto result = catalog.insert_material(std::move(material)); !result) {
        return failure(result.error().with_context("create material"));
    }
    return commit_material_catalog(
        std::move(catalog), action.expected_project_revision, "Create Material");
}

core::Result<DispatchReceipt> ApplicationSession::update_material(
    const UpdateMaterialAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material editing requires the human authoring boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material edit snapshot no longer matches the project revision"));
    }
    const auto& existing = document_.material_catalog().materials();
    const auto found = existing.find(action.material_id);
    if (found == existing.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found, "material edit references a missing material"));
    }
    if (found->second.revision != action.expected_material_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material edit snapshot no longer matches the material source revision"));
    }
    if (found->second.revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "material source revision space is exhausted"));
    }
    if (auto result = action.definition.validate(); !result) {
        return failure(result.error().with_context("material definition"));
    }
    auto updated = found->second;
    updated.definition = action.definition;
    updated.revision = found->second.revision.next();
    auto catalog = document_.material_catalog();
    if (auto result = catalog.replace_material(std::move(updated)); !result) {
        return failure(result.error().with_context("update material"));
    }
    return commit_material_catalog(
        std::move(catalog), action.expected_project_revision, "Update Material");
}

core::Result<DispatchReceipt> ApplicationSession::set_material_region_assignment(
    const SetMaterialRegionAssignmentAction& action) {
    if (active_source_ != OperationSource::human &&
        active_source_ != OperationSource::agent_workbench) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material-region assignment requires the human or scoped Agent Workbench boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material-region assignment snapshot no longer matches the project revision"));
    }
    if (action.key.mesh_asset == 0U || action.key.region_id == 0U ||
        (!action.material.has_value() && action.variant.has_value()) ||
        (action.material.has_value() && action.material.value() == 0U)) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "material-region assignment requires non-zero identities and a material for variants"));
    }

    auto catalog = document_.material_catalog();
    const auto current = catalog.assignments().find(action.key);
    if (!action.material.has_value()) {
        if (current == catalog.assignments().end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "material-region assignment to clear does not exist"));
        }
        if (auto result = catalog.erase_assignment(action.key); !result) {
            return failure(result.error().with_context("clear material-region assignment"));
        }
    } else {
        if (!catalog.materials().contains(action.material.value())) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "material-region assignment references a missing material"));
        }
        if (action.variant.has_value() &&
            !catalog.variants().contains(action.variant.value())) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "material-region assignment references a missing material variant"));
        }
        const project::MaterialRegionAssignment assignment{
            action.key, action.material.value(), action.variant};
        if (current != catalog.assignments().end()) {
            if (current->second.material == assignment.material &&
                current->second.variant == assignment.variant) {
                return failure(invalid_state(
                    "material region already uses the selected material"));
            }
            if (auto result = catalog.replace_assignment(assignment); !result) {
                return failure(result.error().with_context("replace material-region assignment"));
            }
        } else if (auto result = catalog.insert_assignment(assignment); !result) {
            return failure(result.error().with_context("assign material region"));
        }
    }
    return commit_material_catalog(
        std::move(catalog), action.expected_project_revision,
        "Set Material Region Assignment");
}

core::Result<DispatchReceipt> ApplicationSession::set_material_graph(
    const SetMaterialGraphAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material graph authoring requires the human authoring boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material graph edit snapshot no longer matches the project revision"));
    }
    if (action.expected_graph_revision.has_value() !=
        action.expected_graph_digest.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "material graph edits require both an expected source revision and digest"));
    }
    if (auto result = action.graph.validate(); !result) {
        return failure(result.error().with_context("material graph source"));
    }

    auto catalog = document_.material_catalog();
    const auto found = catalog.material_graphs().find(action.graph.identity);
    if (!action.expected_graph_revision.has_value()) {
        if (found != catalog.material_graphs().end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "material graph identity already exists; update its exact source instead"));
        }
        if (action.graph.revision != core::Revision{1U}) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "new material graph source must begin at revision one"));
        }
        if (!action.graph.retired_node_ids.empty()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "new material graph sources cannot invent retired node lineage"));
        }
        if (auto result = catalog.insert_material_graph(action.graph); !result) {
            return failure(result.error().with_context("create material graph"));
        }
    } else {
        if (found == catalog.material_graphs().end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "material graph update references a missing source"));
        }
        if (found->second.revision != *action.expected_graph_revision) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "material graph source revision no longer matches the edit snapshot"));
        }
        const auto current_digest = found->second.canonical_digest();
        if (!current_digest || current_digest.value() != *action.expected_graph_digest) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "material graph source digest no longer matches the edit snapshot"));
        }
        if (found->second.revision.exhausted()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "material graph source revision space is exhausted"));
        }
        if (action.graph.revision != found->second.revision.next()) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "updated material graph must advance exactly one source revision"));
        }
        std::set<std::uint64_t> expected_retired(
            found->second.retired_node_ids.begin(),
            found->second.retired_node_ids.end());
        for (const auto& previous_node : found->second.nodes) {
            const bool retained = std::any_of(
                action.graph.nodes.begin(), action.graph.nodes.end(),
                [&previous_node](const auto& candidate) {
                    return candidate.id == previous_node.id;
                });
            if (!retained) expected_retired.insert(previous_node.id);
        }
        const std::vector<std::uint64_t> expected_retired_ids(
            expected_retired.begin(), expected_retired.end());
        if (action.graph.retired_node_ids != expected_retired_ids) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "graph edits must preserve prior node tombstones and record each removed node exactly once"));
        }
        if (auto result = catalog.replace_material_graph(action.graph); !result) {
            return failure(result.error().with_context("update material graph"));
        }
    }
    return commit_material_catalog(
        std::move(catalog), action.expected_project_revision,
        "Set Material Graph");
}

core::Result<DispatchReceipt> ApplicationSession::set_material_graph_binding(
    const SetMaterialGraphBindingAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material graph binding requires the human authoring boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material graph binding snapshot no longer matches the project revision"));
    }
    const auto& existing = document_.material_catalog().materials();
    const auto material = existing.find(action.material_id);
    if (material == existing.end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "material graph binding references a missing material"));
    }
    if (material->second.revision != action.expected_material_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material graph binding snapshot no longer matches the material source revision"));
    }
    if (material->second.revision.exhausted()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "material source revision space is exhausted"));
    }
    if (action.graph_identity.has_value() != action.expected_graph_revision.has_value() ||
        action.graph_identity.has_value() != action.expected_graph_digest.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "graph binding requires the exact graph identity, revision, and digest"));
    }

    auto catalog = document_.material_catalog();
    if (action.graph_identity.has_value()) {
        const auto graph = catalog.material_graphs().find(*action.graph_identity);
        if (graph == catalog.material_graphs().end()) {
            return failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "material graph binding references a missing graph source"));
        }
        const auto digest = graph->second.canonical_digest();
        if (!digest || graph->second.revision != *action.expected_graph_revision ||
            digest.value() != *action.expected_graph_digest) {
            return failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "material graph source revision or digest no longer matches the binding snapshot"));
        }
        if (auto result = catalog.bind_material_graph(
                action.material_id, *action.graph_identity); !result) {
            return failure(result.error().with_context("bind material graph source"));
        }
    } else if (auto result = catalog.unbind_material_graph(action.material_id); !result) {
        return failure(result.error().with_context("unbind material graph source"));
    }

    auto updated_material = material->second;
    updated_material.revision = material->second.revision.next();
    if (auto result = catalog.replace_material(std::move(updated_material)); !result) {
        return failure(result.error().with_context("advance material graph binding revision"));
    }
    return commit_material_catalog(
        std::move(catalog), action.expected_project_revision,
        action.graph_identity.has_value() ? "Bind Material Graph" : "Unbind Material Graph");
}

core::Result<DispatchReceipt> ApplicationSession::commit_material_catalog(
    project::MaterialCatalog catalog,
    core::Revision expected_project_revision,
    std::string action) {
    if (active_source_ != OperationSource::human &&
        active_source_ != OperationSource::agent_workbench) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "material catalog changes require the human or scoped Agent Workbench boundary"));
    }
    if (document_.revision() != expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material catalog snapshot no longer matches the project revision"));
    }
    if (auto result = catalog.validate(); !result) {
        return failure(result.error().with_context("material catalog validation"));
    }
    if (catalog.serialize() == document_.material_catalog().serialize()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument, "material catalog edit contains no changes"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectMaterialCatalogCommand>(
            admission, document_, document_.material_catalog(), std::move(catalog)),
        std::move(action));
}

core::Result<DispatchReceipt> ApplicationSession::set_road_authoring(
    const SetRoadAuthoringAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "road authoring requires the human authoring boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road authoring snapshot no longer matches the project revision"));
    }
    if (auto result = action.centerline.validate(); !result) {
        return failure(result.error().with_context("road centerline authoring"));
    }
    if (auto result = action.road.validate(); !result) {
        return failure(result.error().with_context("road source authoring"));
    }
    if (action.road.centerline_identity != action.centerline.source_identity ||
        action.road.centerline_revision != action.centerline.source_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "road source must bind the exact authored centerline identity and revision"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectRoadAuthoringCommand>(
            admission, document_, action.centerline, action.road),
        "Set Road Authoring");
}

core::Result<DispatchReceipt> ApplicationSession::set_sweep_source(
    const SetSweepSourceAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "procedural sweep authoring requires the human command boundary"));
    }
    if (document_.revision() != action.expected_project_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "procedural sweep authoring snapshot no longer matches the project revision"));
    }
    if (auto result = action.source.validate(); !result) {
        return failure(result.error().with_context("procedural sweep source authoring"));
    }
    const auto centerline = document_.curve_paths().find(action.source.centerline_identity);
    if (centerline == document_.curve_paths().end() ||
        centerline->second.source_revision != action.source.centerline_revision) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "procedural sweep must bind an exact current centerline source"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::SetProjectSweepSourceCommand>(
            admission, document_, action.source),
        "Set Procedural Sweep Source");
}

core::Result<DispatchReceipt> ApplicationSession::undo() {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    if (auto result = history_.undo(); !result) {
        return failure(result.error().with_context("undo"));
    }
    if (auto result = append_mutation_event("Undo", before, document_.revision()); !result) {
        const auto rollback = history_.redo();
        document_ = std::move(before_document);
        viewport_mesh_cache_.clear();
        if (!rollback) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "durable journal append failed and undo rollback failed: " +
                    rollback.error().message));
        }
        return failure(result.error().with_context("undo rolled back"));
    }
    return accepted("Undo", before);
}

core::Result<DispatchReceipt> ApplicationSession::redo() {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    if (auto result = history_.redo(); !result) {
        return failure(result.error().with_context("redo"));
    }
    if (auto result = append_mutation_event("Redo", before, document_.revision()); !result) {
        const auto rollback = history_.undo();
        document_ = std::move(before_document);
        viewport_mesh_cache_.clear();
        if (!rollback) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "durable journal append failed and redo rollback failed: " +
                    rollback.error().message));
        }
        return failure(result.error().with_context("redo rolled back"));
    }
    return accepted("Redo", before);
}

core::Result<DispatchReceipt> ApplicationSession::accepted(
    std::string action,
    core::Revision revision_before,
    AffectedSet affected,
    std::optional<ParameterPayload> parameters) {
    const bool supported_native_trace_action = active_source_ == OperationSource::human &&
        (action == "Invoke Tool: mesh.inset-face" ||
         action == "Invoke Tool: mesh.extrude-face" ||
         action == "Invoke Tool: mesh.poke-face" ||
         action == "Invoke Tool: mesh.set-vertex-position" ||
         action == "Set Object Transform");
    if (revision_before != document_.revision() && !supported_native_trace_action) {
        native_operation_trace_.clear();
    }
    const OperationId operation_id = next_operation_id_;
    next_operation_id_ = next_operation_id_ == std::numeric_limits<OperationId>::max()
        ? OperationId{1U}
        : next_operation_id_ + 1U;
    return core::Result<DispatchReceipt>::success(DispatchReceipt{
        std::move(action), revision_before, document_.revision(),
        revision_before != document_.revision(), operation_id, active_source_,
        active_provenance_id_,
        std::move(affected), std::move(parameters), {}});
}

void ApplicationSession::append_native_operation_trace(NativeOperationTraceEntry entry) {
    constexpr std::size_t kMaxNativeTraceEntries = 256U;
    if (entry.source != OperationSource::human || entry.operation_id == 0U ||
        !entry.target_object || entry.revision_after <= entry.revision_before) {
        native_operation_trace_.clear();
        return;
    }
    if (!native_operation_trace_.empty() &&
        native_operation_trace_.back().revision_after != entry.revision_before) {
        native_operation_trace_.clear();
    }
    if (native_operation_trace_.size() >= kMaxNativeTraceEntries) {
        native_operation_trace_.erase(native_operation_trace_.begin());
    }
    native_operation_trace_.push_back(std::move(entry));
}

core::Result<DispatchReceipt> ApplicationSession::failure(core::Diagnostic diagnostic) {
    if (problems_.size() >= kMaxProblems) {
        problems_.erase(problems_.begin());
    }
    problems_.push_back(diagnostic);
    return core::Result<DispatchReceipt>::failure(std::move(diagnostic));
}

void ApplicationSession::reset_editor_state() noexcept {
    selection_.clear();
    history_.clear_history();
    problems_.clear();
    last_tool_action_.reset();
    last_tool_context_.reset();
    native_operation_trace_.clear();
}

ApplicationSnapshot ApplicationSession::snapshot() const {
    ApplicationSnapshot result;
    result.project_name = document_.name();
    result.project_path = project_path_;
    result.project_generation = project_generation_;
    result.project_revision = document_.revision();
    result.dirty = document_.revision() != saved_revision_;
    result.workspace = workspace_;
    result.tools = tools_.descriptors();
    result.undo_count = history_.undo_count();
    result.redo_count = history_.redo_count();
    result.problems = problems_;
    result.native_operation_trace = native_operation_trace_;
    const auto append_problem = [&result](core::Diagnostic diagnostic) {
        if (result.problems.size() >= kMaxProblems) {
            result.problems.erase(result.problems.begin());
        }
        result.problems.push_back(std::move(diagnostic));
    };

    result.spatial_fields.reserve(document_.spatial_fields().size());
    for (const auto& [identity, field] : document_.spatial_fields()) {
        const auto digest = field.canonical_digest();
        if (!digest) {
            append_problem(digest.error().with_context("spatial field snapshot"));
            continue;
        }
        result.spatial_fields.push_back(SpatialFieldView{
            identity, field.semantic(), field.source_revision(),
            field.width(), field.height(), digest.value()});
    }
    result.curve_paths.reserve(document_.curve_paths().size());
    for (const auto& [identity, path] : document_.curve_paths()) {
        const std::string encoded = path.serialize();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
        const auto digest = assets::sha256(std::span<const std::uint8_t>(bytes, encoded.size()));
        result.curve_paths.push_back(CurvePathSourceView{
            identity, path.source_revision, path.segments.size(), digest});
    }
    result.road_sources.reserve(document_.road_sources().size());
    for (const auto& [identity, road] : document_.road_sources()) {
        result.road_sources.push_back(RoadSourceView{
            identity, road.source_revision, road.centerline_identity,
            road.centerline_revision, road.width_meters, road.shoulder_meters,
            road.maximum_grade_ratio, road.minimum_curve_radius_meters,
            road.surface_class, road.function_class, road.access_state,
            road.drainage_intent, road.source_digest()});
    }
    result.sweep_sources.reserve(document_.sweep_sources().size());
    for (const auto& [identity, source] : document_.sweep_sources()) {
        static_cast<void>(identity);
        const std::string encoded = source.serialize();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
        result.sweep_sources.push_back(SweepSourceView{
            source, assets::sha256(std::span<const std::uint8_t>(bytes, encoded.size()))});
    }
    result.source_assets.reserve(document_.source_asset_registry().records().size());
    for (const auto& [identity, record] : document_.source_asset_registry().records()) {
        result.source_assets.push_back(SourceAssetView{
            identity, record.relative_source_path, record.kind, record.source_generation,
            record.source_digest, record.importer_identity, record.importer_version,
            record.artifact_identity, record.artifact_digest});
    }
    result.source_asset_tombstone_count =
        document_.source_asset_registry().lineage().size() -
        document_.source_asset_registry().records().size();
    result.materials.reserve(document_.material_catalog().materials().size());
    for (const auto& [identity, material] : document_.material_catalog().materials()) {
        static_cast<void>(identity);
        result.materials.push_back(material);
    }
    result.material_graphs.reserve(
        document_.material_catalog().material_graphs().size());
    for (const auto& [identity, graph] : document_.material_catalog().material_graphs()) {
        result.material_graphs.push_back(MaterialGraphView{
            identity, graph.revision, graph.nodes.size()});
    }
    result.material_graph_bindings.reserve(
        document_.material_catalog().material_graph_bindings().size());
    for (const auto& [material, graph_identity] :
            document_.material_catalog().material_graph_bindings()) {
        const auto graph = document_.material_catalog().material_graphs().find(
            graph_identity.graph_identity);
        bool source_current = false;
        if (graph != document_.material_catalog().material_graphs().end()) {
            const auto digest = graph->second.canonical_digest();
            source_current = digest && graph->second.revision == graph_identity.source_revision &&
                digest.value() == graph_identity.source_digest;
        }
        result.material_graph_bindings.push_back(MaterialGraphBindingView{
            material, graph_identity.graph_identity, graph_identity.source_revision,
            graph_identity.source_digest, source_current});
    }
    result.material_assignments.reserve(
        document_.material_catalog().assignments().size());
    for (const auto& [key, assignment] : document_.material_catalog().assignments()) {
        static_cast<void>(key);
        result.material_assignments.push_back(assignment);
    }

    if (!document_.engineering_authoring().has_value()) {
        result.engineering.status = "No engineering authoring is bound to this project.";
    } else {
        result.engineering.present = true;
        result.engineering.digest = document_.engineering_authoring()->digest;
        const auto decoded = engineering::AuthoringDocument::deserialize(
            document_.engineering_authoring()->bytes);
        if (!decoded) {
            result.engineering.status = "Engineering authoring is invalid; publication is blocked.";
            append_problem(decoded.error().with_context("engineering authoring snapshot"));
        } else {
            const auto& authoring = decoded.value();
            result.engineering.project_namespace = authoring.project_namespace;
            result.engineering.authored_revision = authoring.project_revision;
            result.engineering.source_count = authoring.sources.size();
            result.engineering.sources.reserve(authoring.sources.size());
            for (const auto& source : authoring.sources) {
                result.engineering.sources.push_back(EngineeringAuthoringView::Source{
                    source.id,
                    engineering::source_kind_name(source.kind),
                    source.locator,
                    source.revision,
                    source.digest});
            }
            result.engineering.part_count = authoring.parts.size();
            result.engineering.occurrence_count = authoring.occurrences.size();
            result.engineering.topology_count = authoring.topology.size();
            result.engineering.bom_count = authoring.bom.size();
            result.engineering.requirement_count = authoring.requirements.size();
            result.engineering.characteristic_count = authoring.characteristics.size();
            result.engineering.target_count = authoring.targets.size();
            result.engineering.derived_product_count = authoring.derived_products.size();
            result.engineering.stale =
                std::any_of(authoring.requirements.begin(), authoring.requirements.end(),
                    [](const engineering::EngineeringRequirement& value) {
                        return value.extraction == engineering::ExtractionState::stale;
                    }) ||
                std::any_of(authoring.characteristics.begin(), authoring.characteristics.end(),
                    [](const engineering::InspectionCharacteristic& value) {
                        return value.status == engineering::InspectionStatus::stale;
                    }) ||
                std::any_of(authoring.targets.begin(), authoring.targets.end(),
                    [](const engineering::InspectionTarget& value) {
                        return value.status == engineering::TargetStatus::stale;
                    }) ||
                (authoring.path.has_value() &&
                 authoring.path->status == engineering::DerivedProductStatus::stale) ||
                std::any_of(authoring.derived_products.begin(), authoring.derived_products.end(),
                    [](const engineering::DerivedProductState& value) {
                        return value.status == engineering::DerivedProductStatus::stale;
                    });
            result.engineering.workcell_present = authoring.workcell.has_value();
            result.engineering.path_present = authoring.path.has_value();
            const auto package = engineering::InspectionPackage::publish(
                authoring, document_.revision(), authoring.project_namespace);
            if (!package) {
                result.engineering.status = "Authoring is present; publication is blocked: " +
                    package.error().message;
            } else if (result.engineering.stale) {
                result.engineering.status =
                    "Engineering source drift is recorded; dependent evidence is stale and publication is blocked.";
            } else {
                result.engineering.publication_ready = true;
                const auto admission = engineering::admit_for_vanta(package.value());
                if (admission && admission.value().status ==
                        engineering::VantaAdmissionStatus::admit) {
                    result.engineering.vanta_admissible = true;
                    result.engineering.status =
                        "Publication package is current and admitted at the neutral runtime boundary.";
                } else {
                    result.engineering.status =
                        "Publication package is current; runtime admission is refused.";
                }
            }
        }
    }

    if (!document_.condition_authoring().has_value()) {
        result.condition.status =
            "No CONDITION source-authoring document is bound to this project.";
    } else {
        result.condition.present = true;
        result.condition.digest = document_.condition_authoring()->digest;
        const auto decoded = condition::AuthoringDocument::deserialize(
            document_.condition_authoring()->bytes);
        if (!decoded) {
            result.condition.status =
                "CONDITION source authoring is invalid; publication is blocked.";
            append_problem(decoded.error().with_context("CONDITION authoring snapshot"));
        } else {
            const auto& authoring = decoded.value();
            result.condition.project_namespace = authoring.project_namespace;
            result.condition.authored_revision = authoring.project_revision;
            result.condition.source_count = authoring.sources.size();
            result.condition.sources.reserve(authoring.sources.size());
            for (const auto& source : authoring.sources) {
                result.condition.sources.push_back(ConditionAuthoringView::Source{
                    source.id,
                    std::string(condition::source_kind_name(source.kind)),
                    source.label,
                    source.logical_locator,
                    source.revision,
                    source.digest,
                    source.current,
                    source.reviewed,
                    source.diagnostics});
            }
            result.condition.asset_count = authoring.assets.size();
            result.condition.assets.reserve(authoring.assets.size());
            for (const auto& asset : authoring.assets) {
                result.condition.assets.push_back(ConditionAuthoringView::Asset{
                    asset.id, asset.stable_name,
                    std::string(condition::asset_class_name(asset.asset_class)),
                    asset.authored_object_id});
            }
            result.condition.safe_place_count = authoring.safe_places.size();
            result.condition.safe_places.reserve(authoring.safe_places.size());
            for (const auto& profile : authoring.safe_places) {
                result.condition.safe_places.push_back(ConditionAuthoringView::SafePlace{
                    profile.id, profile.asset_id,
                    {profile.privacy, profile.human_proximity,
                     profile.controllable_entries, profile.supply_access,
                     profile.ambient_noise, profile.visibility_outward,
                     profile.visibility_inward, profile.isolation}});
            }
            result.condition.surface_states.reserve(authoring.surface_states.size());
            for (const auto& surface : authoring.surface_states) {
                result.condition.surface_states.push_back(
                    ConditionAuthoringView::SurfaceState{
                        surface.id, surface.asset_id,
                        std::string(condition::occupancy_state_name(surface.occupancy)),
                        std::string(condition::maintenance_state_name(surface.maintenance)),
                        std::string(condition::damage_state_name(surface.damage)),
                        surface.wetness});
            }
            result.condition.state_branches.reserve(authoring.state_branches.size());
            result.condition.state_branch_count = authoring.state_branches.size();
            for (const auto& branch : authoring.state_branches) {
                result.condition.state_branches.push_back(
                    ConditionAuthoringView::StateBranch{
                        branch.id, branch.parent_branch_id, branch.name,
                        branch.applicability, branch.deltas.size()});
            }
            result.condition.familiarity_count = authoring.familiarity.size();
            result.condition.view_condition_count = authoring.view_conditions.size();
            result.condition.variant_count = authoring.variants.size();
            result.condition.temporal_package_count = authoring.temporal_packages.size();
            result.condition.surface_state_count = authoring.surface_states.size();
            result.condition.interaction_anchor_count = authoring.interaction_anchors.size();
            result.condition.collection_count = authoring.collections.size();
            result.condition.prefab_count = authoring.prefabs.size();
            result.condition.region_kit_count = authoring.region_kits.size();
            result.condition.audio_source_count = authoring.audio_sources.size();
            result.condition.acoustic_zone_count = authoring.acoustic_zones.size();
            result.condition.effect_source_count = authoring.effect_sources.size();
            result.condition.particle_emitter_count = authoring.particle_emitters.size();
            result.condition.field_influence_count = authoring.field_influences.size();
            result.condition.vegetation_scatter_count = authoring.vegetation_scatters.size();
            result.condition.vegetation_scatter_ids.reserve(authoring.vegetation_scatters.size());
            for (const auto& recipe : authoring.vegetation_scatters) {
                result.condition.vegetation_scatter_ids.push_back(recipe.id);
            }
            result.condition.audio_sources.reserve(authoring.audio_sources.size());
            for (const auto& source : authoring.audio_sources) {
                result.condition.audio_sources.push_back(ConditionAuthoringView::AudioSource{
                    source.id, source.asset_id, source.clip_source_id, source.local_transform,
                    std::string(condition::audio_bus_class_name(source.bus)), source.gain,
                    source.minimum_distance, source.maximum_distance,
                    source.spatial, source.enabled});
            }
            result.condition.acoustic_zones.reserve(authoring.acoustic_zones.size());
            for (const auto& zone : authoring.acoustic_zones) {
                result.condition.acoustic_zones.push_back(ConditionAuthoringView::AcousticZone{
                    zone.id, zone.asset_id, zone.local_transform,
                    std::string(condition::acoustic_zone_shape_name(zone.shape)),
                    zone.half_extents, zone.radius, zone.acoustic_class,
                    zone.semantic_region, zone.priority, zone.enabled});
            }
            result.condition.effect_sources.reserve(authoring.effect_sources.size());
            for (const auto& effect : authoring.effect_sources) {
                result.condition.effect_sources.push_back(ConditionAuthoringView::EffectSource{
                    effect.id, effect.asset_id, effect.semantic_owner,
                    effect.normal_operating_state, effect.disabled_operating_state,
                    effect.enabled});
            }
            result.condition.stale = authoring.has_stale_sources();
            const auto package = condition::Package::publish(
                authoring, document_.revision(), authoring.project_namespace);
            if (!package) {
                result.condition.status = "Source authoring is present; publication is blocked: " +
                    package.error().message;
            } else {
                result.condition.publication_ready = true;
                const auto admission = condition::admit_for_consumer(package.value());
                if (admission && admission.value().status ==
                        condition::PackageAdmissionStatus::admit) {
                    result.condition.consumer_admissible = true;
                    result.condition.status =
                        "Source package is current and admitted at the public consumer boundary.";
                } else {
                    result.condition.status =
                        "Source package is current; consumer admission is refused.";
                }
            }
        }
    }

    const auto scene_objects = document_.scene().objects_sorted();
    result.objects.reserve(scene_objects.size());
    for (const auto& object : scene_objects) {
        auto world = document_.scene().world_transform(object.id);
        if (!world) {
            append_problem(world.error().with_context("scene snapshot"));
            result.viewport.error = world.error().with_context("viewport transform");
            continue;
        }
        result.objects.push_back(ObjectView{object, world.value()});
    }

    result.selection.mode = selection_.mode();
    result.selection.objects = selection_.selected_objects();
    result.selection.vertices = selection_.selected_vertices();
    result.selection.edges = selection_.selected_edges();
    result.selection.faces = selection_.selected_faces();
    result.selection.component_object = selection_.component_object();
    result.selection.component_mesh_revision = selection_.component_mesh_revision();
    if (result.selection.component_object.has_value()) {
        const scene::SceneObject* selected_object = document_.scene().find(
            *result.selection.component_object);
        if (selected_object != nullptr && selected_object->mesh_asset.has_value()) {
            result.sculpt_mesh_asset = *selected_object->mesh_asset;
            const auto stored_stack = document_.sculpt_stacks().find(*result.sculpt_mesh_asset);
            if (stored_stack != document_.sculpt_stacks().end()) {
                auto stack = production::SculptStack::deserialize(stored_stack->second.bytes);
                if (!stack) {
                    append_problem(stack.error().with_context("SculptStack snapshot"));
                } else {
                    result.sculpt_layers.reserve(stack.value().layers.size());
                    for (const production::SculptLayer& layer : stack.value().layers) {
                        result.sculpt_layers.push_back(SculptLayerView{
                            layer.identity,
                            layer.name,
                            layer.visible,
                            layer.weight,
                            layer.strokes.size()});
                    }
                }
            }
        }
    }
    result.viewport.project_revision = document_.revision();

    render::RenderScene viewport_scene;
    for (auto cache = viewport_mesh_cache_.begin(); cache != viewport_mesh_cache_.end();) {
        if (!document_.meshes().contains(cache->first)) {
            cache = viewport_mesh_cache_.erase(cache);
        } else {
            ++cache;
        }
    }
    for (const auto& object_view : result.objects) {
        const auto& object = object_view.object;
        if (!object.visible || !object.mesh_asset.has_value()) {
            continue;
        }
        const auto mesh = document_.meshes().find(*object.mesh_asset);
        if (mesh == document_.meshes().end()) {
            result.viewport.error = core::Diagnostic(
                core::ErrorCode::stale_data,
                "scene object references a missing mesh asset");
            break;
        }
        const auto source_revision = mesh->second.revision();
        const auto stored_stack = document_.sculpt_stacks().find(*object.mesh_asset);
        const std::optional<assets::Sha256Digest> sculpt_stack_digest =
            stored_stack == document_.sculpt_stacks().end()
                ? std::nullopt
                : std::optional<assets::Sha256Digest>{stored_stack->second.digest};
        const auto cached = viewport_mesh_cache_.find(*object.mesh_asset);
        std::shared_ptr<const geometry::CompiledMesh> compiled_mesh;
        if (cached != viewport_mesh_cache_.end() &&
            cached->second.project_generation == project_generation_ &&
            cached->second.source == &mesh->second &&
            cached->second.source_revision == source_revision &&
            cached->second.sculpt_stack_digest == sculpt_stack_digest &&
            cached->second.mesh != nullptr) {
            compiled_mesh = cached->second.mesh;
        } else {
            geometry::EditableMesh viewport_mesh = mesh->second;
            if (stored_stack != document_.sculpt_stacks().end()) {
                auto stack = production::SculptStack::deserialize(stored_stack->second.bytes);
                if (!stack) {
                    result.viewport.error = stack.error().with_context(
                        "SculptStack viewport source");
                    break;
                }
                const auto digest = stack.value().digest();
                if (!digest || digest.value() != stored_stack->second.digest) {
                    result.viewport.error = core::Diagnostic(
                        core::ErrorCode::validation_failed,
                        "SculptStack viewport bytes do not match their recorded digest");
                    break;
                }
                const auto evaluation = production::compile_sculpt_stack(
                    stack.value(), mesh->second);
                if (!evaluation) {
                    result.viewport.error = evaluation.error().with_context(
                        "SculptStack viewport evaluation");
                    break;
                }
                geometry::MeshPatch patch;
                patch.expected_revision = viewport_mesh.revision();
                for (const geometry::Vertex& evaluated : evaluation.value().vertices) {
                    const geometry::Vertex* source = viewport_mesh.find_vertex(evaluated.id);
                    if (source == nullptr) {
                        result.viewport.error = core::Diagnostic(
                            core::ErrorCode::stale_data,
                            "SculptStack viewport output references a missing source vertex");
                        break;
                    }
                    if (source->position.x != evaluated.position.x ||
                        source->position.y != evaluated.position.y ||
                        source->position.z != evaluated.position.z) {
                        patch.vertex_changes.push_back(geometry::VertexChange{
                            evaluated.id, source->position, evaluated.position});
                    }
                }
                if (result.viewport.error.has_value()) break;
                if (!patch.vertex_changes.empty()) {
                    if (auto applied = viewport_mesh.apply_patch(patch); !applied) {
                        result.viewport.error = applied.error().with_context(
                            "SculptStack viewport projection");
                        break;
                    }
                }
            }
            auto compiled = viewport_mesh.compile();
            if (!compiled) {
                result.viewport.error = compiled.error().with_context("viewport compilation");
                break;
            }
            auto owned_mesh = std::make_shared<geometry::CompiledMesh>(std::move(compiled.value()));
            compiled_mesh = owned_mesh;
            viewport_mesh_cache_[*object.mesh_asset] = ViewportMeshCacheEntry{
                project_generation_, &mesh->second, source_revision,
                sculpt_stack_digest, std::move(owned_mesh)};
        }
        if (auto inserted = viewport_scene.upsert({
                object.id, compiled_mesh, object_view.world_transform,
                compiled_mesh->source_revision, object.mesh_asset});
            !inserted) {
            result.viewport.error = inserted.error().with_context("viewport snapshot");
            break;
        }
    }
    if (!result.viewport.error.has_value()) {
        result.viewport.scene = std::move(viewport_scene);
    }
    return result;
}

core::Result<assets::Sha256Digest> ApplicationSession::native_structure_signature(
    scene::ObjectId object_id,
    std::variant<std::monostate, geometry::FaceId, geometry::VertexId> component) const {
    const auto* object = document_.scene().find(object_id);
    if (object == nullptr || !object->mesh_asset.has_value()) {
        return core::Result<assets::Sha256Digest>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "structural correspondence target has no current mesh object"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return core::Result<assets::Sha256Digest>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "structural correspondence target references a missing mesh asset"));
    }
    return structure_signature_for_mesh(mesh->second, component);
}

core::Result<std::vector<NativeStructureMatch>>
ApplicationSession::find_native_structure_matches(
    NativeTraceTargetKind kind,
    const assets::Sha256Digest& signature,
    std::optional<scene::ObjectId> excluded_object) const {
    if (signature.is_zero()) {
        return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "structural correspondence signature is empty"));
    }
    if (kind != NativeTraceTargetKind::object && kind != NativeTraceTargetKind::face &&
        kind != NativeTraceTargetKind::vertex) {
        return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "structural correspondence target kind is unknown"));
    }
    if (document_.scene().size() > kMaxNativeStructureMeshElements) {
        return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "structural correspondence exceeds the bounded scene object limit"));
    }

    std::vector<NativeStructureMatch> matches;
    std::size_t inspected_components = 0U;
    for (const auto& object : document_.scene().objects_sorted()) {
        if ((excluded_object.has_value() && object.id == *excluded_object) ||
            !object.visible || object.locked || !object.mesh_asset.has_value() ||
            (kind == NativeTraceTargetKind::object && object.parent.has_value())) {
            continue;
        }
        const auto mesh = document_.meshes().find(*object.mesh_asset);
        if (mesh == document_.meshes().end()) {
            return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "structural correspondence candidate references a missing mesh asset"));
        }
        const std::size_t component_count = kind == NativeTraceTargetKind::face
            ? mesh->second.face_count()
            : (kind == NativeTraceTargetKind::vertex ? mesh->second.vertex_count() : 1U);
        if (component_count > kMaxNativeStructureMeshElements - inspected_components) {
            return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "structural correspondence exceeds the bounded candidate scan limit"));
        }
        inspected_components += component_count;
        auto index = build_native_structure_signature_index(
            mesh->second,
            kind == NativeTraceTargetKind::face,
            kind == NativeTraceTargetKind::vertex);
        if (!index) {
            return core::Result<std::vector<NativeStructureMatch>>::failure(index.error());
        }

        if (kind == NativeTraceTargetKind::object) {
            if (index.value().object == signature) {
                matches.push_back({object.id, std::monostate{}});
            }
        } else if (kind == NativeTraceTargetKind::face) {
            for (const auto& [face, candidate_signature] : index.value().faces) {
                if (candidate_signature == signature) {
                    matches.push_back({object.id, face});
                }
            }
        } else {
            for (const auto& [vertex, candidate_signature] : index.value().vertices) {
                if (candidate_signature == signature) {
                    matches.push_back({object.id, vertex});
                }
            }
        }
        if (matches.size() > kMaxNativeStructureMatches) {
            return core::Result<std::vector<NativeStructureMatch>>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "structural correspondence exceeds the bounded match result limit"));
        }
    }
    return core::Result<std::vector<NativeStructureMatch>>::success(std::move(matches));
}

} // namespace carto::application
