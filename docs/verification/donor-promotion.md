# CONDITION cross-repository donor promotion

This note closes the first tranche of the CONDITION cross-repository donor
series. It records provenance and promotion decisions without importing donor
runtime code into Cartographer.

## Boundary

Cartographer remains the authoritative public authoring repository. A donor
reference is evidence, not a dependency. The public promotion ledger stores
only:

- a stable logical promotion ID and target module;
- a donor class: implementation, architecture pattern, or greenfield;
- a verified lower-case Git object ID, or an explicit reason why no Git object
  is available;
- relative logical references, never workstation paths;
- the semantic changes, removed authorities, tests, fixtures, license basis,
  and explicit non-claims; and
- a digest bound to the canonical receipt content.

The ledger rejects absolute paths, URLs, credential-shaped material, missing
source identity, unstable list order, duplicate promotion IDs, and accepted
states without implementation tests and a CONDITION fixture. It has no file
loader, network client, runtime handle, permission check, state-transition
authority, or mutation callback.

## Donor identity boundary

Repository names, object IDs, and source-file references for private or
owner-controlled donor repositories are intentionally omitted from this public
note. When publication of donor identity is approved, a receipt can carry a
verified Git object ID; otherwise it must state why that identity is absent.
This document does not infer donor presence, ownership, licensing, or promotion
from an adjacent checkout, and it does not publish local repository metadata.

## What this tranche implements

`DonorPromotionLedger` and `DonorPromotionReceipt` are public-neutral
verification contracts. Appending a receipt assigns its sequence and computes
the receipt digest. Reopening validates the digest and the full decision
history. A record can state that work is planned, adapting, architecture-only,
accepted, or rejected, but the state cannot silently imply a stronger result.

The accepted state is intentionally narrow: it requires an implementation
disposition, a verified donor identity, tests, and at least one named
CONDITION fixture. Greenfield work must explicitly say that it has no donor
evidence and can never be represented as an accepted donor promotion.

The first concrete interaction promotion is now also present as a
Cartographer-owned `carto::editor::EditorCamera`. It independently implements
target orbit, pan, dolly, perspective fly, orthographic/perspective span
preservation, bounds framing, bounded validation, and deterministic local
serialization. It has no donor header or runtime dependency and does not write
scene camera state. The accompanying `SnapProfile`/`SnapSession` policy now
adds deterministic priority, acquire/retain/break hysteresis, switch margin,
distance remeasurement, and source-revision refusal over existing
revision-bound candidates. Scene-wide candidate collection now finds visible
objects' vertex, edge-midpoint, and face-center targets in world space. Each
candidate binds both the project revision and mesh revision, and carries the
scene-object/mesh-asset identity pair so instanced topology IDs cannot collide.
The native model workspace now draws X/Z translation handles for one selected,
unlocked root object. Pointer capture computes a preview only; Ctrl applies a
0.25 m increment snap, while Shift queries nearby compiled vertices, edge
midpoints, and face centers with object, mesh-asset, project-revision, and
mesh-revision provenance. A transient acquire/retain/break lock stabilizes
feature identity as the pointer moves. The top-down X/Z gizmo snaps only its
active plan coordinate, preserves elevation, and draws a revision-checked
target marker. Release submits one project-revision-bound transform action
through normal application history/journaling; Ctrl grid snapping takes
precedence and stale snapshots cancel the drag. Candidate enumeration is
bounded and walks compiled geometry during drag; it does not imply ray-based
or perspective target construction. The current X/Z work surface has a
separate bounded, process-local orthographic camera: initial fit, explicit
Frame all, pointer-anchored wheel zoom, and middle-button pan. It is view state
only, is not serialized into the project, and cannot mutate scene truth.
Parent-local gizmos, perspective navigation wiring, rotate/scale handles,
occlusion-aware ray construction, and edge/face/surface/normal projection
remain unclaimed.

The material graph and presentation catalog also have focused adversarial
round-trip and project-transaction tests. They validate typed graph topology,
unsupported-node reporting, texture-source references, camera/light records,
schema migration, dangling source references, and journal behavior.

