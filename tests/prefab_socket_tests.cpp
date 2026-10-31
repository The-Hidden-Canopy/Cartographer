#include <carto/condition/prefab_socket.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ": " + #condition); } while (false)

using namespace carto;

assets::Sha256Digest fixture_digest() {
    constexpr std::array<std::uint8_t, 4U> bytes{0x43U, 0x41U, 0x52U, 0x54U};
    return assets::sha256(bytes);
}

struct Fixture {
    condition::PrefabRecord prefab;
    condition::PrefabInstanceRecord instance;
    connection::ConnectionPortGraph ports;

    Fixture() {
        prefab.id = 1U;
        prefab.name = "Room";
        prefab.asset_ids = {4U};
        prefab.sockets = {"entry"};
        prefab.source_revision = core::Revision{1U};

        condition::PrefabInstanceMember member;
        member.asset_id = 4U;
        member.source_object_id = 10U;
        member.instance_object_id = 12U;
        member.mesh_asset_id = 20U;
        member.mesh_revision = core::Revision{3U};
        member.source_revision = "source-r3";
        member.source_digest = fixture_digest();
        member.instance_mesh_asset_id = 21U;
        member.snapshot_mesh_digest = fixture_digest();
        member.source_snapshot_mesh_digest = member.snapshot_mesh_digest;
        member.instance_mesh_revision = member.mesh_revision;
        member.instance_local_transform.translation = {4.0, 0.0, 0.0};

        instance.id = 2U;
        instance.prefab_id = prefab.id;
        instance.name = "Placed Room";
        instance.root_object_id = 11U;
        instance.captured_project_revision = core::Revision{5U};
        instance.source_authoring_revision = core::Revision{1U};
        instance.prefab_source_revision = prefab.source_revision;
        instance.members.push_back(member);
    }

    connection::ConnectionPort port(
        std::uint64_t id,
        std::string frame,
        std::optional<std::string> parent,
        core::Vec3d translation,
        std::uint64_t bound_source_object = 10U) const {
        connection::ConnectionPort value;
        value.id = connection::PortId{id};
        value.owner = scene::ObjectId{10U};
        value.authoring.kind = world::AuthoringBindingKind::scene_object;
        value.authoring.identity = bound_source_object;
        value.local_frame.frame_identity = std::move(frame);
        value.local_frame.parent_frame = std::move(parent);
        value.local_frame.transform_to_parent.translation = translation;
        value.local_frame.coordinate_convention = "carto.core/v1";
        return value;
    }

    void add_port(connection::ConnectionPort value) {
        REQUIRE(ports.insert_port(std::move(value)));
    }
};

void resolves_named_socket_through_parent_frames() {
    Fixture fixture;
    fixture.add_port(fixture.port(1U, "root", std::nullopt, {0.0, 2.0, 0.0}));
    fixture.add_port(fixture.port(2U, "entry", "root", {1.0, 0.0, 0.0}));

    const auto resolved = condition::resolve_prefab_socket(
        fixture.prefab, fixture.instance, fixture.ports, "entry");
    REQUIRE(resolved);
    REQUIRE(resolved.value().prefab_id == fixture.prefab.id);
    REQUIRE(resolved.value().instance_id == fixture.instance.id);
    REQUIRE(resolved.value().member_asset_id == 4U);
    REQUIRE(resolved.value().source_object_id == 10U);
    REQUIRE(resolved.value().instance_object_id == 12U);
    REQUIRE(resolved.value().source_port_id == 2U);
    REQUIRE(resolved.value().frame_chain_port_ids.size() == 2U);
    REQUIRE(resolved.value().frame_chain_port_ids[0] == 2U);
    REQUIRE(resolved.value().frame_chain_port_ids[1] == 1U);
    REQUIRE(resolved.value().frame_chain_port_digests.size() == 2U);
    REQUIRE(resolved.value().local_to_prefab_root.translation.x == 5.0);
    REQUIRE(resolved.value().local_to_prefab_root.translation.y == 2.0);
    REQUIRE(resolved.value().local_to_prefab_root.translation.z == 0.0);
    REQUIRE(resolved.value().validate());
    auto duplicated_chain = resolved.value();
    duplicated_chain.frame_chain_port_ids[1] = duplicated_chain.frame_chain_port_ids[0];
    REQUIRE(!duplicated_chain.validate());
    auto oversized_chain = resolved.value();
    oversized_chain.frame_chain_port_ids.assign(
        condition::PrefabSocketResolution::kMaxFrameDepth + 1U, 1U);
    oversized_chain.frame_chain_port_digests.assign(
        condition::PrefabSocketResolution::kMaxFrameDepth + 1U, fixture_digest());
    REQUIRE(!oversized_chain.validate());
}

