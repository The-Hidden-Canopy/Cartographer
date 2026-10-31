# Project source-asset registry and reimport plans

Introduced in Cartographer project schema 21 and retained in the current
schema 31, the `.carto` document stores a bounded source-asset registry. A
record binds a stable source identity to a project-relative path, source
generation and digest, importer identity/version, typed import options, a
stable artifact identity/digest, and optional per-domain digests for geometry,
hierarchy, and material output. The registry retains generation tombstones
after removal, so deleted identities cannot silently restart at an old
generation.

The native OBJ/PLY/STL import path now reads each selected file once under a
128 MiB bound, stages those exact bytes in the project's
`.carto-assets/source-blobs` content-addressed store, and parses that stored
snapshot. A saved-project import binds the source digest, built-in importer
identity/version, declared options, geometry digest, and project-relative blob
path to the created mesh asset. The original workstation path is never written
to the project. `.carto-assets/` is ignored by Git because it contains local
project payloads, not source code. An unsaved workspace can still import, but
the receipt warns that the source is not linked and recommends saving and
importing again.

Reimporting from the same source path reuses its opaque source identity.
Unchanged input is a no-op; a changed geometry replaces the existing mesh
asset in place, while a metadata-only source change preserves the current mesh
and updates only provenance. The mesh change and registry update are one
undoable, journaled command. Undo/redo keep source-generation lineage
monotonic. Parsing happens against a transient project-local snapshot; parser
failure removes that snapshot and never promotes a persistent blob. If a later
command or journal admission fails after the content-addressed write, an
unreferenced blob may remain; it cannot appear in project truth and is retained
until a future safe garbage-collection workflow exists.

The OBJ parser also preserves complete per-corner UV data and finite nonzero
`vn` records in `cartographer.uv0` and `cartographer.normal0` corner layers,
including distinct values at a shared-vertex seam. Its exporter writes active
render UVs, the named custom-normal layer, and Cartographer face material-region
IDs through triangulation; it omits material assets/assignments, additional UV
sets, and unsupported attributes. PLY/STL remain geometry-only, and no built-in
preserves source hierarchy or third-party importer state. The production
render-mesh compiler consumes `cartographer.normal0` by default, splits render
vertices at normal discontinuities, and carries those normals through
deterministic artifact serialization and PBR upload packing. The native
Modify panel now provides a human-only selected-face custom-normal control.
It writes the normalized vector to that face's stable corner records in the
`cartographer.normal0` layer, seeds other corners from the current derived
normals when the layer is first created, and uses the project mesh command so
the edit is revision-bound, journaled, saveable, undoable, and redoable. The
Modify panel also provides human-only selected-edge sharp mark and clear
controls. They write explicit true/false values to the complete typed boolean
`cartographer.edge_sharp` edge layer through a revision-bound project mesh
command; stale selections, shared/locked mesh owners, missing values, and AI
requests fail closed. These are durable source annotations and do not change
render-normal selection. The application snapshot builds bounded custom-normal
and hard-edge line overlays from the selected mesh's current authored source;
the desktop projects those lines over the viewport and rejects stale source
bindings. Surface shading remains geometry-derived. Native click-through and
screenshot acceptance, plus multi-face authoring, remain open. There is no
automatic watch/relink flow for a moved original file, missing-source repair UI,
orphan-blob garbage collection, or side-by-side candidate review panel. Stored
source blobs can be large and are not encrypted; projects and their
`.carto-assets/` directories may contain private user data.
The `.carto` project stores the registry's bounded serialization as hex so
embedded line breaks cannot disrupt the surrounding project record stream;
this is encoding, not encryption.

`plan_source_reimport` is read-only and classifies added, unchanged,
metadata-only, geometry, hierarchy, material, multiple-domain, unclassified,
removed, and identity-conflict cases. Applying a plan recomputes it against the
exact base registry digest, refuses stale/forged classifications and unresolved
identity conflicts, then produces a new registry without mutating its input.
Application adoption is a human-dispatched action. It is undoable and redoable;
replay advances source generations monotonically instead of restoring stale
generation numbers. Application mutation journaling records the project
snapshot and operation like other authored changes.

