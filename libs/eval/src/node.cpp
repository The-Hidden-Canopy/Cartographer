#include <carto/eval/node.hpp>

#include <set>
#include <string_view>
#include <utility>

namespace carto::eval {

namespace {

core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, std::move(message)));
}

bool valid_direction(PortDirection direction) noexcept {
    return direction == PortDirection::input || direction == PortDirection::output;
}

bool valid_value_kind(ValueKind kind) noexcept {
    switch (kind) {
    case ValueKind::scalar:
    case ValueKind::transform:
    case ValueKind::editable_mesh:
    case ValueKind::evaluated_mesh:
    case ValueKind::curve_set:
    case ValueKind::brep_body:
    case ValueKind::material_graph:
        return true;
    }
    return false;
}

bool contains_whitespace(std::string_view value) noexcept {
    return value.find_first_of(" \t\r\n") != std::string_view::npos;
}

} // namespace

core::Result<void> NodeTypeDefinition::validate() const {
    if (id.value.empty() || contains_whitespace(id.value)) {
        return invalid("evaluation node type requires a whitespace-free id");
    }
    if (ports.empty()) return invalid("evaluation node type requires at least one port");
    std::set<PortId> ids;
    for (const auto& port : ports) {
        if (port.id.value.empty() || contains_whitespace(port.id.value) ||
            !valid_direction(port.direction) ||
            !valid_value_kind(port.value_kind)) {
            return invalid("evaluation node type contains an invalid port");
        }
        if (!ids.insert(port.id).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "evaluation node type contains duplicate port ids"));
        }
    }
    return core::Result<void>::success();
}

const PortDescriptor* NodeTypeDefinition::find_port(PortId port) const noexcept {
    for (const auto& candidate : ports) {
        if (candidate.id == port) return &candidate;
    }
    return nullptr;
}

core::Result<void> NodeInstance::validate() const {
    if (!id || type.value.empty() || parameters_digest.empty() ||
        contains_whitespace(type.value) || contains_whitespace(parameters_digest)) {
        return invalid("evaluation node requires an id, type, and parameter digest");
    }
    if (parameter_revision.exhausted()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "evaluation node parameter revision space is exhausted"));
    }
    return core::Result<void>::success();
}

} // namespace carto::eval