CONDITION assets can now be explicitly bound to existing scene objects from
the native authoring instrument. The human-only action checks project and
authoring revisions, rejects missing assets/objects, and uses the project
command journal with undo/redo. It supplies the native link needed for source
attenuation and acoustic-zone guides without adding runtime ownership or
simulation.

CONDITION's embedded authoring codec has schema-3 audio-source and
acoustic-zone records. They bind a source to an owner asset and reviewed/current
imported clip evidence, preserve bounded gain/pitch/attenuation/bus/occlusion/
reverb and normal-state/familiarity intent, and represent bounded box/sphere
zones with semantic room identity. Native CONDITION controls append these
records through the existing project command and journal path. WAVE intake now
checks bounded RIFF framing, uncompressed PCM/IEEE-float formats, alignment,
sample limits, and finite float samples; it records only a content digest,
content-addressed logical locator, and inspected metadata. The absolute source
path and sample bytes are not persisted or included in a `.carto-condition`
package. Imported evidence remains unreviewed until an explicit authoring
action; stale evidence cannot be reviewed or published. Audio descriptors
support explicit enabled/disabled fixture state. Decoded samples and
mixer/playback state remain outside authored project truth. The native
Windows shell can transiently decode and audition a three-second excerpt after
rechecking the selected file against the current source digest; the excerpt and
absolute path are not persisted. Native min/max distance rings, sphere/box zone
volume handles, and per-zone priority labels now edit through revision-checked
project commands; the authoring panel shows bounded conservative overlap
candidates and stored priorities without simulating runtime precedence.
The generated synthetic acoustic QA fixture now exercises WAVE metadata and
digest binding, `.carto` save/reopen, scene-object mapping, and consumer
admission. It is explicitly synthetic and does not validate production audio
or runtime acoustics. Pointer-drag visual acceptance and production-fixture
audio validation remain open; the Windows file-picker capture helper failed
during this turn's native smoke attempt.

Schema 3 also persists effect-source intent, bounded deterministic particle
emitters, and field-influence bindings. Every influence captures the exact
project field identity, revision, and digest; preview refuses stale or missing
field inputs and out-of-coverage sampling rather than substituting values. The
native CONDITION workspace can author these records and preview exact normal or
disabled states as a bounded local-space 2D point display. Preview output is
ephemeral and does not mutate the project or create a runtime pool. Wind,
thermal, and drainage use authored directional acceleration; avoidance reverses
that direction; turbulence adds bounded deterministic perturbation. This is not
a full vector-field solver and does not claim collision, fluid coupling, volume
rendering, or integration with CONDITION game runtime. Schema-1 and schema-2
CONDITION documents migrate with empty new sections. Schema 4 adds source-only
vegetation scatter recipes over exact moisture, shade, disturbance, species
suitability, and elevation fields. The native CONDITION panel authors the
recipe against vegetation-class assets, previews a bounded deterministic point
set, and marks it stale when its authoring or any source field changes. Missing,
wrong-semantic, stale, or uncovered field samples and candidate/output overflow
fail closed. The preview is not persisted as placement data and does not create
scene instances or invoke donor/runtime code. Schema-3 documents reopen with no
scatter recipes. Schema 5 adds typed branchable Safe Place and surface-state
deltas with explicit source before-values, parent-branch identity, and
applicability notes. The native workbench creates nested Safe Place alternatives
and surface-state alternatives for occupancy, maintenance, damage, and wetness.
It projects exact source-state values, compares branches against their common
ancestor, marks sibling edits to the same property as contested, and refuses
stale before-values. Application snapshots expose authored surface-state values
to the panel without granting it direct mutation access. Source records are
never overwritten: branches are review data, not runtime state selection or
automatic reconciliation. Schema-4 documents reopen with no state branches.

