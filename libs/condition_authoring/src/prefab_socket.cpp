#include <carto/condition/prefab_socket.hpp>

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace carto::condition {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_socket_name(std::string_view value) {
    return !value.empty() && value.size() <= 128U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
                byte != '\r' && byte != '\n';
        });
}

core::Result<core::Transform> resolve_frame_transform(
    const connection::ConnectionPortGraph& ports,
    const connection::ConnectionPort& port,
    std::uint64_t expected_source_object,
    std::vector<connection::PortId>& stack,
    std::vector<std::uint64_t>& frame_chain) {
    if (stack.size() >= PrefabSocketResolution::kMaxFrameDepth) {
        return core::Result<core::Transform>::failure(validation(
            "prefab socket frame hierarchy exceeds its depth bound"));
    }
    if (std::find(stack.begin(), stack.end(), port.id) != stack.end()) {
        return core::Result<core::Transform>::failure(validation(
            "prefab socket frame hierarchy contains a cycle"));
    }
    if (auto valid = port.validate(); !valid) {
        return core::Result<core::Transform>::failure(
            valid.error().with_context("prefab socket connection port"));
    }
    if (port.owner.value != expected_source_object ||
        port.authoring.kind != world::AuthoringBindingKind::scene_object ||
        port.authoring.identity != expected_source_object) {
        return core::Result<core::Transform>::failure(validation(
            "prefab socket frame hierarchy contains a foreign source binding"));
    }

    stack.push_back(port.id);
    frame_chain.push_back(port.id.value);
    core::Transform resolved = port.local_frame.transform_to_parent;
    if (port.local_frame.parent_frame.has_value()) {
        const connection::ConnectionPort* parent = nullptr;
        for (const auto& [id, candidate] : ports.ports()) {
            static_cast<void>(id);
            if (candidate.owner != port.owner ||
                candidate.local_frame.frame_identity != *port.local_frame.parent_frame) {
                continue;
            }
            if (parent != nullptr) {
                stack.pop_back();
                return core::Result<core::Transform>::failure(validation(
                    "prefab socket parent frame identity is ambiguous"));
            }
            parent = &candidate;
        }
        if (parent == nullptr) {
            stack.pop_back();
            return core::Result<core::Transform>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "prefab socket parent frame is not present on its source member"));
        }
        auto parent_transform = resolve_frame_transform(
            ports, *parent, expected_source_object, stack, frame_chain);
        if (!parent_transform) {
            stack.pop_back();
            return parent_transform;
        }
        resolved = parent_transform.value().combine(resolved);
    }
    stack.pop_back();
    if (!resolved.finite()) {
        return core::Result<core::Transform>::failure(validation(
            "prefab socket frame composition is not finite"));
    }
    return core::Result<core::Transform>::success(resolved);
}

} // namespace

