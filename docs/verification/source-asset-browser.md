# Source asset browser registry projection

Scope: Cartographer, 2026-10-25. This is a code-only continuation of the
started source asset browser task. It adds recorded provenance search,
explicit authored tags and dependency edges, a human-reviewed metadata edit
path, stored-blob verification, geometry thumbnails, and mesh-card drag/drop.
It does not claim full asset-browser acceptance.

## Capability delta

`ApplicationSnapshot::source_assets` carries source identity, typed geometry,
hierarchy, and material digests, authored tags, dependency identities, and an
optional exact mesh binding. The source browser searches recorded importer
versions, artifact and source digests, domain labels, tags, and dependency
identities. Mesh cards join a source record only when its artifact identity is
the canonical `mesh.asset.<id>` form and that exact mesh asset exists in the
current project. Malformed or unmatched artifact identities never infer a
binding.

Authored tags and dependency edges persist in source record format version 3.
Records without annotations keep their version 1 or version 2 serialization,
including existing botanical generation receipts. Tags are bounded,
lowercase tokens stored sorted and unique. Dependencies are bounded, sorted
source identities that must resolve within the complete active registry. The
browser allows editing these fields as a draft, prepares a full-registry
`SourceReimportPlan` against the captured project revision and source
generation, and displays the candidate generation and plan digest. Applying
the plan remains a separate explicit human action through the existing
project command, journal, undo, and redo path. Source and artifact digests are
preserved for a metadata-only edit.

The UI labels original-source currentness as unknown. A stored source digest
and a record generation do not prove that the original external file exists or
matches. An explicit read-only check now verifies the project-local content-addressed
source blob against the recorded digest and exact project-relative locator.
Success is labeled as stored-blob integrity only; the original source file is
not retained as an external live locator, so its currentness remains unknown.

Mesh cards draw bounded geometry-only thumbnails from the exact compiled mesh
currently shown by the live viewport snapshot. Projection refuses invalid,
degenerate, nonfinite, or over-bound geometry and does not invent a fallback
mesh. Cache admission binds the project generation and exact source revision,
the current shared object, and a SHA-256 digest of the position/index bytes
that determine the projection. A same-object mutation rebuilds the preview;
revision-mismatched or invalid geometry clears it. Material and texture
appearance is explicitly omitted. A card can be
dragged into the live model viewport to add a shared mesh instance at the
drop position. The card chooses any grouped root instance that is visible,
unlocked, and explicitly bound to a nonzero mesh revision, then confirms the viewport
snapshot contains that exact compiled revision before exposing the drag source.
Missing revision data is not treated as current. The payload carries the source
object and mesh identities; the drop revalidates both against the current
snapshot and dispatches the existing human project command. Preview workspaces
and stale, hidden, locked, or nested sources refuse placement.

## Boundary and invariants

- Drafting and reviewing metadata do not mutate project state.
- Stale project revisions, project generations, and source generations refuse
  review; the UI discards a draft after its captured context changes.
- Applying a reviewed plan still checks the plan's exact base registry and
  uses the human-only source reimport command. AI proposal submission cannot
  apply the plan.
- Stored-blob verification requires the exact project and source snapshot,
  resolves only the expected digest-addressed blob inside the project, checks
  its recorded relative locator, and recomputes SHA-256 without mutation.
- Thumbnail projection is limited to 8,192 vertices and 16,384 triangles per
  mesh and caches only derived geometry for the current project/mesh revision.
- Drag/drop creates a shared instance only from an exact current root-level
  source with an explicit matching mesh revision into the live modeling
  viewport; grouped-card placement does not depend on the display representative.
  No source bytes or registry metadata are modified by placement.
- Dependency targets must be active in the complete candidate registry. A
  self-edge, unknown identity, invalid token, duplicate, or unsorted stored
  set is rejected.
- The source-record serializer accepts versions 1, 2, and 3; unannotated
  legacy records remain at their old version. Botanical receipts remain
  independently validated.
- Browser projection rejects malformed tag/dependency data and does not
  infer metadata, fabricate thumbnail geometry, or claim original-source
  currentness.
- Search remains bounded and rejects embedded NUL and overlong input.

## Files changed for this continuation