## Code-only blob inventory continuation (2026-10-26; unverified)

The source browser now exposes a read-only inventory of verified content-addressed
blobs that are absent from the current source registry, material texture catalog,
and reference-plane catalog. The query is bound to the captured project generation,
revision, and path; it reports missing live references and corrupt stored digests
separately, and never deletes files. The desktop shows at most eight digests per
finding category at a time, and annotates source rows whose stored bytes are
missing or fail integrity verification. It refuses inventories above 8,192
entries or 512 MiB of total stored bytes rather than scanning without a bound.

At this checkpoint, only current-snapshot reachability was included. A later
code-only continuation adds bounded durable journal snapshots; drafts and
pending operations remain outside the scan, and no candidate is safe to delete.
The original regression source covers live texture/reference/source retention,
unreferenced candidate detection, missing and corrupt references, stale-snapshot
refusal, and unsaved-project refusal. No build or test was run in that
continuation. Safe blob reclamation, native interaction, and external
original-source currentness remain open; no project schema changed.

## Code-only durable journal reachability continuation (2026-10-27; unverified)

The inventory now reads the saved project's validated journal while holding the
project writer lease, parses each bounded full-project snapshot, and retains
source, material-texture, and reference-plane digests found in any durable
revision. It refuses mismatched journal tails, malformed snapshots, journals
over 8,192 entries or 512 MiB of snapshot payload, snapshots whose decoded
revision disagrees with their journal entry, and reachability sets above
524,288 unique digests. This prevents a removed source
blob that remains reachable through recorded history from being reported as an
orphan candidate. The later scoped continuation also protects a digest held by
the current mesh-reimport review. Other draft and pending-work reachability
remains unknown; reports remain advisory and expose no deletion action.

- **Files changed:** `libs/application/src/application.cpp`, the application
  API comment, desktop inventory wording, application regression source, this
  ledger, and the ignored local queue.
- **Capability gained:** blob inventory includes bounded durable project
  history when classifying stored candidates.
- **Governance preserved:** history is read under the existing project writer
  lease; journal integrity and snapshot revisions are checked; no project,
  journal, blob, or application state is mutated.
- **Tests authored:** remove a source from the current registry and verify its
  retained journal snapshot prevents orphan classification, and corrupt the
  journal tail to require fail-closed inventory. Run: none, per the user's
  instruction.
- **Tests not run:** application/storage builds and suites, native browser
  interaction, and concurrent writer execution.
- **Risks:** old or very large journals above the scan bounds produce an
  inventory error; drafts and pending-work references are not represented in
  journal snapshots. Journal snapshot parsing cost has not been measured.
- **Rollback:** restore current-snapshot-only digest collection, previous UI
  wording, and the source-history regression; no schema or blob bytes change.
- **Rejected:** treating the latest snapshot as the full reachability set,
  silently skipping an unrecognized journal entry, or enabling deletion based
  on the advisory report.

## Active mesh-reimport review protection (2026-10-27; code-only)

The source-blob inventory can now receive one pending mesh-reimport binding.
The application checks the review's project generation/revision and its source
identity, generation, kind, and current digest against the project registry
before considering the candidate source digest; the desktop invalidates a
cached report if that review changes. A
verified stored blob protected by the review is listed separately from durable
live references and excluded from orphan candidates; an absent pending digest
is not reported as a missing durable reference. Other UI drafts and pending
work are still not enumerated, and the report remains advisory with no deletion
operation.

- **Files changed:** `libs/assets/include/carto/assets/blob_store.hpp`,
  `libs/assets/src/blob_store.cpp`, the application and UI inventory signatures,
  `apps/desktop/main.cpp`, `tests/storage_journal_tests.cpp`,
  `tests/application_tests.cpp`, this ledger, and the ignored local queue.
- **Capability gained:** a live mesh-reimport review can protect its exact
  candidate digest from advisory orphan classification.
- **Boundary improved:** pending source bytes do not become a false missing
  durable reference, and cached inventory is invalidated when the active review
  candidate changes.
