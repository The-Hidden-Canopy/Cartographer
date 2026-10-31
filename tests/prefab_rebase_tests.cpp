#include <carto/condition/authoring.hpp>
#include <carto/condition/prefab_rebase.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

#define REQUIRE(condition) \
    do { \
        if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

carto::assets::Sha256Digest digest_text(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

carto::condition::AuthoringDocument make_authoring() {
    auto fixture = carto::condition::make_condition_fixture();
    REQUIRE(fixture);
    auto authoring = std::move(fixture.value());
    authoring.project_revision = carto::core::Revision{1U};
    return authoring;
}

carto::condition::SourceAsset duplicate_asset(
    const carto::condition::SourceAsset& source,
    carto::condition::Id id,
    std::uint64_t object_id,
    std::string name) {
    auto asset = source;
    asset.id = id;
    asset.stable_name = std::move(name);
    asset.authored_object_id = object_id;
    return asset;
}

carto::condition::PrefabInstanceMember member_for(
    const carto::condition::SourceAsset& asset,
    std::uint64_t instance_object_id,
    std::uint64_t mesh_asset_id) {
    carto::condition::PrefabInstanceMember member;
    member.asset_id = asset.id;
    member.source_object_id = asset.authored_object_id;
    member.instance_object_id = instance_object_id;
    member.mesh_asset_id = mesh_asset_id;
    member.mesh_revision = carto::core::Revision{1U};
    member.source_revision = asset.source_revision;
    member.source_digest = asset.source_digest;
    return member;
}

void membership_changes_are_explicit_and_snapshot_bound() {
    using namespace carto;
    auto authoring = make_authoring();
    REQUIRE(!authoring.prefabs.empty() && !authoring.assets.empty());
    const condition::Id retained_asset_id = authoring.prefabs.front().asset_ids.front();
    const auto retained_position = std::lower_bound(authoring.assets.begin(),
        authoring.assets.end(), retained_asset_id, [](const auto& asset, condition::Id id) {
            return asset.id < id;
        });
    REQUIRE(retained_position != authoring.assets.end() && retained_position->id == retained_asset_id);
    const auto retained_asset = *retained_position;
    const condition::Id next_asset_id = authoring.assets.back().id + 1U;
    auto added_asset = duplicate_asset(retained_asset, next_asset_id,
        retained_asset.authored_object_id + 100U, "rebase-added-member");
    auto removed_asset = duplicate_asset(retained_asset, next_asset_id + 1U,
        retained_asset.authored_object_id + 101U, "rebase-removed-member");
    authoring.assets.push_back(added_asset);
    authoring.assets.push_back(removed_asset);

    condition::PrefabInstanceRecord instance;
    instance.id = 900U;
    instance.prefab_id = authoring.prefabs.front().id;
    instance.name = "Review fixture";
    instance.root_object_id = 901U;
    instance.captured_project_revision = core::Revision{1U};
    instance.source_authoring_revision = core::Revision{1U};
    instance.prefab_source_revision = authoring.prefabs.front().source_revision;
    instance.members.push_back(member_for(retained_asset, 902U, 1001U));
    instance.members.push_back(member_for(removed_asset, 903U, 1002U));
    authoring.prefab_instances.push_back(instance);

    auto& prefab = authoring.prefabs.front();
    prefab.asset_ids.push_back(added_asset.id);
    std::sort(prefab.asset_ids.begin(), prefab.asset_ids.end());
    prefab.source_revision = core::Revision{2U};
    authoring.project_revision = core::Revision{2U};
    REQUIRE(authoring.validate());
    const auto before = authoring.serialize();

    const auto review = condition::PrefabRebaseReview::between(authoring, instance.id);
    REQUIRE(review && review.value().has_changes());
    REQUIRE(review.value().project_mesh_review_required);
    REQUIRE(review.value().instance_prefab_revision != review.value().current_prefab_revision);
    REQUIRE(review.value().changes.size() == 2U);
    REQUIRE(review.value().changes[0U].kind == condition::PrefabRebaseChangeKind::member_added);
    REQUIRE(review.value().changes[0U].asset_id == added_asset.id);
    REQUIRE(review.value().changes[1U].kind == condition::PrefabRebaseChangeKind::member_removed);
    REQUIRE(review.value().changes[1U].asset_id == removed_asset.id);
    REQUIRE(review.value().validate(authoring));
    const auto all_reviews = condition::PrefabRebaseReview::all(authoring);
    REQUIRE(all_reviews && all_reviews.value().size() == 1U);
    REQUIRE(all_reviews.value().front() == review.value());
    REQUIRE(authoring.serialize() == before);

    auto forged = review.value();
    forged.changes.front().asset_id = removed_asset.id;
    REQUIRE(!forged.validate(authoring));
    auto changed = authoring;
    REQUIRE(changed.invalidate_source(retained_asset.source_id,
        "review-source-next", digest_text("changed source")));
    REQUIRE(!review.value().validate(changed));
}

void stale_source_evidence_is_reported_without_approving_rebase() {
    using namespace carto;
    auto authoring = make_authoring();
    const condition::Id asset_id = authoring.prefabs.front().asset_ids.front();
    const auto asset = std::lower_bound(authoring.assets.begin(), authoring.assets.end(), asset_id,
        [](const auto& value, condition::Id target) { return value.id < target; });
    REQUIRE(asset != authoring.assets.end() && asset->id == asset_id);
    condition::PrefabInstanceRecord instance;
    instance.id = 910U;
    instance.prefab_id = authoring.prefabs.front().id;
    instance.name = "Stale source fixture";
    instance.root_object_id = 911U;
    instance.captured_project_revision = authoring.project_revision;
    instance.source_authoring_revision = authoring.project_revision;
    instance.prefab_source_revision = authoring.prefabs.front().source_revision;
    instance.members.push_back(member_for(*asset, 912U, 1011U));
    authoring.prefab_instances.push_back(instance);
    REQUIRE(authoring.validate());
    const condition::Id source_id = asset->source_id;

    REQUIRE(authoring.invalidate_source(source_id,
        "invalidated", digest_text("invalidated source")));
    const auto review = condition::PrefabRebaseReview::between(authoring, instance.id);
    REQUIRE(review && review.value().has_changes());
    REQUIRE(std::any_of(review.value().changes.begin(), review.value().changes.end(),
        [](const auto& change) {
            return change.kind == condition::PrefabRebaseChangeKind::source_evidence_stale;
        }));
    REQUIRE(review.value().validate(authoring));
    // A review explains current metadata drift; it cannot mark a source current
    // or authorize project mutation.
    const auto stale_source = std::find_if(authoring.sources.begin(), authoring.sources.end(),
        [source_id](const auto& source) { return source.id == source_id; });
    REQUIRE(stale_source != authoring.sources.end() && !stale_source->current);
}

} // namespace

int main() {
    try {
        membership_changes_are_explicit_and_snapshot_bound();
        stale_source_evidence_is_reported_without_approving_rebase();
        std::cout << "Prefab rebase review tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Prefab rebase review test failed: " << error.what() << '\n';
        return 1;
    }
}
