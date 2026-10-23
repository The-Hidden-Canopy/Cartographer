#include <carto/assets/asset_lock.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace carto::assets {
namespace {

constexpr std::size_t kMaxIdentityBytes = 128U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic conflict(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool valid_identity(const std::string& value) noexcept {
    return !value.empty() && value.size() <= kMaxIdentityBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= 0x21U && character <= 0x7eU;
        });
}

core::Result<void> validate_lease_request(
    const std::string& owner_id,
    AssetLockUtcSeconds now_utc_seconds,
    std::uint32_t duration_seconds) {
    if (!valid_identity(owner_id)) {
        return core::Result<void>::failure(invalid(
            "asset lock owner identity is invalid"));
    }
    if (duration_seconds == 0U ||
        duration_seconds > AssetLockRegistry::kMaxLeaseDurationSeconds) {
        return core::Result<void>::failure(invalid(
            "asset lock duration is outside the allowed range"));
    }
    if (now_utc_seconds > std::numeric_limits<AssetLockUtcSeconds>::max() -
            static_cast<AssetLockUtcSeconds>(duration_seconds)) {
        return core::Result<void>::failure(invalid(
            "asset lock expiry timestamp overflows"));
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> AssetLockScope::validate() const {
    if (!valid_identity(organization_id) || !valid_identity(project_id) ||
        !valid_identity(asset_id)) {
        return core::Result<void>::failure(invalid(
            "asset lock scope requires bounded organization, project, and asset identities"));
    }
    return core::Result<void>::success();
}

bool AssetLockScope::operator<(const AssetLockScope& other) const noexcept {
    if (organization_id != other.organization_id) {
        return organization_id < other.organization_id;
    }
    if (project_id != other.project_id) {
        return project_id < other.project_id;
    }
    return asset_id < other.asset_id;
}

core::Result<AssetLockAcquisition> AssetLockRegistry::acquire(
    AssetLockScope scope,
    std::string owner_id,
    AssetLockUtcSeconds now_utc_seconds,
    std::uint32_t duration_seconds,
    std::optional<std::uint64_t> acknowledge_expired_lease_id) {
    const auto scope_valid = scope.validate();
    if (!scope_valid) {
        return core::Result<AssetLockAcquisition>::failure(scope_valid.error());
    }
    const auto request_valid = validate_lease_request(
        owner_id, now_utc_seconds, duration_seconds);
    if (!request_valid) {
        return core::Result<AssetLockAcquisition>::failure(request_valid.error());
    }

    std::scoped_lock lock(mutex_);
    const auto existing = leases_.find(scope);
    std::optional<AssetLockLease> displaced;
    if (existing != leases_.end()) {
        if (now_utc_seconds < existing->second.expires_at_utc_seconds) {
            return core::Result<AssetLockAcquisition>::failure(conflict(
                "asset already has an active collaboration lease"));
        }
        if (!acknowledge_expired_lease_id.has_value() ||
            *acknowledge_expired_lease_id != existing->second.lease_id) {
            return core::Result<AssetLockAcquisition>::failure(conflict(
                "expired asset lease requires acknowledgement of its exact lease ID"));
        }
        displaced = existing->second;
    } else if (acknowledge_expired_lease_id.has_value()) {
        return core::Result<AssetLockAcquisition>::failure(conflict(
            "acknowledged expired asset lease is no longer current"));
    }

    if (next_lease_id_ == 0U) {
        return core::Result<AssetLockAcquisition>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "asset lock lease ID space is exhausted"));
    }
    if (existing == leases_.end() && leases_.size() >= kMaxRecords) {
        return core::Result<AssetLockAcquisition>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "asset lock registry record limit is reached"));
    }

    const auto lease_id = next_lease_id_;
    AssetLockLease lease{
        std::move(scope),
        std::move(owner_id),
        lease_id,
        now_utc_seconds,
        now_utc_seconds + static_cast<AssetLockUtcSeconds>(duration_seconds),
    };
    AssetLockAcquisition result{lease, displaced};
    if (existing == leases_.end()) {
        leases_.emplace(lease.scope, lease);
    } else {
        existing->second = std::move(lease);
    }
    ++next_lease_id_;
    return core::Result<AssetLockAcquisition>::success(std::move(result));
}

