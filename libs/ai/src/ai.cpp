#include <carto/ai/ai.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace carto::ai {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic invalid_state(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

constexpr std::size_t kMaxIntentBytes = 4096U;
constexpr std::size_t kMaxOperations = 128U;

bool safe_token(std::string_view value) {
    if (value.empty() || value.size() > 128U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
    });
}

template <typename T>
bool unique_values(const std::vector<T>& values) {
    std::set<T> unique;
    for (const auto value : values) {
        if (!unique.insert(value).second) return false;
    }
    return true;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return value;
}

std::vector<std::string> tokens(std::string_view text) {
    std::istringstream stream{std::string(text)};
    std::vector<std::string> result;
    std::string token;
    while (stream >> token) result.push_back(lower(std::move(token)));
    return result;
}

core::Result<double> parse_number(std::string token) {
    if (!token.empty() && token.back() == 'm') token.pop_back();
    if (token.empty()) return core::Result<double>::failure(invalid("numeric argument is empty"));
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end != token.c_str() + token.size() || !std::isfinite(value)) {
        return core::Result<double>::failure(invalid("numeric argument is invalid or non-finite"));
    }
    return core::Result<double>::success(value);
}

core::Result<std::uint64_t> parse_unsigned_identity(const std::string& token) {
    if (token.empty() || token.front() < '0' || token.front() > '9') {
        return core::Result<std::uint64_t>::failure(invalid(
            "identity values must be unsigned decimal integers"));
    }
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value == 0U) {
        return core::Result<std::uint64_t>::failure(invalid(
            "identity values must be non-zero unsigned integers in range"));
    }
    return core::Result<std::uint64_t>::success(value);
}

core::Result<std::vector<double>> numbers(const std::vector<std::string>& words) {
    std::vector<double> result;
    for (const auto& word : words) {
        const bool looks_numeric = !word.empty() &&
            ((word.front() >= '0' && word.front() <= '9') || word.front() == '-' ||
             word.front() == '+');
        if (!looks_numeric) continue;
        auto value = parse_number(word);
        if (!value) return core::Result<std::vector<double>>::failure(value.error());
        result.push_back(value.value());
    }
    return core::Result<std::vector<double>>::success(std::move(result));
}

const OperationDescriptor* find_operation(
    const Context& context,
    std::string_view id) {
    const auto iterator = std::find_if(context.operations.begin(), context.operations.end(),
        [id](const OperationDescriptor& operation) { return operation.id == id; });
    return iterator == context.operations.end() ? nullptr : &*iterator;
}

std::string json_escape(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() + 2U);
    for (const char raw_byte : value) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        switch (byte) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20U) {
                result += "\\u00";
                result.push_back(hex[(byte >> 4U) & 0x0FU]);
                result.push_back(hex[byte & 0x0FU]);
            } else {
                result.push_back(raw_byte);
            }
            break;
        }
    }
    return result;
}

std::string preview_name(editor::PreviewKind kind) {
    switch (kind) {
    case editor::PreviewKind::object_transform: return "object_transform";
    case editor::PreviewKind::object_translation_delta: return "object_translation_delta";
    case editor::PreviewKind::extrude_face: return "extrude_face";
    case editor::PreviewKind::inset_face: return "inset_face";
    case editor::PreviewKind::set_vertex_position: return "set_vertex_position";
    case editor::PreviewKind::poke_face: return "poke_face";
    case editor::PreviewKind::set_material_region_assignment:
        return "set_material_region_assignment";
    case editor::PreviewKind::generate_unique_lightmap_uv:
        return "generate_unique_lightmap_uv";
    }
    return "unknown";
}

std::string selection_name(editor::SelectionMode mode) {
    switch (mode) {
    case editor::SelectionMode::object: return "object";
    case editor::SelectionMode::vertex: return "vertex";
    case editor::SelectionMode::edge: return "edge";
    case editor::SelectionMode::face: return "face";
    }
    return "unknown";
}

std::string_view parameter_type_name(ParameterType type) noexcept {
    switch (type) {
    case ParameterType::length: return "length";
    case ParameterType::scalar: return "scalar";
    case ParameterType::count: return "count";
    case ParameterType::unsigned_identity: return "unsigned_identity";
    case ParameterType::boolean: return "boolean";
    case ParameterType::vector3: return "vector3";
    case ParameterType::edge_reference: return "edge_reference";
    }
    return "unknown";
}

std::string_view parameter_unit_name(ParameterUnit unit) noexcept {
    switch (unit) {
    case ParameterUnit::none: return "none";
    case ParameterUnit::world_length: return "world_length";
    case ParameterUnit::unit_interval: return "unit_interval";
    case ParameterUnit::world_position: return "world_position";
    case ParameterUnit::world_displacement: return "world_displacement";
    }
    return "unknown";
}

std::string_view effect_name(CapabilityEffect effect) noexcept {
    switch (effect) {
    case CapabilityEffect::geometry: return "geometry";
    case CapabilityEffect::topology: return "topology";
    case CapabilityEffect::transform: return "transform";
    case CapabilityEffect::material: return "material";
    case CapabilityEffect::uv: return "uv";
    }
    return "unknown";
}

std::string_view target_name(CapabilityTarget target) noexcept {
    switch (target) {
    case CapabilityTarget::selected_object: return "selected_object";
    case CapabilityTarget::selected_face: return "selected_face";
    case CapabilityTarget::selected_faces: return "selected_faces";
    case CapabilityTarget::selected_vertex: return "selected_vertex";
    case CapabilityTarget::selected_vertices: return "selected_vertices";
    case CapabilityTarget::selected_edge: return "selected_edge";
    }
    return "unknown";
}

std::string_view data_name(CapabilityData data) noexcept {
    switch (data) {
    case CapabilityData::object_transform: return "object_transform";
    case CapabilityData::object_hierarchy: return "object_hierarchy";
    case CapabilityData::selected_component: return "selected_component";
    case CapabilityData::target_mesh: return "target_mesh";
    case CapabilityData::incident_topology: return "incident_topology";
    case CapabilityData::material_catalog: return "material_catalog";
    case CapabilityData::material_regions: return "material_regions";
    case CapabilityData::lightmap_uv_inputs: return "lightmap_uv_inputs";
    }
    return "unknown";
}

std::string_view write_name(CapabilityWrite write) noexcept {
    switch (write) {
    case CapabilityWrite::object_transform: return "object_transform";
    case CapabilityWrite::vertex_positions: return "vertex_positions";
    case CapabilityWrite::mesh_topology: return "mesh_topology";
    case CapabilityWrite::material_region_assignment: return "material_region_assignment";
    case CapabilityWrite::uv_set: return "uv_set";
    }
    return "unknown";
}

std::string_view invalidation_name(CapabilityInvalidation value) noexcept {
    switch (value) {
    case CapabilityInvalidation::world_transforms: return "world_transforms";
    case CapabilityInvalidation::spatial_queries: return "spatial_queries";
    case CapabilityInvalidation::render_mesh: return "render_mesh";
    case CapabilityInvalidation::topology_cache: return "topology_cache";
    case CapabilityInvalidation::surface_normals: return "surface_normals";
    case CapabilityInvalidation::tangent_basis: return "tangent_basis";
    case CapabilityInvalidation::collision_candidates: return "collision_candidates";
    case CapabilityInvalidation::bakes: return "bakes";
    case CapabilityInvalidation::navigation: return "navigation";
    }
    return "unknown";
}

