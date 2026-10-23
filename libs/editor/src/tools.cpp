#include <carto/editor/tools.hpp>

#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace carto::editor {

namespace {

using core::Diagnostic;
using core::ErrorCode;

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

bool same_vec3(core::Vec3d left, core::Vec3d right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

} // namespace

class LoopCutProjectSelectedEdgeCommand final : public EditorCommand {
public:
    LoopCutProjectSelectedEdgeCommand(
        ProjectCommandAdmission,
        project::ProjectDocument& document,
        const SelectionState& selection)
        : document_(&document), selection_(&selection) {}

    core::Result<void> execute() override {
        if (!document_ || !selection_) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project loop-cut command has incomplete context"));
        }
        if (!edits_.empty()) return redo_edits();
        if (selection_->mode() != SelectionMode::edge) {
            return core::Result<void>::failure(invalid(
                "project loop-cut requires edge selection mode"));
        }
        const auto groups = selection_->component_groups();
        if (groups.empty()) {
            return core::Result<void>::failure(invalid(
                "project loop-cut requires object-bound edge selections"));
        }

        std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
        std::map<std::uint64_t, scene::ObjectId> asset_owners;
        for (const auto& group : groups) {
            if (group.edges.size() != 1U) {
                return core::Result<void>::failure(core::Diagnostic(
                    group.edges.empty() ? core::ErrorCode::invalid_state
                                        : core::ErrorCode::unsupported,
                    "project loop-cut requires exactly one selected seed edge per object"));
            }
            const auto* object = document_->scene().find(group.object);
            if (!object) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "selected loop-cut owner object is no longer present"));
            }
            if (!object->mesh_asset.has_value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "selected loop-cut owner has no mesh asset"));
            }
            const auto mesh = document_->meshes().find(*object->mesh_asset);
            if (mesh == document_->meshes().end()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "selected loop-cut owner references a missing mesh"));
            }
            if (!asset_owners.emplace(*object->mesh_asset, group.object).second) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "project loop-cut refuses repeated or shared mesh assets"));
            }
            owner_meshes.emplace(group.object, &mesh->second);
        }
        if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
            return result;
        }
        std::map<std::uint64_t, std::size_t> mesh_owner_counts;
        for (const auto& object : document_->scene().objects_sorted()) {
            if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
        }
        for (const auto& [asset, owner] : asset_owners) {
            static_cast<void>(owner);
            const auto uses = mesh_owner_counts.find(asset);
            if (uses == mesh_owner_counts.end() || uses->second != 1U) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "project loop-cut refuses mesh assets shared by other objects"));
            }
        }

        project::ProjectDocument candidate = *document_;
        std::vector<AppliedEdit> staged;
        staged.reserve(groups.size());
        for (const auto& group : groups) {
            const auto* object = candidate.scene().find(group.object);
            const std::uint64_t asset = *object->mesh_asset;
            const auto mesh = candidate.meshes().find(asset);
            if (mesh == candidate.meshes().end()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "selected loop-cut owner references a missing candidate mesh"));
            }
            geometry::EditableMesh before = mesh->second;
            const geometry::LoopCutPatch patch{before.revision(), group.edges.front()};
            auto replaced = candidate.apply_loop_cut_patch(asset, patch);
            if (!replaced) return core::Result<void>::failure(replaced.error());
            const auto& records = candidate.topology_receipts();
            if (records.empty() || records.back().mesh_asset != asset ||
                records.back().receipt.revision_before != before.revision() ||
                records.back().receipt.revision_after != replaced.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "project loop-cut did not retain its topology receipt"));
            }
            staged.push_back(AppliedEdit{
                asset, group.object, group.edges.front(), std::move(before),
                candidate.meshes().at(asset), records.back().receipt, replaced.value()});
        }
        if (auto result = candidate.validate(); !result) return result;
        document_->swap(candidate);
        edits_ = std::move(staged);
        return core::Result<void>::success();
    }

    core::Result<void> undo() override {
        if (!document_ || edits_.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project loop-cut has no completed operation to undo"));
        }
        project::ProjectDocument candidate = *document_;
        auto staged = edits_;
        for (auto& edit : staged) {
            if (auto binding = validate_owner_binding(candidate, edit); !binding) {
                return binding;
            }
            const auto mesh = candidate.meshes().find(edit.asset);
            if (mesh == candidate.meshes().end() ||
                mesh->second.revision() != edit.current_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project loop-cut undo no longer matches its source mesh"));
            }
            auto replaced = candidate.replace_mesh_if_revision(
                edit.asset, edit.current_revision, edit.before);
            if (!replaced) return core::Result<void>::failure(replaced.error());
            edit.current_revision = replaced.value();
        }
        if (auto result = candidate.validate(); !result) return result;
        document_->swap(candidate);
        edits_ = std::move(staged);
        return core::Result<void>::success();
    }

    std::string label() const override { return "Loop Cut Selected Edge"; }