- `libs/assets/include/carto/assets/source_registry.hpp` and
  `libs/assets/src/source_registry.cpp` — bounded authored tag and dependency
  fields, record format version 3, schema compatibility, and registry/candidate
  validation.
- `libs/application/include/carto/application/application.hpp` and
  `libs/application/src/application.cpp` — snapshot projection,
  generation-bound metadata review preparation, and digest-bound stored-blob
  verification.
- `libs/ui/include/carto/ui/ui.hpp` and `libs/ui/src/ui.cpp` — review API
  and blob-verification forwarding.
- `libs/editor/src/command_bus.cpp` — preserve authored metadata across source
  mesh reimports.
- `apps/desktop/studio_browser.hpp` and `apps/desktop/main.cpp` — validate and
  search authored metadata, display source-to-mesh links, and stage/apply
  reviewed metadata edits; draw geometry-only mesh thumbnails and wire
  revision-checked mesh-card drag/drop into the live project viewport.
- `tests/source_asset_registry_tests.cpp`, `tests/desktop_browser_tests.cpp`,
  `tests/application_tests.cpp`, and `tests/ui_tests.cpp` — authored format,
  invalid-boundary, search, history, and human-review coverage.
- `docs/CARTOGRAPHER_WORK_QUEUE.local.md` — continuation and remaining gates.

## Acceptance status

- Authored coverage: legacy source-record versions, version 3 round-trip,
  malformed tags and dependency edges, cross-record target resolution,
  missing-target refusal, tag/dependency search, malformed browser projections,
  read-only review preparation, stale-context refusal, project-history
  application, journal evidence, undo/redo, managed-blob verification, and
  tampered-blob refusal. Thumbnail coverage exercises geometry bounds,
  normalization, deterministic projection, invalid topology, same-revision
  object replacement, same-object in-place geometry drift, and invalidation
  after the same object becomes invalid. This coverage was authored during the
  active no-test sprint and has not been run.
- A later authored browser regression selects only a visible, unlocked,
  nonzero-revision-bound root among grouped instances; locked, hidden, nested,
  unversioned, and zero-revision instances are excluded. It also covers exact,
  stale, missing, and zero revision evidence through the shared predicate. The
  viewport uses that predicate before showing the drag source and again before
  delivery. This regression has not been built or run.
- Tests run: none, per the active user instruction not to test this sprint.
- Builds run: none, per the same instruction.
- Native populated-browser interaction, screenshot evidence, desktop build,
  card-to-viewport drag/drop, and platform acceptance remain unverified.

The task remains **started**. The code now includes geometry thumbnails,
internal card-to-viewport drag/drop, metadata, and reviewed-generation flow.
Verification of an original external source file (which is not retained as a
live locator) and native acceptance evidence remain open. No material preview
or original-source currentness claim is fabricated.

## Cross-product trace

| Product | Direct impact | Follow-up / risk |
| --- | --- | --- |
| Cartographer | Registry schema, snapshot, source browser, and reviewed metadata command | Build and run focused tests, then inspect a populated native project; not done |
| Experimental-Internal-RegOS | None | No state transition, permission, org-scope, audit, or domain-event contract changed |
| IDA_Training | None | No dataset, training, prompt, or evaluation artifact changed |
| hidden_canopy_bot | None | No API, command, moderation, or mutation path changed |
| Ask_IDA | None | No query, answer-provenance, or authority contract changed |

## Risks, rollback, and rejected options

Risk: source-record serialization gains an optional version 3 form and public
C++ snapshot/API fields. The outer registry version stays unchanged because
each nested source record carries its own version. Compatibility, project
history behavior, and digest verification are covered by authored assertions
but remain unverified without the requested build and test runs. Snapshot cost
and native layout are also unmeasured. Thumbnail projection is geometry-only,
and card-to-viewport drop behavior has not been exercised in the native app.

Rollback: selectively revert this continuation's source metadata fields and
serialization, metadata review API and UI, reimport preservation, related
tests, and this ledger. Preserve unrelated dirty worktree changes and other
browser work.

Rejected: infer tags or dependency edges from file names or digests; normalize
unknown dependency identities into a guessed target; apply a draft without a
generation-bound human review; claim a source is current from its stored hash;
fabricate thumbnails; or mark the full task complete before native acceptance.

