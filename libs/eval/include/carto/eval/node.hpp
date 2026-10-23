#pragma once

#include <carto/core/revision.hpp>
#include <carto/core/result.hpp>
#include <carto/eval/ids.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace carto::eval {

enum class PortDirection {
    input,
    output,
};

enum class ValueKind {
    scalar,
    transform,
    editable_mesh,
    evaluated_mesh,
    curve_set,
    brep_body,
    material_graph,
};

struct PortDescriptor {
    PortId id;
    PortDirection direction = PortDirection::input;
    ValueKind value_kind = ValueKind::scalar;
};

struct NodeTypeDefinition {
    NodeTypeId id;
    std::vector<PortDescriptor> ports;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] const PortDescriptor* find_port(PortId port) const noexcept;
};

struct NodeInstance {
    NodeId id;
    NodeTypeId type;
    core::Revision parameter_revision;
    std::string parameters_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct Connection {
    NodeId output_node;
    PortId output_port;
    NodeId input_node;
    PortId input_port;

    [[nodiscard]] constexpr auto operator<=>(const Connection&) const noexcept = default;
};

} // namespace carto::eval
