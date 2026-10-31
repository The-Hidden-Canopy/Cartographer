# Advisory asset lease clock rollback

The process-local `AssetLockRegistry` retains the latest successful timestamp
observation for each lease. Acquire, renew, and inspect refuse a timestamp before
that watermark. Successful inspection advances only this local anti-rollback
metadata, including an expired observation. A later earlier timestamp cannot
resurrect that lease or renew it with an earlier expiry.

Rejected requests leave the watermark and public lease receipts unchanged.
Owner/lease-bound release removes the record; a new lease begins a new watermark.
Separate organization/project/asset scopes do not share a clock watermark.

## Evidence (2026-10-23)

The new renewal-after-renewal regression failed against the previous code, then
`cartographer_asset_lock_tests` rebuilt and passed CTest (1/1). Cases include
inspection/renewal rollback, expired observation, acknowledged takeover,
unchanged displacement receipt, failed-future-request preservation, independent
scopes, and release/reacquisition. `git diff --check` passed.

## Repair ledger

- Files: `libs/assets/include/carto/assets/asset_lock.hpp`,
  `libs/assets/src/asset_lock.cpp`, `tests/asset_lock_tests.cpp`, this note.
- Capability: a retained advisory lease cannot rewind an already observed
  timeline through renewal, inspection, or takeover.
- Invariants: scope separation, bounded leases, exact displacement
  acknowledgement, owner/lease identity, concurrency serialization, and no
  project mutation or write authority.
- Not run: authenticated multi-client/disconnect/deployment acceptance. No
  trusted clock, identity source, or shared coordinator is connected here.
- Risks: caller-supplied time remains untrusted. Successful inspection now
  advances private process-local clock metadata; receipts remain unchanged.
- Rollback: revert the lease change; there is no persisted schema or migration.
- Rejected: granting command authority from a lease, claiming a trusted clock,
  or treating a local mutex as coordination between separate clients.