Terrain source authoring now builds on the persisted spatial-field contract.
`terrain_elevation` and related terrain-analysis semantics were appended
without renumbering existing semantic values. The native authoring panel can
create multiple fields and assigns terrain fields canonical
`terrain.tile.x<X>.z<Z>` identities. `carto::terrain` derives bounded LOD mesh
vertices/indices, normalized normals, per-cell slope, navigation passability,
and an optional water-level wet/dry mask from that exact source. Local rebuilds
require a dirty receipt binding both the previous and resulting source
revision/digest; malformed or skipped revisions fail closed. Derived products
are not project truth and are not persisted. `to_bathymetry_field` adapts the
source to Cartographer's hydro contract only when given an explicit coordinate
frame; it averages each four-vertex terrain cell and copies the exact source
identity, revision, and digest into the hydro binding. The resulting wet/dry
mask matches the terrain cell mask, and edits invalidate previously derived
hydro products. This is a deterministic source adapter, not runoff simulation,
water-surface authoring, or runtime integration. `ApplicationSession` exposes
a read-only terrain-preview operation; the native CONDITION workbench displays
terrain relief and overlays the derived, source-bound triangle mesh on the
central plan work surface without advancing project or field revisions. The
overlay binds exact field identity/revision/digest, chooses the finest mesh
under a 16,384-triangle draw budget, and is suppressed when source evidence is
stale. It creates no scene object, selectable face, collision, navigation, or
runtime authority; this is not committed terrain placement or rendered 3D
visual acceptance.

Wind `wind_u` and `wind_v` scalar sources can also be paired into a derived
vector visualization when both fields have identical dimensions, origin, and
cell spacing. The product binds each exact source revision and digest, samples
at most a 16 × 16 grid, and is drawn as U/V arrows over the native X/Z plan
surface. Either changed or missing source suppresses the overlay. This is a
read-only visualization only; there is no field composition, vector solver,
wind simulation, or persisted derivative.

## Acceptance gates

1. `cartographer_verification_donor_promotion_tests` passes deterministic
   receipt and ledger round trips.
2. Tampering, duplicate promotion IDs, invalid commit identities, absolute
   paths, private/token-shaped text, missing absence reasons, unsorted evidence,
   and invalid promotion states fail closed.
3. The normal headless build links the verification contract without adding a
   donor repository, runtime library, network dependency, or private source
   root.
4. `cartographer_terrain_tests` proves tile identity parsing, source round-trip,
   bounded LOD boundaries, analysis products, incremental/full rebuild parity,
   and stale-receipt refusal.
5. `cartographer_application_tests` proves the native terrain-analysis query
   reads the current source and does not mutate project or field revisions.
6. `cartographer_condition_authoring_tests` proves WAVE metadata bounds,
   malformed/container/float-sample rejection, content provenance, no path or
   sample persistence, explicit review gating, and journal undo/redo.
   The same suite verifies effect schema migration, exact field provenance,
   deterministic state previews, and non-mutation under preview. Vegetation
   coverage includes schema-1/2/3 migration, exact semantic/revision/digest
   bindings, deterministic preview/save-reopen parity, full-coverage refusal,
   density and candidate caps, disabled recipes, stale-field invalidation, and
   journaled add/undo/redo through the application/UI path.
   State branch acceptance covers schema-1 through schema-4 migration, parent
   ancestry and cycle rejection, exact Safe Place and surface-state targets,
   numeric and categorical before-values, baseline and sibling comparison,
   contested conflict marking, stale-source refusal, save/reopen parity, and
   project-command undo/redo.
7. The existing public-boundary audit remains the release gate for tracked
   archives, specifications, credentials, workstation paths, and reachable
   history.

## Explicit non-claims

This tranche does not claim external runtime integration, game/runtime state,
ownership of third-party geometry implementations, hosted service access,
AI-driven mutation authority, or a private hosted promotion service. It also does not
make a licensing or patentability determination; the receipt only preserves
the owner-reviewed basis supplied for a particular public decision.

## Cross-repository impact

| Product | Direct impact | Contract risk |
| --- | --- | --- |
| RegOS | none; no governed state transition or domain mutation is performed | none introduced by this module |
| IDA_Training | none; no dataset, prompt, weight, or eval state is read | none |
| `hidden_canopy_bot` | none; no API or command surface is changed | none |
| `Ask_IDA` | none; no query or authority path is changed | none |

