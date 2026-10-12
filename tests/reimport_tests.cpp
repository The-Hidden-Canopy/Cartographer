#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/revision.hpp>
#include <carto/production/reimport.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

using namespace carto;

production::PreparedSceneNode make_node(
    std::uint64_t id,
    std::string name,
    std::optional<std::uint64_t> parent = std::nullopt,
    double tx = 0.0,
    bool visible = true) {
    production::PreparedSceneNode node;
    node.id = id;
    node.name = std::move(name);
    node.parent = parent;
    node.local_transform.translation = {tx, 0.0, 0.0};
    node.visible = visible;
    return node;
}

void sort_correspondences(production::PreparedSceneEnvelope& envelope) {
    std::sort(envelope.correspondences.begin(), envelope.correspondences.end(),
        [](const production::PreparedSourceCorrespondence& left,
            const production::PreparedSourceCorrespondence& right) {
            return std::tuple{
                static_cast<unsigned>(left.source_kind), left.source_primary,
                left.source_secondary, static_cast<unsigned>(left.prepared_kind),
                left.prepared_primary, left.prepared_secondary} <
                std::tuple{
                    static_cast<unsigned>(right.source_kind), right.source_primary,
                    right.source_secondary, static_cast<unsigned>(right.prepared_kind),
                    right.prepared_primary, right.prepared_secondary};
        });
}

void add_node_correspondence(
    production::PreparedSceneEnvelope& envelope, std::uint64_t node_id) {
    envelope.correspondences.push_back(production::PreparedSourceCorrespondence{
        production::PreparedSourceEntityKind::scene_node, node_id, 0U,
        production::PreparedEntityKind::node, node_id, 0U});
}