std::string_view validator_name(CapabilityValidator value) noexcept {
    switch (value) {
    case CapabilityValidator::target_exists: return "target_exists";
    case CapabilityValidator::root_object: return "root_object";
    case CapabilityValidator::unlocked_subtree: return "unlocked_subtree";
    case CapabilityValidator::finite_parameters: return "finite_parameters";
    case CapabilityValidator::mesh_valid: return "mesh_valid";
    case CapabilityValidator::face_planar: return "face_planar";
    case CapabilityValidator::face_convex: return "face_convex";
    case CapabilityValidator::manifold_topology: return "manifold_topology";
    case CapabilityValidator::operation_preconditions: return "operation_preconditions";
    case CapabilityValidator::material_source_exists: return "material_source_exists";
    case CapabilityValidator::material_variant_exists: return "material_variant_exists";
    case CapabilityValidator::material_region_exists: return "material_region_exists";
    case CapabilityValidator::lightmap_uv_generation_valid:
        return "lightmap_uv_generation_valid";
    }
    return "unknown";
}

std::string_view risk_name(CapabilityRisk risk) noexcept {
    switch (risk) {
    case CapabilityRisk::reversible_transform: return "reversible_transform";
    case CapabilityRisk::reversible_geometry: return "reversible_geometry";
    case CapabilityRisk::reversible_topology: return "reversible_topology";
    case CapabilityRisk::destructive_topology: return "destructive_topology";
    case CapabilityRisk::reversible_material: return "reversible_material";
    case CapabilityRisk::reversible_uv: return "reversible_uv";
    case CapabilityRisk::unknown: return "unknown";
    }
    return "unknown";
}

ParameterDefinition parameter(
    std::string name,
    ParameterType type,
    ParameterUnit unit,
    std::optional<double> minimum,
    std::optional<double> maximum,
    bool minimum_inclusive,
    bool required,
    std::string display_hint,
    bool maximum_inclusive = true) {
    return ParameterDefinition{
        std::move(name), type, unit, minimum, maximum, minimum_inclusive,
        maximum_inclusive, required, false, std::move(display_hint)};
}

void add_capability_contract(
    const editor::ToolDescriptor& tool,
    OperationDescriptor& descriptor) {
    if (descriptor.kernel_operation == "unclassified") return;

    const bool position_only =
        tool.kernel == editor::ToolKernelKind::editable_mesh_set_vertex_position;
    const bool topology_change = !position_only;
    const std::uint32_t geometry = effect_mask(CapabilityEffect::geometry);
    const std::uint32_t topology = effect_mask(CapabilityEffect::topology);

    descriptor.contract_complete = true;
    descriptor.provider_requestable = descriptor.preview_supported;
    descriptor.effects = geometry | (topology_change ? topology : 0U);
    descriptor.reads = {
        CapabilityData::selected_component,
        CapabilityData::target_mesh};
    descriptor.writes = {CapabilityWrite::vertex_positions};
    descriptor.invalidates = {
        CapabilityInvalidation::render_mesh,
        CapabilityInvalidation::surface_normals,
        CapabilityInvalidation::tangent_basis,
        CapabilityInvalidation::collision_candidates,
        CapabilityInvalidation::bakes,
        CapabilityInvalidation::navigation};
    descriptor.validators = {
        CapabilityValidator::target_exists,
        CapabilityValidator::finite_parameters,
        CapabilityValidator::mesh_valid,
        CapabilityValidator::operation_preconditions};
    descriptor.risk = position_only
        ? CapabilityRisk::reversible_geometry
        : CapabilityRisk::reversible_topology;
    descriptor.cost = {1U, topology_change ? 64U : 1U, topology_change ? 32U : 1U};

    using Kernel = editor::ToolKernelKind;
    switch (tool.kernel) {
    case Kernel::editable_mesh_extrude_face:
    case Kernel::editable_mesh_inset_face:
    case Kernel::editable_mesh_delete_face:
    case Kernel::editable_mesh_poke_face:
        descriptor.targets = {CapabilityTarget::selected_face};
        break;
    case Kernel::editable_mesh_set_vertex_position:
    case Kernel::editable_mesh_slide_vertex:
    case Kernel::editable_mesh_bevel_vertex:
        descriptor.targets = {CapabilityTarget::selected_vertex};
        break;
    case Kernel::editable_mesh_split_edge:
    case Kernel::editable_mesh_bevel_edge:
    case Kernel::editable_mesh_dissolve_edge:
        descriptor.targets = {CapabilityTarget::selected_edge};
        break;
    case Kernel::editable_mesh_tri_to_quad:
        descriptor.targets = {CapabilityTarget::selected_faces};
        break;
    case Kernel::editable_mesh_merge_vertices:
        descriptor.targets = {CapabilityTarget::selected_vertices};
        break;
    case Kernel::unclassified:
        descriptor.contract_complete = false;
        descriptor.effects = 0U;
        descriptor.targets.clear();
        descriptor.reads.clear();
        descriptor.writes.clear();
        descriptor.invalidates.clear();
        descriptor.validators.clear();
        descriptor.risk = CapabilityRisk::unknown;
        descriptor.cost = {};
        return;
    }

    if (topology_change) {
        descriptor.reads.push_back(CapabilityData::incident_topology);
        descriptor.writes.push_back(CapabilityWrite::mesh_topology);
        descriptor.invalidates.push_back(CapabilityInvalidation::topology_cache);
    }

    if (tool.kernel == Kernel::editable_mesh_delete_face ||
        tool.kernel == Kernel::editable_mesh_merge_vertices) {
        descriptor.risk = CapabilityRisk::destructive_topology;
    }
    if (tool.kernel == Kernel::editable_mesh_inset_face ||
        tool.kernel == Kernel::editable_mesh_poke_face ||
        tool.kernel == Kernel::editable_mesh_bevel_edge ||
        tool.kernel == Kernel::editable_mesh_bevel_vertex ||
        tool.kernel == Kernel::editable_mesh_dissolve_edge ||
        tool.kernel == Kernel::editable_mesh_tri_to_quad) {
        descriptor.validators.push_back(CapabilityValidator::face_planar);
        descriptor.validators.push_back(CapabilityValidator::face_convex);
    }
    if (tool.kernel == Kernel::editable_mesh_bevel_edge ||
        tool.kernel == Kernel::editable_mesh_bevel_vertex ||
        tool.kernel == Kernel::editable_mesh_dissolve_edge ||
        tool.kernel == Kernel::editable_mesh_tri_to_quad) {
        descriptor.validators.push_back(CapabilityValidator::manifold_topology);
    }

    if (tool.kernel == Kernel::editable_mesh_extrude_face ||
        tool.kernel == Kernel::editable_mesh_inset_face ||
        tool.kernel == Kernel::editable_mesh_bevel_edge ||
        tool.kernel == Kernel::editable_mesh_bevel_vertex) {
        const double lower = 0.0;
        descriptor.parameter_schema.push_back(parameter(
            "distance", ParameterType::length, ParameterUnit::world_length,
            lower, std::nullopt, false, true, "Distance in project world units"));
    }
    if (tool.kernel == Kernel::editable_mesh_set_vertex_position) {
        for (const char axis : {'x', 'y', 'z'}) {
            descriptor.parameter_schema.push_back(parameter(
                std::string(1U, axis), ParameterType::scalar, ParameterUnit::world_length,
                std::nullopt, std::nullopt, true, true,
                std::string("World-space ") + axis + " coordinate"));
        }
    }
    if (tool.kernel == Kernel::editable_mesh_slide_vertex ||
        tool.kernel == Kernel::editable_mesh_split_edge) {
        descriptor.parameter_schema.push_back(parameter(
            "factor", ParameterType::scalar, ParameterUnit::unit_interval,
            0.0, 1.0, false, true, "Position along the support edge", false));
    }
    if (tool.kernel == Kernel::editable_mesh_slide_vertex) {
        descriptor.parameter_schema.push_back(parameter(
            "support_edge", ParameterType::edge_reference, ParameterUnit::none,
            std::nullopt, std::nullopt, true, false,
            "Optional incident edge used as the slide support"));
    }
    if (tool.kernel == Kernel::editable_mesh_delete_face) {
        descriptor.parameter_schema.push_back(parameter(
            "remove_orphaned_vertices", ParameterType::boolean, ParameterUnit::none,
            std::nullopt, std::nullopt, true, true,
            "Remove vertices left without incident faces"));
    }
    if (tool.kernel == Kernel::editable_mesh_bevel_edge ||
        tool.kernel == Kernel::editable_mesh_bevel_vertex) {
        descriptor.parameter_schema.push_back(parameter(
            "segments", ParameterType::count, ParameterUnit::none,
            1.0, 1.0, true, true, "Supported segment count"));
    }
}

