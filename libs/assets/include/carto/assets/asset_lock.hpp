#pragma once

#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace carto::assets {

using AssetLockUtcSeconds = std::int64_t;

struct AssetLockScope final {
    std::string organization_id;
    std::string project_id;
    std::string asset_id;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const AssetLockScope&) const noexcept = default;
    [[nodiscard]] bool operator<(const AssetLockScope& other) const noexcept;
};

struct AssetLockLease final {
    AssetLockScope scope;
    std::string owner_id;
    std::uint64_t lease_id = 0U;
    AssetLockUtcSeconds acquired_at_utc_seconds = 0;
    AssetLockUtcSeconds expires_at_utc_seconds = 0;

    [[nodiscard]] bool operator==(const AssetLockLease&) const noexcept = default;
};

enum class AssetLockState : std::uint8_t {
    active = 0U,
    expired = 1U,
};

struct AssetLockSnapshot final {
    AssetLockState state = AssetLockState::active;
    AssetLockLease lease;
};

struct AssetLockAcquisition final {
    AssetLockLease lease;
    // Present only when the caller explicitly acknowledged the exact expired
    // lease ID. This preserves a receipt of the displaced owner.
    std::optional<AssetLockLease> displaced_expired_lease;
};

// This process-local registry provides collaboration conflict signals only.
// It is neither durable nor distributed, and a lease never grants write
// authority; callers must perform their normal permission and project checks.
class AssetLockRegistry final {
public:
    static constexpr std::size_t kMaxRecords = 65'536U;
    static constexpr std::uint32_t kMaxLeaseDurationSeconds = 900U;

    [[nodiscard]] core::Result<AssetLockAcquisition> acquire(
        AssetLockScope scope,
        std::string owner_id,
        AssetLockUtcSeconds now_utc_seconds,
        std::uint32_t duration_seconds,
        std::optional<std::uint64_t> acknowledge_expired_lease_id = std::nullopt);

    [[nodiscard]] core::Result<AssetLockLease> renew(
        const AssetLockScope& scope,
        const std::string& owner_id,
        std::uint64_t lease_id,
        AssetLockUtcSeconds now_utc_seconds,
        std::uint32_t duration_seconds);

    [[nodiscard]] core::Result<void> release(
        const AssetLockScope& scope,
        const std::string& owner_id,
        std::uint64_t lease_id);

    [[nodiscard]] core::Result<std::optional<AssetLockSnapshot>> inspect(
        const AssetLockScope& scope,
        AssetLockUtcSeconds now_utc_seconds) const;

    [[nodiscard]] std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::map<AssetLockScope, AssetLockLease> leases_;
    std::uint64_t next_lease_id_ = 1U;
};

} // namespace carto::assets