// A valid nodes-only prepared revision; every node maps 1:1 to its stable
// source identity.
production::PreparedSceneEnvelope make_envelope(
    std::uint64_t revision,
    std::vector<production::PreparedSceneNode> nodes) {
    production::PreparedSceneEnvelope envelope;
    envelope.source_namespace = "torture.world";
    envelope.source_revision = core::Revision{revision};
    envelope.source_digest = digest("torture-source-v" + std::to_string(revision));
    envelope.profile_identity = "portable.torture.v1";
    envelope.profile_digest = digest("portable.torture.v1");
    envelope.coordinates.meters_per_unit = 1.0;
    envelope.coordinates.handedness = production::PreparedHandedness::right_handed;
    envelope.coordinates.up = production::PreparedAxis::positive_y;
    envelope.coordinates.forward = production::PreparedAxis::positive_z;
    std::sort(nodes.begin(), nodes.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    envelope.nodes = std::move(nodes);
    for (const auto& node : envelope.nodes) add_node_correspondence(envelope, node.id);
    sort_correspondences(envelope);
    require(static_cast<bool>(envelope.validate()), "fixture envelope validates");
    return envelope;
}

// A valid envelope carrying one reusable mesh asset, one material region
// binding, and the two products (render mesh + scoped material catalog) that
// the exact-closure rules require.
production::PreparedSceneEnvelope make_rich_envelope(
    std::uint64_t revision,
    std::vector<production::PreparedSceneNode> nodes) {
    production::PreparedSceneEnvelope envelope = make_envelope(revision, std::move(nodes));
    const std::string mesh_product = "product.mesh.10";
    const std::string material_product = "product.materials";
    envelope.products.push_back(production::PreparedProductReference{
        material_product, production::PreparedProductKind::material_catalog, 1U,
        digest("materials-v" + std::to_string(revision)), "products/materials.json"});
    envelope.products.push_back(production::PreparedProductReference{
        mesh_product, production::PreparedProductKind::render_mesh, 1U,
        digest("mesh-v" + std::to_string(revision)), "products/mesh-10.bin"});
    std::sort(envelope.products.begin(), envelope.products.end(),
        [](const auto& left, const auto& right) { return left.identity < right.identity; });
    envelope.dependencies.push_back(production::PreparedDependencyReference{
        material_product, production::PreparedDependencyDisposition::included_product,
        true, core::Revision{revision}, digest("materials-v" + std::to_string(revision)),
        "products/materials.json"});
    envelope.dependencies.push_back(production::PreparedDependencyReference{
        mesh_product, production::PreparedDependencyDisposition::included_product,
        true, core::Revision{revision}, digest("mesh-v" + std::to_string(revision)),
        "products/mesh-10.bin"});
    std::sort(envelope.dependencies.begin(), envelope.dependencies.end(),
        [](const auto& left, const auto& right) { return left.identity < right.identity; });
    envelope.assets.push_back(production::PreparedAssetDefinition{
        10U, "crate-mesh", mesh_product, std::nullopt,
        production::prepared_positions | production::prepared_normals |
            production::prepared_source_correspondence |
            production::prepared_material_regions,
        {3U}});
    envelope.material_bindings.push_back(production::PreparedMaterialBinding{
        10U, 3U, 71U, std::nullopt});
    envelope.correspondences.push_back(production::PreparedSourceCorrespondence{
        production::PreparedSourceEntityKind::mesh_asset, 10U, 0U,
        production::PreparedEntityKind::asset, 10U, 0U});
    envelope.correspondences.push_back(production::PreparedSourceCorrespondence{
        production::PreparedSourceEntityKind::material_region, 10U, 3U,
        production::PreparedEntityKind::material_binding, 10U, 3U});
    sort_correspondences(envelope);
    require(static_cast<bool>(envelope.validate()), "rich fixture envelope validates");
    return envelope;
}

production::RuntimeSourceState intact_state(
    std::uint64_t node_id,
    std::optional<std::uint64_t> parent,
    std::uint64_t generation) {
    production::RuntimeSourceState state;
    state.source_kind = production::PreparedSourceEntityKind::scene_node;
    state.source_primary = node_id;
    state.state_digest = digest("state-" + std::to_string(generation) + "-" +
        std::to_string(node_id));
    state.admitted_state_digest = state.state_digest;
    state.placement_digest = digest("place-admitted-" + std::to_string(node_id));
    state.admitted_placement_digest = state.placement_digest;
    state.parent_source = parent;
    return state;
}

void move_state(production::RuntimeSourceState& state, std::uint64_t generation) {
    state.placement_digest = digest("place-moved-g" + std::to_string(generation) + "-" +
        std::to_string(state.source_primary));
}

production::RuntimeSourceState destroyed_state(std::uint64_t node_id) {
    production::RuntimeSourceState state;
    state.source_kind = production::PreparedSourceEntityKind::scene_node;
    state.source_primary = node_id;
    state.present = false;
    return state;
}

production::RuntimeWorldSnapshot make_snapshot(
    std::uint64_t live_revision,
    std::vector<production::RuntimeSourceState> states) {
    production::RuntimeWorldSnapshot snapshot;
    snapshot.source_namespace = "torture.world";
    snapshot.live_revision = core::Revision{live_revision};
    std::sort(states.begin(), states.end(),
        [](const auto& left, const auto& right) {
            return std::tuple{static_cast<unsigned>(left.source_kind), left.source_primary,
                left.source_secondary} <
                std::tuple{static_cast<unsigned>(right.source_kind), right.source_primary,
                    right.source_secondary};
        });
    snapshot.states = std::move(states);
    require(static_cast<bool>(snapshot.validate()), "fixture snapshot validates");
    return snapshot;
}

production::RuntimeSourceState* find_state(
    std::vector<production::RuntimeSourceState>& states, std::uint64_t node_id) {
    const auto found = std::find_if(states.begin(), states.end(),
        [node_id](const production::RuntimeSourceState& state) {
            return state.source_kind == production::PreparedSourceEntityKind::scene_node &&
                state.source_primary == node_id;
        });
    return found == states.end() ? nullptr : &*found;
}

const production::ReimportConflict* find_conflict(
    const production::ReimportPlan& plan,
    std::uint64_t node_id) {
    const auto found = std::find_if(plan.conflicts.begin(), plan.conflicts.end(),
        [node_id](const production::ReimportConflict& conflict) {
            return conflict.entity.source_kind ==
                    production::PreparedSourceEntityKind::scene_node &&
                conflict.entity.source_primary == node_id;
        });
    return found == plan.conflicts.end() ? nullptr : &*found;
}

bool plan_touches(const production::ReimportPlan& plan, std::uint64_t node_id) {
    const auto matches = [node_id](const production::PreparedEntityRef& ref) {
        return ref.source_kind == production::PreparedSourceEntityKind::scene_node &&
            ref.source_primary == node_id;
    };
    return std::any_of(plan.creates.begin(), plan.creates.end(),
            [&](const auto& entry) { return matches(entry.entity); }) ||
        std::any_of(plan.updates.begin(), plan.updates.end(),
            [&](const auto& entry) { return matches(entry.entity); }) ||
        std::any_of(plan.deletes.begin(), plan.deletes.end(),
            [&](const auto& entry) { return matches(entry.entity); }) ||
        std::any_of(plan.conflicts.begin(), plan.conflicts.end(),
            [&](const auto& conflict) { return matches(conflict.entity); });
}

const production::PreparedEntityChange* find_change(
    const std::vector<production::PreparedEntityChange>& changes,
    std::uint64_t node_id) {
    const auto found = std::find_if(changes.begin(), changes.end(),
        [node_id](const production::PreparedEntityChange& change) {
            return change.entity.source_kind ==
                    production::PreparedSourceEntityKind::scene_node &&
                change.entity.source_primary == node_id;
        });
    return found == changes.end() ? nullptr : &*found;
}

void authored_delta_classifies_all_change_kinds() {
    auto base = make_rich_envelope(1U, {make_node(1U, "camp"), make_node(2U, "tent")});
    auto incoming = make_rich_envelope(2U, {make_node(1U, "camp", std::nullopt, 4.0),
        make_node(3U, "watchtower")});
    incoming.assets.front().channel_mask |= production::prepared_tangents;
    incoming.material_bindings.front().material = 72U;

    const auto delta = production::diff_prepared_revisions(base, incoming);
    require(static_cast<bool>(delta), "rich revisions diff");
    require(delta.value().added.size() == 1U && delta.value().removed.size() == 1U &&
            delta.value().changed.size() == 3U,
        "delta classifies one add, one removal, and three changes");
    const auto* added_node = find_change(delta.value().added, 3U);
    require(added_node != nullptr &&
            added_node->entity.prepared_kind == production::PreparedEntityKind::node,
        "added watchtower is keyed by stable source identity");
    const auto* removed_node = find_change(delta.value().removed, 2U);
    require(removed_node != nullptr, "removed tent is keyed by stable source identity");
    const auto* moved_node = find_change(delta.value().changed, 1U);
    require(moved_node != nullptr &&
            (moved_node->aspects & production::changed_node_transform) != 0U &&
            (moved_node->aspects & production::changed_node_name) == 0U,
        "authored move touches exactly the transform aspect");
    const auto channel_change = std::find_if(delta.value().changed.begin(), delta.value().changed.end(),
        [](const production::PreparedEntityChange& change) {
            return change.entity.source_kind ==
                production::PreparedSourceEntityKind::mesh_asset;
        });
    require(channel_change != delta.value().changed.end() &&
            (channel_change->aspects & production::changed_asset_channels) != 0U,
        "asset channel drift is classified by aspect");
    const auto binding_change = std::find_if(
        delta.value().changed.begin(), delta.value().changed.end(),
        [](const production::PreparedEntityChange& change) {
            return change.entity.source_kind ==
                production::PreparedSourceEntityKind::material_region;
        });
    require(binding_change != delta.value().changed.end() &&
            (binding_change->aspects & production::changed_binding_material) != 0U,
        "material binding drift is classified by aspect");

    const std::string encoded = delta.value().serialize();
    const auto reopened = production::PreparedRevisionDelta::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "prepared delta survives deterministic round trip");
    const auto first_digest = delta.value().canonical_digest();
    const auto second_digest = reopened.value().canonical_digest();
    require(first_digest && second_digest && first_digest.value() == second_digest.value(),
        "prepared delta identity survives reopen");
    const auto rediff = production::diff_prepared_revisions(base, incoming);
    require(rediff && rediff.value().serialize() == encoded,
        "recomputed delta is byte-identical");

    auto foreign = make_envelope(9U, {make_node(1U, "camp")});
    foreign.source_namespace = "elsewhere.world";
    require(!production::diff_prepared_revisions(base, foreign),
        "diffing revisions from different source namespaces fails closed");
}

