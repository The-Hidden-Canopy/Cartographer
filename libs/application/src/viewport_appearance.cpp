#include <carto/application/viewport_appearance.hpp>

#include <carto/attributes/layer.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/project/material_catalog.hpp>
#include <carto/project/project.hpp>

#include <variant>

namespace carto::application {

MeshAppearance resolve_mesh_appearance(
    const project::ProjectDocument& document, std::uint64_t mesh_asset) {
    MeshAppearance out;
    out.mesh_asset = mesh_asset;
    const auto mesh = document.meshes().find(mesh_asset);
    if (mesh == document.meshes().end()) {
        out.state = FaceAppearanceState::no_region_layer;
        out.detail = "mesh asset is missing";
        return out;
    }
    out.mesh_revision = mesh->second.revision();
    const auto* condition_layer = mesh->second.attributes().find("condition.material_region");
    const auto* generic_layer = mesh->second.attributes().find("cartographer.material_slot");
    if (condition_layer != nullptr && generic_layer != nullptr) {
        out.state = FaceAppearanceState::ambiguous_region_layers;
        out.detail = "mesh has both condition.material_region and cartographer.material_slot";
        return out;
    }
    const auto* layer = condition_layer != nullptr ? condition_layer : generic_layer;
    if (layer == nullptr) {
        out.state = FaceAppearanceState::no_region_layer;
        out.detail = "mesh has no authored material-region layer";
        return out;
    }
    out.region_layer = condition_layer != nullptr ? "condition.material_region"
                                                  : "cartographer.material_slot";
    const auto faces = mesh->second.faces_sorted();
    if (layer->descriptor.domain != attributes::AttributeDomain::face ||
        layer->descriptor.type != attributes::AttributeType::uint32 ||
        layer->values.size() != faces.size()) {
        out.state = FaceAppearanceState::no_region_layer;
        out.detail = "material-region layer is not face-domain uint32 data for every face";
        return out;
    }
    const auto& catalog = document.material_catalog();
    for (std::size_t index = 0U; index < faces.size(); ++index) {
        FaceAppearance face;
        const auto* region = std::get_if<std::uint32_t>(&layer->values[index]);
        if (region == nullptr) {
            face.state = FaceAppearanceState::no_region_layer;
            out.faces.emplace(faces[index].id, face);
            continue;
        }
        const auto assignment = catalog.assignments().find(
            project::MaterialRegionKey{mesh_asset, *region});
        if (assignment == catalog.assignments().end()) {
            face.state = FaceAppearanceState::unassigned_region;
            out.faces.emplace(faces[index].id, face);
            continue;
        }
        const auto resolved = catalog.resolve_material(
            assignment->second.material, assignment->second.variant);
        if (!resolved || !resolved.value().base_color_factor.finite()) {
            face.state = FaceAppearanceState::unresolved_material;
            face.material = assignment->second.material;
            out.faces.emplace(faces[index].id, face);
            continue;
        }
        face.state = FaceAppearanceState::authored;
        face.material = assignment->second.material;
        face.base_color = resolved.value().base_color_factor;
        face.base_color_texture_not_sampled = resolved.value().base_color.asset != 0U;
        out.faces.emplace(faces[index].id, face);
    }
    return out;
}

std::map<std::uint64_t, MeshAppearance> resolve_viewport_appearance(
    const project::ProjectDocument& document) {
    std::map<std::uint64_t, MeshAppearance> out;
    for (const auto& [id, mesh] : document.meshes()) {
        static_cast<void>(mesh);
        out.emplace(id, resolve_mesh_appearance(document, id));
    }
    return out;
}

const char* to_string(FaceAppearanceState state) noexcept {
    switch (state) {
    case FaceAppearanceState::authored: return "authored";
    case FaceAppearanceState::no_region_layer: return "no material-region layer";
    case FaceAppearanceState::ambiguous_region_layers: return "ambiguous material-region layers";
    case FaceAppearanceState::unassigned_region: return "material region has no assignment";
    case FaceAppearanceState::unresolved_material: return "assigned material does not resolve";
    }
    return "unknown";
}

} // namespace carto::application