Dataset progress: none. Training/prompt progress: none. Capability progress:
authored metadata persistence, search, reviewed-edit wiring, and project-local
blob integrity verification; unverified.
Promotion progress: none; the task remains started.

## Subsequent executed acceptance

The later verification continuation built the desktop and ran the current
browser/registry regressions plus thirteen related authoring suites: 15/15
passed in 61.39 seconds. It also exercised a populated native browser, card
placement and undo/redo, exact-byte OBJ import, stored-blob verification, and
separate metadata review/apply. Continued native history exposed an ordered
source-command failure; its regression failed before repair, then passed with
journal-write failure rollback and contested-state refusal. These executed
results supersede the earlier code-only verification gaps for those bounded
paths. Other native/device/original-source gates remain open. See
[source history acceptance](source-history-acceptance.md) for exact scope,
remaining risks, input fingerprints, and rollback.

## Follow-on placement hardening (2026-10-25)

The subsequent code-only pass changed mesh-card placement source selection to
use any eligible grouped root instance and require a nonzero mesh revision that
matches the live viewport mesh. Missing or zero revision metadata is refused.
An adversarial browser regression was authored for eligible and excluded
instances. No build/test result was captured for this follow-on, so the earlier
executed results do not verify this latest change. Native drag/drop remains
unverified.

## Import-option snapshot wiring (2026-10-27; unverified)

The source snapshot now carries the registered `SourceImportOptions` as a
typed field. The source row displays its unit scale, up axis, handedness,
triangulation, hierarchy/material/normal flags, and weld settings. Browser
projection rejects invalid importer options instead of rendering malformed
registry state. Source-specific reimport pins the importer version and options
and carries them forward into the existing history command.

- Capability gained: operators can inspect recorded importer settings, and
  source-specific reimport preserves them rather than replacing them with
  generic-import defaults.
- Invariants preserved: source identity, generation, project revision, and
  artifact binding checks remain in force; project mutation still goes through
  the existing human command/journal/history path.
- Files changed: application snapshot, UI controller, desktop browser row,
  browser projection, their source regressions, and the source-registry ledger.
- Tests authored: importer-option validation refusal and reimport preservation.
  Run: none, at the user's instruction.
- Tests not run: builds, UI/browser/application suites, native review, and
  source reimport save/reopen.
- Risks: C++ snapshot shape and desktop rendering are uncompiled; legacy or
  malformed records may be rejected in the browser projection. No project
  file schema changed.
- Rollback: remove the snapshot options field and its display/validation, then
  restore the former generic option assignment in source-specific reimport.
- Rejected: silently replacing registered options with defaults or claiming
  compile/native acceptance without execution.
- Cross-product trace: Cartographer only. No RegOS, IDA_Training,
  hidden_canopy_bot, or Ask_IDA contract/data changed. Dataset and training
  progress are unchanged; capability is source-only; promotion is unassessed.
- Remaining risk/follow-up: compile and run focused browser, UI, and application
  regressions, then review a populated native source row and save/reopen.

## Changed-source comparison flow (2026-10-27; unverified)

The source row now separates candidate parsing/review from project mutation.
Its two-column review shows registered and candidate source-byte digests,
imported-geometry digests, and current/candidate vertex and face counts. Apply
reparses the selected file and compares both digests before persisting the
source blob or dispatching the existing undoable source-mesh command. A file
changed after review is refused. Generic mesh import also refuses changed bytes
for a registered source identity, so it cannot bypass the explicit review.

- Capability gained: an operator gets a read-only candidate comparison before
  replacing a source-bound mesh and can cancel without project mutation.
- Invariants preserved: source identity, project/source generations, importer
  version/options, artifact binding, digest-addressed storage, and the existing
  human command/journal/history boundary.
- Files changed: application snapshot and source-count projection, UI
  preview/apply APIs, source browser, and UI/browser regression sources.
- Tests authored: preview read-only behavior, unreviewed-import refusal,
  candidate-file drift refusal, importer-option retention, and apply/undo/redo
  lineage. Run: none, at the user's instruction.
- Tests not run: compilation, focused suites, save/reopen, and native display
  review.