void no_op_reimport_preserves_game_owned_state() {
    const auto base = make_envelope(3U, {make_node(1U, "camp"), make_node(2U, "tent", 1U),
        make_node(3U, "crate"), make_node(4U, "lantern"), make_node(5U, "chest")});
    const auto incoming = base;

    std::vector<production::RuntimeSourceState> states;
    states.push_back(intact_state(1U, std::nullopt, 3U));
    auto moved = intact_state(2U, 1U, 3U);
    move_state(moved, 3U);
    states.push_back(moved);
    auto damaged = intact_state(3U, std::nullopt, 3U);
    damaged.condition = production::RuntimeConditionKind::damaged;
    damaged.state_digest = digest("state-damaged-3");
    states.push_back(damaged);
    states.push_back(destroyed_state(4U));
    auto looted = intact_state(5U, std::nullopt, 3U);
    looted.condition = production::RuntimeConditionKind::looted;
    looted.state_digest = digest("state-looted-5");
    states.push_back(looted);
    const auto live = make_snapshot(7U, std::move(states));

    const auto plan = production::plan_reimport(base, incoming, live);
    require(static_cast<bool>(plan), "no-op reimport reconciles");
    require(plan.value().creates.empty() && plan.value().updates.empty() && plan.value().conflicts.empty(),
        "game-owned drift without authored change stages no carto action");
    require(plan.value().deletes.size() == 1U &&
            plan.value().deletes.front().entity.source_primary == 4U &&
            plan.value().deletes.front().ownership == production::PropertyOwnership::game_owned,
        "the destroyed lantern stays dead through a staged game-owned deletion");

    const auto ownership = production::classify_property_ownership(base, incoming, live);
    require(static_cast<bool>(ownership), "ownership classification runs");
    const auto lantern = std::find_if(ownership.value().begin(), ownership.value().end(),
        [](const production::PropertyOwnershipRecord& record) {
            return record.source_primary == 4U;
        });
    const auto tent = std::find_if(ownership.value().begin(), ownership.value().end(),
        [](const production::PropertyOwnershipRecord& record) {
            return record.source_primary == 2U;
        });
    const auto camp = std::find_if(ownership.value().begin(), ownership.value().end(),
        [](const production::PropertyOwnershipRecord& record) {
            return record.source_primary == 1U;
        });
    require(lantern != ownership.value().end() &&
            lantern->ownership == production::PropertyOwnership::game_owned &&
            tent != ownership.value().end() &&
            tent->ownership == production::PropertyOwnership::game_owned &&
            camp != ownership.value().end() &&
            camp->ownership == production::PropertyOwnership::carto_owned,
        "drifted entities are game-owned; untouched entities stay carto-owned");

    const std::string encoded = plan.value().serialize();
    const auto reopened = production::ReimportPlan::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "reimport plan survives deterministic round trip");

    const auto receipt = production::record_reimport_receipt(plan.value(), {});
    require(receipt && receipt.value().decisions.empty(),
        "a conflict-free plan records an empty decision set");
    const std::string receipt_text = receipt.value().serialize();
    const auto receipt_reopened = production::ReimportReceipt::deserialize(receipt_text);
    require(receipt_reopened && receipt_reopened.value().serialize() == receipt_text,
        "reimport receipt survives deterministic round trip");
}

