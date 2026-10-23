#include <carto/world/model.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace carto::world {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_WORLD_MODEL";
constexpr std::size_t kMaxSemanticBytes = 128U;
constexpr std::size_t kMaxNameBytes = 256U;
constexpr std::size_t kMaxTagBytes = 128U;
constexpr std::size_t kMaxTagsPerArea = 64U;
constexpr std::size_t kMaxRecipeBytes = 256U;
constexpr std::size_t kMaxIntentBytes = 256U;
constexpr std::size_t kMaxPropertyBytes = 128U;
constexpr std::size_t kMaxValueBytes = 4096U;
constexpr std::uint64_t kMaxGeneratedInstances = 1'000'000'000U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool valid_binding_kind(AuthoringBindingKind kind) {
    switch (kind) {
    case AuthoringBindingKind::none:
    case AuthoringBindingKind::scene_object:
    case AuthoringBindingKind::mesh_asset:
        return true;
    }
    return false;
}

bool valid_target_kind(VariantTargetKind kind) {
    switch (kind) {
    case VariantTargetKind::semantic_area:
    case VariantTargetKind::semantic_path:
    case VariantTargetKind::structure_graph:
    case VariantTargetKind::instance_field:
        return true;
    }
    return false;
}

bool valid_variant_operation(VariantOperation operation) {
    switch (operation) {
    case VariantOperation::set_value:
    case VariantOperation::remove_value:
        return true;
    }
    return false;
}

template <typename Key, typename Value>
core::Result<void> insert_unique(
    std::map<Key, Value>& target,
    Key key,
    Value value,
    std::size_t maximum,
    std::string_view label) {
    if (target.size() >= maximum) {
        return core::Result<void>::failure(validation(
            std::string(label) + " collection has reached its bounded capacity"));
    }
    const auto [iterator, inserted] = target.emplace(key, std::move(value));
    static_cast<void>(iterator);
    if (!inserted) {
        return core::Result<void>::failure(invalid(
            std::string(label) + " identity is duplicated"));
    }
    return core::Result<void>::success();
}