private:
    struct AppliedEdit {
        std::uint64_t asset = 0U;
        scene::ObjectId owner;
        geometry::EdgeId edge;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::TopologyEditReceipt receipt;
        core::Revision current_revision;
    };

    core::Result<void> redo_edits() {
        project::ProjectDocument candidate = *document_;
        auto staged = edits_;
        for (auto& edit : staged) {
            if (auto binding = validate_owner_binding(candidate, edit); !binding) {
                return binding;
            }
            const auto mesh = candidate.meshes().find(edit.asset);
            if (mesh == candidate.meshes().end() ||
                mesh->second.revision() != edit.current_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project loop-cut redo no longer matches its source mesh"));
            }
            const geometry::LoopCutPatch patch{edit.current_revision, edit.edge};
            auto replaced = candidate.apply_loop_cut_patch(edit.asset, patch);
            if (!replaced) return core::Result<void>::failure(replaced.error());
            const auto& records = candidate.topology_receipts();
            if (records.empty() || records.back().mesh_asset != edit.asset ||
                records.back().receipt.revision_before != edit.current_revision ||
                records.back().receipt.revision_after != replaced.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "project loop-cut redo did not retain its topology receipt"));
            }
            edit.after = candidate.meshes().at(edit.asset);
            edit.receipt = records.back().receipt;
            edit.current_revision = replaced.value();
        }
        if (auto result = candidate.validate(); !result) return result;
        document_->swap(candidate);
        edits_ = std::move(staged);
        return core::Result<void>::success();
    }

    static core::Result<void> validate_owner_binding(
        const project::ProjectDocument& document,
        const AppliedEdit& edit) {
        const auto* owner = document.scene().find(edit.owner);
        if (!owner || owner->mesh_asset != edit.asset ||
            !document.meshes().contains(edit.asset)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project loop-cut history no longer matches its object and mesh asset"));
        }
        std::size_t owner_count = 0U;
        for (const auto& object : document.scene().objects_sorted()) {
            if (object.mesh_asset == edit.asset) ++owner_count;
        }
        if (owner_count != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "project loop-cut history refuses a mesh asset shared by other objects"));
        }
        return core::Result<void>::success();
    }

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    std::vector<AppliedEdit> edits_;
};

class KnifeCutProjectSelectedFaceCommand final : public EditorCommand {
public:
    KnifeCutProjectSelectedFaceCommand(
        ProjectCommandAdmission,
        project::ProjectDocument& document,
        const SelectionState& selection,
        std::vector<core::Vec3d> path)
        : document_(&document), selection_(&selection), path_(std::move(path)) {}