void three_way_conflicts_bind_all_legs() {
    const auto base = make_envelope(4U, {make_node(1U, "camp"), make_node(2U, "tent", 1U),
        make_node(3U, "crate")});
    auto incoming = make_envelope(5U, {make_node(1U, "camp"), make_node(3U, "crate", std::nullopt, 9.0)});

    std::vector<production::RuntimeSourceState> states;
    states.push_back(destroyed_state(1U));
    auto moved = intact_state(2U, 1U, 4U);
    move_state(moved, 4U);
    states.push_back(moved);
    auto damaged = intact_state(3U, std::nullopt, 4U);
    damaged.condition = production::RuntimeConditionKind::damaged;
    damaged.state_digest = digest("state-damaged-3");
    states.push_back(damaged);
    const auto live = make_snapshot(5U, std::move(states));

    const auto plan = production::plan_reimport(base, incoming, live);
    require(static_cast<bool>(plan), "three-way reconcile runs");
    require(plan.value().deletes.size() == 1U && plan.value().deletes.front().entity.source_primary == 1U &&
            plan.value().deletes.front().ownership == production::PropertyOwnership::game_owned,
        "the runtime-destroyed camp is staged dead, not resurrected");
    require(plan.value().conflicts.size() == 2U, "two authored changes hit runtime drift");

    const auto* tent = find_conflict(plan.value(), 2U);
    require(tent != nullptr && tent->kind == production::ReimportConflictKind::moved &&
            !tent->base_record_digest.is_zero() && tent->incoming_record_digest.is_zero() &&
            !tent->live_state_digest.is_zero(),
        "authored removal of a runtime-moved tent binds all three legs");
    const auto* crate = find_conflict(plan.value(), 3U);
    require(crate != nullptr && crate->kind == production::ReimportConflictKind::damaged &&
            !crate->base_record_digest.is_zero() &&
            !crate->incoming_record_digest.is_zero() &&
            !crate->live_state_digest.is_zero(),
        "authored change to a damaged crate binds all three legs");

    const std::vector<production::ReimportOperatorChoice> choices{
        {production::PreparedSourceEntityKind::scene_node, 2U, 0U,
            production::ReimportOperatorDecision::override_decision, "operator.a"},
        {production::PreparedSourceEntityKind::scene_node, 3U, 0U,
            production::ReimportOperatorDecision::skip, "operator.a"},
    };
    const auto receipt = production::record_reimport_receipt(plan.value(), choices);
    require(receipt && receipt.value().decisions.size() == 2U,
        "every conflict receives exactly one recorded decision");
    require(receipt.value().decisions.front().base_record_digest == tent->base_record_digest &&
            receipt.value().decisions.front().incoming_record_digest ==
                tent->incoming_record_digest &&
            receipt.value().decisions.front().live_state_digest == tent->live_state_digest,
        "decisions inherit the exact three leg digests of their conflict");
    const std::string encoded = receipt.value().serialize();
    const auto reopened = production::ReimportReceipt::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "decision receipt survives deterministic round trip");

    require(!production::record_reimport_receipt(plan.value(),
                std::vector<production::ReimportOperatorChoice>{}),
        "missing decisions fail closed");
    std::vector<production::ReimportOperatorChoice> extra = choices;
    extra.push_back(choices.front());
    require(!production::record_reimport_receipt(plan.value(), extra),
        "duplicate choices fail closed");
    std::vector<production::ReimportOperatorChoice> unknown = choices;
    unknown.back().source_primary = 99U;
    require(!production::record_reimport_receipt(plan.value(), unknown),
        "choices that do not match the plan conflicts fail closed");
}