void refuses_undeclared_and_stale_prefabs() {
    Fixture fixture;
    fixture.add_port(fixture.port(1U, "entry", std::nullopt, {}));
    REQUIRE(!condition::resolve_prefab_socket(
        fixture.prefab, fixture.instance, fixture.ports, "door"));
    REQUIRE(!condition::resolve_prefab_socket(
        fixture.prefab, fixture.instance, fixture.ports, "entry\n"));

    auto stale = fixture.instance;
    stale.prefab_source_revision = core::Revision{2U};
    REQUIRE(!condition::resolve_prefab_socket(
        fixture.prefab, stale, fixture.ports, "entry"));
}

void refuses_ambiguous_or_foreign_source_ports() {
    Fixture ambiguous;
    ambiguous.add_port(ambiguous.port(1U, "entry", std::nullopt, {}));
    ambiguous.add_port(ambiguous.port(2U, "entry", std::nullopt, {1.0, 0.0, 0.0}));
    REQUIRE(!condition::resolve_prefab_socket(
        ambiguous.prefab, ambiguous.instance, ambiguous.ports, "entry"));

    Fixture foreign;
    foreign.add_port(foreign.port(1U, "entry", std::nullopt, {}, 99U));
    REQUIRE(!condition::resolve_prefab_socket(
        foreign.prefab, foreign.instance, foreign.ports, "entry"));
}

void refuses_cyclic_and_missing_parent_frames() {
    Fixture cyclic;
    cyclic.add_port(cyclic.port(1U, "entry", "root", {}));
    cyclic.add_port(cyclic.port(2U, "root", "entry", {}));
    REQUIRE(!condition::resolve_prefab_socket(
        cyclic.prefab, cyclic.instance, cyclic.ports, "entry"));

    Fixture missing;
    missing.add_port(missing.port(1U, "entry", "missing", {}));
    REQUIRE(!condition::resolve_prefab_socket(
        missing.prefab, missing.instance, missing.ports, "entry"));

    Fixture foreign_parent;
    foreign_parent.add_port(foreign_parent.port(
        1U, "root", std::nullopt, {}, 99U));
    foreign_parent.add_port(foreign_parent.port(2U, "entry", "root", {}));
    REQUIRE(!condition::resolve_prefab_socket(
        foreign_parent.prefab, foreign_parent.instance, foreign_parent.ports, "entry"));
}

void refuses_legacy_unisolated_instances() {
    Fixture fixture;
    fixture.add_port(fixture.port(1U, "entry", std::nullopt, {}));
    auto legacy = fixture.instance;
    legacy.members.front().instance_mesh_asset_id = 0U;
    legacy.members.front().snapshot_mesh_digest = {};
    legacy.members.front().source_snapshot_mesh_digest = {};
    legacy.members.front().instance_mesh_revision = {};
    legacy.members.front().instance_local_transform = core::Transform::identity();
    REQUIRE(!condition::resolve_prefab_socket(
        fixture.prefab, legacy, fixture.ports, "entry"));
}

void refuses_overdeep_frame_hierarchies() {
    Fixture fixture;
    fixture.add_port(fixture.port(1U, "entry", "frame-0", {}));
    for (std::uint64_t index = 0U;
         index <= condition::PrefabSocketResolution::kMaxFrameDepth; ++index) {
        const std::string frame = "frame-" + std::to_string(index);
        std::optional<std::string> parent;
        if (index != condition::PrefabSocketResolution::kMaxFrameDepth) {
            parent = "frame-" + std::to_string(index + 1U);
        }
        fixture.add_port(fixture.port(index + 2U, frame, parent, {}));
    }
    REQUIRE(!condition::resolve_prefab_socket(
        fixture.prefab, fixture.instance, fixture.ports, "entry"));
}

} // namespace

int main() {
    try {
        resolves_named_socket_through_parent_frames();
        refuses_undeclared_and_stale_prefabs();
        refuses_ambiguous_or_foreign_source_ports();
        refuses_cyclic_and_missing_parent_frames();
        refuses_legacy_unisolated_instances();
        refuses_overdeep_frame_hierarchies();
        std::cout << "Prefab socket resolution tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