- Risks: the added thumbnails are independent-fit 2D projections, not a
  shared-scale 3D overlay; candidate and current source bytes may originate
  from different external paths. Build and UI layout are unverified.
- Rollback: remove the review API and row state, restore the previous import
  behavior, and remove its regression and ledger entries.
- Rejected: applying bytes captured only by a file dialog without showing
  evidence; deleting a newly persisted blob as a race response when it could be
  shared or referenced.
- Cross-product trace: Cartographer only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts/data are unchanged. Dataset and
  training progress are unchanged; capability is source-only; promotion is
  unassessed.
- Remaining risk/follow-up: compile and run the focused browser/UI/application
  regressions; exercise save/reopen and native interaction; add rendered
  shared-scale 3D geometry comparison and define safe orphan reachability
  before reclamation.

## Bounded source-reimport geometry comparison (2026-10-27; unverified)

The candidate review now retains a compiled preview mesh only when it fits the
thumbnail budget (8,192 vertices and 16,384 triangles). The desktop source row
shows current and candidate 2D geometry thumbnails side by side, plus exact
mesh-unit extents so independently fitted thumbnails do not hide scale changes.
Digest revalidation remains authoritative when Apply reparses the selected
file. Oversized candidates retain digest/count review but show no thumbnail.

- Capability gained: operators can inspect bounded silhouette and scale
  evidence before applying a reviewed source reimport.
- Invariants preserved: the preview mesh is immutable, transient, and never
  admitted to the project; apply reparses and checks the full source and
  geometry digests before mutation.
- Files changed: UI review receipt/controller, desktop review panel, UI
  regression source, and the source-asset verification ledgers.
- Tests authored: the source reimport case requires a valid compiled candidate
  preview. Run: none, at the user's instruction.
- Tests not run: no compile, tests, native layout review, or performance checks.
- Risks: independent-fit thumbnails are not a shared-scale 3D comparison;
  cached projections and rendering cost are unmeasured.
- Rollback: remove the compiled preview from the review receipt and desktop
  panel while preserving digest/count review and apply revalidation.
- Rejected: compiling unbounded candidate meshes for a thumbnail or treating a
  displayed preview as the mutation payload.
- Cross-product trace: Cartographer only; other products and datasets are
  untouched. Dataset/training progress is unchanged; capability is source-only;
  promotion remains open.
- Remaining risk/follow-up: build, run the browser/UI/application suites,
  inspect native layout and thumbnail parity, then add a shared-scale viewport
  comparison and safe blob-reachability policy.

## Shared-scale reimport comparison (2026-10-27; unverified)

The registered and candidate oblique projections now retain their projected
bounds and use one shared projected frame and mesh-unit scale. Both are drawn
with a uniform screen scale in the equal review panels, preserving the oblique
projection's aspect ratio and showing relative size and local-position changes;
the existing XYZ extents remain explicit. The review is still a bounded 2D
thumbnail and does not claim interactive 3D viewport parity.

- Capability gained: source reimport review shows relative projected size and
  silhouette changes at a common scale before Apply.
- Invariants preserved: thumbnails remain transient compiled-mesh projections;
  exact byte and geometry digest revalidation remains the apply boundary.
- Files changed: `apps/desktop/studio_browser.hpp`,
  `apps/desktop/main.cpp`, `tests/desktop_browser_tests.cpp`, this ledger, and
  the ignored local queue.
- Tests authored: projection bounds, common-scale size and local-offset
  behavior, including invalid vertex/frame refusal. Run: none, per the user's
  instruction.
- Tests not run: no build, test, UI execution, native screenshot, or performance
  measurement.
- Risks: oblique projections can still hide depth/topology differences; native
  layout, actual viewport comparison, and cached-render performance are
  unverified.
- Rollback: revert the retained projected bounds, common-scale mapper, panel
  wiring, and associated regression together; bounded independent thumbnails
  remain available.
- Rejected: treating 2D thumbnails as full 3D review or normalizing away
  geometry scale and local-offset changes.
- Cross-product trace: Cartographer only. Other products, datasets, training,
  schema, and deployment are unchanged; promotion and native acceptance remain
  open.