    core::Result<void> execute() override {
        if (!document_ || !selection_) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project knife-cut command has incomplete context"));
        }
        if (!edits_.empty()) return redo_edit();
        if (selection_->mode() != SelectionMode::face) {
            return core::Result<void>::failure(invalid(
                "project knife cut requires face selection mode"));
        }
        const auto groups = selection_->component_groups();
        if (groups.size() != 1U || groups.front().faces.size() != 1U) {
            return core::Result<void>::failure(invalid(
                "project knife cut requires exactly one object-bound selected face"));
        }
        const ComponentSelectionGroup& group = groups.front();
        const auto* object = document_->scene().find(group.object);
        if (object == nullptr) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected knife-cut owner object is no longer present"));
        }
        if (object->locked || !object->visible) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "knife cut requires a visible, unlocked mesh object"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected knife-cut owner has no mesh asset"));
        }
        const std::uint64_t asset = *object->mesh_asset;
        const auto mesh = document_->meshes().find(asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected knife-cut owner references a missing mesh"));
        }
        std::size_t asset_owner_count = 0U;
        for (const scene::SceneObject& candidate : document_->scene().objects_sorted()) {
            if (candidate.mesh_asset == asset) ++asset_owner_count;
        }
        if (asset_owner_count != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "project knife cut refuses shared mesh assets"));
        }
        const std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes{
            {group.object, &mesh->second}};
        if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
            return result;
        }

        project::ProjectDocument candidate = *document_;
        geometry::EditableMesh before = mesh->second;
        const geometry::KnifeCutPatch patch{
            before.revision(), group.faces.front(), path_};
        auto replaced = candidate.apply_knife_cut_patch(asset, patch);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        const auto& records = candidate.topology_receipts();
        if (records.empty() || records.back().mesh_asset != asset ||
            records.back().receipt.revision_before != before.revision() ||
            records.back().receipt.revision_after != replaced.value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project knife cut did not retain its topology receipt"));
        }
        if (auto result = candidate.validate(); !result) return result;
        const geometry::TopologyEditReceipt receipt = records.back().receipt;
        document_->swap(candidate);
        edits_.push_back(AppliedEdit{
            asset, group.object, group.faces.front(), std::move(before),
            document_->meshes().at(asset), receipt, replaced.value()});
        return core::Result<void>::success();
    }

    core::Result<void> undo() override {
        if (!document_ || edits_.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project knife cut has no completed operation to undo"));
        }
        project::ProjectDocument candidate = *document_;
        AppliedEdit staged = edits_.front();
        if (auto binding = validate_owner_binding(candidate, staged); !binding) return binding;
        const auto mesh = candidate.meshes().find(staged.asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != staged.current_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project knife-cut undo no longer matches its source mesh"));
        }
        auto replaced = candidate.replace_mesh_if_revision(
            staged.asset, staged.current_revision, staged.before);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        staged.current_revision = replaced.value();
        if (auto result = candidate.validate(); !result) return result;
        document_->swap(candidate);
        edits_.front() = std::move(staged);
        return core::Result<void>::success();
    }

    std::string label() const override { return "Knife Cut Selected Face"; }

