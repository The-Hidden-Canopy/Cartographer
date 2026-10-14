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
0.25 m increment snap, and release submits one project-revision-bound transform
action through normal application history/journaling. Stale snapshots cancel
the drag. The current X/Z work surface has a separate bounded, process-local
orthographic camera: initial fit, explicit Frame all, pointer-anchored wheel
zoom, and middle-button pan. It is view state only, is not serialized into the
project, and cannot mutate scene truth. Parent-local gizmos, perspective
navigation wiring, rotate/scale handles, geometry-target snap
markers/application, occlusion-aware ray construction, and
edge/face/surface/normal projection remain unclaimed.

The material graph and presentation catalog also have focused adversarial
round-trip and project-transaction tests. They validate typed graph topology,
unsupported-node reporting, texture-source references, camera/light records,
schema migration, dangling source references, and journal behavior.

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
Pointer-drag visual acceptance and production acoustic-fixture validation
remain open.

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
and compares branches against their common ancestor; sibling edits to the same
property are marked contested, stale before-values fail comparison, and source
records are never overwritten. Branches are authored review data, not runtime
state selection or automatic reconciliation. Schema-4 documents reopen with no
state branches.

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
terrain relief and can report derived mesh/analysis counts without advancing
project or field revisions. It does not yet insert the generated mesh into the
model viewport.

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
   ancestry and cycle rejection, exact target/before-value checks, baseline and
   sibling comparison, contested conflict marking, stale-source refusal,
   save/reopen parity, and project-command undo/redo.
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
  is implemented for vertices, edge midpoints, and face centers. Ray-based
  surface targets and most of the interaction surface are not claimed. The
  native work surface supports revision-bound root-object X/Z translation
  preview/commit and optional 0.25 m Ctrl snapping; no perspective or
  parent-local gizmo is claimed. Plan-view framing, zoom, and pan are covered
  by a pure bounded camera contract and are connected to the native viewport;
  the rebuilt native shell and empty-workspace camera zoom have been smoke
  verified, while geometry-fixture visual captures and broader interaction
  acceptance remain open.
- Material authoring: a typed, bounded, deterministic graph is now persisted
  inside the governed material catalog; unknown nodes remain explicit and are
  not claimed as shader lowering. Texture samples must resolve to source
  texture records. Shader compilation and runtime admission remain unclaimed.
- Audio/acoustic authoring: bounded source and zone intent records are persisted
  in CONDITION authoring schema 3 and can be added through native project
  transactions. Bounded WAVE metadata inspection creates pathless,
  content-digest-bound source evidence, which remains unreviewed until an
  explicit action. Current reviewed clip evidence and owner assets are required,
  and stale evidence blocks publication. The native work surface projects
  attenuation rings and box/sphere zone outlines only when their authored owner
  asset maps to an existing scene object; these are visual guides, not runtime
  occlusion or acoustic simulation. Sample decoding, editable radius/volume
  handles, audition, overlap preview, and runtime playback remain open.
- Effect authoring: native effect sources, bounded particle emitters, and
  exact field-influence bindings are persisted in CONDITION schema 3. The UI
  generates deterministic local-space previews for exact normal/disabled
  states; stale/missing fields fail closed and preview leaves project revisions
  unchanged. No runtime pool, field-vector solver, collision, or rendered world
  integration is claimed; full gas-station VFX acceptance remains open.
- Terrain, procedural, state, and remaining greenfield gaps: separate
  reviewable tranches; terrain has the source/derived-product slice below but
  is not yet a full world-authoring workflow.
- Greenfield geometry: a standalone bounded cubic path source/evaluator now
  supplies a shared curve primitive persisted in project schema v20. The
  native CONDITION workbench creates and edits one cubic segment paired with
  road intent through an atomic, revision-bound, undoable application command.
  Curve and road source lineages persist deletion tombstones, so undo/redo and
  recreation advance rather than reuse source revisions. The editor refuses
  stale project snapshots, stale road/centerline bindings, and multiple road
  consumers of one centerline in this slice. Multi-segment editing,
  frames/offsets, terrain mesh placement into the scene, road mesh/frontage
  generation, asset registry/reimport, and runoff/flow coupling remain open.
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
  identities, an editable heatmap, revision-bound brushes, and read-only
  terrain analysis. Field graphs,
  vector composition, runoff/hydrology simulation, and a world viewport overlay
  remain unclaimed.
- Public/private boundary: preserved; no attached archive or private runtime
  payload is included in the source tree.
