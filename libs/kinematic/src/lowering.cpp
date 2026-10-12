#include <carto/kinematic/lowering.hpp>

#include <carto/kinematic/graph.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace carto::kinematic {
namespace {

constexpr std::string_view kSkeletalMagic =
    "CARTOGRAPHER_SKELETAL_ASSET_DESCRIPTOR";
constexpr std::string_view kArchetypeMagic =
    "CARTOGRAPHER_ARCHETYPE_DESCRIPTOR";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
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

bool valid_capability(KinematicCapability feature) {
    switch (feature) {
    case KinematicCapability::skeletal_deformation:
    case KinematicCapability::rigid_articulation:
    case KinematicCapability::angular_limits:
    case KinematicCapability::linear_limits:
        return true;
    }
    return false;
}

bool valid_disposition(KinematicCapabilityDisposition disposition) {
    switch (disposition) {
    case KinematicCapabilityDisposition::supported:
    case KinematicCapabilityDisposition::unsupported:
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

bool read_bool(std::istream& input, bool& value) {
    unsigned encoded = 0U;
    if (!read_integer(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
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

void write_digest(std::ostream& output, const assets::Sha256Digest& digest) {
    output << digest.hex();
}

bool read_digest(std::istream& input, assets::Sha256Digest& digest) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) return false;
    digest = parsed.value();
    return true;
}

assets::Sha256Digest digest_text(const std::string& text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

// Upward closure: every node that is skinned or an ancestor of a skinned node.
std::set<JointId> deforming_closure(const KinematicGraph& graph) {
    std::set<JointId> marked;
    for (const auto& [joint, node] : graph.nodes()) {
        if (node.node_class == NodeClass::skinned) marked.insert(joint);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [joint, node] : graph.nodes()) {
            if (!marked.contains(joint) || !node.parent.has_value()) continue;
            if (marked.insert(node.parent.value()).second) changed = true;
        }
    }
    return marked;
}

} // namespace

core::Result<void> KinematicCapabilityCheck::validate() const {
    if (!valid_capability(feature)) {
        return core::Result<void>::failure(invalid(
            "kinematic capability feature is invalid"));
    }
    if (!valid_disposition(disposition)) {
        return core::Result<void>::failure(invalid(
            "kinematic capability disposition is invalid"));
    }
    if (!safe_text(detail, 1024U, true)) {
        return core::Result<void>::failure(invalid(
            "kinematic capability detail is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> require_capabilities(
    std::span<const KinematicCapabilityCheck> checks) {
    for (const KinematicCapabilityCheck& check : checks) {
        if (auto result = check.validate(); !result) return result;
        if (check.required &&
            check.disposition == KinematicCapabilityDisposition::unsupported) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "required kinematic capability is unsupported by the target"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> SkeletalAssetDescriptor::validate() const {
    if (mesh_asset == 0U || graph_digest.is_zero() || skin_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "skeletal asset descriptor must name mesh and source digests"));
    }
    if (joints.empty() || joints.size() > kMaxJoints) {
        return core::Result<void>::failure(validation(
            "skeletal asset descriptor must contain a bounded joint set"));
    }
    for (const auto& [joint, node] : joints) {
        if (joint != node.joint || !node.joint) {
            return core::Result<void>::failure(validation(
                "skeletal joint descriptor identity does not match its record"));
        }
        if (node.parent.has_value() &&
            (!node.parent.value() || !joints.contains(node.parent.value()))) {
            return core::Result<void>::failure(validation(
                "skeletal joint descriptor parent is missing"));
        }
        if (!valid_joint_kind(node.kind)) {
            return core::Result<void>::failure(invalid(
                "skeletal joint descriptor kind is invalid"));
        }
        if (!node.rest.finite()) {
            return core::Result<void>::failure(invalid(
                "skeletal joint descriptor rest transform is not finite"));
        }
    }
    return core::Result<void>::success();
}

std::string SkeletalAssetDescriptor::serialize() const {
    std::ostringstream output;
    output << kSkeletalMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "MESH " << mesh_asset << '\n';
    output << "GRAPH_DIGEST ";
    write_digest(output, graph_digest);
    output << '\n';
    output << "SKIN_DIGEST ";
    write_digest(output, skin_digest);
    output << '\n';
    output << "LIMIT_DIGEST ";
    write_digest(output, limit_digest);
    output << '\n';
    output << "JOINTS " << joints.size() << '\n';
    for (const auto& [joint, node] : joints) {
        output << "SJOINT " << joint.value << ' '
               << (node.parent.has_value() ? node.parent->value : 0U) << ' '
               << static_cast<unsigned>(node.kind) << ' '
               << (node.has_skin ? 1 : 0) << ' ';
        write_transform(output, node.rest);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> SkeletalAssetDescriptor::canonical_digest()
    const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<SkeletalAssetDescriptor> SkeletalAssetDescriptor::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<SkeletalAssetDescriptor>::failure(parse_error(
            "skeletal asset descriptor text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kSkeletalMagic) {
        return core::Result<SkeletalAssetDescriptor>::failure(parse_error(
            "skeletal asset descriptor framing is invalid"));
    }
    if (!read_integer(input, version)) {
        return core::Result<SkeletalAssetDescriptor>::failure(parse_error(
            "skeletal asset descriptor schema version is missing"));
    }
    if (version != kSchemaVersion) {
        return core::Result<SkeletalAssetDescriptor>::failure(version_error(
            "skeletal asset descriptor schema version is not supported"));
    }

    SkeletalAssetDescriptor descriptor;
    if (!require_token(input, "MESH") ||
        !read_integer(input, descriptor.mesh_asset) ||
        !require_token(input, "GRAPH_DIGEST") ||
        !read_digest(input, descriptor.graph_digest) ||
        !require_token(input, "SKIN_DIGEST") ||
        !read_digest(input, descriptor.skin_digest) ||
        !require_token(input, "LIMIT_DIGEST") ||
        !read_digest(input, descriptor.limit_digest)) {
        return core::Result<SkeletalAssetDescriptor>::failure(parse_error(
            "skeletal asset descriptor header is invalid"));
    }

    std::size_t joint_count = 0U;
    if (!require_token(input, "JOINTS") ||
        !read_count(input, kMaxJoints, joint_count)) {
        return core::Result<SkeletalAssetDescriptor>::failure(parse_error(
            "skeletal asset descriptor joint header is invalid"));
    }
    for (std::size_t index = 0U; index < joint_count; ++index) {
        SkeletalJointDescriptor node;
        std::uint64_t parent = 0U;
        unsigned kind = 0U;
        if (!require_token(input, "SJOINT") ||
            !read_integer(input, node.joint.value) ||
            !read_integer(input, parent) ||
            !read_integer(input, kind) ||
            !read_bool(input, node.has_skin) ||
            !read_transform(input, node.rest)) {
            return core::Result<SkeletalAssetDescriptor>::failure(parse_error(
                "skeletal joint descriptor record is malformed"));
        }
        node.parent = parent == 0U
            ? std::optional<JointId>{}
            : std::optional<JointId>{JointId{parent}};
        node.kind = static_cast<JointKind>(kind);
        if (!descriptor.joints.emplace(node.joint, node).second) {
            return core::Result<SkeletalAssetDescriptor>::failure(parse_error(
                "skeletal joint descriptor identity is duplicated"));
        }
    }

    if (!require_token(input, "END")) {
        return core::Result<SkeletalAssetDescriptor>::failure(parse_error(
            "skeletal asset descriptor is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<SkeletalAssetDescriptor>::failure(parse_error(
            "skeletal asset descriptor contains trailing unbound data"));
    }
    if (auto result = descriptor.validate(); !result) {
        return core::Result<SkeletalAssetDescriptor>::failure(result.error());
    }
    return core::Result<SkeletalAssetDescriptor>::success(std::move(descriptor));
}

core::Result<void> ArchetypeDescriptor::validate() const {
    if (graph_digest.is_zero() ||
        !safe_text(archetype_identity, kMaxIdentityBytes)) {
        return core::Result<void>::failure(validation(
            "archetype descriptor must name its graph digest and identity"));
    }
    if (nodes.empty() || nodes.size() > kMaxNodes) {
        return core::Result<void>::failure(validation(
            "archetype descriptor must contain a bounded node set"));
    }
    for (const auto& [joint, node] : nodes) {
        if (joint != node.joint || !node.joint) {
            return core::Result<void>::failure(validation(
                "archetype node descriptor identity does not match its record"));
        }
        if (node.parent.has_value() &&
            (!node.parent.value() || !nodes.contains(node.parent.value()))) {
            return core::Result<void>::failure(validation(
                "archetype node descriptor parent is missing"));
        }
        if (!valid_joint_kind(node.kind)) {
            return core::Result<void>::failure(invalid(
                "archetype node descriptor kind is invalid"));
        }
        if (!node.rest.finite()) {
            return core::Result<void>::failure(invalid(
                "archetype node descriptor rest transform is not finite"));
        }
    }
    return core::Result<void>::success();
}

std::string ArchetypeDescriptor::serialize() const {
    std::ostringstream output;
    output << kArchetypeMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "GRAPH_DIGEST ";
    write_digest(output, graph_digest);
    output << '\n';
    output << "IDENTITY " << std::quoted(archetype_identity) << '\n';
    output << "NODES " << nodes.size() << '\n';
    for (const auto& [joint, node] : nodes) {
        output << "ANODE " << joint.value << ' ' << node.source_object.value
               << ' ' << (node.parent.has_value() ? node.parent->value : 0U)
               << ' ' << static_cast<unsigned>(node.kind) << ' '
               << node.mesh_asset << ' ';
        write_transform(output, node.rest);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> ArchetypeDescriptor::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<ArchetypeDescriptor> ArchetypeDescriptor::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<ArchetypeDescriptor>::failure(parse_error(
            "archetype descriptor text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kArchetypeMagic) {
        return core::Result<ArchetypeDescriptor>::failure(parse_error(
            "archetype descriptor framing is invalid"));
    }
    if (!read_integer(input, version)) {
        return core::Result<ArchetypeDescriptor>::failure(parse_error(
            "archetype descriptor schema version is missing"));
    }
    if (version != kSchemaVersion) {
        return core::Result<ArchetypeDescriptor>::failure(version_error(
            "archetype descriptor schema version is not supported"));
    }

    ArchetypeDescriptor descriptor;
    if (!require_token(input, "GRAPH_DIGEST") ||
        !read_digest(input, descriptor.graph_digest) ||
        !require_token(input, "IDENTITY") ||
        !(input >> std::quoted(descriptor.archetype_identity))) {
        return core::Result<ArchetypeDescriptor>::failure(parse_error(
            "archetype descriptor header is invalid"));
    }

    std::size_t node_count = 0U;
    if (!require_token(input, "NODES") ||
        !read_count(input, kMaxNodes, node_count)) {
        return core::Result<ArchetypeDescriptor>::failure(parse_error(
            "archetype descriptor node header is invalid"));
    }
    for (std::size_t index = 0U; index < node_count; ++index) {
        ArchetypeNodeDescriptor node;
        std::uint64_t parent = 0U;
        unsigned kind = 0U;
        if (!require_token(input, "ANODE") ||
            !read_integer(input, node.joint.value) ||
            !read_integer(input, node.source_object.value) ||
            !read_integer(input, parent) ||
            !read_integer(input, kind) ||
            !read_integer(input, node.mesh_asset) ||
            !read_transform(input, node.rest)) {
            return core::Result<ArchetypeDescriptor>::failure(parse_error(
                "archetype node descriptor record is malformed"));
        }
        node.parent = parent == 0U
            ? std::optional<JointId>{}
            : std::optional<JointId>{JointId{parent}};
        node.kind = static_cast<JointKind>(kind);
        if (!descriptor.nodes.emplace(node.joint, node).second) {
            return core::Result<ArchetypeDescriptor>::failure(parse_error(
                "archetype node descriptor identity is duplicated"));
        }
    }

    if (!require_token(input, "END")) {
        return core::Result<ArchetypeDescriptor>::failure(parse_error(
            "archetype descriptor is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ArchetypeDescriptor>::failure(parse_error(
            "archetype descriptor contains trailing unbound data"));
    }
    if (auto result = descriptor.validate(); !result) {
        return core::Result<ArchetypeDescriptor>::failure(result.error());
    }
    return core::Result<ArchetypeDescriptor>::success(std::move(descriptor));
}

core::Result<SkeletalAssetDescriptor> describe_deforming_subgraph(
    std::uint64_t mesh_asset,
    const KinematicGraph& graph,
    const SkinBinding& skin,
    const JointLimitSet& limits) {
    if (mesh_asset == 0U) {
        return core::Result<SkeletalAssetDescriptor>::failure(invalid(
            "deforming subgraph lowering requires a mesh identity"));
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<SkeletalAssetDescriptor>::failure(result.error());
    }
    if (auto result = skin.validate(); !result) {
        return core::Result<SkeletalAssetDescriptor>::failure(result.error());
    }
    if (auto result = limits.validate(); !result) {
        return core::Result<SkeletalAssetDescriptor>::failure(result.error());
    }
    const auto graph_digest = graph.canonical_digest();
    if (!(skin.graph_digest == graph_digest.value())) {
        return core::Result<SkeletalAssetDescriptor>::failure(stale(
            "skin binding was authored against a different graph revision"));
    }

    const std::set<JointId> deforming = deforming_closure(graph);
    if (deforming.empty()) {
        return core::Result<SkeletalAssetDescriptor>::failure(validation(
            "graph has no deforming subgraph to lower"));
    }

    std::set<JointId> skinned_joints;
    for (const auto& [vertex, influences] : skin.influences) {
        static_cast<void>(vertex);
        for (const JointInfluence& influence : influences) {
            skinned_joints.insert(influence.joint);
        }
    }

    SkeletalAssetDescriptor descriptor;
    descriptor.mesh_asset = mesh_asset;
    descriptor.graph_digest = graph_digest.value();
    descriptor.skin_digest = skin.canonical_digest().value();
    descriptor.limit_digest = limits.canonical_digest().value();
    for (const JointId joint : deforming) {
        const KinematicNode& node = graph.nodes().at(joint);
        SkeletalJointDescriptor skeletal_joint;
        skeletal_joint.joint = joint;
        if (node.parent.has_value() && deforming.contains(node.parent.value())) {
            skeletal_joint.parent = node.parent;
        }
        skeletal_joint.rest = node.rest;
        skeletal_joint.kind = node.kind;
        skeletal_joint.has_skin = skinned_joints.contains(joint);
        descriptor.joints.emplace(joint, skeletal_joint);
    }
    if (auto result = descriptor.validate(); !result) {
        return core::Result<SkeletalAssetDescriptor>::failure(result.error());
    }
    return core::Result<SkeletalAssetDescriptor>::success(std::move(descriptor));
}

core::Result<ArchetypeDescriptor> describe_rigid_subgraph(
    const KinematicGraph& graph) {
    if (auto result = graph.validate(); !result) {
        return core::Result<ArchetypeDescriptor>::failure(result.error());
    }
    const auto graph_digest = graph.canonical_digest();

    const std::set<JointId> deforming = deforming_closure(graph);
    std::vector<JointId> rigid_joints;
    for (const auto& [joint, node] : graph.nodes()) {
        static_cast<void>(node);
        if (!deforming.contains(joint)) rigid_joints.push_back(joint);
    }
    if (rigid_joints.empty()) {
        return core::Result<ArchetypeDescriptor>::failure(validation(
            "graph has no rigid subgraph to lower"));
    }

    // Structural digest of the rigid subgraph alone: this is what lets one
    // archetype serve every repeated instance of the same mechanism.
    std::ostringstream structural;
    structural << std::setprecision(17);
    for (const JointId joint : rigid_joints) {
        const KinematicNode& node = graph.nodes().at(joint);
        structural << "RIGID_NODE " << joint.value << ' '
                   << (node.parent.has_value() ? node.parent->value : 0U) << ' '
                   << static_cast<unsigned>(node.kind) << ' '
                   << static_cast<unsigned>(node.node_class);
        for (const double component : {
                 node.rest.translation.x, node.rest.translation.y,
                 node.rest.translation.z, node.rest.rotation.x,
                 node.rest.rotation.y, node.rest.rotation.z, node.rest.rotation.w,
                 node.rest.scale.x, node.rest.scale.y, node.rest.scale.z}) {
            structural << ' ' << component;
        }
        structural << ' ' << node.semantic_type << '\n';
    }
    const assets::Sha256Digest structural_digest =
        digest_text(structural.str());

    ArchetypeDescriptor descriptor;
    descriptor.graph_digest = graph_digest.value();
    descriptor.archetype_identity =
        "kinematic-archetype-" + structural_digest.hex();
    for (const JointId joint : rigid_joints) {
        const KinematicNode& node = graph.nodes().at(joint);
        ArchetypeNodeDescriptor archetype_node;
        archetype_node.joint = joint;
        archetype_node.source_object = scene::ObjectId{0U};
        if (node.parent.has_value() && !deforming.contains(node.parent.value())) {
            archetype_node.parent = node.parent;
        }
        archetype_node.rest = node.rest;
        archetype_node.kind = node.kind;
        descriptor.nodes.emplace(joint, archetype_node);
    }
    if (auto result = descriptor.validate(); !result) {
        return core::Result<ArchetypeDescriptor>::failure(result.error());
    }
    return core::Result<ArchetypeDescriptor>::success(std::move(descriptor));
}

} // namespace carto::kinematic
