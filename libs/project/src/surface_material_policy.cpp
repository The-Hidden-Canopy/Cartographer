#include <carto/project/surface_material_policy.hpp>

#include <algorithm>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace carto::project {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

std::string digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

bool valid_regions(const std::vector<MaterialRegionKey>& regions) {
    return !regions.empty() &&
        std::all_of(regions.begin(), regions.end(), [](const auto& region) {
            return region.mesh_asset != 0U && region.region_id != 0U;
        }) && std::is_sorted(regions.begin(), regions.end()) &&
        std::adjacent_find(regions.begin(), regions.end()) == regions.end();
}

core::Result<std::vector<ResolvedRepresentationMaterial>> resolve_regions(
    const std::vector<MaterialRegionKey>& regions,
    const MaterialCatalog& catalog) {
    std::vector<ResolvedRepresentationMaterial> resolved;
    resolved.reserve(regions.size());
    for (const auto& region : regions) {
        const auto assignment = catalog.assignments().find(region);
        if (assignment == catalog.assignments().end()) {
            return core::Result<std::vector<ResolvedRepresentationMaterial>>::failure(
                core::Diagnostic(core::ErrorCode::not_found,
                    "representation material region has no catalog assignment"));
        }
        const auto material = catalog.resolve_material(
            assignment->second.material, assignment->second.variant);
        if (!material) {
            return core::Result<std::vector<ResolvedRepresentationMaterial>>::failure(
                material.error().with_context("representation material policy"));
        }
        resolved.push_back({region, assignment->second.material, assignment->second.variant});
    }
    return core::Result<std::vector<ResolvedRepresentationMaterial>>::success(
        std::move(resolved));
}

} // namespace