## Rotatable shared-scale reimport comparison (2026-10-27; unverified)

The registered and candidate thumbnail projections now share reviewer-controlled
orthographic azimuth and elevation in bounded five-degree steps. Both projections
use the same camera and projected frame, so changing the view does not normalize
away relative scale or offset. Projection caching includes the camera setting.
Triangle painter ordering remains approximate; this is still a review thumbnail,
not a depth-buffered viewport or rendered acceptance image.

- **Capability gained:** reviewers can rotate both bounded source-reimport
  previews together to inspect more of the mesh depth and silhouette before
  applying the digest-checked reimport.
- **Boundary improved:** camera steps outside azimuth 0–355 degrees or elevation
  -85–85 degrees refuse projection; both models receive the same view and common
  scale.
- **Governance preserved:** preview geometry remains transient and read-only;
  Apply still reparses the selected source and checks the reviewed source and
  geometry digests before mutation.
- **Files changed:** `apps/desktop/studio_browser.hpp`, `apps/desktop/main.cpp`,
  `tests/desktop_browser_tests.cpp`, this ledger, and the ignored local queue.
- **Tests authored:** camera-view change, invalid camera bounds, and cache
  invalidation when the shared view changes. **Run:** none, at the user's
  instruction.
- **Tests not run:** no build, test, desktop interaction, screenshot, or
  performance measurement.
- **Risks:** painter ordering can misrepresent intersecting or concave geometry;
  native layout and the cost of rebuilding projections for changed camera steps
  are unverified.
- **Rollback:** remove the view parameter and cache key, the two comparison
  sliders, and their source regressions; the original fixed oblique comparison
  remains available.
- **Rejected:** claiming a real viewport or final render from the painter-ordered
  ImGui triangles, or independently fitting the two meshes after each orbit.
- **Cross-product trace:** direct impact is Cartographer's source browser only.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts and datasets are
  unchanged. Dataset/training progress is unchanged, capability work is
  source-only, deployment is unverified, and promotion remains open.
- **Shared contract/API/schema risk:** none; this changes transient editor
  projection settings and does not change serialized project or source records.
- **Dataset/evaluation risk:** none; no dataset, eval, or training artifact was
  changed.
- **Deployment risk:** the native desktop build and visual behavior are
  unverified.
- **Remaining risk:** build and run the browser/UI/application suites, inspect
  native interaction, add a depth-correct 3D comparison, and complete save/reopen
  and source-blob reachability acceptance.

## Bounded overlap-aware thumbnail depth order (2026-10-27; unverified)

Source-browser and mesh-library thumbnails now constrain draw order using the
actual projected overlap polygon and linearly interpolated camera depth at its
vertices. A far-to-near partial order is topologically sorted; a crossing depth
surface, cyclic occlusion, invalid overlap, or more than 262,144 candidate
pair checks makes the thumbnail unavailable. This prevents average-depth
sorting from silently putting an intersecting surface on top. The preview is
At this checkpoint, the reimport preview was still a bounded opaque triangle
painter, not a depth-buffered 3D viewport, and did not split triangles at depth
crossings. The later depth-tested source-reimport and mesh-library continuation
below supersedes that limitation for thumbnail visibility.

- **Capability gained:** bounded mesh previews use overlap-specific depth
  ordering when a consistent painter order exists.
- **Boundary improved:** ambiguous, cyclic, malformed, and over-budget depth
  relationships fail closed instead of producing a misleading thumbnail.
- **Governance preserved:** preview generation remains transient and read-only;
  applying a reviewed reimport still rechecks exact source and geometry digests.
- **Files changed:** `apps/desktop/studio_browser.hpp`,
  `apps/desktop/main.cpp`, `tests/desktop_browser_tests.cpp`, this ledger, and
  the ignored local queue.
- **Tests authored:** overlapping parallel layers must render far-to-near;
  triangles whose depth order crosses over their shared projected area must
  refuse. **Run:** none, per the user's instruction.
- **Tests not run:** no build, unit test, desktop interaction, screenshot, or
  performance measurement.
- **Risks:** legitimate intersecting/concave meshes may have cyclic or crossing
  visibility and will show no thumbnail. The pair-work cap may also suppress a
  dense mesh preview. Compile behavior and practical preview coverage remain
  unverified.
