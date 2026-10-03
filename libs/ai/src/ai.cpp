#include <carto/ai/ai.hpp>

#include <algorithm>
#include <cctype>
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
    case editor::PreviewKind::extrude_face: return "extrude_face";
    case editor::PreviewKind::inset_face: return "inset_face";
    case editor::PreviewKind::set_vertex_position: return "set_vertex_position";
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

} // namespace

core::Result<void> OperationDescriptor::validate() const {
    if (!safe_token(id) || display_name.empty() || display_name.size() > 256U ||
        !safe_token(kernel_operation)) {
        return core::Result<void>::failure(invalid("AI operation descriptor identity is invalid"));
    }
    if (parameters.size() > 32U || precondition.empty() || precondition.size() > 512U) {
        return core::Result<void>::failure(invalid("AI operation descriptor is too large"));
    }
    for (const auto& parameter : parameters) {
        if (!safe_token(parameter)) {
            return core::Result<void>::failure(invalid("AI operation parameter name is invalid"));
        }
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
    if (!operation->preview_supported || operation->preview_kind != preview_kind) {
        return core::Result<void>::failure(invalid_state(
            "AI proposal references an operation without a matching native preview"));
    }
    if (preview_kind == editor::PreviewKind::extrude_face ||
        preview_kind == editor::PreviewKind::inset_face) {
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
                "exactly one object-bound face selected; face is finite, planar, strictly convex, and has a centroid strictly inside; operation requires a future preview contract";
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
                tool.id == "mesh.dissolve-edge" ||
                tool.id == "mesh.tri-to-quad" ||
                tool.id == "mesh.merge-vertices";
            descriptor.precondition = known_id
                ? "registered tool ID and native kernel contract do not match; preview disabled"
                : "native tool-specific validation required";
        }
        result.push_back(std::move(descriptor));
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
            ",\"preview_kind\":";
        if (operation.preview_kind.has_value()) {
            result += "\"" + preview_name(*operation.preview_kind) + "\"";
        } else {
            result += "null";
        }
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

    if (words.front() == "extrude" || words.front() == "extrude-face") {
        if (parsed_numbers.value().size() != 1U) {
            return core::Result<Proposal>::failure(invalid(
                "extrude intent requires exactly one distance"));
        }
        return make_face_proposal(
            request_id, std::string(intent), context, "mesh.extrude-face",
            editor::PreviewKind::extrude_face, parsed_numbers.value().front());
    }
    if (words.front() == "inset" || words.front() == "inset-face") {
        if (parsed_numbers.value().size() != 1U) {
            return core::Result<Proposal>::failure(invalid(
                "inset intent requires exactly one distance"));
        }
        return make_face_proposal(
            request_id, std::string(intent), context, "mesh.inset-face",
            editor::PreviewKind::inset_face, parsed_numbers.value().front());
    }
    if (words.front() == "move" || words.front() == "set") {
        if (words.size() < 2U || words[1] != "vertex" || parsed_numbers.value().size() != 3U) {
            return core::Result<Proposal>::failure(invalid(
                "move vertex intent requires exactly three coordinates"));
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
        "native planner supports extrude, inset, and move vertex intents"));
}

} // namespace carto::ai
