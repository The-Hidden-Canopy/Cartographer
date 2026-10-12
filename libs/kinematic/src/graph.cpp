#include <carto/kinematic/graph.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>

namespace carto::kinematic {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_KINEMATIC_GRAPH";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic version_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool valid_joint_kind(JointKind kind) {
    switch (kind) {
    case JointKind::fixed:
    case JointKind::hinge:
    case JointKind::slider:
    case JointKind::ball:
        return true;
    }
    return false;
}

bool valid_node_class(NodeClass node_class) {
    switch (node_class) {
    case NodeClass::rigid:
    case NodeClass::skinned:
        return true;
    }
    return false;
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

void write_transform(std::ostream& output, const core::Transform& transform) {
    output << transform.translation.x << ' ' << transform.translation.y << ' '
           << transform.translation.z << ' ' << transform.rotation.x << ' '
           << transform.rotation.y << ' ' << transform.rotation.z << ' '
           << transform.rotation.w << ' ' << transform.scale.x << ' '
           << transform.scale.y << ' ' << transform.scale.z;
}

bool read_transform(std::istream& input, core::Transform& transform) {
    return static_cast<bool>(
        input >> transform.translation.x >> transform.translation.y >>
        transform.translation.z >> transform.rotation.x >> transform.rotation.y >>
        transform.rotation.z >> transform.rotation.w >> transform.scale.x >>
        transform.scale.y >> transform.scale.z);
}

} // namespace

std::string_view joint_kind_name(JointKind kind) noexcept {
    switch (kind) {
    case JointKind::fixed: return "fixed";
    case JointKind::hinge: return "hinge";
    case JointKind::slider: return "slider";
    case JointKind::ball: return "ball";
    }
    return "unknown";
}

std::string_view node_class_name(NodeClass node_class) noexcept {
    switch (node_class) {
    case NodeClass::rigid: return "rigid";
    case NodeClass::skinned: return "skinned";
    }
    return "unknown";
}

core::Result<void> KinematicNode::validate() const {
    if (!id) {
        return core::Result<void>::failure(invalid(
            "kinematic node identity is invalid"));
    }
    if (parent.has_value() && (!parent.value() || parent.value() == id)) {
        return core::Result<void>::failure(validation(
            "kinematic node parent is invalid or self-referential"));
    }
    if (!valid_joint_kind(kind)) {
        return core::Result<void>::failure(invalid(
            "kinematic node joint kind is invalid"));
    }
    if (!valid_node_class(node_class)) {
        return core::Result<void>::failure(invalid(
            "kinematic node class is invalid"));
    }
    if (!safe_text(semantic_type, KinematicGraph::kMaxSemanticBytes, true)) {
        return core::Result<void>::failure(invalid(
            "kinematic node semantic type is invalid"));
    }
    if (!rest.finite()) {
        return core::Result<void>::failure(invalid(
            "kinematic node rest transform is not finite"));
    }
    return core::Result<void>::success();
}

core::Result<void> KinematicGraph::insert_node(KinematicNode node) {
    if (nodes_.size() >= kMaxNodes) {
        return core::Result<void>::failure(validation(
            "kinematic graph has reached its bounded node capacity"));
    }
    if (auto result = node.validate(); !result) return result;
    const auto [iterator, inserted] = nodes_.emplace(node.id, std::move(node));
    static_cast<void>(iterator);
    if (!inserted) {
        return core::Result<void>::failure(invalid(
            "kinematic node identity is duplicated"));
    }
    return core::Result<void>::success();
}

const KinematicNode* KinematicGraph::find(JointId id) const noexcept {
    const auto found = nodes_.find(id);
    return found == nodes_.end() ? nullptr : &found->second;
}

core::Result<void> KinematicGraph::validate() const {
    if (nodes_.size() > kMaxNodes) {
        return core::Result<void>::failure(validation(
            "kinematic graph exceeds its bounded node capacity"));
    }

    std::size_t roots = 0U;
    for (const auto& [identity, node] : nodes_) {
        if (identity != node.id) {
            return core::Result<void>::failure(validation(
                "kinematic node map identity does not match its record"));
        }
        if (auto result = node.validate(); !result) return result;
        if (node.parent.has_value() && !nodes_.contains(node.parent.value())) {
            return core::Result<void>::failure(validation(
                "kinematic node references a missing parent"));
        }
        if (!node.parent.has_value()) ++roots;
    }
    if (roots != 1U) {
        return core::Result<void>::failure(validation(
            "kinematic graph must have exactly one root"));
    }

    for (const auto& [identity, node] : nodes_) {
        std::set<JointId> visited;
        const KinematicNode* current = &node;
        while (current->parent.has_value()) {
            if (!visited.insert(current->id).second) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::cycle_detected,
                    "kinematic graph hierarchy contains a cycle"));
            }
            current = &nodes_.at(current->parent.value());
        }
    }
    return core::Result<void>::success();
}

std::string KinematicGraph::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);

    output << "NODES " << nodes_.size() << '\n';
    for (const auto& [identity, node] : nodes_) {
        output << "NODE " << identity.value << ' '
               << (node.parent.has_value() ? node.parent->value : 0U) << ' '
               << static_cast<unsigned>(node.kind) << ' '
               << static_cast<unsigned>(node.node_class) << ' '
               << std::quoted(node.semantic_type) << ' ';
        write_transform(output, node.rest);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> KinematicGraph::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<KinematicGraph> KinematicGraph::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<KinematicGraph>::failure(parse_error(
            "kinematic graph text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic) {
        return core::Result<KinematicGraph>::failure(parse_error(
            "kinematic graph framing is invalid"));
    }
    if (!read_integer(input, version)) {
        return core::Result<KinematicGraph>::failure(parse_error(
            "kinematic graph schema version is missing"));
    }
    if (version != kSchemaVersion) {
        return core::Result<KinematicGraph>::failure(version_error(
            "kinematic graph schema version is not supported"));
    }

    KinematicGraph graph;
    std::size_t node_count = 0U;
    if (!require_token(input, "NODES") ||
        !read_count(input, kMaxNodes, node_count)) {
        return core::Result<KinematicGraph>::failure(parse_error(
            "kinematic graph node header is invalid"));
    }
    for (std::size_t index = 0U; index < node_count; ++index) {
        KinematicNode node;
        std::uint64_t parent = 0U;
        unsigned kind = 0U;
        unsigned node_class = 0U;
        if (!require_token(input, "NODE") ||
            !read_integer(input, node.id.value) ||
            !read_integer(input, parent) ||
            !read_integer(input, kind) ||
            !read_integer(input, node_class) ||
            !(input >> std::quoted(node.semantic_type)) ||
            !read_transform(input, node.rest)) {
            return core::Result<KinematicGraph>::failure(parse_error(
                "kinematic graph node record is malformed"));
        }
        node.parent = parent == 0U
            ? std::optional<JointId>{}
            : std::optional<JointId>{JointId{parent}};
        node.kind = static_cast<JointKind>(kind);
        node.node_class = static_cast<NodeClass>(node_class);
        if (auto result = graph.insert_node(std::move(node)); !result) {
            return core::Result<KinematicGraph>::failure(result.error());
        }
    }

    if (!require_token(input, "END")) {
        return core::Result<KinematicGraph>::failure(parse_error(
            "kinematic graph is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<KinematicGraph>::failure(parse_error(
            "kinematic graph contains trailing unbound data"));
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<KinematicGraph>::failure(result.error());
    }
    return core::Result<KinematicGraph>::success(std::move(graph));
}

} // namespace carto::kinematic
