#include <carto/production/material_binding.hpp>
#include <carto/production/authoring_viewport.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace carto::production {
core::Result<std::optional<std::string>> authoring_material_slot_layer(const geometry::EditableMesh& mesh) {
    const bool standard=mesh.attributes().find("cartographer.material_slot")!=nullptr;
    const bool condition=mesh.attributes().find("condition.material_region")!=nullptr;
    if(standard&&condition)return core::Result<std::optional<std::string>>::failure(
        {core::ErrorCode::validation_failed,"mesh declares both material-region aliases; source binding is ambiguous"});
    return core::Result<std::optional<std::string>>::success(standard?std::optional<std::string>{"cartographer.material_slot"}:
        condition?std::optional<std::string>{"condition.material_region"}:std::nullopt);
}

core::Result<render::StandardMaterial>
resolve_authoring_material(const project::MaterialCatalog &catalog,
                           render::MaterialId material_id,
                           std::optional<project::MaterialVariantId> variant) {
    using Result = core::Result<render::StandardMaterial>;
    using Kind = project::MaterialGraphNodeKind;
    using Type = project::MaterialGraphValueType;
    auto material = catalog.resolve_material(material_id, variant);
    if (!material)
        return material;
    const auto binding = catalog.material_graph_bindings().find(material_id);
    if (binding == catalog.material_graph_bindings().end())
        return material;
    const auto fail = [](std::string reason) {
        return Result::failure(
            {core::ErrorCode::validation_failed,
             "authoring material graph: " + std::move(reason)});
    };
    const auto found =
        catalog.material_graphs().find(binding->second.graph_identity);
    if (found == catalog.material_graphs().end())
        return fail("bound graph is missing");
    const auto &graph = found->second;
    if (graph.nodes.size() > 128U)
        return fail("128-node execution budget exceeded");
    const auto current = project::validate_material_graph_source(
        graph, binding->second.graph_identity, binding->second.source_revision,
        binding->second.source_digest);
    if (!current)
        return Result::failure(
            current.error().with_context("authoring material graph"));
    const auto &outputs = graph.outputs;
    if (outputs.normal || outputs.occlusion || outputs.transmission ||
        outputs.subsurface || outputs.sheen || outputs.anisotropy ||
        outputs.opacity || outputs.displacement)
        return fail(
            "normal, occlusion, transmission, subsurface, sheen, "
            "anisotropy, opacity "
            "and displacement outputs are not executable in this viewport");
    const auto order = graph.topological_order();
    if (!order)
        return fail(order.error().message);
    const auto &source = material.value();
    std::map<std::uint64_t, std::array<double, 4>> values;
    for (auto id : order.value()) {
        const auto &node = *graph.find(id);
        std::array<double, 4> value{};
        switch (node.kind) {
        case Kind::constant_scalar:
        case Kind::constant_color:
            value = node.constants;
            break;
        case Kind::parameter_color:
            if (node.parameter_name == "base_color")
                value = {source.base_color_factor.x, source.base_color_factor.y,
                         source.base_color_factor.z,
                         source.base_color_factor.w};
            else if (node.parameter_name == "emissive")
                value = {source.emissive_factor.x * source.emissive_strength,
                         source.emissive_factor.y * source.emissive_strength,
                         source.emissive_factor.z * source.emissive_strength,
                         1};
            else
                return fail("unbound color parameter at node " +
                            std::to_string(id));
            break;
        case Kind::parameter_scalar:
            if (node.parameter_name == "alpha")
                value[0] = source.base_color_factor.w;
            else if (node.parameter_name == "roughness")
                value[0] = source.roughness;
            else if (node.parameter_name == "metallic")
                value[0] = source.metallic;
            else if (node.parameter_name == "dielectric_f0")
                value[0] = source.dielectric_f0;
            else if (node.parameter_name == "clearcoat")
                value[0] = source.clearcoat;
            else if (node.parameter_name == "clearcoat_roughness")
                value[0] = source.clearcoat_roughness;
            else
                return fail("unbound scalar parameter at node " +
                            std::to_string(id));
            break;
        case Kind::add:
        case Kind::multiply:
        case Kind::mix: {
            const auto &a = values.at(node.inputs[0]);
            const auto &b = values.at(node.inputs[1]);
            const double weight =
                node.kind == Kind::mix ? values.at(node.inputs[2])[0] : 0;
            for (std::size_t c = 0;
                 c < (node.value_type == Type::scalar ? 1U : 4U); ++c)
                value[c] = node.kind == Kind::add ? a[c] + b[c]
                           : node.kind == Kind::multiply
                               ? a[c] * b[c]
                               : a[c] * (1 - weight) + b[c] * weight;
            break;
        }
        default:
            return fail(
                "unsupported executable node " + std::to_string(id) +
                " (texture and surface-dependent evaluation unavailable)");
        }
        if (!std::all_of(value.begin(), value.end(), [](double x) {
                return std::isfinite(x) && std::abs(x) <= 1e12;
            }))
            return fail("nonfinite or excessive intermediate at node " +
                        std::to_string(id));
        values.emplace(id, value);
    }
    auto result = source;
    const auto &color = values.at(outputs.base_color);
    result.base_color_factor = {color[0], color[1], color[2],
                                values.at(outputs.alpha)[0]};
    result.roughness = values.at(outputs.roughness)[0];
    result.metallic = values.at(outputs.metallic)[0];
    if (outputs.clearcoat)
        result.clearcoat = values.at(outputs.clearcoat)[0];
    if (outputs.clearcoat_roughness)
        result.clearcoat_roughness = values.at(outputs.clearcoat_roughness)[0];
    if (outputs.emissive) {
        const auto &emission = values.at(outputs.emissive);
        result.emissive_factor = {emission[0], emission[1], emission[2]};
        result.emissive_strength = 1;
    }
    const auto valid = result.validate();
    if (!valid)
        return fail(valid.error().message);
    return Result::success(std::move(result));
}
} // namespace carto::production