- **Governance preserved:** inventory remains read-only; durable missing
  references stay separate from volatile review protection; no blob is deleted.
- **Tests authored:** stored pending digest protection, absent pending digest
  non-reporting, corrupt pending digest classification, over-bound refusal, and
  application forwarding. **Run:** none, per the user's instruction.
- **Tests not run:** no build, storage/application suite, or native interaction.
- **Risks:** only the active source-mesh reimport candidate is supplied by the
  desktop. Other drafts and pending workflows remain outside the scan.
- **Rollback:** remove the pending-work argument/report field and UI candidate
  forwarding, then restore the prior inventory wording and regression source.
- **Rejected:** treating a volatile candidate as a required durable reference,
  deleting advisory candidates, or claiming full draft/pending-work coverage.
- **Cross-product trace:** Cartographer storage/UI only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA are unchanged. Dataset/evaluation state is
  unchanged; capability is code-only and unverified; deployment and promotion
  remain open.

## Code-only material texture status continuation (2026-10-26; unverified)

Material texture and reference-image rows now display the stored-blob status
from the same generation-, revision-, and path-bound inventory used by the
source registry.
The shared status projection treats stale or absent reports as unchecked,
surfaces missing/corrupt digests, and flags a catalogued digest reported as an
orphan candidate as a reachability mismatch. Source rows use the same
projection while retaining their explicit per-source verification result.
Authored browser regression coverage exercises stale-report suppression and
verified, missing, corrupt, and catalog-mismatch outcomes. The test was not
run; no build or native click-through was performed. This adds no persistence,
mutation, or schema behavior.

## Verification

- `cartographer_source_asset_registry_tests` covers source-record validation,
  canonical round trips, hostile paths/options, deterministic classification,
  non-mutating preview, stale and forged plan rejection, stable artifact
  identity conflicts, adoption, and retained tombstones.
- `cartographer_obj_uv_tests` covers OBJ UV and custom corner-normal seams,
  exact import/export round trips, malformed/partial payload rejection, and
  the documented lossy material boundary.
- `cartographer_project_source_asset_registry_tests` covers journaled
  transaction adoption, stale-plan refusal, `.carto` save/reopen, schema-20
  migration, removal tombstones, and exact source generation continuity.
- `cartographer_production_closure_tests` covers authored corner-normal
  admission, hard-normal vertex splits, custom-normal tangent generation,
  artifact serialization/reopen, PBR upload propagation, and fail-closed
  missing, zero-length, and wrong-domain normal requests.
- `cartographer_application_tests` covers application-command undo/redo and
  generation advancement across replay, plus human-only selected-edge mark and
  clear receipts, stale-selection rejection, save/reopen, undo/redo, and exact
  edge-layer contents. `cartographer_ai_tests` keeps the edge operation
  unclassified, non-previewable, non-requestable, and not auto-approvable.
- `cartographer_ui_tests` covers the native controller preview/adoption route
  and rejection of raw AI mutation attempts, saved-project OBJ/PLY/STL source
  linkage, absolute-path exclusion, unchanged-import no-op behavior,
  content-addressed source cleanup after parse failure, in-place source
  reimport, undo/redo generation monotonicity, and explicit unsaved-import
  warnings.

### Viewport attribute overlay code continuation (2026-10-25; unverified)

The current application source builds bounded `ViewportState` line overlays
from the authored `cartographer.edge_sharp` boolean edge layer and the
`cartographer.normal0` corner-normal layer. It validates complete domain/type
bindings, uses stable topology IDs, caps displayed segments at 4,096, and
attaches them to the selected object's current mesh revision. The desktop
projects the lines over the viewport and checks the object, mesh asset, and
compiled source revisions before drawing. The edge selection inspector now
describes this overlay instead of claiming preview is pending. The overlay is
diagnostic visualization; surface shading remains geometry-derived in this
viewport path.

Application regression cases assert custom-normal directions, hard-edge IDs,
and mesh revision bindings. No builds, tests, or native screenshots were run in
this sprint. Native click-through and visual capture remain acceptance gates.

## Remaining work