void region_scoped_filtering_keeps_global_entities() {
    const auto base = make_envelope(6U, {make_node(1U, "camp"), make_node(2U, "tent"),
        make_node(3U, "crate"), make_node(4U, "far-tower")});
    auto incoming = make_envelope(7U, {make_node(1U, "camp", std::nullopt, 1.0),
        make_node(2U, "tent", 1U, 2.0), make_node(3U, "crate", std::nullopt, 3.0),
        make_node(4U, "far-tower", std::nullopt, 40.0)});

    std::vector<production::RuntimeSourceState> states;
    for (const auto& node : base.nodes) {
        auto state = intact_state(node.id, node.parent, 6U);
        if (node.id == 1U || node.id == 2U) {
            state.footprint = production::SpatialTileRange{"world-grid", 2U,
                0, 0, 0, 3, 3, 3};
        }
        if (node.id == 4U) {
            state.footprint = production::SpatialTileRange{"world-grid", 2U,
                100, 100, 0, 103, 103, 3};
        }
        // Node 3 deliberately carries no footprint: it is global.
        states.push_back(state);
    }
    const auto live = make_snapshot(7U, std::move(states));

    const auto delta = production::diff_prepared_revisions(base, incoming);
    require(delta && delta.value().changed.size() == 4U, "every authored move enters the delta");
    const production::SpatialTileRange dirty{"world-grid", 2U, 0, 0, 0, 7, 7, 3};
    const auto filtered =
        production::filter_delta_to_region(delta.value(), live, dirty);
    require(filtered && filtered.value().changed.size() == 3U,
        "region filtering drops the disjoint far-tower change");
    require(find_change(filtered.value().changed, 1U) != nullptr &&
            find_change(filtered.value().changed, 2U) != nullptr &&
            find_change(filtered.value().changed, 3U) != nullptr &&
            find_change(filtered.value().changed, 4U) == nullptr,
        "intersecting and global entities stay dirty");
    require(filtered.value().base_digest == delta.value().base_digest &&
            filtered.value().incoming_digest == delta.value().incoming_digest,
        "filtered delta keeps its revision binding");

    const production::SpatialTileRange other_level{"world-grid", 3U, 0, 0, 0, 7, 7, 3};
    require(!production::filter_delta_to_region(delta.value(), live, other_level),
        "incompatible grid levels fail closed instead of assuming non-overlap");

    production::RuntimeWorldSnapshot other_namespace = live;
    other_namespace.source_namespace = "elsewhere.world";
    require(!production::filter_delta_to_region(delta.value(), other_namespace, dirty),
        "region filtering across namespaces fails closed");
}