- **Rollback:** restore average-depth sorting and remove per-corner depth,
  overlap clipping, and the new regression cases. No serialized data changes.
- **Rejected:** claiming true depth-buffered viewport behavior or drawing an
  ambiguous painter result as if it were correct.
- **Cross-product trace:** Cartographer source and mesh browsers only. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA contracts and datasets remain
  unchanged; dataset/training progress is unchanged, capability progress is
  source-only, deployment is unverified, and promotion remains open.
- **Shared contract/API/schema risk:** none; this changes a private desktop
  projection helper and does not alter project schema or source records.
- **Dataset/evaluation risk:** none; no dataset or evaluation artifact changed.
- **Deployment risk:** desktop compilation and native visual behavior remain
  unverified.
- **Required follow-up:** compile and run `cartographer_desktop_browser_tests`,
  exercise overlapping/crossing fixtures, and review dense real meshes in the
  native source browser. A real depth-buffered comparison remains outstanding.

## Active mesh-reimport blob protection (2026-10-27; unverified)

The read-only blob inventory accepts a pending mesh-reimport binding. The
application checks the review's project generation/revision and its source
identity, generation, kind, and current digest against the project registry
before considering the candidate digest. The source browser binds the cached
report to the complete review binding. A
matching verified stored blob is shown as retained by the active review and
excluded from orphan candidates. A pending digest absent from storage is not
reported as a missing durable reference. Other drafts and pending workflows
remain outside the inventory; candidates remain advisory and no deletion is
exposed.

- **Capability gained:** the active mesh-reimport review's source bytes are
  considered when the operator inspects stored blob candidates.
- **Boundary improved:** volatile review data is separated from durable
  catalog/history references and cannot inflate missing-live counts.
- **Governance preserved:** the query remains read-only and project-generation,
  project-revision, path, and active-review bound.
- **Files changed:** BlobStore report/API, ApplicationSession and UiController
  inventory signatures, desktop source-browser state/rendering, storage and
  application regression source, the source-asset registry ledger, and the
  ignored local queue.
- **Tests authored:** active stored digest retention, absent pending digest
  separation, corrupt/over-bound cases, and application forwarding. **Run:**
  none, at the user's instruction.
- **Tests not run:** no build, storage/application/browser suite, or native
  interaction.
- **Risks:** only the active mesh-reimport source candidate is represented;
  other unsaved drafts may still refer to stored blobs.
- **Rollback:** remove the pending digest input and report projection, revert
  UI cache binding, and restore the previous inventory text/tests.
- **Rejected:** treating volatile candidate digests as missing durable assets,
  inferring that unscanned drafts are unreachable, or enabling deletion.
- **Cross-product trace:** Cartographer only; RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA are unchanged. Dataset/evaluation is unchanged;
  capability progress is code-only; deployment and promotion are unverified.
- **Required follow-up:** run storage/application/desktop browser suites and
  exercise report refresh while opening, replacing, applying, and cancelling a
  mesh-reimport review. Add other draft classes only when their exact stored
  digest ownership is known.

## Depth-tested source reimport thumbnails (2026-10-27; unverified)

The source-reimport comparison rasterizes both previews into bounded 144×144
RGBA8 images with a per-pixel camera-depth buffer. It keeps the same orthographic
view and shared mesh-unit frame, resolves crossing surfaces by interpolated
camera depth, and caches results by source digest, view, shared frame, and image
size. The desktop uploads the two images through the existing deferred Vulkan
texture-retirement path; reimport sliders defer raster and texture work until
dragging ends. Mesh-library cards now use the same depth-tested rasterizer at
64×64, drawing cached horizontal color runs directly into ImGui so each asset
does not need its own GPU texture.

- **Capability gained:** source reimport review and mesh-library cards display
  depth-tested geometry thumbnails for overlapping and crossing surfaces
  without treating triangle draw order as visibility.
- **Boundary improved:** raster dimensions, triangle counts, and candidate
  pixel work are bounded; invalid geometry, non-finite depth, and work-budget
  overflow produce no image. A failed upload clears its slot rather than
  showing an older preview.
