#include <carto/production/preparation_scope.hpp>

#include <carto/project/project.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace carto::production {
namespace {

constexpr std::size_t kMaxScopeItems = 65'536U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_kind(PreparationScopeKind kind) {
    return static_cast<unsigned>(kind) <=
        static_cast<unsigned>(PreparationScopeKind::scene);
}

bool canonical_ids(const std::vector<std::uint64_t>& values) {
    return values.size() <= kMaxScopeItems &&
        std::is_sorted(values.begin(), values.end()) &&
        std::adjacent_find(values.begin(), values.end()) == values.end() &&
        std::none_of(values.begin(), values.end(),
            [](std::uint64_t value) { return value == 0U; });
}

std::vector<std::uint64_t> sorted(const std::set<std::uint64_t>& values) {
    return {values.begin(), values.end()};
}

} // namespace

core::Result<void> PreparationScopeRequest::validate() const {
    if (!valid_kind(kind) || expected_source_revision.exhausted() ||
        !canonical_ids(asset_ids) || !canonical_ids(node_ids)) {
        return core::Result<void>::failure(invalid(
            "preparation scope kind, revision, or stable-ID set is invalid"));
    }
    switch (kind) {
    case PreparationScopeKind::asset:
        if (asset_ids.size() != 1U || !node_ids.empty()) {
            return core::Result<void>::failure(invalid(
                "asset scope requires exactly one mesh asset and no scene nodes"));
        }
        break;
    case PreparationScopeKind::selection:
        if (asset_ids.empty() && node_ids.empty()) {
            return core::Result<void>::failure(invalid(
                "selection scope requires at least one stable asset or node ID"));
        }
        break;
    case PreparationScopeKind::assembly:
        if (!asset_ids.empty() || node_ids.size() != 1U) {
            return core::Result<void>::failure(invalid(
                "assembly scope requires exactly one root node and no direct asset IDs"));
        }
        break;
    case PreparationScopeKind::scene:
        if (!asset_ids.empty() || !node_ids.empty()) {
            return core::Result<void>::failure(invalid(
                "scene scope derives all IDs and cannot carry a mutable selection"));
        }
        break;
    }
    return core::Result<void>::success();
}

core::Result<void> ResolvedPreparationScope::validate(
    const project::ProjectDocument& document) const {
    if (auto result = document.validate(); !result) return result;
    if (!valid_kind(kind) || source_revision.exhausted() ||
        source_revision != document.revision() || !canonical_ids(asset_ids) ||
        !canonical_ids(node_ids)) {
        return core::Result<void>::failure(validation(
            "resolved preparation scope is stale or non-canonical"));
    }
    for (const auto asset : asset_ids) {
        if (!document.meshes().contains(asset)) {
            return core::Result<void>::failure(validation(
                "resolved preparation scope references a missing mesh asset"));
        }
    }
    const std::set<std::uint64_t> asset_set(asset_ids.begin(), asset_ids.end());
    const std::set<std::uint64_t> node_set(node_ids.begin(), node_ids.end());
    for (const auto node_id : node_ids) {
        const auto* node = document.scene().find(scene::ObjectId{node_id});
        if (node == nullptr) {
            return core::Result<void>::failure(validation(
                "resolved preparation scope references a missing scene node"));
        }
        if (node->parent.has_value() && !node_set.contains(node->parent->value)) {
            return core::Result<void>::failure(validation(
                "resolved preparation scope is missing a node ancestor"));
        }
        if (node->mesh_asset.has_value() && !asset_set.contains(node->mesh_asset.value())) {
            return core::Result<void>::failure(validation(
                "resolved preparation scope is missing a node's mesh asset"));
        }
    }
    if (kind == PreparationScopeKind::asset &&
        (asset_ids.size() != 1U || !node_ids.empty())) {
        return core::Result<void>::failure(validation(
            "resolved asset scope has an invalid shape"));
    }
    if ((kind == PreparationScopeKind::selection ||
         kind == PreparationScopeKind::assembly) &&
        asset_ids.empty() && node_ids.empty()) {
        return core::Result<void>::failure(validation(
            "resolved selection or assembly scope is empty"));
    }
    if (kind == PreparationScopeKind::scene) {
        if (asset_ids.size() != document.meshes().size() ||
            node_ids.size() != document.scene().size()) {
            return core::Result<void>::failure(validation(
                "resolved scene scope is not complete"));
        }
    }
    return core::Result<void>::success();
}