The module is evidence-only, so org scope, role tier, human approval, domain
events, and timestamp freshness are intentionally not modeled here. Those
checks belong to the governing product that consumes a promotion decision; a
Cartographer receipt cannot be used to bypass them.

## Tranche status

- Provenance/extraction ledger: complete for the public-safe contract.
- Donor implementation promotion: editor camera and bounded snap policy slices
  implemented independently; scene-wide visible-object candidate collection
  is implemented for vertices, edge midpoints, and face centers. The native
  work surface supports revision-bound root-object X/Z translation
  preview/commit, 0.25 m Ctrl grid snapping, and Shift geometry snapping with
  stable nearby feature identities and a visual target marker. A synthetic
  Synthetic room gesture fixture runs the same shared transform-snap policy,
  commits through the application command path, verifies undo/redo, and proves
  that source-revision drift blocks an active drag. Quad-mesh tests prove that
  compiler triangulation diagonals never gain editable snap identities. The
  snap-service, viewport-interaction, and application suites pass; the desktop
  target builds, and the full 107-test CTest suite passes with these acceptance
  additions. Native pixel-level drag acceptance remains open because
  Windows capture failed twice in this environment; production source geometry
  and visual captures, and orthographic/perspective UI acceptance, are not
  claimed. No perspective or parent-local gizmo is claimed. Plan-view
  framing, zoom, and pan remain covered by a bounded camera contract and
  connected to the native viewport. Ray-based surface targets and most of the
  interaction surface remain open.
- Material authoring: a typed, bounded, deterministic graph is persisted
  inside the governed material catalog; unknown nodes remain explicit and are
  not claimed as shader lowering. Texture samples must resolve to source
  texture records. The Material workspace is now available, with a native
  movable instrument for creating and editing base color, metallic, roughness,
  and clearcoat source factors. Changes require a human command, exact project
  and material revisions, and use normal journal/undo/redo semantics. The
  native instrument can also assign, replace, and clear a material binding on
  an authored face region, with project-revision checks and the same command
  journal. A region must already exist in the mesh's
  `condition.material_region` or `cartographer.material_slot` face attribute;
  the UI accepts its non-zero ID and the project validates it before commit.
  Bindings are keyed by mesh asset and therefore apply to every scene instance
  sharing that asset. The instrument's swatch is a source-color display, not a
  rendered material preview. The native graph editor can create starter graphs
  from selected PBR factors, add typed constant/parameter/surface/operator
  nodes, edit constant and parameter source values, rewire matching input
  sockets, and route typed graph outputs through a project-revision,
  graph-revision, and canonical-digest guarded human command. Proposed topology
  changes are validated before draft replacement; cycle and type errors fail
  without mutating the source. Save, reopen, undo, and redo preserve exact graph
  source. Materials can be explicitly bound to a graph identity, revision, and
  canonical digest; editing the graph makes the old binding visibly stale until
  a human rebinds it. Unsupported nodes remain visible as diagnostics and are
  retained. The support check is structural only, not GPU shader compilation.
  Safe deletion of disconnected supported nodes now records bounded tombstones;
  unsupported donor payloads remain undeletable until understood. Texture
  picking/intake, shader lowering, live viewport shading, automatic region
  authoring/discovery, and production material-board acceptance
  remain open.
- Audio/acoustic authoring: bounded source and zone intent records are persisted
  in CONDITION authoring schema 3 and can be added through native project
  transactions. Bounded WAVE metadata inspection creates pathless,
  content-digest-bound source evidence, which remains unreviewed until an
  explicit action. Current reviewed clip evidence and owner assets are required,
  and stale evidence blocks publication. The native work surface projects
  attenuation rings and box/sphere zone outlines only when their authored owner
  asset maps to an existing scene object; these are visual guides, not runtime
  occlusion or acoustic simulation. Bounded transient sample decoding and
  audition, revision-bound radius/volume handles, and conservative overlap
  candidate preview are implemented. A generated synthetic acoustic fixture
  now binds an initially unbound asset through the native UI command, checks
  project/authoring revision advancement, and verifies save/reopen plus package
  admission without embedding the WAVE bytes or workstation path. Native
  pointer-drag visual acceptance and production-recording audio validation
  remain open; runtime playback is intentionally out of scope.
