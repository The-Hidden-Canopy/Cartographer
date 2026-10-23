#include <carto/semantic/schema.hpp>
#include <carto/semantic/sidecar.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
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

carto::semantic::SchemaField field(
    std::string name,
    carto::semantic::SchemaValueKind kind,
    carto::semantic::FieldOwnership ownership) {
    carto::semantic::SchemaField result;
    result.name = std::move(name);
    result.value_kind = kind;
    result.ownership = ownership;
    return result;
}

carto::semantic::SchemaRevision make_revision(
    std::string name_space,
    std::string name,
    std::uint32_t version,
    std::vector<carto::semantic::SchemaField> fields) {
    carto::semantic::SchemaRevision revision;
    revision.schema = carto::semantic::SchemaIdentity{
        std::move(name_space), std::move(name)};
    revision.version = carto::semantic::SchemaVersion{version};
    std::sort(fields.begin(), fields.end(),
        [](const carto::semantic::SchemaField& left,
           const carto::semantic::SchemaField& right) {
            return left.name < right.name;
        });
    revision.fields = std::move(fields);
    require(static_cast<bool>(revision.refresh_digest()),
        "schema revision digest refreshes");
    return revision;
}

void versions_order_and_revisions_bind_declarations() {
    using namespace carto::semantic;
    require(SchemaVersion{1U} < SchemaVersion{2U}, "schema versions order");
    require(SchemaVersion{3U} == SchemaVersion{3U}, "schema versions compare equal");
    require(SchemaVersion{4U} > SchemaVersion{2U}, "schema versions order descending");

    const auto first = make_revision(
        "condition.example", "shelter", 1U,
        {field("state", SchemaValueKind::string, FieldOwnership::game_owned),
         field("flammability", SchemaValueKind::real, FieldOwnership::native_lowerable)});
    const auto reordered = make_revision(
        "condition.example", "shelter", 1U,
        {field("flammability", SchemaValueKind::real, FieldOwnership::native_lowerable),
         field("state", SchemaValueKind::string, FieldOwnership::game_owned)});
    const auto first_digest = first.canonical_digest();
    const auto reordered_digest = reordered.canonical_digest();
    require(first_digest && reordered_digest &&
                first_digest.value() == reordered_digest.value(),
        "field authoring order does not alter the schema revision digest");

    const auto reowned = make_revision(
        "condition.example", "shelter", 1U,
        {field("flammability", SchemaValueKind::real, FieldOwnership::native_lowerable),
         field("state", SchemaValueKind::string, FieldOwnership::native_lowerable)});
    const auto reowned_digest = reowned.canonical_digest();
    require(first_digest && reowned_digest &&
                first_digest.value() != reowned_digest.value(),
        "field ownership changes the schema revision digest");

    const auto reopened = SchemaRevision::deserialize(first.serialize());
    require(reopened && reopened.value().serialize() == first.serialize(),
        "schema revision survives deterministic round trip");
    auto drifting = first;
    require(drifting.fields.back().name == "state",
        "sorted fixture exposes the game-owned state field last");
    drifting.fields.back().ownership = FieldOwnership::native_lowerable;
    require(!drifting.validate(),
        "drifting from the declared digest fails closed");
}

void registry_admission_and_fail_closed_lookup() {
    using namespace carto::semantic;
    SchemaRegistry registry;
    const auto first = make_revision(
        "condition.example", "shelter", 1U,
        {field("state", SchemaValueKind::string, FieldOwnership::game_owned),
         field("flammability", SchemaValueKind::real, FieldOwnership::native_lowerable)});
    const auto second = make_revision(
        "condition.example", "shelter", 2U,
        {field("state", SchemaValueKind::string, FieldOwnership::game_owned),
         field("flammability", SchemaValueKind::real, FieldOwnership::native_lowerable),
         field("wear", SchemaValueKind::real, FieldOwnership::game_owned)});

    require(static_cast<bool>(registry.register_schema(first)),
        "first revision registers");
    require(static_cast<bool>(registry.register_schema(second)),
        "second revision registers");
    require(static_cast<bool>(registry.register_schema(first)),
        "identical re-registration is idempotent");
    auto conflicting = first;
    conflicting.fields.back().value_kind = SchemaValueKind::integer;
    require(static_cast<bool>(conflicting.refresh_digest()),
        "conflicting revision refreshes its digest");
    require(!registry.register_schema(conflicting),
        "conflicting digest for a registered version is rejected");

    const auto found = registry.find(first.schema, SchemaVersion{1U});
    require(found && found.value() != nullptr &&
                found.value()->revision_digest == first.revision_digest,
        "registered revision resolves by exact version");
    const auto latest = registry.latest_registered(first.schema);
    require(latest && latest.value() == SchemaVersion{2U},
        "latest registered version resolves");
    require(registry.contains(first.schema, SchemaVersion{2U}) &&
                !registry.contains(first.schema, SchemaVersion{9U}),
        "membership checks fail closed for unknown versions");
    require(!registry.find(
                SchemaIdentity{"unknown.example", "shelter"}, SchemaVersion{1U}),
        "unknown namespace fails closed");
    require(!registry.latest_registered(SchemaIdentity{"unknown.example", "shelter"}),
        "latest lookup of an unknown namespace fails closed");
}