- **Governance preserved:** thumbnails remain transient and read-only; Apply
  still rechecks source and geometry digests before changing project state.
- **Files changed:** `apps/desktop/studio_browser.hpp`,
  `apps/desktop/main.cpp`, `tests/desktop_browser_tests.cpp`, this ledger, and
  the ignored local queue.
- **Tests authored:** layered surfaces select the nearer camera depth, crossing
  surfaces produce order-independent depth output, invalid raster bounds are
  refused, and the per-mesh raster cache clears on absent input and rebuilds
  deterministically. Run: none, at the user's instruction.
- **Tests not run:** no build, test, desktop interaction, screenshot, or
  performance measurement.
- **Risks:** fixed neutral thumbnail shading does not represent materials or
  textures; dense meshes can exceed the 4,194,304 candidate-pixel-work budget;
  large meshes can emit many horizontal ImGui runs; Vulkan upload lifetime,
  visual output, and post-drag responsiveness are unverified.
- **Rollback:** restore both ImGui triangle-painting paths, remove the raster
  cache and RLE helper, and remove the two temporary source-reimport texture
  slots. Project data and schema are unchanged.
- **Rejected:** claiming a full 3D viewport, material preview, scene-depth
  occlusion, or native acceptance based on a bounded thumbnail.
- **Cross-product trace:** Cartographer desktop only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA are unchanged; dataset and training progress
  are unchanged, capability work is source-only, and deployment/promotion are
  unverified.
- **Shared contract/API/schema risk:** none; texture slots and raster data are
  private desktop presentation state.
- **Dataset/evaluation risk:** none; no dataset, evaluation, or training
  artifact changed.
- **Required follow-up:** compile the desktop and browser tests, inspect real
  dense/crossing meshes in both instruments, and measure upload latency after
  slider release. Full viewport and material/texture review remain separate
  acceptance work.

## Symlink-safe source-blob inventory (2026-10-27; unverified)

The content-addressed `BlobStore` now rejects symlinked store directories and
blob files on put, read, and list. Inventory fails closed for inaccessible
entries, non-regular digest paths, malformed digest filenames, and shard paths
that do not match their digest, including 64-character digest-like filenames
at the wrong directory depth or with noncanonical casing. The application
inventory includes bounded saved-journal snapshots when calculating reachable
digests and rejects a symlink or non-directory in the project-local
`.carto-assets/source-blobs` chain. This keeps the report from following an
external path or calling unreadable contents orphan candidates. The report
remains advisory; no blob is deleted.

- Capability gained: source-blob verification and inventory refuse linked
  paths before reading through them or classifying reachability.
- Invariants preserved: content digests remain the source identity, inventory
  remains read-only and generation-bound, and orphan candidates carry no delete
  authority.
- Files changed: `libs/assets/src/blob_store.cpp`,
  `libs/application/src/application.cpp`,
  `tests/storage_journal_tests.cpp`, `tests/application_tests.cpp`, this ledger,
  and the ignored local queue.
- Tests authored: linked blob-file refusal for read/put/list/inventory,
  project-root symlink refusal for application inventory, and rejection of
  digest-named entries outside the canonical shard depth or with noncanonical
  casing. Application source covers retaining a digest referenced by a prior
  durable journal snapshot after current-registry removal. Run: none, per the
  user's instruction.
- Tests not run: builds, storage/application suites, symlink-capability paths,
  and platform-specific filesystem review.
- Risks: path status checks do not eliminate a concurrent filesystem swap
  between validation and open; journals beyond 8,192 entries or 512 MiB of
  snapshot payload, or 524,288 unique reachable digests, fail the inventory
  bound; only the active mesh-reimport candidate is supplied as pending work,
  while other drafts remain outside the scan; no automatic orphan reclamation
  is implemented.
- Rollback: revert the new BlobStore path admission and application managed-root
  check with their focused regressions. No project data or schema changes.
- Rejected: following symlinks and labeling their targets as project-local
  blobs, silently skipping inaccessible inventory entries, or adding deletion
  based on an advisory report.
- Cross-product trace: Cartographer storage and application inventory only;
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA are unchanged. Dataset and
  training progress are unchanged; capability is source-only; promotion and
  safe reclamation remain open.