private:
    struct AppliedEdit {
        std::uint64_t asset = 0U;
        scene::ObjectId owner;
        geometry::FaceId face;
        geometry::EditableMesh before;
        geometry::EditableMesh after;
        geometry::TopologyEditReceipt receipt;
        core::Revision current_revision;
    };

    core::Result<void> redo_edit() {
        project::ProjectDocument candidate = *document_;
        AppliedEdit staged = edits_.front();
        if (auto binding = validate_owner_binding(candidate, staged); !binding) return binding;
        const auto mesh = candidate.meshes().find(staged.asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != staged.current_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project knife-cut redo no longer matches its source mesh"));
        }
        const geometry::KnifeCutPatch patch{
            staged.current_revision, staged.face, path_};
        auto replaced = candidate.apply_knife_cut_patch(staged.asset, patch);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        const auto& records = candidate.topology_receipts();
        if (records.empty() || records.back().mesh_asset != staged.asset ||
            records.back().receipt.revision_before != staged.current_revision ||
            records.back().receipt.revision_after != replaced.value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project knife-cut redo did not retain its topology receipt"));
        }
        staged.after = candidate.meshes().at(staged.asset);
        staged.receipt = records.back().receipt;
        staged.current_revision = replaced.value();
        if (auto result = candidate.validate(); !result) return result;
        document_->swap(candidate);
        edits_.front() = std::move(staged);
        return core::Result<void>::success();
    }

    static core::Result<void> validate_owner_binding(
        const project::ProjectDocument& document,
        const AppliedEdit& edit) {
        const auto* owner = document.scene().find(edit.owner);
        if (owner == nullptr || owner->mesh_asset != edit.asset ||
            !document.meshes().contains(edit.asset)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project knife-cut history no longer matches its object and mesh asset"));
        }
        std::size_t owner_count = 0U;
        for (const scene::SceneObject& candidate : document.scene().objects_sorted()) {
            if (candidate.mesh_asset == edit.asset) ++owner_count;
        }
        if (owner_count != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "project knife-cut history refuses a mesh asset shared by other objects"));
        }
        return core::Result<void>::success();
    }

    project::ProjectDocument* document_;
    const SelectionState* selection_;
    std::vector<core::Vec3d> path_;
    std::vector<AppliedEdit> edits_;
};