void reconciliation_and_compatibility_stay_fail_closed() {
    using namespace carto::semantic;
    SchemaRegistry registry;
    const auto v1 = make_revision(
        "condition.example", "shelter", 1U,
        {field("state", SchemaValueKind::string, FieldOwnership::game_owned),
         field("flammability", SchemaValueKind::real, FieldOwnership::native_lowerable)});
    const auto v2 = make_revision(
        "condition.example", "shelter", 2U,
        {field("state", SchemaValueKind::string, FieldOwnership::game_owned),
         field("flammability", SchemaValueKind::real, FieldOwnership::native_lowerable),
         field("wear", SchemaValueKind::real, FieldOwnership::game_owned)});
    const auto v3_drops_game_owned = make_revision(
        "condition.example", "shelter", 3U,
        {field("state", SchemaValueKind::string, FieldOwnership::game_owned),
         field("flammability", SchemaValueKind::real, FieldOwnership::native_lowerable)});
    const auto v4_reowns = make_revision(
        "condition.example", "shelter", 4U,
        {field("state", SchemaValueKind::string, FieldOwnership::native_lowerable),
         field("flammability", SchemaValueKind::real, FieldOwnership::native_lowerable)});
    require(static_cast<bool>(registry.register_schema(v1)) &&
                static_cast<bool>(registry.register_schema(v2)) &&
                static_cast<bool>(registry.register_schema(v3_drops_game_owned)) &&
                static_cast<bool>(registry.register_schema(v4_reowns)),
        "compatibility fixture revisions register");

    SemanticExtensionRecord record;
    record.schema = v1.schema;
    record.schema_version = 1U;
    record.payload = "{\"state\":\"intact\"}";
    record.payload_digest = [] {
        const std::string payload = "{\"state\":\"intact\"}";
        return carto::assets::sha256(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()});
    }();
    require(static_cast<bool>(registry.reconcile(record)),
        "record reconciles against its registered revision");
    record.schema_version = 9U;
    require(!registry.reconcile(record),
        "reconciliation fails closed for an unregistered version");
    record.schema = SchemaIdentity{"unknown.example", "shelter"};
    record.schema_version = 1U;
    require(!registry.reconcile(record),
        "reconciliation fails closed for an unknown namespace");

    require(static_cast<bool>(registry.compatible(
                v1.schema, SchemaVersion{1U}, SchemaVersion{2U})),
        "adding fields keeps revisions compatible");
    require(!registry.compatible(
                v1.schema, SchemaVersion{2U}, SchemaVersion{3U}),
        "dropping a game-owned field breaks compatibility");
    require(!registry.compatible(
                v1.schema, SchemaVersion{1U}, SchemaVersion{4U}),
        "re-owning a game-owned field breaks compatibility");
    require(!registry.compatible(
                v1.schema, SchemaVersion{2U}, SchemaVersion{1U}),
        "backwards compatibility moves fail closed");
    require(!registry.compatible(
                SchemaIdentity{"unknown.example", "shelter"},
                SchemaVersion{1U}, SchemaVersion{2U}),
        "compatibility of an unknown namespace fails closed");
}

} // namespace

int main() {
    versions_order_and_revisions_bind_declarations();
    registry_admission_and_fail_closed_lookup();
    reconciliation_and_compatibility_stay_fail_closed();
    std::cout << "semantic schema tests passed\n";
    return EXIT_SUCCESS;
}