std::string make_request_id(std::uint64_t sequence) {
    return "native-ai-" + std::to_string(sequence);
}

core::Result<Proposal> make_face_proposal(
    std::string request_id,
    std::string intent,
    const Context& context,
    std::string tool_id,
    editor::PreviewKind kind,
    double distance) {
    Proposal proposal{
        std::move(request_id), context.project_revision, context.authoring, kind,
        editor::PreviewParameters{std::nullopt, distance, std::nullopt},
        std::move(tool_id), std::move(intent),
        "Native planner produced a revision-bound face-operation preview."};
    if (auto result = proposal.validate(context); !result) {
        return core::Result<Proposal>::failure(result.error());
    }
    return core::Result<Proposal>::success(std::move(proposal));
}

core::Result<Proposal> make_object_translation_proposal(
    std::string request_id,
    std::string intent,
    const Context& context,
    core::Vec3d delta) {
    Proposal proposal{
        std::move(request_id), context.project_revision, context.authoring,
        editor::PreviewKind::object_translation_delta,
        editor::PreviewParameters{std::nullopt, std::nullopt, delta},
        "object.translate", std::move(intent),
        "Native planner produced a bounded root-object translation delta."};
    if (auto result = proposal.validate(context); !result) {
        return core::Result<Proposal>::failure(result.error());
    }
    return core::Result<Proposal>::success(std::move(proposal));
}

} // namespace

core::Result<void> OperationDescriptor::validate() const {
    if (!safe_token(id) || display_name.empty() || display_name.size() > 256U ||
        !safe_token(kernel_operation)) {
        return core::Result<void>::failure(invalid("AI operation descriptor identity is invalid"));
    }
    if (parameters.size() > 32U || parameter_schema.size() > 32U ||
        precondition.empty() || precondition.size() > 512U) {
        return core::Result<void>::failure(invalid("AI operation descriptor is too large"));
    }
    for (const auto& parameter : parameters) {
        if (!safe_token(parameter)) {
            return core::Result<void>::failure(invalid("AI operation parameter name is invalid"));
        }
    }
    for (std::size_t index = 0U; index < parameter_schema.size(); ++index) {
        if (auto result = parameter_schema[index].validate(); !result) return result;
        if (index >= parameters.size() || parameters[index] != parameter_schema[index].name) {
            return core::Result<void>::failure(invalid(
                "AI operation typed parameter schema does not match its native parameters"));
        }
    }
    if (contract_complete && parameter_schema.size() != parameters.size()) {
        return core::Result<void>::failure(invalid(
            "AI operation capability contract omits a typed native parameter"));
    }
    const std::uint32_t known_effects =
        effect_mask(CapabilityEffect::geometry) | effect_mask(CapabilityEffect::topology) |
        effect_mask(CapabilityEffect::transform) | effect_mask(CapabilityEffect::material) |
        effect_mask(CapabilityEffect::uv);
    if ((effects & ~known_effects) != 0U) {
        return core::Result<void>::failure(invalid(
            "AI operation declares an unknown capability effect"));
    }
    if (preview_supported && (!contract_complete || !provider_requestable)) {
        return core::Result<void>::failure(invalid(
            "AI preview operation is missing its complete native capability contract"));
    }
    if (contract_complete && (effects == 0U || targets.empty() || reads.empty() ||
            writes.empty() || invalidates.empty() || validators.empty() ||
            risk == CapabilityRisk::unknown || cost.objects_read == 0U)) {
        return core::Result<void>::failure(invalid(
            "AI operation capability contract is incomplete"));
    }
    if (provider_requestable && (!contract_complete || !preview_supported || effects == 0U)) {
        return core::Result<void>::failure(invalid(
            "provider-requestable operation lacks a complete native preview contract"));
    }
    if (!unique_values(targets) || !unique_values(reads) || !unique_values(writes) ||
        !unique_values(invalidates) || !unique_values(validators)) {
        return core::Result<void>::failure(invalid(
            "AI operation capability contract contains duplicate metadata"));
    }
    if (!contract_complete && (provider_requestable || effects != 0U ||
            risk != CapabilityRisk::unknown)) {
        return core::Result<void>::failure(invalid(
            "unclassified AI operation claims an incomplete capability contract"));
    }
    if (preview_supported != preview_kind.has_value()) {
        return core::Result<void>::failure(invalid(
            "AI operation preview support and preview kind disagree"));
    }
    if (auto_approvable && !preview_supported) {
        return core::Result<void>::failure(invalid(
            "AI operation cannot be auto-approvable without preview support"));
    }
    return core::Result<void>::success();
}