Safe orphan-blob reclamation and a viewport-rendered side-by-side changed-source
review remain open. Moved-source relinking is an advisory plan/apply pair over
the registry (digest-identical moves only; changed content fails closed into
the reimport flow) journaled through the application action boundary. The
source browser now also launches an identity-bound mesh reimport for changed
content. It requires the source's original built-in importer, pins the project
and source generations before parsing, rechecks them before dispatch, and uses
the existing human `ImportSourceMeshCommand` to replace the bound mesh and
advance the registry through one undoable/journaled operation. The desktop now
parses a candidate without project mutation and displays registered/candidate
source and geometry digests plus current/candidate vertex and face counts in a
two-column review. Bounded 2D geometry thumbnails and local mesh-unit extents
are also compared when both meshes fit the thumbnail limits. Applying reparses
the selected file and requires its source and geometry digests to match the review. Generic import refuses a changed
registered source so it cannot bypass that review. Importer execution remains
inside Cartographer's existing built-in providers, not a private donor runtime.

### Code-only source-specific mesh reimport review wiring (2026-10-27)

The source registry row now offers a source-specific candidate review for OBJ,
PLY, and STL sources, followed by a distinct apply action. The controller binds
the review to the selected source identity, importer identity and version,
importer options, artifact identity, source generation, and exact project
generation/revision. It stages and parses the candidate without project
mutation, then rechecks the project and source snapshot before persisting the
source blob and immediately before dispatch. Apply reparses and refuses if the
candidate bytes or geometry differ from the displayed review. Reimport retains
the recorded importer version and options instead of replacing them with
generic-import defaults. A format/importer mismatch or stale snapshot refuses
without changing project history. Accepted reimports pass through the existing
`ImportSourceMeshCommand`, preserving the bound mesh artifact and updating its
source record in the same undoable operation. The UI states that all linked
instances use the replaced mesh and that undo is available.

- **Files changed:** `libs/ui/include/carto/ui/ui.hpp`, `libs/ui/src/ui.cpp`,
  `libs/application/include/carto/application/application.hpp`,
  `libs/application/src/application.cpp`, `apps/desktop/main.cpp`,
  `apps/desktop/studio_browser.hpp`, their UI/browser regression sources, this
  ledger, and the ignored local work queue.
- **Capability gained:** an operator can compare source and imported-geometry
  digests, mesh counts, local extents, and bounded geometry thumbnails, then
  reimport changed bytes for the selected source without creating a second mesh
  asset or losing source lineage.
- **Boundary improved:** reimport is constrained to the selected mesh source,
  its existing built-in importer, artifact, and current project/source
  generations; applying rechecks both candidate digests and every linked mesh
  instance updates through the existing human command path. Generic import
  cannot silently bypass the selected-source review.
- **Governance preserved:** the action is a human UI command; project mesh and
  registry mutations remain one undoable, journaled command. No AI or provider
  mutation route, project schema, or authority change was introduced.
- **Tests authored:** UI source now covers a bounded candidate mesh thumbnail,
  successful in-place reimport and
  monotonic undo/redo lineage through the explicit route, plus stale-project
  and wrong-importer refusal without project/source revision changes. It also
  covers read-only candidate preview, source-file drift between review/apply,
  refusal of unreviewed generic reimport, and retention of importer version and
  options. **Run:** none, at the user's instruction.
- **Not run:** build, UI/application tests, native click-through, source-file
  race exercise, or save/reopen verification.
- **Risks:** code is uncompiled and unverified; a successfully staged content
  blob can remain an advisory orphan if the project changes after blob
  persistence but before dispatch. Thumbnails independently fit their bounds,
  so local extents must be read alongside them to compare scale. They are not a
  shared-scale 3D viewport comparison. The UI does not claim that the selected
  file came from the same external workstation path.
- **Rollback:** remove the source-row review/apply state and `UiController`
  preview/apply helpers, restore generic import behavior, and remove these
  source tests and ledger/queue notes. The existing import command and registry
  schema remain unchanged.
- **Rejected:** silently creating a new source identity for a selected record,
  changing importer families, deleting orphan blobs, or claiming a shared-scale
  3D comparison based on the bounded 2D thumbnails.