// The long-save torture chain: cook -> admit -> runtime mutations (destroy,
// move) -> recook -> reconcile, repeated across five generations. Destroyed
// entities stay dead and runtime-moved entities keep their placement no
// matter how often authoring changes around them.
void long_save_torture_chain() {
    std::vector<production::RuntimeSourceState> live;
    live.push_back(intact_state(1U, std::nullopt, 0U));
    live.push_back(intact_state(2U, 1U, 0U));
    live.push_back(intact_state(3U, std::nullopt, 0U));
    live.push_back(intact_state(4U, std::nullopt, 0U));

    auto admitted = make_envelope(10U, {make_node(1U, "camp"), make_node(2U, "tent", 1U),
        make_node(3U, "crate"), make_node(4U, "lantern")});

    const auto campfire_moved_digest = digest("place-moved-g1-1");

    // Generation 1: authored renames the camp, moves the tent, removes the
    // lantern, and adds a watchtower. The runtime destroys the crate, moves
    // the camp, and damages the tent.
    {
        auto recooked = make_envelope(11U, {make_node(1U, "camp-fire"), make_node(2U, "tent", 1U, 8.0),
            make_node(3U, "crate"), make_node(5U, "watchtower")});
        *find_state(live, 3U) = destroyed_state(3U);
        move_state(*find_state(live, 1U), 1U);
        auto* tent_state = find_state(live, 2U);
        tent_state->condition = production::RuntimeConditionKind::damaged;
        tent_state->state_digest = digest("state-damaged-g1-2");
        const auto snapshot = make_snapshot(1U, live);

        const auto plan = production::plan_reimport(admitted, recooked, snapshot);
        require(static_cast<bool>(plan), "generation 1 reconciles");
        const auto* camp = find_conflict(plan.value(), 1U);
        require(plan.value().deletes.size() == 2U &&
                plan.value().deletes.front().entity.source_primary == 3U &&
                plan.value().deletes.front().ownership == production::PropertyOwnership::game_owned &&
                plan.value().deletes.back().entity.source_primary == 4U &&
                plan.value().deletes.back().ownership == production::PropertyOwnership::carto_owned &&
                camp != nullptr && camp->kind == production::ReimportConflictKind::moved &&
                plan.value().creates.size() == 1U &&
                plan.value().creates.front().entity.source_primary == 5U,
            "generation 1: destroyed crate stays dead, removed lantern deletes, moved camp conflicts");
        const auto* tent = find_conflict(plan.value(), 2U);
        require(tent != nullptr && tent->kind == production::ReimportConflictKind::damaged,
            "generation 1: damaged tent conflicts with the authored move");

        const std::vector<production::ReimportOperatorChoice> choices{
            {production::PreparedSourceEntityKind::scene_node, 1U, 0U,
                production::ReimportOperatorDecision::override_decision, "operator.a"},
            {production::PreparedSourceEntityKind::scene_node, 2U, 0U,
                production::ReimportOperatorDecision::override_decision, "operator.a"},
        };
        const auto receipt = production::record_reimport_receipt(plan.value(), choices);
        require(receipt && static_cast<bool>(receipt.value().validate()),
            "generation 1 receipt binds the operator decisions");
        admitted = std::move(recooked);
        live.push_back(intact_state(5U, std::nullopt, 1U));
        // Admission: the authored lantern removal lands, so the runtime drops
        // it; the destroyed crate keeps its tombstone.
        live.erase(std::remove_if(live.begin(), live.end(),
            [](const production::RuntimeSourceState& state) {
                return state.source_primary == 4U;
            }),
            live.end());
    }

    // Generation 2: authored moves the watchtower and adds a depot. The
    // runtime moves the tent again and loots the watchtower.
    {
        auto recooked = make_envelope(12U, {make_node(1U, "camp-fire"),
            make_node(2U, "tent", 1U, 8.0), make_node(3U, "crate"),
            make_node(5U, "watchtower", std::nullopt, 20.0), make_node(6U, "depot")});
        move_state(*find_state(live, 2U), 2U);
        auto* watchtower = find_state(live, 5U);
        watchtower->condition = production::RuntimeConditionKind::looted;
        watchtower->state_digest = digest("state-looted-g2-5");
        const auto snapshot = make_snapshot(2U, live);

        const auto plan = production::plan_reimport(admitted, recooked, snapshot);
        require(static_cast<bool>(plan), "generation 2 reconciles");
        const auto* watchtower_conflict = find_conflict(plan.value(), 5U);
        require(watchtower_conflict != nullptr &&
                watchtower_conflict->kind == production::ReimportConflictKind::looted &&
                plan.value().creates.size() == 1U &&
                plan.value().creates.front().entity.source_primary == 6U &&
                !plan_touches(plan.value(), 2U),
            "generation 2: looted watchtower conflicts, depot creates, moved tent rests");
        require(find_state(live, 2U)->placement_digest ==
                digest("place-moved-g2-2"),
            "generation 2 records the tent's second runtime placement");

        const std::vector<production::ReimportOperatorChoice> choices{
            {production::PreparedSourceEntityKind::scene_node, 5U, 0U,
                production::ReimportOperatorDecision::override_decision, "operator.a"},
        };
        require(static_cast<bool>(
                production::record_reimport_receipt(plan.value(), choices)),
            "generation 2 receipt records the looted-watchtower override");
        admitted = std::move(recooked);
        live.push_back(intact_state(6U, std::nullopt, 2U));
    }

    // Generation 3: authored moves the long-dead crate. The runtime destroys
    // the depot and moves the watchtower.
    {
        auto recooked = make_envelope(13U, {make_node(1U, "camp-fire"),
            make_node(2U, "tent", 1U, 8.0), make_node(3U, "crate", std::nullopt, 2.0),
            make_node(5U, "watchtower", std::nullopt, 20.0), make_node(6U, "depot")});
        *find_state(live, 6U) = destroyed_state(6U);
        move_state(*find_state(live, 5U), 3U);
        const auto snapshot = make_snapshot(3U, live);
        require(find_state(live, 1U)->placement_digest == campfire_moved_digest,
            "generation 3 still sees the generation-1 campfire placement");

        const auto plan = production::plan_reimport(admitted, recooked, snapshot);
        require(static_cast<bool>(plan), "generation 3 reconciles");
        const auto* crate = find_conflict(plan.value(), 3U);
        require(crate != nullptr && crate->kind == production::ReimportConflictKind::destroyed &&
                crate->live_state_digest.is_zero(),
            "generation 3: authored re-add of the destroyed crate is a destroyed conflict");
        require(plan.value().deletes.size() == 1U &&
                plan.value().deletes.front().entity.source_primary == 6U &&
                plan.value().deletes.front().ownership == production::PropertyOwnership::game_owned,
            "generation 3: the depot tombstone stays dead while the crate forces a conflict");

        const std::vector<production::ReimportOperatorChoice> choices{
            {production::PreparedSourceEntityKind::scene_node, 3U, 0U,
                production::ReimportOperatorDecision::override_decision, "operator.a"},
        };
        require(static_cast<bool>(
                production::record_reimport_receipt(plan.value(), choices)),
            "generation 3 receipt records the never-resurrect override");
        admitted = std::move(recooked);
    }

    // Generation 4: authored moves the camp, removes the watchtower, and adds
    // a pier. The runtime damages-and-loots the tent.
    {
        auto recooked = make_envelope(14U, {make_node(1U, "camp-fire", std::nullopt, 6.0),
            make_node(2U, "tent", 1U, 8.0), make_node(3U, "crate", std::nullopt, 2.0),
            make_node(7U, "pier")});
        auto* tent = find_state(live, 2U);
        tent->condition = production::RuntimeConditionKind::damaged_and_looted;
        tent->state_digest = digest("state-worn-g4-2");
        const auto snapshot = make_snapshot(4U, live);

        const auto plan = production::plan_reimport(admitted, recooked, snapshot);
        require(static_cast<bool>(plan), "generation 4 reconciles");
        const auto* camp = find_conflict(plan.value(), 1U);
        const auto* watchtower = find_conflict(plan.value(), 5U);
        require(camp != nullptr && camp->kind == production::ReimportConflictKind::moved &&
                watchtower != nullptr &&
                watchtower->kind == production::ReimportConflictKind::moved,
            "generation 4: authored camp move and watchtower removal hit runtime drift");
        require(plan.value().creates.size() == 1U &&
                plan.value().creates.front().entity.source_primary == 7U,
            "generation 4: the pier is staged as a create");

        // The operator admits both: the camp adopts the authored placement
        // and the watchtower is deleted. The tent's worn state stays untouched.
        const std::vector<production::ReimportOperatorChoice> choices{
            {production::PreparedSourceEntityKind::scene_node, 1U, 0U,
                production::ReimportOperatorDecision::admit, "operator.b"},
            {production::PreparedSourceEntityKind::scene_node, 5U, 0U,
                production::ReimportOperatorDecision::admit, "operator.b"},
        };
        const auto receipt = production::record_reimport_receipt(plan.value(), choices);
        require(receipt && receipt.value().decisions.size() == 2U &&
                receipt.value().decisions.front().decision ==
                    production::ReimportOperatorDecision::admit,
            "generation 4 receipt records the two admissions");
        admitted = std::move(recooked);

        // Admission: the camp's runtime placement adopts the authored move;
        // the watchtower leaves the live world, and the depot tombstone is
        // dropped now that authoring and runtime agree it is gone.
        auto* camp_state = find_state(live, 1U);
        camp_state->placement_digest = digest("place-admitted-g4-1");
        camp_state->admitted_placement_digest = camp_state->placement_digest;
        live.erase(std::remove_if(live.begin(), live.end(),
            [](const production::RuntimeSourceState& state) {
                return state.source_primary == 5U || state.source_primary == 6U;
            }),
            live.end());
        live.push_back(intact_state(7U, std::nullopt, 4U));
    }

    // Generation 5: authored renames the pier. The tent is damaged again.
    // Nothing else moves: the chain converges to a quiet world.
    {
        auto recooked = make_envelope(15U, {make_node(1U, "camp-fire", std::nullopt, 6.0),
            make_node(2U, "tent", 1U, 8.0), make_node(3U, "crate", std::nullopt, 2.0),
            make_node(7U, "long-pier")});
        auto* tent = find_state(live, 2U);
        tent->condition = production::RuntimeConditionKind::damaged;
        tent->state_digest = digest("state-damaged-g5-2");
        const auto snapshot = make_snapshot(5U, live);

        const auto plan = production::plan_reimport(admitted, recooked, snapshot);
        require(static_cast<bool>(plan), "generation 5 reconciles");
        require(plan.value().conflicts.empty() && plan.value().creates.empty() &&
                plan.value().deletes.size() == 1U &&
                plan.value().deletes.front().entity.source_primary == 3U &&
                plan.value().deletes.front().ownership == production::PropertyOwnership::game_owned &&
                plan.value().updates.size() == 1U &&
                plan.value().updates.front().entity.source_primary == 7U &&
                (plan.value().updates.front().aspects & production::changed_node_name) != 0U,
            "generation 5 converges to a quiet authored update and the standing crate tombstone");
        require(!plan_touches(plan.value(), 2U),
            "generation 5 leaves the worn tent's game-owned state alone");
    }

    // Chain-wide invariants.
    const auto* camp_state = find_state(live, 1U);
    require(camp_state != nullptr &&
            camp_state->placement_digest == digest("place-admitted-g4-1"),
        "the campfire never reverted to its pre-admission placement");
    require(find_state(live, 2U) != nullptr &&
            find_state(live, 2U)->placement_digest == digest("place-moved-g2-2"),
        "the tent's runtime placement survived every later generation");
    require(find_state(live, 3U) == nullptr || !find_state(live, 3U)->present,
        "the crate never came back to life");
    require(find_state(live, 4U) == nullptr || !find_state(live, 4U)->present,
        "the lantern never came back to life");
    require(find_state(live, 5U) == nullptr || !find_state(live, 5U)->present,
        "the watchtower never came back to life");
    require(campfire_moved_digest != digest("place-admitted-g4-1"),
        "the torture chain really moved the campfire");
}