core::Result<void> ParameterDefinition::validate() const {
    if (!safe_token(name) || display_hint.empty() || display_hint.size() > 256U) {
        return core::Result<void>::failure(invalid("AI parameter definition is invalid"));
    }
    if ((minimum.has_value() && !std::isfinite(*minimum)) ||
        (maximum.has_value() && !std::isfinite(*maximum))) {
        return core::Result<void>::failure(invalid("AI parameter bounds must be finite"));
    }
    if (minimum.has_value() && maximum.has_value() &&
        (*minimum > *maximum || (*minimum == *maximum &&
            (!minimum_inclusive || !maximum_inclusive)))) {
        return core::Result<void>::failure(invalid("AI parameter bounds are contradictory"));
    }
    switch (type) {
    case ParameterType::length:
        if (unit != ParameterUnit::world_length) {
            return core::Result<void>::failure(invalid(
                "length parameter must declare world-length units"));
        }
        break;
    case ParameterType::scalar:
        if (unit != ParameterUnit::none && unit != ParameterUnit::world_length &&
            unit != ParameterUnit::unit_interval) {
            return core::Result<void>::failure(invalid("scalar parameter unit is invalid"));
        }
        if (unit == ParameterUnit::unit_interval &&
            (!minimum.has_value() || !maximum.has_value() || *minimum < 0.0 || *maximum > 1.0)) {
            return core::Result<void>::failure(invalid(
                "unit-interval parameter must be bounded within zero and one"));
        }
        break;
    case ParameterType::count:
        if ((minimum.has_value() && std::floor(*minimum) != *minimum) ||
            (maximum.has_value() && std::floor(*maximum) != *maximum)) {
            return core::Result<void>::failure(invalid(
                "count parameter bounds must be whole numbers"));
        }
        break;
    case ParameterType::unsigned_identity:
        if (unit != ParameterUnit::none || minimum.has_value() || maximum.has_value()) {
            return core::Result<void>::failure(invalid(
                "unsigned identity parameter cannot declare numeric units or bounds"));
        }
        break;
    case ParameterType::boolean:
    case ParameterType::edge_reference:
        if (unit != ParameterUnit::none || minimum.has_value() || maximum.has_value()) {
            return core::Result<void>::failure(invalid(
                "non-numeric parameter cannot declare numeric units or bounds"));
        }
        break;
    case ParameterType::vector3:
        if (unit != ParameterUnit::world_position &&
            unit != ParameterUnit::world_displacement) {
            return core::Result<void>::failure(invalid(
                "vector parameter must declare world-position or displacement units"));
        }
        break;
    }
    return core::Result<void>::success();
}