- **Cross-product trace:** direct impact is Cartographer's source browser and
  project mesh lineage. Indirect impact is limited to objects sharing the
  reimported mesh asset. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA
  contracts/data are untouched. Dataset/eval risk is none; deployment risk is
  unassessed because no build or packaging work ran. Capability is
  source-only; promotion remains open. Required follow-up is UI/application
  tests for success, stale/mismatch refusal, undo/redo, and reopen; then native
  review of the source row and linked-instance replacement behavior.

### Code-only material-texture reimport wiring (2026-10-27)

The Material instrument now offers an explicit reimport action for each saved
texture source. It reads and inspects bounded replacement bytes, stages them in
the project-local content-addressed blob store, then dispatches a human-only
action bound to project generation/revision, texture identity/revision, and
the old and candidate digests. Application admission verifies the staged blob
against its digest, format, and dimensions, rejects stale or duplicate source
bindings, preserves the stable texture ID, display name, provenance, and all
material references, and advances the texture source revision through the
existing material-catalog command/journal path. The prior blob remains
available for undo and is not deleted.

- **Files changed:** `libs/project/include/carto/project/material_catalog.hpp`,
  `libs/project/src/material_catalog.cpp`,
  `libs/application/include/carto/application/application.hpp`,
  `libs/application/src/application.cpp`, `libs/ui/include/carto/ui/ui.hpp`,
  `libs/ui/src/ui.cpp`, `apps/desktop/main.cpp`,
  `tests/material_catalog_tests.cpp`, `tests/ui_tests.cpp`, this ledger, and
  the ignored local work queue.
- **Capability gained:** an operator can repair or replace a saved texture's
  exact source bytes in place while preserving its durable identity and every
  material binding.
- **Boundary improved:** missing/corrupt stored texture bytes can be replaced
  from a newly selected file; stale project generations, stale texture
  identity/revision/digest, missing staged bytes, wrong dimensions, malformed
  candidates, unchanged bytes, duplicate digests, and AI proposal attempts
  refuse without catalog mutation. Accepted changes are undoable, redoable,
  journaled, and persisted on save/reopen.
- **Governance preserved:** only an explicit human UI action reaches the
  replacement handler. The current catalog is copied, validated, and committed
  through `SetProjectMaterialCatalogCommand`; there is no direct project write,
  fallback pixel path, schema migration, or automatic deletion. Org scope,
  role tiers, justification, and wall-clock values are not part of this local
  texture-source boundary.
- **Tests authored:** catalog replacement revision/identity preservation and
  stale/missing refusal; UI exact-byte replacement, stale source/generation,
  wrong identity, missing staged blob, metadata mismatch, malformed/unchanged
  and duplicate bytes, AI refusal, stable material references, journal
  evidence, undo/redo, and save/reopen.
  **Run:** none, per the user's instruction.
- **Tests not run:** all builds and suites, native button interaction, texture
  decoding, GPU upload, and rendered image review.
- **Risks:** code is uncompiled and unverified. A candidate blob staged before
  a concurrent stale-action rejection remains an advisory orphan; it is not
  deleted. Reimport updates authoring truth but does not establish that the new
  pixels decode or render correctly.
- **Rollback:** revert the replacement action, catalog API, UI method/button,
  tests, and this ledger section. Existing import records and their schema stay
  valid; no migration or project-file rewrite is required.
- **Rejected:** assigning a new ID to a repair, changing material bindings,
  deleting prior bytes that undo may reference, accepting a stale review, or
  describing a successfully inspected container as decoded/rendered pixels.
- **Cross-product trace:** direct impact is Cartographer texture catalog and
  Material instrument. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA are
  unchanged; no shared API/schema, dataset/eval, network, or deployment
  boundary was touched. Dataset/training/promotion progress is unchanged;
  capability progress is source-only.
- **Required follow-up:** compile and run material-catalog, UI, and application
  tests, then exercise the Material instrument with a populated project and
  verify displayed material pixels on a supported renderer. Native and visual
  acceptance remain open.
