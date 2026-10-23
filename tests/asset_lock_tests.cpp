#include <carto/assets/asset_lock.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace carto::assets;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

AssetLockScope scope(std::string organization = "org-a") {
    return AssetLockScope{std::move(organization), "project-a", "asset-a"};
}

void organization_and_project_are_isolated_and_active_conflicts_fail_closed() {
    AssetLockRegistry registry;
    const auto first = registry.acquire(scope(), "user-1", 100, 60);
    const auto other_org = registry.acquire(scope("org-b"), "user-2", 100, 60);
    const auto other_project = registry.acquire(
        AssetLockScope{"org-a", "project-b", "asset-a"}, "user-2", 100, 60);
    require(first && other_org && other_project,
        "same asset identity in distinct org/project scopes must not collide");

    const auto competing = registry.acquire(scope(), "user-2", 101, 30);
    require(!competing && competing.error().code == carto::core::ErrorCode::stale_data,
        "a competing active owner must receive a conflict");
    const auto current = registry.inspect(scope(), 101);
    require(current && current.value().has_value() &&
            current.value()->state == AssetLockState::active &&
            current.value()->lease.owner_id == "user-1",
        "conflict must preserve the current lease for inspection");
    require(registry.size() == 3U,
        "failed conflict must not create or replace a lease");
}

void expiry_requires_exact_acknowledgement_and_keeps_displacement_receipt() {
    AssetLockRegistry registry;
    const auto original = registry.acquire(scope(), "user-old", 100, 10);
    require(static_cast<bool>(original), "initial lease should be acquired");
    const auto expired = registry.inspect(scope(), 110);
    require(expired && expired.value().has_value() &&
            expired.value()->state == AssetLockState::expired,
        "lease expires at the exact expiry second");

    const auto silent_takeover = registry.acquire(scope(), "user-new", 110, 10);
    require(!silent_takeover,
        "expired ownership must not be silently replaced");
    const auto wrong_ack = registry.acquire(scope(), "user-new", 110, 10,
        original.value().lease.lease_id + 1U);
    require(!wrong_ack,
        "an acknowledgement for a different lease must not take over the asset");

    const auto takeover = registry.acquire(scope(), "user-new", 110, 10,
        original.value().lease.lease_id);
    require(takeover && takeover.value().displaced_expired_lease.has_value() &&
            *takeover.value().displaced_expired_lease == original.value().lease,
        "acknowledged takeover must return the exact displaced-owner receipt");
    require(!registry.release(scope(), "user-old", original.value().lease.lease_id),
        "stale owner must not release the replacement lease");
    require(!registry.renew(scope(), "user-old", original.value().lease.lease_id,
                111, 10),
        "stale owner must not renew the replacement lease");
}

void renew_release_and_bounds_are_checked() {
    AssetLockRegistry registry;
    const auto initial = registry.acquire(scope(), "user-1", 1000, 30);
    require(static_cast<bool>(initial), "initial lease should be acquired");
    require(!registry.renew(scope(), "user-2", initial.value().lease.lease_id,
                1010, 30),
        "wrong owner must not renew a lease");
    const auto renewed = registry.renew(scope(), "user-1",
        initial.value().lease.lease_id, 1010, 45);
    require(renewed && renewed.value().expires_at_utc_seconds == 1055,
        "valid owner may extend an active lease within the duration cap");
    require(!registry.renew(scope(), "user-1", renewed.value().lease_id,
                1055, 1),
        "an expired lease must not be renewed");
    require(!registry.renew(scope(), "user-1", renewed.value().lease_id,
                999, 1),
        "a timestamp rollback before acquisition must not renew a lease");
    require(!registry.acquire(scope("org-overflow"), "user-2",
                std::numeric_limits<AssetLockUtcSeconds>::max(), 1),
        "timestamp overflow must be rejected");
    require(!registry.acquire(scope(), "user-2", 2000, 0),
        "zero-duration lease must be rejected");
    require(!registry.acquire(scope(), "user-2", 2000,
                AssetLockRegistry::kMaxLeaseDurationSeconds + 1U),
        "overlong lease must be rejected");
    require(!registry.release(scope(), "user-2", renewed.value().lease_id),
        "wrong owner must not release a lease");
    require(static_cast<bool>(registry.release(
                scope(), "user-1", renewed.value().lease_id)),
        "matching owner and lease ID may release the lease");
    require(registry.size() == 0U,
        "release must remove only the matching coordination record");
}

void invalid_scope_and_owner_are_rejected() {
    AssetLockRegistry registry;
    require(!registry.acquire(AssetLockScope{"", "project", "asset"},
                "user", 1, 1),
        "missing organization scope must be rejected");
    require(!registry.acquire(scope(), "user with spaces", 1, 1),
        "malformed owner identity must be rejected");
    require(!registry.inspect(AssetLockScope{"org", "project", ""}, 1),
        "missing asset scope must be rejected");
    require(registry.size() == 0U,
        "invalid requests must not mutate lock state");
}

void concurrent_acquisition_has_one_winner() {
    AssetLockRegistry registry;
    std::atomic<unsigned> acquired{0U};
    std::array<std::thread, 12U> contenders;
    for (std::size_t index = 0U; index < contenders.size(); ++index) {
        contenders[index] = std::thread([&registry, &acquired, index] {
            const auto result = registry.acquire(
                scope(), "user-" + std::to_string(index), 500, 60);
            if (result) {
                acquired.fetch_add(1U, std::memory_order_relaxed);
            }
        });
    }
    for (auto& contender : contenders) contender.join();
    require(acquired.load(std::memory_order_relaxed) == 1U && registry.size() == 1U,
        "concurrent conflicting acquisitions must have exactly one winner");
}

} // namespace

int main() {
    organization_and_project_are_isolated_and_active_conflicts_fail_closed();
    expiry_requires_exact_acknowledgement_and_keeps_displacement_receipt();
    renew_release_and_bounds_are_checked();
    invalid_scope_and_owner_are_rejected();
    concurrent_acquisition_has_one_winner();
    std::cout << "asset lock checks passed\n";
    return EXIT_SUCCESS;
}