ToolContext::ToolContext(
    ProjectCommandAdmission admission,
    project::ProjectDocument& document,
    const SelectionState& selection)
    : admission_(admission), document_(document), selection_(selection) {}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_extrude_selected_face_command(double distance) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<ExtrudeProjectSelectedFaceCommand>(
            admission_,
            document_,
            selection_,
            distance));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_inset_selected_face_command(double distance) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<InsetProjectSelectedFaceCommand>(
            admission_,
            document_,
            selection_,
            distance));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_set_selected_vertex_position_command(core::Vec3d position) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<SetProjectSelectedVertexPositionCommand>(
            admission_,
        document_,
        selection_,
        position));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_set_selected_face_normal_command(core::Vec3d normal) const {
    const double normal_length_squared = normal.length_squared();
    if (!normal.finite() || !std::isfinite(normal_length_squared) ||
        normal_length_squared <= 1e-24) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(invalid(
            "custom face normal must be finite and non-zero"));
    }
    if (selection_.mode() != SelectionMode::face) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(invalid(
            "custom face normals require face selection mode"));
    }
    const auto groups = selection_.component_groups();
    if (groups.size() != 1U || groups.front().faces.size() != 1U) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            groups.empty() ? ErrorCode::invalid_argument : ErrorCode::unsupported,
            "custom face normals require exactly one object-bound selected face"));
    }

    const ComponentSelectionGroup& group = groups.front();
    const auto* object = document_.scene().find(group.object);
    if (object == nullptr || !object->mesh_asset.has_value()) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            ErrorCode::stale_data,
            "selected face owner no longer references a project mesh"));
    }
    if (object->locked) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            ErrorCode::unsupported,
            "custom face normals cannot edit a locked object"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_.meshes().find(mesh_asset);
    if (mesh_iterator == document_.meshes().end()) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            ErrorCode::stale_data,
            "selected face owner references a missing mesh asset"));
    }
    const geometry::EditableMesh& mesh = mesh_iterator->second;
    if (group.mesh_revision != mesh.revision()) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            ErrorCode::stale_data,
            "selected face mesh revision is stale"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes{
        {group.object, &mesh}};
    if (auto result = selection_.validate(document_.scene(), owner_meshes); !result) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(result.error());
    }
    for (const scene::SceneObject& candidate : document_.scene().objects_sorted()) {
        if (candidate.id != group.object && candidate.mesh_asset == object->mesh_asset) {
            return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
                ErrorCode::unsupported,
                "custom face normals refuse mesh assets shared with another object"));
        }
    }

    const auto face_normal = normal.normalized();
    const auto topology = mesh.topology();
    if (!topology) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(topology.error());
    }
    const auto compiled = mesh.compile();
    if (!compiled) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(compiled.error());
    }
    std::map<geometry::VertexId, core::Vec3d> derived_normals;
    for (std::size_t index = 0U; index < compiled.value().vertex_ids.size(); ++index) {
        const auto [entry, inserted] = derived_normals.emplace(
            compiled.value().vertex_ids[index], compiled.value().normals[index]);
        if (!inserted && !same_vec3(entry->second, compiled.value().normals[index])) {
            return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
                ErrorCode::validation_failed,
                "geometry-derived normals disagree for a source vertex"));
        }
    }

    constexpr std::string_view kNormalLayerId = "cartographer.normal0";
    const auto* authored_layer = mesh.attributes().find(kNormalLayerId);
    if (authored_layer != nullptr &&
        (authored_layer->descriptor.domain != attributes::AttributeDomain::corner ||
         authored_layer->descriptor.type != attributes::AttributeType::vec3 ||
         authored_layer->values.size() != topology.value().corners.size())) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            ErrorCode::validation_failed,
            "existing custom normal layer is not a complete corner-domain vec3 layer"));
    }
    std::vector<attributes::AttributeValue> normal_values;
    normal_values.reserve(topology.value().corners.size());
    bool changed = authored_layer == nullptr;
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        const auto& corner = topology.value().corners[index];
        core::Vec3d value;
        if (authored_layer != nullptr) {
            const auto* current = std::get_if<core::Vec3d>(&authored_layer->values[index]);
            if (current == nullptr || !current->finite() ||
                !std::isfinite(current->length_squared()) ||
                current->length_squared() <= 1e-24) {
                return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
                    ErrorCode::validation_failed,
                    "existing custom normal layer contains invalid values"));
            }
            value = *current;
        } else {
            const auto derived = derived_normals.find(corner.vertex);
            if (derived == derived_normals.end() || !derived->second.finite() ||
                derived->second.length_squared() <= 1e-24) {
                return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
                    ErrorCode::validation_failed,
                    "geometry-derived corner normal is unavailable"));
            }
            value = derived->second;
        }
        if (corner.face == group.faces.front()) {
            changed = changed || !same_vec3(value, face_normal);
            value = face_normal;
        }
        normal_values.emplace_back(value);
    }
    if (!changed) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(invalid(
            "selected face already has the requested custom normal"));
    }

    attributes::AttributeSet attribute_payload = mesh.attributes();
    if (authored_layer != nullptr) {
        if (auto result = attribute_payload.remove_layer(kNormalLayerId); !result) {
            return core::Result<std::unique_ptr<EditorCommand>>::failure(result.error());
        }
    }
    if (auto result = attribute_payload.set_domain_count(
            attributes::AttributeDomain::corner, topology.value().corners.size());
        !result) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(result.error());
    }
    if (auto result = attribute_payload.add_layer({
            {std::string{kNormalLayerId}, attributes::AttributeDomain::corner,
             attributes::AttributeType::vec3},
            std::move(normal_values)});
        !result) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(result.error());
    }
    geometry::EditableMesh after = mesh;
    if (auto result = after.set_attribute_payload(
            std::move(attribute_payload), mesh.uv_sets());
        !result) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(result.error());
    }
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<SetProjectMeshCommand>(
            admission_, document_, mesh_asset, document_.revision(), mesh.revision(),
            std::move(after)));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_set_selected_edge_sharp_command(bool sharp) const {
    if (selection_.mode() != SelectionMode::edge) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(invalid(
            "edge sharp marks require edge selection mode"));
    }
    const auto groups = selection_.component_groups();
    if (groups.size() != 1U || groups.front().edges.empty()) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            groups.empty() || (groups.size() == 1U && groups.front().edges.empty())
                ? ErrorCode::invalid_argument : ErrorCode::unsupported,
            "edge sharp marks require selected edges on exactly one object"));
    }

    const ComponentSelectionGroup& group = groups.front();
    const auto* object = document_.scene().find(group.object);
    if (object == nullptr || !object->mesh_asset.has_value()) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            ErrorCode::stale_data,
            "selected edge owner no longer references a project mesh"));
    }
    if (object->locked) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            ErrorCode::unsupported,
            "edge sharp marks cannot edit a locked object"));
    }
    const std::uint64_t mesh_asset = *object->mesh_asset;
    const auto mesh_iterator = document_.meshes().find(mesh_asset);
    if (mesh_iterator == document_.meshes().end()) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            ErrorCode::stale_data,
            "selected edge owner references a missing mesh asset"));
    }
    const geometry::EditableMesh& mesh = mesh_iterator->second;
    if (group.mesh_revision != mesh.revision()) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            ErrorCode::stale_data,
            "selected edge mesh revision is stale"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes{
        {group.object, &mesh}};
    if (auto result = selection_.validate(document_.scene(), owner_meshes); !result) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(result.error());
    }
    for (const scene::SceneObject& candidate : document_.scene().objects_sorted()) {
        if (candidate.id != group.object && candidate.mesh_asset == object->mesh_asset) {
            return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
                ErrorCode::unsupported,
                "edge sharp marks refuse mesh assets shared with another object"));
        }
    }

    constexpr std::string_view kEdgeSharpLayerId = "cartographer.edge_sharp";
    const auto topology = mesh.topology();
    if (!topology) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(topology.error());
    }
    const auto* authored_layer = mesh.attributes().find(kEdgeSharpLayerId);
    if (authored_layer != nullptr &&
        (authored_layer->descriptor.domain != attributes::AttributeDomain::edge ||
         authored_layer->descriptor.type != attributes::AttributeType::boolean ||
         authored_layer->values.size() != topology.value().edges.size())) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
            ErrorCode::validation_failed,
            "existing edge sharp layer is not a complete edge-domain boolean layer"));
    }

    std::map<geometry::EdgeId, std::size_t> edge_indices;
    std::vector<attributes::AttributeValue> sharp_values;
    sharp_values.reserve(topology.value().edges.size());
    bool changed = false;
    for (std::size_t index = 0U; index < topology.value().edges.size(); ++index) {
        const auto& edge = topology.value().edges[index];
        edge_indices.emplace(edge.id, index);
        bool current_sharp = false;
        if (authored_layer != nullptr) {
            const auto* current = std::get_if<bool>(&authored_layer->values[index]);
            if (current == nullptr) {
                return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
                    ErrorCode::validation_failed,
                    "existing edge sharp layer contains non-boolean values"));
            }
            current_sharp = *current;
        }
        sharp_values.emplace_back(current_sharp);
    }
    for (const geometry::EdgeId selected_edge : group.edges) {
        const auto index = edge_indices.find(selected_edge);
        if (index == edge_indices.end()) {
            return core::Result<std::unique_ptr<EditorCommand>>::failure(core::Diagnostic(
                ErrorCode::stale_data,
                "selected edge is no longer present in the current mesh"));
        }
        bool& current_sharp = std::get<bool>(sharp_values[index->second]);
        changed = changed || current_sharp != sharp;
        current_sharp = sharp;
    }
    if (!changed) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(invalid(
            "selected edges already have the requested sharp-mark state"));
    }

    attributes::AttributeSet attribute_payload = mesh.attributes();
    if (authored_layer != nullptr) {
        if (auto result = attribute_payload.remove_layer(kEdgeSharpLayerId); !result) {
            return core::Result<std::unique_ptr<EditorCommand>>::failure(result.error());
        }
    }
    if (auto result = attribute_payload.set_domain_count(
            attributes::AttributeDomain::edge, topology.value().edges.size());
        !result) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(result.error());
    }
    if (auto result = attribute_payload.add_layer({
            {std::string{kEdgeSharpLayerId}, attributes::AttributeDomain::edge,
             attributes::AttributeType::boolean},
            std::move(sharp_values)});
        !result) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(result.error());
    }
    geometry::EditableMesh after = mesh;
    if (auto result = after.set_attribute_payload(
            std::move(attribute_payload), mesh.uv_sets());
        !result) {
        return core::Result<std::unique_ptr<EditorCommand>>::failure(result.error());
    }
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<SetProjectMeshCommand>(
            admission_, document_, mesh_asset, document_.revision(), mesh.revision(),
            std::move(after)));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_slide_selected_vertex_command(
    std::optional<geometry::EdgeId> support_edge,
    double factor) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<SlideProjectSelectedVertexCommand>(
            admission_,
            document_,
            selection_,
            support_edge,
            factor));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_delete_selected_face_command(bool remove_orphaned_vertices) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<DeleteProjectSelectedFaceCommand>(
            admission_,
            document_,
            selection_,
            remove_orphaned_vertices));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_split_selected_edge_command(double factor) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<SplitProjectSelectedEdgeCommand>(
            admission_,
            document_,
            selection_,
            factor));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_loop_cut_selected_edge_command() const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<LoopCutProjectSelectedEdgeCommand>(
            admission_, document_, selection_));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_bevel_selected_edge_command(
    double width,
    std::uint32_t segments) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<BevelProjectSelectedEdgeCommand>(
            admission_,
            document_,
            selection_,
            width,
            segments));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_bevel_selected_vertex_command(
    double width,
    std::uint32_t segments) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<BevelProjectSelectedVertexCommand>(
            admission_,
            document_,
            selection_,
            width,
            segments));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_dissolve_selected_edge_command() const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<DissolveProjectSelectedEdgeCommand>(
            admission_,
            document_,
            selection_));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_tri_to_quad_selected_faces_command() const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<TriToQuadProjectSelectedFacesCommand>(
            admission_,
            document_,
            selection_));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_merge_selected_vertices_command(
    geometry::VertexMergeMode mode) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<MergeProjectSelectedVerticesCommand>(
            admission_,
            document_,
            selection_,
            mode));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_merge_selected_vertices_by_distance_command(double distance) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<MergeProjectSelectedVerticesCommand>(
            admission_,
            document_,
            selection_,
            geometry::VertexMergeMode::target,
            distance));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_poke_selected_face_command() const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<PokeProjectSelectedFaceCommand>(
            admission_,
            document_,
            selection_));
}