core::Result<ResolvedPreparationScope> resolve_preparation_scope(
    const project::ProjectDocument& document,
    const PreparationScopeRequest& request) {
    if (auto result = document.validate(); !result) {
        return core::Result<ResolvedPreparationScope>::failure(
            result.error().with_context("preparation scope source"));
    }
    if (auto result = request.validate(); !result) {
        return core::Result<ResolvedPreparationScope>::failure(result.error());
    }
    if (request.expected_source_revision != document.revision()) {
        return core::Result<ResolvedPreparationScope>::failure(stale(
            "preparation scope was captured from a different project revision"));
    }

    const auto objects = document.scene().objects_sorted();
    std::map<std::uint64_t, const scene::SceneObject*> object_map;
    std::map<std::uint64_t, std::vector<std::uint64_t>> children;
    for (const auto& object : objects) {
        object_map.emplace(object.id.value, &object);
        if (object.parent.has_value()) {
            children[object.parent->value].push_back(object.id.value);
        }
    }

    std::set<std::uint64_t> assets;
    std::set<std::uint64_t> nodes;
    const auto add_ancestors = [&object_map, &nodes](std::uint64_t node_id)
        -> core::Result<void> {
        auto found = object_map.find(node_id);
        if (found == object_map.end()) {
            return core::Result<void>::failure(validation(
                "preparation scope references a missing scene node"));
        }
        while (found != object_map.end()) {
            nodes.insert(found->first);
            if (!found->second->parent.has_value()) break;
            found = object_map.find(found->second->parent->value);
            if (found == object_map.end()) {
                return core::Result<void>::failure(validation(
                    "preparation scope node ancestry is incomplete"));
            }
        }
        return core::Result<void>::success();
    };

    switch (request.kind) {
    case PreparationScopeKind::asset:
        assets.insert(request.asset_ids.front());
        break;
    case PreparationScopeKind::selection:
        assets.insert(request.asset_ids.begin(), request.asset_ids.end());
        for (const auto node : request.node_ids) {
            if (auto result = add_ancestors(node); !result) {
                return core::Result<ResolvedPreparationScope>::failure(result.error());
            }
        }
        break;
    case PreparationScopeKind::assembly: {
        const std::uint64_t root = request.node_ids.front();
        if (auto result = add_ancestors(root); !result) {
            return core::Result<ResolvedPreparationScope>::failure(result.error());
        }
        std::vector<std::uint64_t> pending{root};
        for (std::size_t cursor = 0U; cursor < pending.size(); ++cursor) {
            const auto child_list = children.find(pending[cursor]);
            if (child_list == children.end()) continue;
            for (const auto child : child_list->second) {
                if (nodes.insert(child).second) pending.push_back(child);
            }
        }
        break;
    }
    case PreparationScopeKind::scene:
        for (const auto& [asset, mesh] : document.meshes()) {
            static_cast<void>(mesh);
            assets.insert(asset);
        }
        for (const auto& object : objects) nodes.insert(object.id.value);
        break;
    }

    for (const auto asset : assets) {
        if (!document.meshes().contains(asset)) {
            return core::Result<ResolvedPreparationScope>::failure(validation(
                "preparation scope references a missing mesh asset"));
        }
    }
    for (const auto node : nodes) {
        const auto found = object_map.find(node);
        if (found == object_map.end()) {
            return core::Result<ResolvedPreparationScope>::failure(validation(
                "preparation scope references a missing scene node"));
        }
        if (found->second->mesh_asset.has_value()) {
            assets.insert(found->second->mesh_asset.value());
        }
    }

    ResolvedPreparationScope resolved{
        request.kind, document.revision(), sorted(assets), sorted(nodes)};
    if (auto result = resolved.validate(document); !result) {
        return core::Result<ResolvedPreparationScope>::failure(result.error());
    }
    return core::Result<ResolvedPreparationScope>::success(std::move(resolved));
}

core::Result<std::uint64_t> resolve_single_mesh_export(
    const project::ProjectDocument& document,
    std::optional<std::uint64_t> explicit_mesh_asset) {
    if (auto result = document.validate(); !result) {
        return core::Result<std::uint64_t>::failure(result.error());
    }
    if (explicit_mesh_asset.has_value()) {
        if (explicit_mesh_asset.value() == 0U ||
            !document.meshes().contains(explicit_mesh_asset.value())) {
            return core::Result<std::uint64_t>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "explicit export mesh asset does not exist"));
        }
        return core::Result<std::uint64_t>::success(explicit_mesh_asset.value());
    }
    if (document.meshes().empty()) {
        return core::Result<std::uint64_t>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "project has no mesh assets"));
    }
    if (document.meshes().size() != 1U) {
        return core::Result<std::uint64_t>::failure(invalid(
            "mesh export is ambiguous; provide an explicit stable mesh asset ID"));
    }
    return core::Result<std::uint64_t>::success(document.meshes().begin()->first);
}

} // namespace carto::production
