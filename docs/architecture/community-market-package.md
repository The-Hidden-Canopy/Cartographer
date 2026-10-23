# Community Market package boundary

Cartographer's public market surface is a provider-neutral package contract. It
does not contain checkout, payment, seller payout, tax, entitlement, catalog
authority, private runtime state, or network access.

## Public package identity

`manifest.json` describes an immutable listing version and declares entrypoints,
target artifacts, dependencies, inventory, source revision, and a provenance
receipt path. `content_digest` covers the canonical, sorted payload inventory
and payload bytes. The validator also computes a domain-separated package
identity digest over the canonical manifest; the manifest's content digest is
part of that identity and binds the canonical payload inventory. When a target
artifact declares an `artifact_digest`, directory and archive validation
recompute that file or target-directory digest and reject mismatches.

The package identity digest is the value that a publication receipt, project
lockfile, or caller-supplied install expectation must pin. A payload digest by
itself is not an authenticity claim: a service that publishes packages must
bind the identity digest to its own signed publication evidence before making a
remote listing authoritative. `MarketPublicationReceipt` is the public
provider-neutral evidence shape: it signs the package identity together with
the package/listing/publisher identity, validator version, observation time,
key id, and algorithm. Cartographer accepts only a public `KeyRing`; the
private signing key remains in the publisher or Hub boundary. Verification
returns an explicit `verified`, `unknown_key`, `invalid`, or
`algorithm_unsupported` verdict rather than treating a hash as provenance.

## Data-only install

The public market installer accepts asset and authoring data only. Executable
extensions are rejected even when a caller asks for a permissive validation
mode. Plugin execution is a separate `carto_plugin_package` concern requiring
its own manifest, capability, signature, trust, and install policy.

Directory installation follows this sequence:

1. validate manifest, paths, payload limits, and content digest;
2. compute the package identity digest;
3. copy into a private staging directory;
4. validate the staged copy again and compare its identity digest;
5. publish the staged directory atomically at its immutable package/version
   path.

Each successful local install also writes a small atomic sidecar receipt under
the library's private `receipts/<package_id>/<listing_version_id>.json`
directory. Inspection requires that receipt to match the installed package's
identity digest and the deterministic dependency-resolution plan digest. A
package with declared dependencies cannot be installed without a matching
resolution plan; a dependency-free package receives a deterministic singleton
plan. The sidecar is local install evidence only; it is not a Hub entitlement,
a payment record, or a signature from the publisher. Callers that have
publication evidence can use the signed install overload; it verifies the
receipt before staging, re-verifies the staged package, atomically publishes
the package, and stores a digest-bound publication sidecar. Inspection and
library enumeration fail closed if that sidecar is missing or tampered.

An already occupied immutable path is accepted only when its package identity
digest is identical. A different digest is rejected.

## Compatibility evidence

Compatibility receipts bind package digest, engine, profile, validator version,
evidence digest, observation time, and an explicit expiry. Only fresh
`VERIFIED_PASS` and `VERIFIED_WARN` receipts apply to a target. Failed,
untested, seller-declared, and expired states remain visible evidence but do not
silently authorize compatibility.

Projects can ask the local library to verify a `ProjectMarketLockfile` against
the cached package/version/listing/package digest and any explicitly pinned
target digests. The check is read-only and does not infer entitlement; a
missing package or pin mismatch fails closed, so an installed newer version
cannot silently satisfy an older project lock.

## Service boundary

The Hub or another private service may own catalog authority, orders,
entitlements, payment adapters, payout, moderation, and organization-scoped
policy. It must pass an exact package identity and entitlement decision into a
Cartographer integration boundary. The public package library does not infer
ownership from a download, call a payment provider, or grant runtime authority.

The local library listing is derived from installed package directories, but it
revalidates every package and its receipt before returning an entry. Missing,
symlinked, malformed, or digest-mismatched entries fail the listing instead of
being silently omitted or presented as installed state.

## Native UI projection

The optional native desktop shell exposes the local library as a Market surface
and command-palette refresh action. It can install either a validated package
directory (by selecting its `manifest.json`) or a bounded archive. The surface
shows only locally revalidated identity, payload, license, publisher, source,
digest, and publication-receipt evidence. It does not display remote listings,
entitlements, payment state, executable capability, or private runtime state.
An invalid package or receipt clears the displayed package list and leaves the
surface in an explicit error state until refresh succeeds.

Downstream consumers read reviewed target artifacts through their own runtime and admission
boundaries. Cartographer does not import private downstream runtime code into this
package contract.