core::Result<std::unique_ptr<EditorCommand>>
ToolContext::make_knife_cut_selected_face_command(std::vector<core::Vec3d> path) const {
    return core::Result<std::unique_ptr<EditorCommand>>::success(
        std::make_unique<KnifeCutProjectSelectedFaceCommand>(
            admission_, document_, selection_, std::move(path)));
}

core::Result<void> ToolRegistry::register_tool(
    ToolDescriptor descriptor,
    ToolFactory factory) {
    if (descriptor.id.empty() || descriptor.display_name.empty()) {
        return core::Result<void>::failure(
            invalid("tool registration requires an id and display name"));
    }
    if (!factory) {
        return core::Result<void>::failure(invalid("tool registration requires a factory"));
    }
    if (tools_.contains(descriptor.id)) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::invalid_state, "tool id is already registered"));
    }
    const std::string id = descriptor.id;
    tools_.emplace(id, Entry{std::move(descriptor), std::move(factory)});
    return core::Result<void>::success();
}

core::Result<void> ToolRegistry::register_builtin_tools() {
    if (auto result = register_tool(
        {"mesh.extrude-face", "Extrude selected face",
         ToolKernelKind::editable_mesh_extrude_face},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_extrude_selected_face_command(arguments.distance);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.inset-face", "Inset selected face",
         ToolKernelKind::editable_mesh_inset_face},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_inset_selected_face_command(arguments.distance);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.poke-face", "Poke selected face",
         ToolKernelKind::editable_mesh_poke_face},
        [](const ToolContext& context, const ToolArguments&)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_poke_selected_face_command();
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.remove-face", "Delete selected face",
         ToolKernelKind::editable_mesh_delete_face},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_delete_selected_face_command(
                arguments.remove_orphaned_vertices);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.set-vertex-position", "Set selected vertex position",
         ToolKernelKind::editable_mesh_set_vertex_position},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_set_selected_vertex_position_command(arguments.position);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.set-face-normal", "Set selected face custom normal",
         ToolKernelKind::unclassified},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_set_selected_face_normal_command(arguments.normal);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.set-edge-sharp", "Set selected edge sharp mark",
         ToolKernelKind::unclassified},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            if (!arguments.edge_sharp.has_value()) {
                return core::Result<std::unique_ptr<EditorCommand>>::failure(invalid(
                    "edge sharp mark state must be explicit"));
            }
            return context.make_set_selected_edge_sharp_command(
                arguments.edge_sharp.value());
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.slide-vertex", "Slide selected vertex",
         ToolKernelKind::editable_mesh_slide_vertex},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_slide_selected_vertex_command(
                arguments.support_edge, arguments.factor);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.split-edge", "Split selected edge",
         ToolKernelKind::editable_mesh_split_edge},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_split_selected_edge_command(arguments.factor);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.bevel-edge", "Bevel selected edge",
         ToolKernelKind::editable_mesh_bevel_edge},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_bevel_selected_edge_command(
                arguments.distance, arguments.segments);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.bevel-vertex", "Bevel selected vertex",
         ToolKernelKind::editable_mesh_bevel_vertex},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_bevel_selected_vertex_command(
                arguments.distance, arguments.segments);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.dissolve-edge", "Dissolve selected edge",
         ToolKernelKind::editable_mesh_dissolve_edge},
        [](const ToolContext& context, const ToolArguments&)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_dissolve_selected_edge_command();
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.tri-to-quad", "Convert selected triangles to quad",
         ToolKernelKind::editable_mesh_tri_to_quad},
        [](const ToolContext& context, const ToolArguments&)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_tri_to_quad_selected_faces_command();
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.merge-by-distance", "Merge selected vertices by distance",
         ToolKernelKind::editable_mesh_merge_by_distance},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_merge_selected_vertices_by_distance_command(
                arguments.distance);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.merge-vertices", "Merge selected vertices",
         ToolKernelKind::editable_mesh_merge_vertices},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_merge_selected_vertices_command(arguments.merge_mode);
        });
        !result) {
        return result;
    }
    if (auto result = register_tool(
        {"mesh.knife-cut", "Knife cut selected face",
         ToolKernelKind::editable_mesh_knife_cut},
        [](const ToolContext& context, const ToolArguments& arguments)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_knife_cut_selected_face_command(arguments.knife_path);
        });
        !result) {
        return result;
    }
    return register_tool(
        {"mesh.loop-cut", "Loop cut from selected edge",
         ToolKernelKind::editable_mesh_loop_cut},
        [](const ToolContext& context, const ToolArguments&)
            -> core::Result<std::unique_ptr<EditorCommand>> {
            return context.make_loop_cut_selected_edge_command();
        });
}

core::Result<void> ToolRegistry::invoke(
    std::string_view tool_id,
    const ToolContext& context,
    const ToolArguments& arguments,
    CommandBus& history) const {
    const auto iterator = tools_.find(std::string(tool_id));
    if (iterator == tools_.end()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "tool id is not registered"));
    }
    auto command = iterator->second.factory(context, arguments);
    if (!command) {
        return core::Result<void>::failure(command.error());
    }
    if (!command.value()) {
        return core::Result<void>::failure(
            invalid("tool factory returned a null command"));
    }
    return history.execute(std::move(command.value()));
}

std::vector<ToolDescriptor> ToolRegistry::descriptors() const {
    std::vector<ToolDescriptor> result;
    result.reserve(tools_.size());
    for (const auto& [id, entry] : tools_) {
        static_cast<void>(id);
        result.push_back(entry.descriptor);
    }
    return result;
}

} // namespace carto::editor
