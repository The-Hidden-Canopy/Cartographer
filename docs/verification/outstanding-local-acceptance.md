# Outstanding local authoring acceptance

This pass advances the source-browser thumbnail cache in the current dirty
checkout. The prefab placement and whole-snapshot Agent review remain outside
this repair's verified scope. Existing changes outside the named repair hunks
are preserved; this note does not claim build, test, or native acceptance.

## Thumbnail source boundary

The browser previously keyed thumbnails by project generation, mesh revision,
and shared object identity. An aliased `CompiledMesh` can still change in place
at the same revision, so pointer identity alone could display stale geometry.

`MeshThumbnailCacheEntry::projection_for` now binds the cached projection to
generation, exact source revision, weak object identity, and a SHA-256 digest
of the exact position/index bytes used by projection. Same-object mutations
rebuild the projection; revision-mismatched, invalid, or absent geometry
clears it. The existing 256-entry cache bound and bounded projection remain in
place. This cache creates no project mutation, source identity, write
authority, or fallback geometry.

Repair files:

- `apps/desktop/studio_browser.hpp`: cache admission and projection.
- `apps/desktop/main.cpp`: route thumbnail lookup through that admission.
- `tests/desktop_browser_tests.cpp`: replacement, same-object mutation,
  invalid geometry, revision mismatch, project change, and absent-source
  regressions.
- `tests/source_asset_registry_tests.cpp`: annotation compatibility with
  botanical generation receipts and exact-byte relinking; refuse unreviewed
  annotation changes and removal of referenced dependencies; preserve target
  lineage after explicit edge removal.
- This note, existing verification notes, and the ignored local queue:
  current evidence and remaining gates.

## Verification

Adversarial regression cases are authored for same-revision in-place geometry
mutation, invalidation on same-object topology corruption, and a mismatched
compiled-mesh revision. No build or tests were run during this sprint, as
requested; these cases and the existing browser tests remain unverified.

## Governance, risks, and rollback

The thumbnail repair is read-only derived presentation. Authoring still uses
existing human command admission, project history, and durable journals. The
registry regressions exercise pure review/adoption APIs; they introduce no
background mutation or AI acceptance path. Org scope, roles, and UTC datetime
are not part of this local projection contract.

The cache hashes bounded position/index inputs on lookup so same-pointer drift
cannot reuse stale projected coordinates. It also rechecks mesh validity and
source-revision agreement before reusing a result. The weak reference does not
retain mesh geometry. Hashing work and desktop frame cost are unmeasured until
the desktop build and populated-browser profiling are run.

Rollback only this pass's cache class, native lookup integration, and new
regression hunks. Preserve the earlier browser implementation and all other
dirty work. No serialized format, dependency, public C++ library API,
migration, or deployment change is introduced by this repair.

Rejected: comparing revision alone, retaining an old thumbnail after invalid
input, inventing placeholder geometry, broad refactoring, remote mutation,
or closing broad acceptance gates from local test results.

Direct impact is confined to Cartographer. RegOS, IDA_Training,
hidden_canopy_bot, and Ask_IDA are unchanged; their API/schema, mutation,
dataset/eval, and deployment contracts are untouched. Their tests are outside
this pass. Existing prefab schema and source-record format compatibility
remain risks of their original changes, not new schemas from this repair.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: exact-source thumbnail admission and pending local authoring
verification. Promotion progress: broader native, consumer, provider,
device, and deployment gates remain open.
