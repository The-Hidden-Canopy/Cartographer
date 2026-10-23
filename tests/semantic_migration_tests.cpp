#include <carto/semantic/migration.hpp>
#include <carto/semantic/schema.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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
    std::uint32_t version,
    std::vector<carto::semantic::SchemaField> fields) {
    carto::semantic::SchemaRevision revision;
    revision.schema = carto::semantic::SchemaIdentity{
        "condition.example", "shelter"};
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

carto::semantic::MigrationRule make_rule(
    std::uint32_t from,
    std::uint32_t to,
    std::vector<carto::semantic::FieldRename> renames,
    std::vector<std::string> preserved) {
    carto::semantic::MigrationRule rule;
    rule.schema = carto::semantic::SchemaIdentity{"condition.example", "shelter"};
    rule.from_version = carto::semantic::SchemaVersion{from};
    rule.to_version = carto::semantic::SchemaVersion{to};
    std::sort(renames.begin(), renames.end(),
        [](const carto::semantic::FieldRename& left,
           const carto::semantic::FieldRename& right) {
            return left.from < right.from;
        });
    std::sort(preserved.begin(), preserved.end());
    rule.renames = std::move(renames);
    rule.preserved_game_owned_fields = std::move(preserved);
    return rule;
}

struct MigrationFixture {
    carto::semantic::SchemaRegistry registry;
    carto::semantic::MigrationRule rule_1_to_2;
    carto::semantic::MigrationRule rule_2_to_3;
};

MigrationFixture make_fixture() {
    using namespace carto::semantic;
    MigrationFixture fixture;
    require(static_cast<bool>(fixture.registry.register_schema(make_revision(
                1U,
                {field("state", SchemaValueKind::string, FieldOwnership::game_owned),
                 field("flammability", SchemaValueKind::real,
                       FieldOwnership::native_lowerable)}))),
        "revision v1 registers");
    require(static_cast<bool>(fixture.registry.register_schema(make_revision(
                2U,
                {field("state", SchemaValueKind::string, FieldOwnership::game_owned),
                 field("flammability", SchemaValueKind::real,
                       FieldOwnership::native_lowerable),
                 field("wear", SchemaValueKind::real, FieldOwnership::game_owned)}))),
        "revision v2 registers");
    require(static_cast<bool>(fixture.registry.register_schema(make_revision(
                3U,
                {field("state", SchemaValueKind::string, FieldOwnership::game_owned),
                 field("fuel_load", SchemaValueKind::real,
                       FieldOwnership::native_lowerable),
                 field("wear", SchemaValueKind::real, FieldOwnership::game_owned)}))),
        "revision v3 registers");

    fixture.rule_1_to_2 = make_rule(
        1U, 2U, {}, {"state"});
    fixture.rule_2_to_3 = make_rule(
        2U, 3U, {{"flammability", "fuel_load"}}, {"state", "wear"});
    require(static_cast<bool>(fixture.rule_1_to_2.validate()) &&
                static_cast<bool>(fixture.rule_2_to_3.validate()),
        "fixture migration rules validate");
    return fixture;
}

void plans_compose_across_registered_rules() {
    using namespace carto::semantic;
    const auto fixture = make_fixture();
    const std::vector<MigrationRule> rules{
        fixture.rule_1_to_2, fixture.rule_2_to_3};

    const auto plan = MigrationPlan::compose(
        fixture.registry, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{1U}, SchemaVersion{3U}, rules);
    require(static_cast<bool>(plan), "plan composes across two registered rules");
    require(plan.value().rules().size() == 2U &&
                plan.value().from_version() == SchemaVersion{1U} &&
                plan.value().to_version() == SchemaVersion{3U},
        "composed plan covers the exact version chain");

    const auto reopened = MigrationPlan::deserialize(plan.value().serialize());
    require(reopened &&
                reopened.value().serialize() == plan.value().serialize(),
        "migration plan survives deterministic round trip");
    const auto single_rule = MigrationRule::deserialize(
        fixture.rule_2_to_3.serialize());
    require(single_rule &&
                single_rule.value().serialize() == fixture.rule_2_to_3.serialize(),
        "migration rule survives deterministic round trip");

    const auto identity = MigrationPlan::compose(
        fixture.registry, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{2U}, SchemaVersion{2U}, rules);
    require(static_cast<bool>(identity) && identity.value().rules().empty(),
        "identity migration composes to an empty plan");

    const auto partial = MigrationPlan::compose(
        fixture.registry, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{1U}, SchemaVersion{2U}, rules);
    require(static_cast<bool>(partial) &&
                partial.value().rules().size() == 1U,
        "single-hop migration composes one rule");
}

void unsupported_jumps_and_cycles_fail_closed() {
    using namespace carto::semantic;
    const auto fixture = make_fixture();
    const std::vector<MigrationRule> only_first{fixture.rule_1_to_2};

    const auto gap = MigrationPlan::compose(
        fixture.registry, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{1U}, SchemaVersion{3U}, only_first);
    require(!gap && gap.error().code == carto::core::ErrorCode::not_found,
        "a version jump with no covering rule fails closed as unsupported");

    const std::vector<MigrationRule> cycled{
        fixture.rule_1_to_2, make_rule(2U, 1U, {}, {"state", "wear"})};
    const auto looping = MigrationPlan::compose(
        fixture.registry, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{1U}, SchemaVersion{3U}, cycled);
    require(!looping && looping.error().code == carto::core::ErrorCode::cycle_detected,
        "a rule chain that revisits a version fails closed as a cycle");

    const auto backwards = MigrationPlan::compose(
        fixture.registry, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{3U}, SchemaVersion{1U},
        std::vector<MigrationRule>{fixture.rule_2_to_3, fixture.rule_1_to_2});
    require(!backwards, "backwards migration fails closed");

    SchemaRegistry sparse;
    require(static_cast<bool>(sparse.register_schema(make_revision(
                1U,
                {field("state", SchemaValueKind::string, FieldOwnership::game_owned)}))),
        "sparse registry admits only v1");
    const auto unregistered_target = MigrationPlan::compose(
        sparse, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{1U}, SchemaVersion{2U}, std::vector<MigrationRule>{});
    require(!unregistered_target &&
                unregistered_target.error().code == carto::core::ErrorCode::not_found,
        "migration to an unregistered version fails closed");
}

void game_owned_fields_are_always_preserved() {
    using namespace carto::semantic;
    const auto fixture = make_fixture();
    const std::vector<MigrationRule> rules{
        fixture.rule_1_to_2, fixture.rule_2_to_3};

    const auto valid = MigrationPlan::compose(
        fixture.registry, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{1U}, SchemaVersion{3U}, rules);
    require(static_cast<bool>(valid),
        "preserving plan validates against the registry");

    const std::vector<MigrationRule> dropping{
        make_rule(1U, 2U, {}, {}), fixture.rule_2_to_3};
    const auto dropped = MigrationPlan::compose(
        fixture.registry, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{1U}, SchemaVersion{3U}, dropping);
    require(!dropped &&
                dropped.error().code == carto::core::ErrorCode::validation_failed,
        "a rule that drops a game-owned field fails plan validation");

    const std::vector<MigrationRule> renamed_away{
        make_rule(1U, 2U, {}, {"state"}),
        make_rule(2U, 3U, {}, {"state"})};
    const auto lost = MigrationPlan::compose(
        fixture.registry, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{1U}, SchemaVersion{3U}, renamed_away);
    require(!lost,
        "a later hop that omits an earlier game-owned field fails validation");

    const std::vector<MigrationRule> renaming_missing{
        make_rule(1U, 2U, {{"missing_field", "elsewhere"}}, {"state"}),
        fixture.rule_2_to_3};
    const auto missing = MigrationPlan::compose(
        fixture.registry, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{1U}, SchemaVersion{3U}, renaming_missing);
    require(!missing,
        "renaming a field absent from the source revision fails validation");

    const std::vector<MigrationRule> colliding_target{
        make_rule(1U, 2U, {{"flammability", "state"}}, {"state"}),
        fixture.rule_2_to_3};
    const auto collision = MigrationPlan::compose(
        fixture.registry, SchemaIdentity{"condition.example", "shelter"},
        SchemaVersion{1U}, SchemaVersion{3U}, colliding_target);
    require(!collision,
        "renaming onto an existing field fails validation");
}

} // namespace

int main() {
    plans_compose_across_registered_rules();
    unsupported_jumps_and_cycles_fail_closed();
    game_owned_fields_are_always_preserved();
    std::cout << "semantic migration tests passed\n";
    return EXIT_SUCCESS;
}
