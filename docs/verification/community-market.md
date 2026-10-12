# Community Market verification

The public market slice is accepted only for the bounded data-only contract.

The `cartographer_community_market_tests` suite covers:

- canonical directory and archive validation;
- payload and package-identity digest binding;
- declared target-artifact digest verification for directories and archives;
- metadata tamper detection when an expected package identity is pinned;
- executable payload rejection;
- archive traversal and size-limit overflow rejection;
- empty-path and malformed-input handling;
- archive entry-order independence;
- optional dependency resolution, lockfile round-trip, and cycle rejection;
- dependency-plan identity binding and rejection of dependency-bearing
  installs without a resolution plan;
- fail-closed installed-library enumeration and receipt revalidation;
- fresh compatibility evidence, expiry, status gating, and UTC timestamp rules;
- signed publication evidence over the immutable package identity, exact
  publication context, public-key verification verdicts, signed installation,
  and publication-sidecar tamper detection;
- read-only lockfile verification against cached immutable package/version and
  listing pins;
- staged-copy revalidation, atomic local install receipts, missing/tampered
  receipt rejection, and immutable install collision handling; and
- native UI projection of an empty, ready, and tampered local library,
  including package-directory/archive install routing and command-palette
  refresh availability; and
- absence of payment, secret, and provider-authority fields in the lockfile.

The suite does not claim:

- PayPal or other payment-provider behavior;
- organization-scoped entitlement or seller authorization;
- executable plugin admission or sandboxing;
- remote catalog availability, CDN delivery, or payout reconciliation; or
- VANTA runtime admission or presented-frame performance.

Those concerns remain service or plugin boundaries and require separate
evidence before promotion.