void hostile_inputs_fail_closed() {
    const auto base = make_envelope(20U, {make_node(1U, "camp"), make_node(2U, "tent", 1U)});
    const auto incoming = make_envelope(21U, {make_node(1U, "camp"), make_node(2U, "tent", 1U, 3.0)});
    std::vector<production::RuntimeSourceState> states;
    states.push_back(intact_state(1U, std::nullopt, 20U));
    states.push_back(intact_state(2U, 1U, 20U));
    const auto live = make_snapshot(21U, std::move(states));
    const auto delta = production::diff_prepared_revisions(base, incoming);
    const auto plan = production::plan_reimport(base, incoming, live);
    require(delta && plan, "hostile-input fixtures reconcile");

    // Snapshot: future version, oversized counts, trailing data, duplicates.
    const std::string snapshot_text = live.serialize();
    std::string future_snapshot = snapshot_text;
    future_snapshot.replace(0U, std::string{"CARTOGRAPHER_RUNTIME_SNAPSHOT 1"}.size(),
        "CARTOGRAPHER_RUNTIME_SNAPSHOT 99");
    require(!production::RuntimeWorldSnapshot::deserialize(future_snapshot),
        "future snapshot versions fail precisely");
    require(!production::RuntimeWorldSnapshot::deserialize(snapshot_text + "trailing"),
        "snapshot parser rejects trailing data");
    require(!production::RuntimeWorldSnapshot::deserialize(
                "CARTOGRAPHER_RUNTIME_SNAPSHOT 1\n"
                "SOURCE \"torture.world\" 1\n"
                "STATES 65537\n"),
        "hostile snapshot counts are rejected before allocation");
    auto duplicate_states = live;
    duplicate_states.states.push_back(duplicate_states.states.front());
    require(!duplicate_states.validate(),
        "duplicate runtime identities fail closed");
    auto bad_tombstone = live;
    bad_tombstone.states.front().present = false;
    require(!bad_tombstone.validate(),
        "a destroyed state must be a canonical tombstone");

    // Delta: future version, oversized counts, trailing data, aspect misuse.
    const std::string delta_text = delta.value().serialize();
    std::string future_delta = delta_text;
    future_delta.replace(0U, std::string{"CARTOGRAPHER_PREPARED_DELTA 1"}.size(),
        "CARTOGRAPHER_PREPARED_DELTA 99");
    require(!production::PreparedRevisionDelta::deserialize(future_delta),
        "future delta versions fail precisely");
    require(!production::PreparedRevisionDelta::deserialize(delta_text + "trailing"),
        "delta parser rejects trailing data");
    require(!production::PreparedRevisionDelta::deserialize(
                "CARTOGRAPHER_PREPARED_DELTA 1\n"
                "SOURCE \"torture.world\" 1 2 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "ADDED 65537\n"),
        "hostile delta counts are rejected before allocation");
    require(!production::PreparedRevisionDelta::deserialize(
                "CARTOGRAPHER_PREPARED_DELTA 1\n"
                "SOURCE \"torture.world\" 1 2 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "ADDED 1\nADD 1 5 0 1 5 0 3\nREMOVED 0\nCHANGED 0\nEND\n"),
        "added delta records may not carry aspects");
    require(!production::PreparedRevisionDelta::deserialize(
                "CARTOGRAPHER_PREPARED_DELTA 1\n"
                "SOURCE \"torture.world\" 1 2 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "ADDED 0\nREMOVED 0\nCHANGED 1\nCHANGE 1 2 0 1 2 0 0\nEND\n"),
        "changed delta records require a non-zero aspect mask");

    // Plan: future version, oversized counts, trailing data, staged conflicts.
    const std::string plan_text = plan.value().serialize();
    std::string future_plan = plan_text;
    future_plan.replace(0U, std::string{"CARTOGRAPHER_REIMPORT_PLAN 1"}.size(),
        "CARTOGRAPHER_REIMPORT_PLAN 99");
    require(!production::ReimportPlan::deserialize(future_plan),
        "future plan versions fail precisely");
    require(!production::ReimportPlan::deserialize(plan_text + "trailing"),
        "plan parser rejects trailing data");
    require(!production::ReimportPlan::deserialize(
                plan_text + "EXTRA 1\n"),
        "plan parser rejects rows past the terminal record");
    auto conflicting_plan = plan.value();
    production::ReimportConflict duplicate;
    duplicate.kind = production::ReimportConflictKind::moved;
    duplicate.entity = conflicting_plan.updates.front().entity;
    duplicate.base_record_digest = digest("leg-a");
    duplicate.incoming_record_digest = digest("leg-b");
    duplicate.live_state_digest = digest("leg-c");
    duplicate.detail = "staged twice";
    conflicting_plan.conflicts.push_back(std::move(duplicate));
    require(!conflicting_plan.validate(),
        "one identity cannot stage two actions");

    // Receipt: future version, tampered digest, trailing data.
    const auto receipt = production::record_reimport_receipt(plan.value(), {});
    const std::string receipt_text = receipt.value().serialize();
    std::string future_receipt = receipt_text;
    future_receipt.replace(0U, std::string{"CARTOGRAPHER_REIMPORT_RECEIPT 1"}.size(),
        "CARTOGRAPHER_REIMPORT_RECEIPT 99");
    require(!production::ReimportReceipt::deserialize(future_receipt),
        "future receipt versions fail precisely");
    require(!production::ReimportReceipt::deserialize(receipt_text + "trailing"),
        "receipt parser rejects trailing data");
    std::string tampered = receipt_text;
    const std::string original_digest_line = "DIGEST " + receipt.value().receipt_digest.hex();
    const std::string forged_digest_line =
        "DIGEST 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    tampered.replace(tampered.find(original_digest_line), original_digest_line.size(),
        forged_digest_line);
    require(!production::ReimportReceipt::deserialize(tampered),
        "a tampered receipt digest fails validation");
}

} // namespace

int main() {
    authored_delta_classifies_all_change_kinds();
    no_op_reimport_preserves_game_owned_state();
    three_way_conflicts_bind_all_legs();
    region_scoped_filtering_keeps_global_entities();
    long_save_torture_chain();
    hostile_inputs_fail_closed();
    std::cout << "reimport tests passed\n";
    return EXIT_SUCCESS;
}