core::Result<void> Context::validate() const {
    if (auto result = authoring.validate(); !result) return result;
    if (project_revision != authoring.project_revision) {
        return core::Result<void>::failure(stale(
            "AI context project revision does not match its authoring context"));
    }
    if (operations.empty() || operations.size() > kMaxOperations) {
        return core::Result<void>::failure(invalid("AI context operation ontology is invalid"));
    }
    std::set<std::string> identities;
    for (const auto& operation : operations) {
        if (auto result = operation.validate(); !result) return result;
        if (!identities.insert(operation.id).second) {
            return core::Result<void>::failure(invalid(
                "AI context operation ontology contains a duplicate id"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> Proposal::validate(const Context& expected) const {
    if (auto result = expected.validate(); !result) return result;
    if (!safe_token(request_id) || intent.empty() || intent.size() > kMaxIntentBytes ||
        rationale.empty() || rationale.size() > 1024U) {
        return core::Result<void>::failure(invalid("AI proposal metadata is invalid"));
    }
    if (base_revision != expected.project_revision || expected.authoring != this->context) {
        return core::Result<void>::failure(stale(
            "AI proposal was created against a different authoring context"));
    }
    const auto* operation = find_operation(expected, tool_id);
    if (operation == nullptr) {
        return core::Result<void>::failure(invalid_state(
            "AI proposal references an operation outside the native ontology"));
    }
    if (!operation->preview_supported || !operation->contract_complete ||
        !operation->provider_requestable || operation->preview_kind != preview_kind) {
        return core::Result<void>::failure(invalid_state(
            "AI proposal references an operation without a matching native preview"));
    }
    if (preview_kind == editor::PreviewKind::object_translation_delta) {
        if (expected.authoring.selection_mode != editor::SelectionMode::object ||
            expected.authoring.objects.size() != 1U) {
            return core::Result<void>::failure(invalid_state(
                "object translation requires exactly one selected object"));
        }
    } else if (preview_kind == editor::PreviewKind::extrude_face ||
        preview_kind == editor::PreviewKind::inset_face ||
        preview_kind == editor::PreviewKind::poke_face) {
        if (expected.authoring.selection_mode != editor::SelectionMode::face ||
            expected.authoring.faces.size() != 1U ||
            !expected.authoring.component_object.has_value()) {
            return core::Result<void>::failure(invalid_state(
                "face proposal requires exactly one object-bound selected face"));
        }
    } else if (preview_kind == editor::PreviewKind::set_vertex_position) {
        if (expected.authoring.selection_mode != editor::SelectionMode::vertex ||
            expected.authoring.vertices.size() != 1U ||
            !expected.authoring.component_object.has_value()) {
            return core::Result<void>::failure(invalid_state(
                "vertex proposal requires exactly one object-bound selected vertex"));
        }
    } else if (preview_kind == editor::PreviewKind::set_material_region_assignment) {
        if (expected.authoring.selection_mode != editor::SelectionMode::object ||
            expected.authoring.objects.size() != 1U) {
            return core::Result<void>::failure(invalid_state(
                "material assignment requires exactly one selected mesh object"));
        }
    } else if (preview_kind == editor::PreviewKind::generate_unique_lightmap_uv) {
        if (expected.authoring.selection_mode != editor::SelectionMode::object ||
            expected.authoring.objects.size() != 1U) {
            return core::Result<void>::failure(invalid_state(
                "lightmap UV generation requires exactly one selected mesh object"));
        }
    } else {
        return core::Result<void>::failure(invalid_state(
            "native AI does not admit this preview kind"));
    }
    return parameters.validate(preview_kind);
}

core::Result<void> Proposal::validate_auto_approval(const Context& expected) const {
    if (auto result = validate(expected); !result) return result;
    const auto* operation = find_operation(expected, tool_id);
    if (operation == nullptr || !operation->auto_approvable) {
        return core::Result<void>::failure(invalid_state(
            "AI proposal requires explicit review before it can be auto-applied"));
    }
    return core::Result<void>::success();
}

std::vector<OperationDescriptor> build_operation_ontology(
    std::span<const editor::ToolDescriptor> tools) {
    std::vector<OperationDescriptor> result;
    result.reserve(tools.size());
    for (const auto& tool : tools) {
        OperationDescriptor descriptor;
        descriptor.id = tool.id;
        descriptor.display_name = tool.display_name;
        const auto matches = [&tool](std::string_view id, editor::ToolKernelKind kernel) {
            return tool.id == id && tool.kernel == kernel;
        };
        if (matches("mesh.extrude-face", editor::ToolKernelKind::editable_mesh_extrude_face)) {
            descriptor.kernel_operation = "geometry.EditableMesh.extrude_face";
            descriptor.parameters = {"distance"};
            descriptor.precondition =
                "exactly one object-bound face selected; finite positive distance; face has at least three vertices and a non-zero first-triangle normal";
            descriptor.preview_supported = true;
            descriptor.auto_approvable = true;
            descriptor.preview_kind = editor::PreviewKind::extrude_face;
        } else if (matches("mesh.inset-face", editor::ToolKernelKind::editable_mesh_inset_face)) {
            descriptor.kernel_operation = "geometry.EditableMesh.inset_face";
            descriptor.parameters = {"distance"};
            descriptor.precondition =
                "exactly one object-bound face selected; finite positive distance; face is planar and strictly convex; distance does not collapse or invert the inner face";
            descriptor.preview_supported = true;
            descriptor.auto_approvable = true;
            descriptor.preview_kind = editor::PreviewKind::inset_face;
        } else if (matches("mesh.poke-face", editor::ToolKernelKind::editable_mesh_poke_face)) {
            descriptor.kernel_operation = "geometry.EditableMesh.poke_face";
            descriptor.precondition =
                "exactly one object-bound face selected; face is finite, planar, strictly convex, and has a centroid strictly inside";
            descriptor.preview_supported = true;
            descriptor.preview_kind = editor::PreviewKind::poke_face;
        } else if (matches(
                       "mesh.set-vertex-position",
                       editor::ToolKernelKind::editable_mesh_set_vertex_position)) {
            descriptor.kernel_operation = "geometry.EditableMesh.set_vertex_position";
            descriptor.parameters = {"x", "y", "z"};
            descriptor.precondition =
                "exactly one object-bound vertex selected; target position is finite; resulting mesh remains valid";
            descriptor.preview_supported = true;
            descriptor.auto_approvable = true;
            descriptor.preview_kind = editor::PreviewKind::set_vertex_position;
        } else if (matches(
                       "mesh.slide-vertex",
                       editor::ToolKernelKind::editable_mesh_slide_vertex)) {
            descriptor.kernel_operation = "geometry.EditableMesh.slide_vertex";
            descriptor.parameters = {"factor", "support_edge"};
            descriptor.precondition =
                "exactly one object-bound vertex selected; support edge must be incident or omitted for deterministic lowest-edge selection; finite factor strictly between zero and one; resulting mesh remains valid; operation requires a future preview contract";
        } else if (matches("mesh.remove-face", editor::ToolKernelKind::editable_mesh_delete_face)) {
            descriptor.kernel_operation = "geometry.EditableMesh.delete_face";
            descriptor.parameters = {"remove_orphaned_vertices"};
            descriptor.precondition =
                "exactly one object-bound face selected; remove_orphaned_vertices controls orphan cleanup; destructive operation requires a future preview contract";
        } else if (matches("mesh.split-edge", editor::ToolKernelKind::editable_mesh_split_edge)) {
            descriptor.kernel_operation = "geometry.EditableMesh.split_edge";
            descriptor.parameters = {"factor"};
            descriptor.precondition =
                "exactly one object-bound edge selected; finite factor strictly between zero and one; operation requires a future preview contract";
        } else if (matches(
                       "mesh.bevel-edge",
                       editor::ToolKernelKind::editable_mesh_bevel_edge)) {
            descriptor.kernel_operation = "geometry.EditableMesh.bevel_edge";
            descriptor.parameters = {"distance", "segments"};
            descriptor.precondition =
                "exactly one object-bound internal manifold edge selected; finite positive width; strictly convex planar incident faces; segments must equal one; operation requires a future preview contract";
        } else if (matches(
                       "mesh.bevel-vertex",
                       editor::ToolKernelKind::editable_mesh_bevel_vertex)) {
            descriptor.kernel_operation = "geometry.EditableMesh.bevel_vertex";
            descriptor.parameters = {"distance", "segments"};
            descriptor.precondition =
                "exactly one object-bound valence-three vertex selected; all three incident faces must be triangles and all three edges internal manifold; finite positive width below half the shortest incident edge; segments must equal one; operation requires a future preview contract";
        } else if (matches(
                       "mesh.dissolve-edge",
                       editor::ToolKernelKind::editable_mesh_dissolve_edge)) {
            descriptor.kernel_operation = "geometry.EditableMesh.dissolve_edge";
            descriptor.precondition =
                "exactly one object-bound internal manifold edge selected; its incident faces must be coplanar, convex, and form a simple boundary; boundary, non-planar, and multi-edge dissolves are rejected; operation requires a future preview contract";
        } else if (matches(
                       "mesh.tri-to-quad",
                       editor::ToolKernelKind::editable_mesh_tri_to_quad)) {
            descriptor.kernel_operation = "geometry.EditableMesh.tri_to_quad";
            descriptor.precondition =
                "exactly two object-bound authored triangles selected; they must share exactly one internal edge and form a coplanar convex quad; incompatible topology is rejected; operation requires a future preview contract";
        } else if (matches(
                       "mesh.merge-vertices",
                       editor::ToolKernelKind::editable_mesh_merge_vertices)) {
            descriptor.kernel_operation = "geometry.EditableMesh.merge_vertices";
            descriptor.precondition =
                "exactly two object-bound vertices selected; the lower stable ID is preserved as the target; the resulting mesh must remain simple and valid; operation requires a future preview contract";
        } else {
            descriptor.kernel_operation = "unclassified";
            const bool known_id = tool.id == "mesh.extrude-face" ||
                tool.id == "mesh.inset-face" || tool.id == "mesh.poke-face" ||
                tool.id == "mesh.set-vertex-position" ||
                tool.id == "mesh.slide-vertex" ||
                tool.id == "mesh.remove-face" || tool.id == "mesh.split-edge" ||
                tool.id == "mesh.bevel-edge" ||
                tool.id == "mesh.bevel-vertex" ||
                tool.id == "mesh.dissolve-edge" ||
                tool.id == "mesh.tri-to-quad" ||
                tool.id == "mesh.merge-vertices";
            descriptor.precondition = known_id
                ? "registered tool ID and native kernel contract do not match; preview disabled"
                : "native tool-specific validation required";
        }
        add_capability_contract(tool, descriptor);
        result.push_back(std::move(descriptor));
    }
    OperationDescriptor translation;
    translation.id = "object.translate";
    translation.display_name = "Translate Root Object";
    translation.kernel_operation = "scene.SceneObject.translate_root";
    translation.parameters = {"delta"};
    translation.parameter_schema = {parameter(
        "delta", ParameterType::vector3, ParameterUnit::world_displacement,
        std::nullopt, std::nullopt, true, true,
        "Root-object translation delta in meters")};
    translation.precondition =
        "exactly one root object selected; finite displacement within the work envelope; "
        "the complete descendant subtree is scoped, unprotected, and unlocked";
    translation.preview_supported = true;
    translation.auto_approvable = false;
    translation.preview_kind = editor::PreviewKind::object_translation_delta;
    translation.contract_complete = true;
    translation.provider_requestable = true;
    translation.effects = effect_mask(CapabilityEffect::transform);
    translation.targets = {CapabilityTarget::selected_object};
    translation.reads = {
        CapabilityData::object_transform,
        CapabilityData::object_hierarchy};
    translation.writes = {CapabilityWrite::object_transform};
    translation.invalidates = {
        CapabilityInvalidation::world_transforms,
        CapabilityInvalidation::spatial_queries,
        CapabilityInvalidation::collision_candidates,
        CapabilityInvalidation::bakes,
        CapabilityInvalidation::navigation};
    translation.validators = {
        CapabilityValidator::target_exists,
        CapabilityValidator::root_object,
        CapabilityValidator::unlocked_subtree,
        CapabilityValidator::finite_parameters,
        CapabilityValidator::operation_preconditions};
    translation.risk = CapabilityRisk::reversible_transform;
    translation.cost.objects_read = 1U;
    result.push_back(std::move(translation));

    OperationDescriptor material_assignment;
    material_assignment.id = "material.assign-region";
    material_assignment.display_name = "Assign Material to Region";
    material_assignment.kernel_operation = "project.MaterialCatalog.assign_region";
    material_assignment.parameters = {"material_id", "region_id", "variant_id"};
    material_assignment.parameter_schema = {
        parameter("material_id", ParameterType::unsigned_identity, ParameterUnit::none,
            std::nullopt, std::nullopt, true, true, "Existing material ID"),
        parameter("region_id", ParameterType::unsigned_identity, ParameterUnit::none,
            std::nullopt, std::nullopt, true, true, "Authored face-region ID"),
        parameter("variant_id", ParameterType::unsigned_identity, ParameterUnit::none,
            std::nullopt, std::nullopt, true, false,
            "Optional existing material variant ID"),
    };
    material_assignment.precondition =
        "exactly one unlocked mesh object is selected; the region is declared by a face-domain "
        "uint32 material-region layer; the material and optional variant exist; every instance "
        "of the shared mesh is in-scope, unprotected, and unlocked; source revision is current";
    material_assignment.preview_supported = true;
    material_assignment.auto_approvable = false;
    material_assignment.preview_kind = editor::PreviewKind::set_material_region_assignment;
    material_assignment.contract_complete = true;
    material_assignment.provider_requestable = true;
    material_assignment.effects = effect_mask(CapabilityEffect::material);
    material_assignment.targets = {CapabilityTarget::selected_object};
    material_assignment.reads = {
        CapabilityData::target_mesh,
        CapabilityData::material_catalog,
        CapabilityData::material_regions};
    material_assignment.writes = {CapabilityWrite::material_region_assignment};
    material_assignment.invalidates = {
        CapabilityInvalidation::render_mesh,
        CapabilityInvalidation::tangent_basis,
        CapabilityInvalidation::bakes};
    material_assignment.validators = {
        CapabilityValidator::target_exists,
        CapabilityValidator::unlocked_subtree,
        CapabilityValidator::material_source_exists,
        CapabilityValidator::material_variant_exists,
        CapabilityValidator::material_region_exists,
        CapabilityValidator::operation_preconditions};
    material_assignment.risk = CapabilityRisk::reversible_material;
    material_assignment.cost.objects_read = 1U;
    result.push_back(std::move(material_assignment));

    OperationDescriptor lightmap_uv_generation;
    lightmap_uv_generation.id = "uv.generate-lightmap";
    lightmap_uv_generation.display_name = "Generate Unique Lightmap UVs";
    lightmap_uv_generation.kernel_operation = "production.UniqueLightmapUv.generate";
    lightmap_uv_generation.parameters = {
        "uv_set_id", "resolution", "padding_texels", "maximum_udim_tiles"};
    lightmap_uv_generation.parameter_schema = {
        parameter("uv_set_id", ParameterType::unsigned_identity, ParameterUnit::none,
            std::nullopt, std::nullopt, true, true, "New UV set identity"),
        parameter("resolution", ParameterType::count, ParameterUnit::none,
            16.0, 8192.0, true, true, "Lightmap tile resolution in pixels"),
        parameter("padding_texels", ParameterType::count, ParameterUnit::none,
            1.0, 1024.0, true, true, "Required island padding in texels"),
        parameter("maximum_udim_tiles", ParameterType::count, ParameterUnit::none,
            1.0, 100.0, true, true, "Maximum generated UDIM tile count"),
    };
    lightmap_uv_generation.precondition =
        "exactly one unlocked mesh object is selected; the new UV set and derived corner layer "
        "do not already exist; the mesh has bounded planar faces; requested padding and UDIM "
        "budget fit; every instance of the shared mesh is in-scope, unprotected, and unlocked; "
        "source revision is current";
    lightmap_uv_generation.preview_supported = true;
    lightmap_uv_generation.auto_approvable = false;
    lightmap_uv_generation.preview_kind = editor::PreviewKind::generate_unique_lightmap_uv;
    lightmap_uv_generation.contract_complete = true;
    lightmap_uv_generation.provider_requestable = true;
    lightmap_uv_generation.effects = effect_mask(CapabilityEffect::uv);
    lightmap_uv_generation.targets = {CapabilityTarget::selected_object};
    lightmap_uv_generation.reads = {
        CapabilityData::target_mesh,
        CapabilityData::incident_topology,
        CapabilityData::lightmap_uv_inputs};
    lightmap_uv_generation.writes = {CapabilityWrite::uv_set};
    lightmap_uv_generation.invalidates = {
        CapabilityInvalidation::render_mesh,
        CapabilityInvalidation::tangent_basis,
        CapabilityInvalidation::bakes};
    lightmap_uv_generation.validators = {
        CapabilityValidator::target_exists,
        CapabilityValidator::unlocked_subtree,
        CapabilityValidator::mesh_valid,
        CapabilityValidator::lightmap_uv_generation_valid,
        CapabilityValidator::operation_preconditions};
    lightmap_uv_generation.risk = CapabilityRisk::reversible_uv;
    lightmap_uv_generation.cost.objects_read = 1U;
    result.push_back(std::move(lightmap_uv_generation));
    return result;
}

std::vector<OperationDescriptor> project_capabilities(
    std::span<const OperationDescriptor> capabilities,
    std::uint32_t permitted_effects) {
    const std::uint32_t known_effects =
        effect_mask(CapabilityEffect::geometry) | effect_mask(CapabilityEffect::topology) |
        effect_mask(CapabilityEffect::transform) | effect_mask(CapabilityEffect::material) |
        effect_mask(CapabilityEffect::uv);
    if (permitted_effects == 0U || (permitted_effects & ~known_effects) != 0U) return {};

    std::vector<OperationDescriptor> result;
    result.reserve(capabilities.size());
    for (const auto& capability : capabilities) {
        if (!capability.provider_requestable || !capability.contract_complete ||
            capability.effects == 0U ||
            (capability.effects & ~permitted_effects) != 0U || !capability.validate()) {
            continue;
        }
        result.push_back(capability);
    }
    return result;
}

std::string context_json(const Context& context) {
    std::string result = "{\"project_revision\":" +
        std::to_string(context.project_revision.value()) +
        ",\"selection_mode\":\"" + selection_name(context.authoring.selection_mode) +
        "\",\"object_count\":" + std::to_string(context.authoring.objects.size()) +
        ",\"vertex_count\":" + std::to_string(context.authoring.vertices.size()) +
        ",\"edge_count\":" + std::to_string(context.authoring.edges.size()) +
        ",\"face_count\":" + std::to_string(context.authoring.faces.size()) +
        ",\"operations\":[";
    for (std::size_t index = 0U; index < context.operations.size(); ++index) {
        if (index != 0U) result.push_back(',');
        const auto& operation = context.operations[index];
        result += "{\"id\":\"" + json_escape(operation.id) +
            "\",\"display_name\":\"" + json_escape(operation.display_name) +
            "\",\"kernel_operation\":\"" + json_escape(operation.kernel_operation) +
            "\",\"parameters\":[";
        for (std::size_t parameter_index = 0U;
             parameter_index < operation.parameters.size(); ++parameter_index) {
            if (parameter_index != 0U) result.push_back(',');
            result += "\"" + json_escape(operation.parameters[parameter_index]) + "\"";
        }
        result += "],\"precondition\":\"" + json_escape(operation.precondition) +
            "\",\"preview_supported\":" +
            std::string(operation.preview_supported ? "true" : "false") +
            ",\"auto_approvable\":" +
            std::string(operation.auto_approvable ? "true" : "false") +
            ",\"provider_requestable\":" +
            std::string(operation.provider_requestable ? "true" : "false") +
            ",\"contract_complete\":" +
            std::string(operation.contract_complete ? "true" : "false") +
            ",\"preview_kind\":";
        if (operation.preview_kind.has_value()) {
            result += "\"" + preview_name(*operation.preview_kind) + "\"";
        } else {
            result += "null";
        }
        const auto append_values = [&result](const auto& values, auto to_name) {
            result.push_back('[');
            for (std::size_t value_index = 0U; value_index < values.size(); ++value_index) {
                if (value_index != 0U) result.push_back(',');
                result += "\"" + std::string(to_name(values[value_index])) + "\"";
            }
            result.push_back(']');
        };
        result += ",\"effects\":[";
        bool has_effect = false;
        for (const auto effect : {CapabilityEffect::geometry, CapabilityEffect::topology,
                CapabilityEffect::transform, CapabilityEffect::material, CapabilityEffect::uv}) {
            if ((operation.effects & effect_mask(effect)) == 0U) continue;
            if (has_effect) result.push_back(',');
            result += "\"" + std::string(effect_name(effect)) + "\"";
            has_effect = true;
        }
        result += "],\"targets\":";
        append_values(operation.targets, target_name);
        result += ",\"reads\":";
        append_values(operation.reads, data_name);
        result += ",\"writes\":";
        append_values(operation.writes, write_name);
        result += ",\"invalidates\":";
        append_values(operation.invalidates, invalidation_name);
        result += ",\"validators\":";
        append_values(operation.validators, validator_name);
        result += ",\"risk\":\"" + std::string(risk_name(operation.risk)) +
            "\",\"cost\":{\"objects_read\":" +
            std::to_string(operation.cost.objects_read) +
            ",\"topology_elements_read\":" +
            std::to_string(operation.cost.topology_elements_read) +
            ",\"topology_elements_written\":" +
            std::to_string(operation.cost.topology_elements_written) + "}";
        result += ",\"parameter_schema\":[";
        for (std::size_t parameter_index = 0U;
             parameter_index < operation.parameter_schema.size(); ++parameter_index) {
            if (parameter_index != 0U) result.push_back(',');
            const auto& definition = operation.parameter_schema[parameter_index];
            result += "{\"name\":\"" + json_escape(definition.name) +
                "\",\"type\":\"" + std::string(parameter_type_name(definition.type)) +
                "\",\"unit\":\"" + std::string(parameter_unit_name(definition.unit)) +
                "\",\"required\":" + std::string(definition.required ? "true" : "false") +
                ",\"inference_allowed\":" +
                std::string(definition.inference_allowed ? "true" : "false") +
                ",\"display_hint\":\"" + json_escape(definition.display_hint) + "\"";
            if (definition.minimum.has_value()) {
                result += ",\"minimum\":" + std::to_string(*definition.minimum) +
                    ",\"minimum_inclusive\":" +
                    std::string(definition.minimum_inclusive ? "true" : "false");
            }
            if (definition.maximum.has_value()) {
                result += ",\"maximum\":" + std::to_string(*definition.maximum) +
                    ",\"maximum_inclusive\":" +
                    std::string(definition.maximum_inclusive ? "true" : "false");
            }
            result.push_back('}');
        }
        result.push_back(']');
        result.push_back('}');
    }
    result += "]}";
    return result;
}

std::string proposal_json(const Proposal& proposal) {
    std::string result = "{\"request_id\":\"" + json_escape(proposal.request_id) +
        "\",\"base_revision\":" + std::to_string(proposal.base_revision.value()) +
        ",\"tool_id\":\"" + json_escape(proposal.tool_id) +
        "\",\"preview_kind\":\"" + preview_name(proposal.preview_kind) +
        "\",\"intent\":\"" + json_escape(proposal.intent) +
        "\",\"rationale\":\"" + json_escape(proposal.rationale) + "\"";
    if (proposal.parameters.distance.has_value()) {
        result += ",\"distance\":" + std::to_string(*proposal.parameters.distance);
    }
    if (proposal.parameters.position.has_value()) {
        const auto& position = *proposal.parameters.position;
        result += ",\"position\":{\"x\":" + std::to_string(position.x) +
            ",\"y\":" + std::to_string(position.y) +
            ",\"z\":" + std::to_string(position.z) + "}";
    }
    if (proposal.parameters.material_id.has_value()) {
        result += ",\"material_id\":" + std::to_string(*proposal.parameters.material_id);
    }
    if (proposal.parameters.material_region_id.has_value()) {
        result += ",\"region_id\":" +
            std::to_string(*proposal.parameters.material_region_id);
    }
    if (proposal.parameters.material_variant_id.has_value()) {
        result += ",\"variant_id\":" +
            std::to_string(*proposal.parameters.material_variant_id);
    }
    if (proposal.parameters.lightmap_uv_set_id.has_value()) {
        result += ",\"uv_set_id\":" +
            std::to_string(*proposal.parameters.lightmap_uv_set_id);
    }
    if (proposal.parameters.lightmap_resolution.has_value()) {
        result += ",\"resolution\":" +
            std::to_string(*proposal.parameters.lightmap_resolution);
    }
    if (proposal.parameters.lightmap_padding_texels.has_value()) {
        result += ",\"padding_texels\":" +
            std::to_string(*proposal.parameters.lightmap_padding_texels);
    }
    if (proposal.parameters.lightmap_maximum_udim_tiles.has_value()) {
        result += ",\"maximum_udim_tiles\":" +
            std::to_string(*proposal.parameters.lightmap_maximum_udim_tiles);
    }
    result += "}";
    return result;
}

core::Result<Proposal> NativePlanner::propose(
    std::string_view intent,
    const Context& context) {
    if (intent.empty() || intent.size() > kMaxIntentBytes) {
        return core::Result<Proposal>::failure(invalid(
            "native AI intent must be non-empty and within the size limit"));
    }
    if (auto result = context.validate(); !result) {
        return core::Result<Proposal>::failure(result.error());
    }
    const auto words = tokens(intent);
    if (words.empty()) {
        return core::Result<Proposal>::failure(invalid("native AI intent contains no words"));
    }
    const auto parsed_numbers = numbers(words);
    if (!parsed_numbers) return core::Result<Proposal>::failure(parsed_numbers.error());
    const std::string request_id = make_request_id(next_request_id_++);

    if (words.front() == "generate") {
        const bool exact_syntax = words.size() == 11U && words[1] == "lightmap" &&
            words[2] == "uv" && words[3] == "set" && words[5] == "resolution" &&
            words[7] == "padding" && words[9] == "tiles";
        if (!exact_syntax) {
            return core::Result<Proposal>::failure(invalid(
                "lightmap UV intent must be `generate lightmap uv set <id> resolution "
                "<pixels> padding <texels> tiles <count>`"));
        }
        const auto uv_set = parse_unsigned_identity(words[4]);
        const auto resolution = parse_unsigned_identity(words[6]);
        const auto padding = parse_unsigned_identity(words[8]);
        const auto tiles = parse_unsigned_identity(words[10]);
        if (!uv_set || !resolution || !padding || !tiles ||
            resolution.value() > 8192U || padding.value() > 8192U ||
            tiles.value() > 100U) {
            const core::Diagnostic error = !uv_set ? uv_set.error() :
                !resolution ? resolution.error() : !padding ? padding.error() :
                !tiles ? tiles.error() : invalid("lightmap UV bounds exceed supported ranges");
            return core::Result<Proposal>::failure(error);
        }
        editor::PreviewParameters parameters;
        parameters.lightmap_uv_set_id = uv_set.value();
        parameters.lightmap_resolution = static_cast<std::uint32_t>(resolution.value());
        parameters.lightmap_padding_texels = static_cast<std::uint32_t>(padding.value());
        parameters.lightmap_maximum_udim_tiles = static_cast<std::uint32_t>(tiles.value());
        Proposal proposal{
            request_id, context.project_revision, context.authoring,
            editor::PreviewKind::generate_unique_lightmap_uv, std::move(parameters),
            "uv.generate-lightmap", std::string(intent),
            "Native planner produced a revision-bound lightmap UV generation preview."};
        if (auto result = proposal.validate(context); !result) {
            return core::Result<Proposal>::failure(result.error());
        }
        return core::Result<Proposal>::success(std::move(proposal));
    }

    if (words.front() == "assign") {
        const bool with_variant = words.size() == 8U && words[6] == "variant";
        const bool exact_syntax = (words.size() == 6U || with_variant) &&
            words[1] == "material" && words[3] == "to" && words[4] == "region";
        if (!exact_syntax) {
            return core::Result<Proposal>::failure(invalid(
                "material intent must be `assign material <id> to region <id>` with an "
                "optional `variant <id>` suffix"));
        }
        const auto material = parse_unsigned_identity(words[2]);
        const auto region = parse_unsigned_identity(words[5]);
        if (!material || !region || region.value() >
                std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<Proposal>::failure(!material ? material.error() :
                !region ? region.error() : invalid("material region ID exceeds uint32 range"));
        }
        std::optional<std::uint64_t> variant;
        if (with_variant) {
            const auto parsed_variant = parse_unsigned_identity(words[7]);
            if (!parsed_variant) {
                return core::Result<Proposal>::failure(parsed_variant.error());
            }
            variant = parsed_variant.value();
        }
        editor::PreviewParameters parameters;
        parameters.material_id = material.value();
        parameters.material_region_id = static_cast<std::uint32_t>(region.value());
        parameters.material_variant_id = variant;
        Proposal proposal{
            request_id, context.project_revision, context.authoring,
            editor::PreviewKind::set_material_region_assignment, std::move(parameters),
            "material.assign-region", std::string(intent),
            "Native planner produced a revision-bound material-region assignment preview."};
        if (auto result = proposal.validate(context); !result) {
            return core::Result<Proposal>::failure(result.error());
        }
        return core::Result<Proposal>::success(std::move(proposal));
    }

    if ((words.size() == 2U && words[0] == "poke" && words[1] == "face") ||
        (words.size() == 1U && words[0] == "poke-face")) {
        if (!parsed_numbers.value().empty()) {
            return core::Result<Proposal>::failure(invalid(
                "poke face intent does not accept numeric parameters"));
        }
        Proposal proposal{
            request_id, context.project_revision, context.authoring,
            editor::PreviewKind::poke_face, editor::PreviewParameters{},
            "mesh.poke-face", std::string(intent),
            "Native planner produced a parameterless, revision-bound face-poke preview."};
        if (auto result = proposal.validate(context); !result) {
            return core::Result<Proposal>::failure(result.error());
        }
        return core::Result<Proposal>::success(std::move(proposal));
    }

    if (words.size() == 6U && words[0] == "translate" && words[1] == "object" &&
        words[2] == "by") {
        if (parsed_numbers.value().size() != 3U) {
            return core::Result<Proposal>::failure(invalid(
                "translate object by requires exactly three displacement values"));
        }
        return make_object_translation_proposal(
            request_id, std::string(intent), context,
            core::Vec3d{
                parsed_numbers.value()[0], parsed_numbers.value()[1],
                parsed_numbers.value()[2]});
    }

    if (words.front() == "extrude" || words.front() == "extrude-face") {
        const bool exact_syntax = parsed_numbers.value().size() == 1U &&
            static_cast<bool>(parse_number(words.back())) &&
            ((words.size() == 2U &&
                 (words[0] == "extrude" || words[0] == "extrude-face")) ||
             (words.size() == 3U && words[0] == "extrude" && words[1] == "face") ||
             (words.size() == 4U && words[0] == "extrude" && words[1] == "face" &&
                 words[2] == "by"));
        if (!exact_syntax) {
            return core::Result<Proposal>::failure(invalid(
                "extrude intent must be `extrude <distance>`, `extrude face <distance>`, "
                "`extrude face by <distance>`, or `extrude-face <distance>`"));
        }
        return make_face_proposal(
            request_id, std::string(intent), context, "mesh.extrude-face",
            editor::PreviewKind::extrude_face, parsed_numbers.value().front());
    }
    if (words.front() == "inset" || words.front() == "inset-face") {
        const bool exact_syntax = parsed_numbers.value().size() == 1U &&
            static_cast<bool>(parse_number(words.back())) &&
            ((words.size() == 2U &&
                 (words[0] == "inset" || words[0] == "inset-face")) ||
             (words.size() == 3U && words[0] == "inset" && words[1] == "face") ||
             (words.size() == 4U && words[0] == "inset" && words[1] == "face" &&
                 words[2] == "by"));
        if (!exact_syntax) {
            return core::Result<Proposal>::failure(invalid(
                "inset intent must be `inset <distance>`, `inset face <distance>`, "
                "`inset face by <distance>`, or `inset-face <distance>`"));
        }
        return make_face_proposal(
            request_id, std::string(intent), context, "mesh.inset-face",
            editor::PreviewKind::inset_face, parsed_numbers.value().front());
    }
    if (words.front() == "move" || words.front() == "set") {
        const bool exact_syntax =
            (words.size() == 5U || (words.size() == 6U && words[2] == "to")) &&
            words[1] == "vertex" && parsed_numbers.value().size() == 3U;
        const std::size_t coordinate_offset = words.size() == 6U ? 3U : 2U;
        const bool coordinates_valid = exact_syntax &&
            static_cast<bool>(parse_number(words[coordinate_offset])) &&
            static_cast<bool>(parse_number(words[coordinate_offset + 1U])) &&
            static_cast<bool>(parse_number(words[coordinate_offset + 2U]));
        if (!coordinates_valid) {
            return core::Result<Proposal>::failure(invalid(
                "vertex intent must be `move vertex <x> <y> <z>` or "
                "`move vertex to <x> <y> <z>`"));
        }
        Proposal proposal{
            request_id, context.project_revision, context.authoring,
            editor::PreviewKind::set_vertex_position,
            editor::PreviewParameters{
                std::nullopt, std::nullopt,
                core::Vec3d{
                    parsed_numbers.value()[0], parsed_numbers.value()[1],
                    parsed_numbers.value()[2]}},
            "mesh.set-vertex-position", std::string(intent),
            "Native planner produced a revision-bound vertex-position preview."};
        if (auto result = proposal.validate(context); !result) {
            return core::Result<Proposal>::failure(result.error());
        }
        return core::Result<Proposal>::success(std::move(proposal));
    }
    return core::Result<Proposal>::failure(invalid(
        "native planner supports translate object, extrude, inset, poke face, move vertex, "
        "material assignment, and lightmap UV generation intents"));
}

} // namespace carto::ai