template <typename T>
bool read_integer(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool read_signed_integer(std::istream& input, std::int64_t& value) {
    std::string token;
    if (!(input >> token)) return false;
    std::int64_t parsed = 0;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

template <typename T>
bool sorted_unique_nonzero(const std::vector<T>& values) {
    return std::is_sorted(values.begin(), values.end()) &&
        std::adjacent_find(values.begin(), values.end()) == values.end() &&
        std::all_of(values.begin(), values.end(), [](const T& value) {
            return static_cast<bool>(value);
        });
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_integer(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool read_optional_id(std::istream& input, std::optional<AreaId>& value) {
    std::uint64_t encoded = 0U;
    if (!read_integer(input, encoded)) return false;
    value = encoded == 0U ? std::nullopt : std::optional<AreaId>{AreaId{encoded}};
    return true;
}

bool read_optional_path_id(std::istream& input, std::optional<PathId>& value) {
    std::uint64_t encoded = 0U;
    if (!read_integer(input, encoded)) return false;
    value = encoded == 0U ? std::nullopt : std::optional<PathId>{PathId{encoded}};
    return true;
}

bool read_bool(std::istream& input, bool& value) {
    unsigned encoded = 0U;
    if (!read_integer(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
    return true;
}

bool same_point(core::Vec3d left, core::Vec3d right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

auto variant_sort_key(const VariantEdit& edit) {
    return std::tuple{
        static_cast<std::uint8_t>(edit.target_kind), edit.target_identity,
        edit.property, static_cast<std::uint8_t>(edit.operation), edit.canonical_value};
}

} // namespace

std::string_view authoring_binding_kind_name(AuthoringBindingKind kind) noexcept {
    switch (kind) {
    case AuthoringBindingKind::none: return "none";
    case AuthoringBindingKind::scene_object: return "scene-object";
    case AuthoringBindingKind::mesh_asset: return "mesh-asset";
    }
    return "unknown";
}

core::Result<void> AuthoringBinding::validate() const {
    if (!valid_binding_kind(kind)) {
        return core::Result<void>::failure(invalid("world authoring binding kind is invalid"));
    }
    if ((kind == AuthoringBindingKind::none && identity != 0U) ||
        (kind != AuthoringBindingKind::none && identity == 0U)) {
        return core::Result<void>::failure(validation(
            "world authoring binding kind and identity do not agree"));
    }
    return core::Result<void>::success();
}

core::Result<void> SemanticArea::validate() const {
    if (!id || !safe_text(semantic_type, kMaxSemanticBytes)) {
        return core::Result<void>::failure(invalid(
            "semantic area identity or type is invalid"));
    }
    if (auto result = geometry.validate(); !result) return result;
    if (parent.has_value() && (!parent.value() || parent.value() == id)) {
        return core::Result<void>::failure(validation(
            "semantic area parent is invalid or self-referential"));
    }
    if (tags.size() > kMaxTagsPerArea ||
        !std::is_sorted(tags.begin(), tags.end()) ||
        std::adjacent_find(tags.begin(), tags.end()) != tags.end()) {
        return core::Result<void>::failure(validation(
            "semantic area tags must be bounded, sorted, and unique"));
    }
    if (!std::all_of(tags.begin(), tags.end(), [](const std::string& tag) {
            return safe_text(tag, kMaxTagBytes);
        })) {
        return core::Result<void>::failure(invalid("semantic area tag is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> SemanticPath::validate() const {
    if (!id || !safe_text(semantic_type, kMaxSemanticBytes) ||
        !std::isfinite(width) || width < 0.0 ||
        control_points.size() < (closed ? 3U : 2U) ||
        control_points.size() > WorldModel::kMaxControlPointsPerPath) {
        return core::Result<void>::failure(invalid(
            "semantic path identity, type, width, or control-point count is invalid"));
    }
    if (closed && (start_area.has_value() || end_area.has_value())) {
        return core::Result<void>::failure(validation(
            "closed semantic paths may not claim terminal areas"));
    }
    for (std::size_t index = 0U; index < control_points.size(); ++index) {
        if (!control_points[index].finite() ||
            (index > 0U && same_point(control_points[index - 1U], control_points[index]))) {
            return core::Result<void>::failure(validation(
                "semantic path contains non-finite or duplicate consecutive points"));
        }
    }
    if (closed && same_point(control_points.front(), control_points.back())) {
        return core::Result<void>::failure(validation(
            "closed semantic paths close implicitly and may not repeat the first point"));
    }
    return core::Result<void>::success();
}

core::Result<void> WorldRegion::validate() const {
    if (!id || revision == 0U ||
        area_ids.size() > WorldModel::kMaxRegionMemberships ||
        path_ids.size() > WorldModel::kMaxRegionMemberships ||
        neighbor_ids.size() > WorldModel::kMaxRegionMemberships ||
        cross_region_path_ids.size() > WorldModel::kMaxRegionMemberships ||
        !sorted_unique_nonzero(area_ids) || !sorted_unique_nonzero(path_ids) ||
        !sorted_unique_nonzero(neighbor_ids) ||
        !sorted_unique_nonzero(cross_region_path_ids) ||
        std::binary_search(neighbor_ids.begin(), neighbor_ids.end(), id) ||
        !std::includes(path_ids.begin(), path_ids.end(),
            cross_region_path_ids.begin(), cross_region_path_ids.end())) {
        return core::Result<void>::failure(validation(
            "world region identity, revision, members, or sorted references are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> StructureNode::validate() const {
    if (id == 0U || !safe_text(semantic_type, kMaxSemanticBytes)) {
        return core::Result<void>::failure(invalid(
            "structure node identity or type is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> StructureEdge::validate() const {
    if (id == 0U || first_node == 0U || second_node == 0U ||
        first_node == second_node || !safe_text(relationship, kMaxSemanticBytes)) {
        return core::Result<void>::failure(invalid(
            "structure edge identity, endpoints, or relationship is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> StructureGraph::validate() const {
    if (!id || !safe_text(semantic_type, kMaxSemanticBytes) ||
        nodes.size() > WorldModel::kMaxStructureItemsPerGraph ||
        edges.size() > WorldModel::kMaxStructureItemsPerGraph) {
        return core::Result<void>::failure(invalid(
            "structure graph identity, type, or bounded size is invalid"));
    }
    for (const auto& [identity, node] : nodes) {
        if (identity != node.id) {
            return core::Result<void>::failure(validation(
                "structure graph node map identity does not match its record"));
        }
        if (auto result = node.validate(); !result) return result;
    }
    for (const auto& [identity, edge] : edges) {
        if (identity != edge.id) {
            return core::Result<void>::failure(validation(
                "structure graph edge map identity does not match its record"));
        }
        if (auto result = edge.validate(); !result) return result;
        if (!nodes.contains(edge.first_node) || !nodes.contains(edge.second_node)) {
            return core::Result<void>::failure(validation(
                "structure graph edge references a missing node"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> InstanceField::validate() const {
    if (!id || !safe_text(semantic_type, kMaxSemanticBytes) || !bounds.valid() ||
        !safe_text(recipe_identity, kMaxRecipeBytes) || recipe_digest.is_zero() ||
        maximum_instances == 0U || maximum_instances > kMaxGeneratedInstances ||
        suppressed_instances.size() > WorldModel::kMaxSuppressedInstances ||
        suppressed_instances.size() > maximum_instances) {
        return core::Result<void>::failure(invalid(
            "instance field identity, bounds, recipe, or population bound is invalid"));
    }
    if (!std::is_sorted(suppressed_instances.begin(), suppressed_instances.end()) ||
        std::adjacent_find(
            suppressed_instances.begin(), suppressed_instances.end()) !=
                suppressed_instances.end() ||
        std::any_of(
            suppressed_instances.begin(), suppressed_instances.end(),
            [](std::uint64_t value) { return value == 0U; })) {
        return core::Result<void>::failure(validation(
            "suppressed instance identities must be non-zero, sorted, and unique"));
    }
    if ((geometry_evidence.has_value() && geometry_evidence->is_zero()) ||
        !safe_text(physical_material_intent, kMaxIntentBytes, true) ||
        !safe_text(construction_profile, kMaxIntentBytes, true) ||
        !safe_text(surface_domain_identity, kMaxIntentBytes, true)) {
        return core::Result<void>::failure(invalid(
            "instance field geometry evidence or materialization intent is invalid"));
    }
    return core::Result<void>::success();
}

std::string_view variant_target_kind_name(VariantTargetKind kind) noexcept {
    switch (kind) {
    case VariantTargetKind::semantic_area: return "semantic-area";
    case VariantTargetKind::semantic_path: return "semantic-path";
    case VariantTargetKind::structure_graph: return "structure-graph";
    case VariantTargetKind::instance_field: return "instance-field";
    }
    return "unknown";
}

std::string_view variant_operation_name(VariantOperation operation) noexcept {
    switch (operation) {
    case VariantOperation::set_value: return "set-value";
    case VariantOperation::remove_value: return "remove-value";
    }
    return "unknown";
}

core::Result<void> VariantEdit::validate() const {
    if (!valid_target_kind(target_kind) || target_identity == 0U ||
        !valid_variant_operation(operation) ||
        !safe_text(property, kMaxPropertyBytes) ||
        !safe_text(canonical_value, kMaxValueBytes, true) ||
        (operation == VariantOperation::remove_value && !canonical_value.empty())) {
        return core::Result<void>::failure(invalid(
            "variant edit target, operation, property, or value is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> VariantLayer::validate() const {
    if (!id || !safe_text(name, kMaxNameBytes) || base_project_revision.exhausted() ||
        edits.size() > WorldModel::kMaxVariantEdits) {
        return core::Result<void>::failure(invalid(
            "variant layer identity, name, base revision, or edit count is invalid"));
    }
    std::set<std::tuple<std::uint8_t, std::uint64_t, std::string>> targets;
    for (const auto& edit : edits) {
        if (auto result = edit.validate(); !result) return result;
        if (!targets.emplace(
                static_cast<std::uint8_t>(edit.target_kind), edit.target_identity,
                edit.property).second) {
            return core::Result<void>::failure(validation(
                "variant layer contains duplicate target-property edits"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> WorldModel::insert_area(SemanticArea area) {
    if (auto result = area.validate(); !result) return result;
    return insert_unique(
        areas_, area.id, std::move(area), kMaxPrimitivesPerKind, "semantic area");
}

core::Result<void> WorldModel::insert_path(SemanticPath path) {
    if (auto result = path.validate(); !result) return result;
    return insert_unique(
        paths_, path.id, std::move(path), kMaxPrimitivesPerKind, "semantic path");
}

core::Result<void> WorldModel::insert_region(WorldRegion region) {
    if (auto result = region.validate(); !result) return result;
    return insert_unique(
        regions_, region.id, std::move(region), kMaxPrimitivesPerKind, "world region");
}

core::Result<WorldRegionEditReceipt> WorldModel::apply_region_edits(
    std::vector<WorldRegionEdit> edits) {
    if (edits.empty() || edits.size() > kMaxRegionEditBatch) {
        return core::Result<WorldRegionEditReceipt>::failure(validation(
            "world region edit batch is empty or exceeds its bounded capacity"));
    }

    std::set<WorldRegionId> edited_ids;
    auto candidate_regions = regions_;
    WorldRegionEditReceipt receipt;
    receipt.changes.reserve(edits.size());
    for (auto& edit : edits) {
        if (!edit.id || !edited_ids.insert(edit.id).second) {
            return core::Result<WorldRegionEditReceipt>::failure(invalid(
                "world region edit identity is zero or duplicated"));
        }

        const auto current = regions_.find(edit.id);
        std::uint64_t revision_before = 0U;
        std::uint64_t revision_after = 1U;
        if (current == regions_.end()) {
            if (edit.expected_revision != 0U) {
                return core::Result<WorldRegionEditReceipt>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "world region creation expected an existing region revision"));
            }
        } else {
            revision_before = current->second.revision;
            if (edit.expected_revision == 0U ||
                edit.expected_revision != current->second.revision) {
                return core::Result<WorldRegionEditReceipt>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data, "world region edit revision is stale"));
            }
            if (current->second.revision == std::numeric_limits<std::uint64_t>::max()) {
                return core::Result<WorldRegionEditReceipt>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state, "world region revision is exhausted"));
            }
            revision_after = current->second.revision + 1U;
        }

        WorldRegion replacement{
            .id = edit.id,
            .revision = revision_after,
            .cell_x = edit.cell_x,
            .cell_y = edit.cell_y,
            .area_ids = std::move(edit.area_ids),
            .path_ids = std::move(edit.path_ids),
            .neighbor_ids = std::move(edit.neighbor_ids),
            .cross_region_path_ids = std::move(edit.cross_region_path_ids),
        };
        if (auto result = replacement.validate(); !result) {
            return core::Result<WorldRegionEditReceipt>::failure(result.error());
        }
        candidate_regions.insert_or_assign(edit.id, std::move(replacement));
        receipt.changes.push_back({edit.id, revision_before, revision_after});
    }
    if (candidate_regions.size() > kMaxPrimitivesPerKind) {
        return core::Result<WorldRegionEditReceipt>::failure(validation(
            "world region collection has reached its bounded capacity"));
    }

    WorldModel candidate = *this;
    candidate.regions_ = std::move(candidate_regions);
    if (auto result = candidate.validate(); !result) {
        return core::Result<WorldRegionEditReceipt>::failure(result.error());
    }
    std::sort(receipt.changes.begin(), receipt.changes.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    regions_ = std::move(candidate.regions_);
    return core::Result<WorldRegionEditReceipt>::success(std::move(receipt));
}

core::Result<void> WorldModel::insert_structure_graph(StructureGraph graph) {
    if (auto result = graph.validate(); !result) return result;
    return insert_unique(
        structure_graphs_, graph.id, std::move(graph), kMaxPrimitivesPerKind,
        "structure graph");
}

core::Result<void> WorldModel::insert_instance_field(InstanceField field) {
    if (auto result = field.validate(); !result) return result;
    return insert_unique(
        instance_fields_, field.id, std::move(field), kMaxPrimitivesPerKind,
        "instance field");
}

core::Result<void> WorldModel::insert_variant_layer(VariantLayer layer) {
    if (auto result = layer.validate(); !result) return result;
    return insert_unique(
        variant_layers_, layer.id, std::move(layer), kMaxPrimitivesPerKind,
        "variant layer");
}

bool WorldModel::empty() const noexcept {
    return areas_.empty() && paths_.empty() && regions_.empty() &&
        structure_graphs_.empty() &&
        instance_fields_.empty() && variant_layers_.empty();
}

core::Result<void> WorldModel::validate() const {
    if (areas_.size() > kMaxPrimitivesPerKind || paths_.size() > kMaxPrimitivesPerKind ||
        regions_.size() > kMaxPrimitivesPerKind ||
        structure_graphs_.size() > kMaxPrimitivesPerKind ||
        instance_fields_.size() > kMaxPrimitivesPerKind ||
        variant_layers_.size() > kMaxPrimitivesPerKind) {
        return core::Result<void>::failure(validation(
            "world model exceeds its bounded primitive capacity"));
    }

    for (const auto& [identity, area] : areas_) {
        if (identity != area.id) {
            return core::Result<void>::failure(validation(
                "semantic area map identity does not match its record"));
        }
        if (auto result = area.validate(); !result) return result;
        if (area.parent.has_value() && !areas_.contains(area.parent.value())) {
            return core::Result<void>::failure(validation(
                "semantic area references a missing parent"));
        }
        std::set<AreaId> visited;
        const SemanticArea* current = &area;
        while (current->parent.has_value()) {
            if (!visited.insert(current->id).second) {
                return core::Result<void>::failure(validation(
                    "semantic area hierarchy contains a cycle"));
            }
            current = &areas_.at(current->parent.value());
        }
    }

    for (const auto& [identity, path] : paths_) {
        if (identity != path.id) {
            return core::Result<void>::failure(validation(
                "semantic path map identity does not match its record"));
        }
        if (auto result = path.validate(); !result) return result;
        if ((path.start_area.has_value() && !areas_.contains(path.start_area.value())) ||
            (path.end_area.has_value() && !areas_.contains(path.end_area.value()))) {
            return core::Result<void>::failure(validation(
                "semantic path references a missing terminal area"));
        }
    }

    std::set<std::pair<std::int64_t, std::int64_t>> region_cells;
    std::map<AreaId, WorldRegionId> area_regions;
    std::map<PathId, std::set<WorldRegionId>> path_regions;
    std::map<PathId, std::set<WorldRegionId>> cross_path_regions;
    for (const auto& [identity, region] : regions_) {
        if (identity != region.id) {
            return core::Result<void>::failure(validation(
                "world region map identity does not match its record"));
        }
        if (auto result = region.validate(); !result) return result;
        if (!region_cells.emplace(region.cell_x, region.cell_y).second) {
            return core::Result<void>::failure(validation(
                "world regions have duplicate authored cell coordinates"));
        }
        for (const auto area_id : region.area_ids) {
            if (!areas_.contains(area_id) || !area_regions.emplace(area_id, identity).second) {
                return core::Result<void>::failure(validation(
                    "world region references a missing or multiply assigned semantic area"));
            }
        }
        for (const auto path_id : region.path_ids) {
            if (!paths_.contains(path_id)) {
                return core::Result<void>::failure(validation(
                    "world region references a missing semantic path"));
            }
            path_regions[path_id].insert(identity);
        }
        for (const auto neighbor_id : region.neighbor_ids) {
            if (!regions_.contains(neighbor_id)) {
                return core::Result<void>::failure(validation(
                    "world region references a missing neighboring region"));
            }
        }
        for (const auto path_id : region.cross_region_path_ids) {
            if (!paths_.contains(path_id)) {
                return core::Result<void>::failure(validation(
                    "world region cross-reference names a missing semantic path"));
            }
            cross_path_regions[path_id].insert(identity);
        }
    }

    for (const auto& [identity, region] : regions_) {
        for (const auto neighbor_id : region.neighbor_ids) {
            const auto& neighbor = regions_.at(neighbor_id);
            if (!std::binary_search(neighbor.neighbor_ids.begin(),
                    neighbor.neighbor_ids.end(), identity)) {
                return core::Result<void>::failure(validation(
                    "world region neighbor relationships must be reciprocal"));
            }
        }
    }

    for (const auto& [path_id, cross_regions] : cross_path_regions) {
        const auto& path = paths_.at(path_id);
        if (!path.start_area || !path.end_area ||
            !area_regions.contains(*path.start_area) ||
            !area_regions.contains(*path.end_area)) {
            return core::Result<void>::failure(validation(
                "cross-region path must bind two region-assigned terminal areas"));
        }
        const auto first_region = area_regions.at(*path.start_area);
        const auto second_region = area_regions.at(*path.end_area);
        if (first_region == second_region || cross_regions.size() != 2U ||
            !cross_regions.contains(first_region) || !cross_regions.contains(second_region) ||
            !path_regions[path_id].contains(first_region) ||
            !path_regions[path_id].contains(second_region) ||
            !std::binary_search(regions_.at(first_region).neighbor_ids.begin(),
                regions_.at(first_region).neighbor_ids.end(), second_region)) {
            return core::Result<void>::failure(validation(
                "cross-region path must be bound from both neighboring endpoint regions"));
        }
    }
    for (const auto& [path_id, path] : paths_) {
        if (!path.start_area || !path.end_area ||
            !area_regions.contains(*path.start_area) ||
            !area_regions.contains(*path.end_area)) {
            continue;
        }
        const auto first_region = area_regions.at(*path.start_area);
        const auto second_region = area_regions.at(*path.end_area);
        if (first_region != second_region && !cross_path_regions.contains(path_id)) {
            return core::Result<void>::failure(validation(
                "path between region-assigned areas is missing its cross-region references"));
        }
    }

    for (const auto& [identity, graph] : structure_graphs_) {
        if (identity != graph.id) {
            return core::Result<void>::failure(validation(
                "structure graph map identity does not match its record"));
        }
        if (auto result = graph.validate(); !result) return result;
        for (const auto& [node_identity, node] : graph.nodes) {
            static_cast<void>(node_identity);
            if (node.area.has_value() && !areas_.contains(node.area.value())) {
                return core::Result<void>::failure(validation(
                    "structure node references a missing semantic area"));
            }
        }
        for (const auto& [edge_identity, edge] : graph.edges) {
            static_cast<void>(edge_identity);
            if (edge.path.has_value() && !paths_.contains(edge.path.value())) {
                return core::Result<void>::failure(validation(
                    "structure edge references a missing semantic path"));
            }
        }
    }

    for (const auto& [identity, field] : instance_fields_) {
        if (identity != field.id) {
            return core::Result<void>::failure(validation(
                "instance field map identity does not match its record"));
        }
        if (auto result = field.validate(); !result) return result;
        if (field.area.has_value() && !areas_.contains(field.area.value())) {
            return core::Result<void>::failure(validation(
                "instance field references a missing semantic area"));
        }
    }

    for (const auto& [identity, layer] : variant_layers_) {
        if (identity != layer.id) {
            return core::Result<void>::failure(validation(
                "variant layer map identity does not match its record"));
        }
        if (auto result = layer.validate(); !result) return result;
        for (const auto& edit : layer.edits) {
            bool target_exists = false;
            switch (edit.target_kind) {
            case VariantTargetKind::semantic_area:
                target_exists = areas_.contains(AreaId{edit.target_identity});
                break;
            case VariantTargetKind::semantic_path:
                target_exists = paths_.contains(PathId{edit.target_identity});
                break;
            case VariantTargetKind::structure_graph:
                target_exists = structure_graphs_.contains(
                    StructureGraphId{edit.target_identity});
                break;
            case VariantTargetKind::instance_field:
                target_exists = instance_fields_.contains(
                    InstanceFieldId{edit.target_identity});
                break;
            }
            if (!target_exists) {
                return core::Result<void>::failure(validation(
                    "variant edit references a missing world primitive"));
            }
        }
    }
    return core::Result<void>::success();
}

std::string WorldModel::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);

    output << "AREAS " << areas_.size() << '\n';
    for (const auto& [identity, area] : areas_) {
        output << "AREA " << identity.value << ' '
               << (area.parent.has_value() ? area.parent->value : 0U) << ' '
               << static_cast<unsigned>(area.geometry.kind) << ' '
               << area.geometry.identity << ' ' << std::quoted(area.semantic_type) << ' '
               << area.tags.size();
        for (const auto& tag : area.tags) output << ' ' << std::quoted(tag);
        output << '\n';
    }

    output << "PATHS " << paths_.size() << '\n';
    for (const auto& [identity, path] : paths_) {
        output << "PATH " << identity.value << ' '
               << (path.start_area.has_value() ? path.start_area->value : 0U) << ' '
               << (path.end_area.has_value() ? path.end_area->value : 0U) << ' '
               << (path.closed ? 1 : 0) << ' ' << path.width << ' '
               << std::quoted(path.semantic_type) << ' ' << path.control_points.size();
        for (const auto point : path.control_points) {
            output << ' ' << point.x << ' ' << point.y << ' ' << point.z;
        }
        output << '\n';
    }

    output << "STRUCTURE_GRAPHS " << structure_graphs_.size() << '\n';
    for (const auto& [identity, graph] : structure_graphs_) {
        output << "STRUCTURE_GRAPH " << identity.value << ' '
               << std::quoted(graph.semantic_type) << ' ' << graph.nodes.size() << ' '
               << graph.edges.size() << '\n';
        for (const auto& [node_identity, node] : graph.nodes) {
            output << "STRUCTURE_NODE " << node_identity << ' '
                   << (node.area.has_value() ? node.area->value : 0U) << ' '
                   << std::quoted(node.semantic_type) << '\n';
        }
        for (const auto& [edge_identity, edge] : graph.edges) {
            output << "STRUCTURE_EDGE " << edge_identity << ' ' << edge.first_node << ' '
                   << edge.second_node << ' '
                   << (edge.path.has_value() ? edge.path->value : 0U) << ' '
                   << (edge.directed ? 1 : 0) << ' '
                   << std::quoted(edge.relationship) << '\n';
        }
    }

    output << "INSTANCE_FIELDS " << instance_fields_.size() << '\n';
    for (const auto& [identity, field] : instance_fields_) {
        output << "INSTANCE_FIELD " << identity.value << ' '
               << (field.area.has_value() ? field.area->value : 0U) << ' '
               << field.bounds.minimum.x << ' ' << field.bounds.minimum.y << ' '
               << field.bounds.minimum.z << ' ' << field.bounds.maximum.x << ' '
               << field.bounds.maximum.y << ' ' << field.bounds.maximum.z << ' '
               << field.seed << ' ' << field.maximum_instances << ' '
               << std::quoted(field.semantic_type) << ' '
               << std::quoted(field.recipe_identity) << ' ' << field.recipe_digest.hex() << ' '
               << field.suppressed_instances.size();
        for (const auto suppressed : field.suppressed_instances) {
            output << ' ' << suppressed;
        }
        output << ' '
               << (field.geometry_evidence.has_value() ? field.geometry_evidence->hex() : "-")
               << ' ' << std::quoted(field.physical_material_intent) << ' '
               << std::quoted(field.construction_profile) << ' '
               << std::quoted(field.surface_domain_identity) << '\n';
    }

    output << "REGIONS " << regions_.size() << '\n';
    for (const auto& [identity, region] : regions_) {
        output << "REGION " << identity.value << ' ' << region.revision << ' '
               << region.cell_x << ' ' << region.cell_y << ' '
               << region.area_ids.size();
        for (const auto area_id : region.area_ids) output << ' ' << area_id.value;
        output << ' ' << region.path_ids.size();
        for (const auto path_id : region.path_ids) output << ' ' << path_id.value;
        output << ' ' << region.neighbor_ids.size();
        for (const auto neighbor_id : region.neighbor_ids) {
            output << ' ' << neighbor_id.value;
        }
        output << ' ' << region.cross_region_path_ids.size();
        for (const auto path_id : region.cross_region_path_ids) {
            output << ' ' << path_id.value;
        }
        output << '\n';
    }

    output << "VARIANT_LAYERS " << variant_layers_.size() << '\n';
    for (const auto& [identity, layer] : variant_layers_) {
        output << "VARIANT_LAYER " << identity.value << ' '
               << layer.base_project_revision.value() << ' ' << std::quoted(layer.name) << ' '
               << layer.edits.size() << '\n';
        std::vector<VariantEdit> edits = layer.edits;
        std::sort(edits.begin(), edits.end(), [](const VariantEdit& left, const VariantEdit& right) {
            return variant_sort_key(left) < variant_sort_key(right);
        });
        for (const auto& edit : edits) {
            output << "VARIANT_EDIT " << static_cast<unsigned>(edit.target_kind) << ' '
                   << edit.target_identity << ' '
                   << static_cast<unsigned>(edit.operation) << ' '
                   << std::quoted(edit.property) << ' '
                   << std::quoted(edit.canonical_value) << '\n';
        }
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> WorldModel::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<WorldModel> WorldModel::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<WorldModel>::failure(parse_error(
            "world model text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !read_integer(input, version) ||
        magic != kMagic ||
        (version != 1U && version != 2U && version != kSchemaVersion)) {
        return core::Result<WorldModel>::failure(parse_error(
            "world model framing or schema version is invalid"));
    }

    WorldModel model;
    std::size_t area_count = 0U;
    if (!require_token(input, "AREAS") ||
        !read_count(input, kMaxPrimitivesPerKind, area_count)) {
        return core::Result<WorldModel>::failure(parse_error(
            "world model area header is invalid"));
    }
    for (std::size_t index = 0U; index < area_count; ++index) {
        SemanticArea area;
        std::uint64_t parent = 0U;
        unsigned binding_kind = 0U;
        std::size_t tag_count = 0U;
        if (!require_token(input, "AREA") || !read_integer(input, area.id.value) ||
            !read_integer(input, parent) || !read_integer(input, binding_kind) ||
            !read_integer(input, area.geometry.identity) ||
            !(input >> std::quoted(area.semantic_type)) ||
            !read_count(input, kMaxTagsPerArea, tag_count)) {
            return core::Result<WorldModel>::failure(parse_error(
                "world model semantic area record is malformed"));
        }
        area.parent = parent == 0U ? std::nullopt : std::optional<AreaId>{AreaId{parent}};
        area.geometry.kind = static_cast<AuthoringBindingKind>(binding_kind);
        area.tags.reserve(tag_count);
        for (std::size_t tag_index = 0U; tag_index < tag_count; ++tag_index) {
            std::string tag;
            if (!(input >> std::quoted(tag))) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model semantic area tag is malformed"));
            }
            area.tags.push_back(std::move(tag));
        }
        if (auto result = model.insert_area(std::move(area)); !result) {
            return core::Result<WorldModel>::failure(result.error());
        }
    }

    std::size_t path_count = 0U;
    if (!require_token(input, "PATHS") ||
        !read_count(input, kMaxPrimitivesPerKind, path_count)) {
        return core::Result<WorldModel>::failure(parse_error(
            "world model path header is invalid"));
    }
    for (std::size_t index = 0U; index < path_count; ++index) {
        SemanticPath path;
        std::size_t point_count = 0U;
        if (!require_token(input, "PATH") || !read_integer(input, path.id.value) ||
            !read_optional_id(input, path.start_area) ||
            !read_optional_id(input, path.end_area) || !read_bool(input, path.closed) ||
            !(input >> path.width >> std::quoted(path.semantic_type)) ||
            !read_count(input, kMaxControlPointsPerPath, point_count)) {
            return core::Result<WorldModel>::failure(parse_error(
                "world model semantic path record is malformed"));
        }
        path.control_points.reserve(point_count);
        for (std::size_t point_index = 0U; point_index < point_count; ++point_index) {
            core::Vec3d point;
            if (!(input >> point.x >> point.y >> point.z)) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model semantic path point is malformed"));
            }
            path.control_points.push_back(point);
        }
        if (auto result = model.insert_path(std::move(path)); !result) {
            return core::Result<WorldModel>::failure(result.error());
        }
    }

    std::size_t graph_count = 0U;
    if (!require_token(input, "STRUCTURE_GRAPHS") ||
        !read_count(input, kMaxPrimitivesPerKind, graph_count)) {
        return core::Result<WorldModel>::failure(parse_error(
            "world model structure-graph header is invalid"));
    }
    for (std::size_t index = 0U; index < graph_count; ++index) {
        StructureGraph graph;
        std::size_t node_count = 0U;
        std::size_t edge_count = 0U;
        if (!require_token(input, "STRUCTURE_GRAPH") ||
            !read_integer(input, graph.id.value) ||
            !(input >> std::quoted(graph.semantic_type)) ||
            !read_count(input, kMaxStructureItemsPerGraph, node_count) ||
            !read_count(input, kMaxStructureItemsPerGraph, edge_count)) {
            return core::Result<WorldModel>::failure(parse_error(
                "world model structure-graph record is malformed"));
        }
        for (std::size_t node_index = 0U; node_index < node_count; ++node_index) {
            StructureNode node;
            if (!require_token(input, "STRUCTURE_NODE") ||
                !read_integer(input, node.id) || !read_optional_id(input, node.area) ||
                !(input >> std::quoted(node.semantic_type)) ||
                !graph.nodes.emplace(node.id, node).second) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model structure-node record is malformed or duplicated"));
            }
        }
        for (std::size_t edge_index = 0U; edge_index < edge_count; ++edge_index) {
            StructureEdge edge;
            if (!require_token(input, "STRUCTURE_EDGE") ||
                !read_integer(input, edge.id) || !read_integer(input, edge.first_node) ||
                !read_integer(input, edge.second_node) ||
                !read_optional_path_id(input, edge.path) || !read_bool(input, edge.directed) ||
                !(input >> std::quoted(edge.relationship)) ||
                !graph.edges.emplace(edge.id, edge).second) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model structure-edge record is malformed or duplicated"));
            }
        }
        if (auto result = model.insert_structure_graph(std::move(graph)); !result) {
            return core::Result<WorldModel>::failure(result.error());
        }
    }

    std::size_t field_count = 0U;
    if (!require_token(input, "INSTANCE_FIELDS") ||
        !read_count(input, kMaxPrimitivesPerKind, field_count)) {
        return core::Result<WorldModel>::failure(parse_error(
            "world model instance-field header is invalid"));
    }
    for (std::size_t index = 0U; index < field_count; ++index) {
        InstanceField field;
        std::string recipe_digest;
        std::size_t suppressed_count = 0U;
        if (!require_token(input, "INSTANCE_FIELD") ||
            !read_integer(input, field.id.value) || !read_optional_id(input, field.area) ||
            !(input >> field.bounds.minimum.x >> field.bounds.minimum.y >>
              field.bounds.minimum.z >> field.bounds.maximum.x >>
              field.bounds.maximum.y >> field.bounds.maximum.z >> field.seed >>
              field.maximum_instances >> std::quoted(field.semantic_type) >>
              std::quoted(field.recipe_identity) >> recipe_digest) ||
            !read_count(input, kMaxSuppressedInstances, suppressed_count)) {
            return core::Result<WorldModel>::failure(parse_error(
                "world model instance-field record is malformed"));
        }
        const auto parsed_digest = assets::Sha256Digest::from_hex(recipe_digest);
        if (!parsed_digest) {
            return core::Result<WorldModel>::failure(parse_error(
                "world model instance-field recipe digest is malformed"));
        }
        field.recipe_digest = parsed_digest.value();
        field.suppressed_instances.reserve(suppressed_count);
        for (std::size_t suppressed_index = 0U;
             suppressed_index < suppressed_count; ++suppressed_index) {
            std::uint64_t suppressed = 0U;
            if (!read_integer(input, suppressed)) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model suppressed instance identity is malformed"));
            }
            field.suppressed_instances.push_back(suppressed);
        }
        if (version >= 2U) {
            std::string evidence;
            if (!(input >> evidence)) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model instance-field geometry evidence is malformed"));
            }
            if (evidence != "-") {
                const auto parsed_evidence = assets::Sha256Digest::from_hex(evidence);
                if (!parsed_evidence) {
                    return core::Result<WorldModel>::failure(parse_error(
                        "world model instance-field geometry evidence is malformed"));
                }
                field.geometry_evidence = parsed_evidence.value();
            }
            if (!(input >> std::quoted(field.physical_material_intent) >>
                  std::quoted(field.construction_profile) >>
                  std::quoted(field.surface_domain_identity))) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model instance-field materialization intent is malformed"));
            }
        }
        if (auto result = model.insert_instance_field(std::move(field)); !result) {
            return core::Result<WorldModel>::failure(result.error());
        }
    }

    if (version >= 3U) {
        std::size_t region_count = 0U;
        if (!require_token(input, "REGIONS") ||
            !read_count(input, kMaxPrimitivesPerKind, region_count)) {
            return core::Result<WorldModel>::failure(parse_error(
                "world model region header is invalid"));
        }
        for (std::size_t index = 0U; index < region_count; ++index) {
            WorldRegion region;
            std::size_t region_area_count = 0U;
            std::size_t region_path_count = 0U;
            std::size_t neighbor_count = 0U;
            std::size_t cross_path_count = 0U;
            if (!require_token(input, "REGION") ||
                !read_integer(input, region.id.value) ||
                !read_integer(input, region.revision) ||
                !read_signed_integer(input, region.cell_x) ||
                !read_signed_integer(input, region.cell_y) ||
                !read_count(input, kMaxRegionMemberships, region_area_count)) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model region record is malformed"));
            }
            region.area_ids.reserve(region_area_count);
            for (std::size_t member = 0U; member < region_area_count; ++member) {
                AreaId area_id;
                if (!read_integer(input, area_id.value)) {
                    return core::Result<WorldModel>::failure(parse_error(
                        "world model region area reference is malformed"));
                }
                region.area_ids.push_back(area_id);
            }
            if (!read_count(input, kMaxRegionMemberships, region_path_count)) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model region path count is malformed"));
            }
            region.path_ids.reserve(region_path_count);
            for (std::size_t member = 0U; member < region_path_count; ++member) {
                PathId path_id;
                if (!read_integer(input, path_id.value)) {
                    return core::Result<WorldModel>::failure(parse_error(
                        "world model region path reference is malformed"));
                }
                region.path_ids.push_back(path_id);
            }
            if (!read_count(input, kMaxRegionMemberships, neighbor_count)) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model region neighbor count is malformed"));
            }
            region.neighbor_ids.reserve(neighbor_count);
            for (std::size_t member = 0U; member < neighbor_count; ++member) {
                WorldRegionId neighbor_id;
                if (!read_integer(input, neighbor_id.value)) {
                    return core::Result<WorldModel>::failure(parse_error(
                        "world model region neighbor reference is malformed"));
                }
                region.neighbor_ids.push_back(neighbor_id);
            }
            if (!read_count(input, kMaxRegionMemberships, cross_path_count)) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model cross-region path count is malformed"));
            }
            region.cross_region_path_ids.reserve(cross_path_count);
            for (std::size_t member = 0U; member < cross_path_count; ++member) {
                PathId path_id;
                if (!read_integer(input, path_id.value)) {
                    return core::Result<WorldModel>::failure(parse_error(
                        "world model cross-region path reference is malformed"));
                }
                region.cross_region_path_ids.push_back(path_id);
            }
            if (auto result = model.insert_region(std::move(region)); !result) {
                return core::Result<WorldModel>::failure(result.error());
            }
        }
    }

    std::size_t layer_count = 0U;
    if (!require_token(input, "VARIANT_LAYERS") ||
        !read_count(input, kMaxPrimitivesPerKind, layer_count)) {
        return core::Result<WorldModel>::failure(parse_error(
            "world model variant-layer header is invalid"));
    }
    for (std::size_t index = 0U; index < layer_count; ++index) {
        VariantLayer layer;
        std::uint64_t revision = 0U;
        std::size_t edit_count = 0U;
        if (!require_token(input, "VARIANT_LAYER") ||
            !read_integer(input, layer.id.value) || !read_integer(input, revision) ||
            !(input >> std::quoted(layer.name)) ||
            !read_count(input, kMaxVariantEdits, edit_count)) {
            return core::Result<WorldModel>::failure(parse_error(
                "world model variant-layer record is malformed"));
        }
        layer.base_project_revision = core::Revision{revision};
        layer.edits.reserve(edit_count);
        for (std::size_t edit_index = 0U; edit_index < edit_count; ++edit_index) {
            VariantEdit edit;
            unsigned target_kind = 0U;
            unsigned operation = 0U;
            if (!require_token(input, "VARIANT_EDIT") ||
                !read_integer(input, target_kind) ||
                !read_integer(input, edit.target_identity) ||
                !read_integer(input, operation) ||
                !(input >> std::quoted(edit.property) >>
                  std::quoted(edit.canonical_value))) {
                return core::Result<WorldModel>::failure(parse_error(
                    "world model variant edit is malformed"));
            }
            edit.target_kind = static_cast<VariantTargetKind>(target_kind);
            edit.operation = static_cast<VariantOperation>(operation);
            layer.edits.push_back(std::move(edit));
        }
        if (auto result = model.insert_variant_layer(std::move(layer)); !result) {
            return core::Result<WorldModel>::failure(result.error());
        }
    }

    if (!require_token(input, "END")) {
        return core::Result<WorldModel>::failure(parse_error(
            "world model is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<WorldModel>::failure(parse_error(
            "world model contains trailing unbound data"));
    }
    if (auto result = model.validate(); !result) {
        return core::Result<WorldModel>::failure(result.error());
    }
    return core::Result<WorldModel>::success(std::move(model));
}

} // namespace carto::world