- Effect authoring: native effect sources, bounded particle emitters, and
  exact field-influence bindings are persisted in CONDITION schema 3. The UI
  generates deterministic local-space previews for exact normal/disabled
  states; stale/missing fields fail closed and preview leaves project revisions
  unchanged. No runtime pool, field-vector solver, collision, or rendered world
  integration is claimed; full gas-station VFX acceptance remains open.
- Terrain, procedural, state, and remaining greenfield gaps: separate
  reviewable tranches; terrain has the source/derived-product slice below but
  is not yet a full world-authoring workflow. A first independently authored
  generic sweep source now preserves a convex profile, explicit reference
  frame, taper/cap policy, and exact centerline identity/revision; bounded
  deterministic generation allocates native `EditableMesh` IDs. Project schema
  22 persists the recipe and monotonic source lineage through journaled
  transactions, validates the centerline binding, and blocks deletion of a
  referenced path. The native Condition instrument authors regular radial
  profiles, preserves custom profiles without flattening, and provides a
  source-bound, preview-only wireframe. Synthetic pipe, railing-part, and
  branch fixtures cover the primitive; project/application tests cover
  save/reopen, stale bindings, deletion/recreation, undo, and redo. This is not
  yet an accepted CONDITION kit promotion: material/UV transfer, reviewed
  donor-family parity, any governed derived-mesh placement workflow, and
  desktop visual acceptance remain open (see
  `docs/verification/procedural-sweep.md`).
- Greenfield geometry: a standalone bounded cubic path source/evaluator now
  supplies a shared curve primitive persisted in project schema v20. The
  native CONDITION workbench creates and edits up to 16 exactly joined cubic
  segments paired with road intent through an atomic, revision-bound, undoable
  application command. New segments extend the previous endpoint along its
  final tangent; loaded joined sources preserve all segment control points.
  Curve and road source lineages persist deletion tombstones, so undo/redo and
  recreation advance rather than reuse source revisions. The editor refuses
  stale project snapshots, stale road/centerline bindings, and multiple road
  consumers of one centerline in this slice. Native editing is capped at 16
  segments although the source contract is more permissive. It now derives an
  ephemeral road/shoulder ribbon for the central work surface, bound to exact
  road and centerline digests, with bounded sampled-grade, sampled-curvature,
  and miter-join rejection. This is not an engineering certificate. Committed
  road mesh placement, terrain draping, frontage/driveway/sign generation,
  drainage geometry, moved-source relinking, orphan source-blob cleanup,
  side-by-side reimport review, and runoff/flow coupling remain open. Saved
  project OBJ/PLY/STL imports now bind their content-addressed source snapshot
  to the mesh asset and reimport changed geometry in place; the project source
  registry and reviewed plan-adoption core are implemented in schema 21, with
  limits recorded in [source asset registry](source-asset-registry.md) and
  [road surface preview](road-surface-preview.md).
  Terrain elevation source tiles, source-bound mesh,
  slope, passability, wet/dry derivations, and the explicit-frame hydro
  bathymetry adapter now have focused tests.
- Scene presentation: camera bookmarks and analytic light source intent are
  transactionally persisted and scene-object-bound; preview rendering,
  photometric calibration, resolved reference images, and runtime switching
  are not claimed.
- Spatial-field authoring: a Cartographer-owned, bounded 2D scalar source now
  persists in schema v20 with deterministic bilinear sampling and
  revision-checked radial paint/erase/smooth operations. Dirty receipts bind
  both parent and resulting digests for safe incremental derivatives. The
  native CONDITION workbench supports additional fields, terrain tile
  identities, an editable heatmap, revision-bound brushes, read-only terrain
  analysis, a source-bound terrain-mesh overlay, and a capped wind U/V vector
  overlay on the central work surface. Vector preview requires exact matching
  source grids and suppresses stale receipts without persisting or mutating.
  General field graphs/composition, vector solving, runoff/hydrology simulation,
  and other non-terrain field overlays remain unclaimed.
- Public/private boundary: preserved; no attached archive or private runtime
  payload is included in the source tree.