core::Result<AssetLockLease> AssetLockRegistry::renew(
    const AssetLockScope& scope,
    const std::string& owner_id,
    std::uint64_t lease_id,
    AssetLockUtcSeconds now_utc_seconds,
    std::uint32_t duration_seconds) {
    const auto scope_valid = scope.validate();
    if (!scope_valid) {
        return core::Result<AssetLockLease>::failure(scope_valid.error());
    }
    const auto request_valid = validate_lease_request(
        owner_id, now_utc_seconds, duration_seconds);
    if (!request_valid) {
        return core::Result<AssetLockLease>::failure(request_valid.error());
    }

    std::scoped_lock lock(mutex_);
    const auto existing = leases_.find(scope);
    if (existing == leases_.end()) {
        return core::Result<AssetLockLease>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "asset has no collaboration lease"));
    }
    if (existing->second.lease_id != lease_id ||
        existing->second.owner_id != owner_id) {
        return core::Result<AssetLockLease>::failure(conflict(
            "asset lease owner or ID changed before renewal"));
    }
    if (now_utc_seconds >= existing->second.expires_at_utc_seconds) {
        return core::Result<AssetLockLease>::failure(conflict(
            "expired asset lease cannot be renewed"));
    }
    if (now_utc_seconds < existing->second.acquired_at_utc_seconds) {
        return core::Result<AssetLockLease>::failure(conflict(
            "asset lease renewal timestamp predates acquisition"));
    }

    auto renewed = existing->second;
    renewed.expires_at_utc_seconds =
        now_utc_seconds + static_cast<AssetLockUtcSeconds>(duration_seconds);
    auto returned = renewed;
    existing->second = std::move(renewed);
    return core::Result<AssetLockLease>::success(std::move(returned));
}

core::Result<void> AssetLockRegistry::release(
    const AssetLockScope& scope,
    const std::string& owner_id,
    std::uint64_t lease_id) {
    const auto scope_valid = scope.validate();
    if (!scope_valid) {
        return core::Result<void>::failure(scope_valid.error());
    }
    if (!valid_identity(owner_id)) {
        return core::Result<void>::failure(invalid(
            "asset lock owner identity is invalid"));
    }

    std::scoped_lock lock(mutex_);
    const auto existing = leases_.find(scope);
    if (existing == leases_.end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "asset has no collaboration lease"));
    }
    if (existing->second.lease_id != lease_id ||
        existing->second.owner_id != owner_id) {
        return core::Result<void>::failure(conflict(
            "asset lease owner or ID changed before release"));
    }
    leases_.erase(existing);
    return core::Result<void>::success();
}

core::Result<std::optional<AssetLockSnapshot>> AssetLockRegistry::inspect(
    const AssetLockScope& scope,
    AssetLockUtcSeconds now_utc_seconds) const {
    const auto scope_valid = scope.validate();
    if (!scope_valid) {
        return core::Result<std::optional<AssetLockSnapshot>>::failure(
            scope_valid.error());
    }

    std::scoped_lock lock(mutex_);
    const auto existing = leases_.find(scope);
    if (existing == leases_.end()) {
        return core::Result<std::optional<AssetLockSnapshot>>::success(std::nullopt);
    }
    const auto state = now_utc_seconds < existing->second.expires_at_utc_seconds
        ? AssetLockState::active
        : AssetLockState::expired;
    return core::Result<std::optional<AssetLockSnapshot>>::success(
        AssetLockSnapshot{state, existing->second});
}

std::size_t AssetLockRegistry::size() const {
    std::scoped_lock lock(mutex_);
    return leases_.size();
}

} // namespace carto::assets