core::Result<void> PrefabSocketResolution::validate() const {
    const bool frame_chain_bounded = !frame_chain_port_ids.empty() &&
        frame_chain_port_ids.size() <= kMaxFrameDepth &&
        frame_chain_port_ids.size() == frame_chain_port_digests.size();
    const bool frame_chain_unique = frame_chain_bounded && [&] {
        for (std::size_t index = 0U; index < frame_chain_port_ids.size(); ++index) {
            if (std::find(frame_chain_port_ids.begin(),
                    frame_chain_port_ids.begin() + static_cast<std::ptrdiff_t>(index),
                    frame_chain_port_ids[index]) !=
                frame_chain_port_ids.begin() + static_cast<std::ptrdiff_t>(index)) {
                return false;
            }
        }
        return true;
    }();
    if (prefab_id == 0U || instance_id == 0U || member_asset_id == 0U ||
        !safe_socket_name(socket_name) ||
        source_object_id == 0U || instance_object_id == 0U ||
        source_port_id == 0U || source_port_digest.is_zero() ||
        !frame_chain_bounded ||
        !frame_chain_unique ||
        frame_chain_port_ids.front() != source_port_id ||
        frame_chain_port_digests.front() != source_port_digest ||
        std::any_of(frame_chain_port_ids.begin(), frame_chain_port_ids.end(),
            [](std::uint64_t id) { return id == 0U; }) ||
        std::any_of(frame_chain_port_digests.begin(), frame_chain_port_digests.end(),
            [](const auto& digest) { return digest.is_zero(); }) ||
        !local_to_prefab_root.finite()) {
        return core::Result<void>::failure(validation(
            "prefab socket resolution identity, digest, or frame is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<PrefabSocketResolution> resolve_prefab_socket(
    const PrefabRecord& prefab,
    const PrefabInstanceRecord& instance,
    const connection::ConnectionPortGraph& ports,
    std::string_view socket_name) {
    if (auto valid = prefab.validate(); !valid) {
        return core::Result<PrefabSocketResolution>::failure(
            valid.error().with_context("prefab socket definition"));
    }
    if (auto valid = instance.validate(); !valid) {
        return core::Result<PrefabSocketResolution>::failure(
            valid.error().with_context("prefab socket instance"));
    }
    if (auto valid = ports.validate(); !valid) {
        return core::Result<PrefabSocketResolution>::failure(
            valid.error().with_context("prefab socket port graph"));
    }
    if (!safe_socket_name(socket_name)) {
        return core::Result<PrefabSocketResolution>::failure(invalid(
            "prefab socket name is empty or exceeds its identity bound"));
    }
    if (instance.prefab_id != prefab.id ||
        instance.prefab_source_revision != prefab.source_revision) {
        return core::Result<PrefabSocketResolution>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "prefab socket instance does not bind the supplied prefab revision"));
    }
    if (std::none_of(prefab.sockets.begin(), prefab.sockets.end(),
            [socket_name](const auto& socket) {
                return std::string_view{socket} == socket_name;
            })) {
        return core::Result<PrefabSocketResolution>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "prefab socket name is not declared by the supplied prefab"));
    }
    if (prefab.asset_ids.size() != instance.members.size() ||
        std::any_of(instance.members.begin(), instance.members.end(), [](const auto& member) {
            return member.instance_mesh_asset_id == 0U ||
                member.snapshot_mesh_digest.is_zero() ||
                member.source_snapshot_mesh_digest.is_zero() ||
                member.instance_mesh_revision.value() == 0U;
        })) {
        return core::Result<PrefabSocketResolution>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "prefab socket resolution requires recorded isolated member snapshot bindings"));
    }

    std::map<std::uint64_t, const PrefabInstanceMember*> members_by_source_object;
    for (std::size_t member_index = 0U; member_index < instance.members.size(); ++member_index) {
        const auto& member = instance.members[member_index];
        if (prefab.asset_ids[member_index] != member.asset_id) {
            return core::Result<PrefabSocketResolution>::failure(validation(
                "prefab socket member order does not match the source definition"));
        }
        members_by_source_object.emplace(member.source_object_id, &member);
    }

    const connection::ConnectionPort* matched_port = nullptr;
    const PrefabInstanceMember* matched_member = nullptr;
    for (const auto& [id, candidate] : ports.ports()) {
        static_cast<void>(id);
        if (candidate.local_frame.frame_identity != socket_name) continue;
        const auto member = members_by_source_object.find(candidate.owner.value);
        if (member == members_by_source_object.end()) continue;
        if (candidate.authoring.kind != world::AuthoringBindingKind::scene_object ||
            candidate.authoring.identity != member->second->source_object_id) {
            return core::Result<PrefabSocketResolution>::failure(validation(
                "prefab socket port does not carry the exact source-object binding"));
        }
        if (matched_port != nullptr) {
            return core::Result<PrefabSocketResolution>::failure(validation(
                "prefab socket name resolves to more than one source member port"));
        }
        matched_port = &candidate;
        matched_member = member->second;
    }
    if (matched_port == nullptr || matched_member == nullptr) {
        return core::Result<PrefabSocketResolution>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "declared prefab socket has no source-member connection port"));
    }

    std::vector<connection::PortId> frame_stack;
    std::vector<std::uint64_t> frame_chain;
    auto frame = resolve_frame_transform(
        ports, *matched_port, matched_member->source_object_id,
        frame_stack, frame_chain);
    if (!frame) {
        return core::Result<PrefabSocketResolution>::failure(frame.error());
    }
    std::vector<assets::Sha256Digest> frame_chain_digests;
    frame_chain_digests.reserve(frame_chain.size());
    for (const std::uint64_t port_id : frame_chain) {
        const auto* chain_port = ports.find_port(connection::PortId{port_id});
        if (chain_port == nullptr) {
            return core::Result<PrefabSocketResolution>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "prefab socket frame chain changed during resolution"));
        }
        auto digest = chain_port->canonical_digest();
        if (!digest) {
            return core::Result<PrefabSocketResolution>::failure(
                digest.error().with_context("prefab socket frame-chain digest"));
        }
        frame_chain_digests.push_back(digest.value());
    }

    PrefabSocketResolution result;
    result.prefab_id = prefab.id;
    result.instance_id = instance.id;
    result.member_asset_id = matched_member->asset_id;
    result.socket_name = std::string(socket_name);
    result.source_object_id = matched_member->source_object_id;
    result.instance_object_id = matched_member->instance_object_id;
    result.source_port_id = matched_port->id.value;
    result.source_port_digest = frame_chain_digests.front();
    result.frame_chain_port_ids = std::move(frame_chain);
    result.frame_chain_port_digests = std::move(frame_chain_digests);
    result.local_to_prefab_root =
        matched_member->instance_local_transform.combine(frame.value());
    if (auto valid = result.validate(); !valid) {
        return core::Result<PrefabSocketResolution>::failure(valid.error());
    }
    return core::Result<PrefabSocketResolution>::success(std::move(result));
}

} // namespace carto::condition