core::Result<RepresentationMaterialPolicyOutcome> evaluate_inherited_material_policy(
    const surface::SurfaceRepresentationTree& tree,
    const std::vector<surface::RepresentationInheritanceRecord>& inheritance,
    const std::vector<RepresentationMaterialBinding>& bindings,
    const MaterialCatalog& catalog,
    surface::RepresentationId representation) {
    const surface::SurfaceRepresentationNode* target = nullptr;
    std::map<surface::RepresentationId, const surface::SurfaceRepresentationNode*> nodes;
    for (const auto& node : tree.nodes()) {
        nodes.emplace(node.id, &node);
        if (node.id == representation) target = &node;
    }
    if (target == nullptr) {
        return core::Result<RepresentationMaterialPolicyOutcome>::failure(
            core::Diagnostic(core::ErrorCode::not_found,
                "material policy target is absent from the representation tree"));
    }
    if (auto valid = tree.validate(target->domain); !valid) {
        return core::Result<RepresentationMaterialPolicyOutcome>::failure(valid.error());
    }
    if (auto valid = catalog.validate(); !valid) {
        return core::Result<RepresentationMaterialPolicyOutcome>::failure(valid.error());
    }
    const auto catalog_digest = catalog.canonical_digest();
    if (!catalog_digest) {
        return core::Result<RepresentationMaterialPolicyOutcome>::failure(catalog_digest.error());
    }

    std::map<surface::RepresentationId,
        const surface::RepresentationInheritanceRecord*> records;
    for (const auto& record : inheritance) {
        if (auto valid = record.validate(); !valid) {
            return core::Result<RepresentationMaterialPolicyOutcome>::failure(valid.error());
        }
        const auto node = nodes.find(record.representation);
        if (node == nodes.end() || !node->second->parent.has_value() ||
            *node->second->parent != record.parent ||
            !records.emplace(record.representation, &record).second) {
            return core::Result<RepresentationMaterialPolicyOutcome>::failure(
                invalid("material inheritance does not match the representation tree"));
        }
    }

    std::map<surface::RepresentationId, const RepresentationMaterialBinding*> material_bindings;
    for (const auto& binding : bindings) {
        if (!binding.representation || !nodes.contains(binding.representation) ||
            !valid_regions(binding.source_regions) ||
            !material_bindings.emplace(binding.representation, &binding).second) {
            return core::Result<RepresentationMaterialPolicyOutcome>::failure(
                invalid("material binding has an invalid identity, region set, or duplicate"));
        }
    }

    std::vector<const surface::SurfaceRepresentationNode*> path;
    const surface::SurfaceRepresentationNode* current = target;
    while (current != nullptr) {
        path.push_back(current);
        if (!current->parent.has_value()) break;
        const auto parent = nodes.find(*current->parent);
        if (parent == nodes.end()) {
            return core::Result<RepresentationMaterialPolicyOutcome>::failure(
                invalid("material policy ancestry is incomplete"));
        }
        current = parent->second;
    }
    std::reverse(path.begin(), path.end());

    const std::vector<MaterialRegionKey>* effective_regions = nullptr;
    std::string inheritance_material = "surface-material-inheritance-v1\n";
    inheritance_material.append("root=");
    inheritance_material.append(std::to_string(path.front()->id.value));
    inheritance_material.push_back('\n');
    for (std::size_t index = 0U; index < path.size(); ++index) {
        const auto* node = path[index];
        const auto binding_it = material_bindings.find(node->id);
        const RepresentationMaterialBinding* binding =
            binding_it == material_bindings.end() ? nullptr : binding_it->second;
        if (index == 0U) {
            if (records.contains(node->id) || binding == nullptr) {
                return core::Result<RepresentationMaterialPolicyOutcome>::failure(
                    core::Diagnostic(core::ErrorCode::unsupported,
                        "material policy root requires an explicit catalog-region binding"));
            }
            effective_regions = &binding->source_regions;
            inheritance_material.append("root-regions=");
            for (const auto& region : *effective_regions) {
                inheritance_material.append(std::to_string(region.mesh_asset));
                inheritance_material.push_back(':');
                inheritance_material.append(std::to_string(region.region_id));
                inheritance_material.push_back(',');
            }
            inheritance_material.push_back('\n');
            if (auto resolved = resolve_regions(*effective_regions, catalog); !resolved) {
                return core::Result<RepresentationMaterialPolicyOutcome>::failure(
                    resolved.error());
            }
            continue;
        }
        const auto record_it = records.find(node->id);
        if (record_it == records.end()) {
            return core::Result<RepresentationMaterialPolicyOutcome>::failure(
                core::Diagnostic(core::ErrorCode::unsupported,
                    "representation has no material inheritance declaration"));
        }
        const auto& record = *record_it->second;
        const bool inherits_material = std::binary_search(
            record.inherited_fields.begin(), record.inherited_fields.end(),
            surface::RepresentationPolicyField::material);
        const bool overrides_material = std::binary_search(
            record.override_fields.begin(), record.override_fields.end(),
            surface::RepresentationPolicyField::material);
        const auto encoded = record.serialize();
        if (!encoded) {
            return core::Result<RepresentationMaterialPolicyOutcome>::failure(encoded.error());
        }
        inheritance_material.append(encoded.value());
        inheritance_material.push_back('\n');
        if (inherits_material) {
            if (binding != nullptr && binding->source_regions != *effective_regions) {
                return core::Result<RepresentationMaterialPolicyOutcome>::failure(
                    invalid("inherited material regions differ from the parent binding"));
            }
        } else if (overrides_material) {
            if (binding == nullptr) {
                return core::Result<RepresentationMaterialPolicyOutcome>::failure(
                    core::Diagnostic(core::ErrorCode::unsupported,
                        "material override has no explicit catalog-region binding"));
            }
            effective_regions = &binding->source_regions;
        } else {
            return core::Result<RepresentationMaterialPolicyOutcome>::failure(
                core::Diagnostic(core::ErrorCode::unsupported,
                    "representation does not inherit or override material policy"));
        }
        if (auto resolved = resolve_regions(*effective_regions, catalog); !resolved) {
            return core::Result<RepresentationMaterialPolicyOutcome>::failure(
                resolved.error());
        }
    }

    const auto resolved = resolve_regions(*effective_regions, catalog);
    if (!resolved) {
        return core::Result<RepresentationMaterialPolicyOutcome>::failure(resolved.error());
    }
    std::ostringstream assignment_canonical;
    assignment_canonical << "surface-material-assignments-v1\n";
    for (const auto& item : resolved.value()) {
        assignment_canonical << item.source_region.mesh_asset << ':'
                             << item.source_region.region_id << '=' << item.material << ':';
        if (item.variant) assignment_canonical << *item.variant;
        else assignment_canonical << '-';
        assignment_canonical << '\n';
    }
    const std::string assignment_digest = digest_text(assignment_canonical.str());
    const std::string inheritance_digest = digest_text(inheritance_material);
    std::ostringstream outcome_canonical;
    outcome_canonical << "surface-material-policy-outcome-v1\n"
                      << "representation=" << target->id.value << '\n'
                      << "representation_digest=" << target->content_digest << '\n'
                      << "catalog_digest=" << catalog_digest.value().hex() << '\n'
                      << "inheritance_digest=" << inheritance_digest << '\n'
                      << "assignment_digest=" << assignment_digest << '\n';
    RepresentationMaterialPolicyOutcome outcome{
        target->id, target->content_digest, catalog_digest.value().hex(),
        inheritance_digest, assignment_digest, digest_text(outcome_canonical.str()),
        resolved.value()};
    return core::Result<RepresentationMaterialPolicyOutcome>::success(std::move(outcome));
}

} // namespace carto::project
