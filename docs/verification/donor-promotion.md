# Cross-repository donor promotion

This note closes the first tranche of the cross-repository donor authoring
series. It records provenance and promotion decisions without importing donor
runtime code into Cartographer.

## Boundary

Cartographer remains the authoritative public authoring repository. A donor
reference is evidence, not a dependency. The public promotion ledger stores
only:

- a stable logical promotion ID and target module;
- a donor class: implementation, architecture pattern, or greenfield;
- a bounded evidence state (`not_applicable`, `private_review_pending`, or
  `privately_verified`), without repository names, commit IDs, or source paths;
- the semantic changes, removed authorities, tests, fixtures, license basis,
  and explicit non-claims; and
- a digest bound to the canonical receipt content.

The ledger rejects unsafe decision text, invalid evidence states, unstable
list order, duplicate promotion IDs, and accepted states without privately
verified evidence, implementation tests, and a named Cartographer fixture. It
has no file loader, network client, runtime handle, permission check,
state-transition authority, or mutation callback.

## Donor identity boundary

Repository names, object IDs, and source-file references for private or
owner-controlled donor repositories are not representable in the public
receipt schema. The schema records only whether a private evidence review is
pending or complete; it does not serialize the identity or contents reviewed.
Schema v2 rejects schema-v1 receipts and ledgers, because v1 could persist
identity-bearing donor fields. This document does not infer donor presence,
ownership, licensing, or promotion from an adjacent checkout, and it does not
publish local repository metadata.

## What this tranche implements

`DonorPromotionLedger` and `DonorPromotionReceipt` are public-neutral
verification contracts. Appending a receipt assigns its sequence and computes
the receipt digest. Reopening validates the digest and the full decision
history. A record can state that work is planned, adapting, architecture-only,
accepted, or rejected, but the state cannot silently imply a stronger result.

The accepted state is intentionally narrow: it requires an implementation
disposition, privately verified evidence, tests, and at least one named
Cartographer fixture. Greenfield work carries no donor-evidence state and can
never be represented as an accepted donor promotion.

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

Authored source assets can now be explicitly bound to existing scene objects from
the native authoring instrument. The human-only action checks project and
authoring revisions, rejects missing assets/objects, and uses the project
command journal with undo/redo. It supplies the native link needed for source
attenuation and acoustic-zone guides without adding runtime ownership or
simulation.

Native CONDITION authoring now also creates interaction anchors, prefab
assemblies, asset collections, and region-kit manifests from the neutral source
records. The application assigns identity, validates references and coverage,
records the prefab's source authoring revision, and commits each mutation
through the revision-checked, journaled project command. The CONDITION
instrument exposes bounded anchor geometry, multi-asset prefab composition,
named sockets, nested collection membership, and region-kit asset/prefab,
required-class, coverage, and validation controls. The CONDITION viewport now
renders and accepts the same selected-root translation handles as Model;
Ctrl-drag quantizes to the 0.25 m grid and Shift-drag requests geometry snap.
Empty-canvas left-drag pans the Model/CONDITION viewfinder without consuming
object/gizmo hits.
Save/reopen, stale-revision
rejection, malformed-input rejection, undo/redo, and non-reuse after undo are
covered by `cartographer_condition_authoring_tests`. Native edit/delete controls
now expose these four records through the same application command boundary.
Edits retain identity and use the captured project and authoring revisions;
stale drafts are blocked. Deletes require a separate confirmation and full
document validation protects referenced records. Backend action tests and a
native desktop build verify this boundary; screenshot-backed gesture acceptance
and the remaining donor-fidelity tranches remain open. This does not claim a
promoted donor runtime or full game integration.

CONDITION schema 8 binds each articulated part to the scene object, mesh asset,
and mesh revision it drives. The native instrument keeps the CONDITION owner
asset and moving scene-mesh selection separate, so a room source can own an
articulation whose hinge drives a door-leaf mesh. Human-only create/update
actions verify that binding against the live project; a changed or removed mesh makes the assembly
non-current and blocks publication until an explicit rebind. Schema-7
articulation remains readable but unbound and non-publishable. The Motel 8
fixture uses a separate door-leaf mesh, rejects a stale mesh revision without
advancing project history, and persists the exact binding through `.carto`
save/reopen. A native Viewfinder control now derives a bounded, read-only hinge
sample from that source and applies it only to a temporary render snapshot; the
project revision, serialized authoring, and operation journal remain unchanged.
Missing/stale bindings and unsupported hinge conventions fail closed. The
fixture checks the authored travel endpoints, hinge pivot, unchanged project
state, and stale-mesh rejection. Screenshot-backed visual acceptance, runtime
animation, and production door behavior remain unclaimed.

The embedded authoring codec has schema-3 audio-source and
acoustic-zone records. They bind a source to an owner asset and reviewed/current
imported clip evidence, preserve bounded gain/pitch/attenuation/bus/occlusion/
reverb and normal-state/familiarity intent, and represent bounded box/sphere
zones with semantic room identity. Native controls append these
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
The generated synthetic Motel 8 fixture now creates exterior, sign, and room
scene objects; binds all three CONDITION assets through revision-bound UI
commands; and authors an entry anchor, room prefab, production collection, and
region kit through the same transaction path. Imported WAVE evidence begins
unreviewed, blocks package construction, and is reviewed explicitly through the
UI command before admission. It verifies one-step project/authoring revision
advancement and operation receipts, undo/redo, stale-revision binding
rejection, and save/reopen of scene bindings and authored records. After
successful package admission, source invalidation makes publication readiness
false and package construction fail closed; undo restores admission and redo
reapplies the refusal. The fixture also checks that
`.carto` contains neither the workstation path nor WAVE bytes. It remains
synthetic: production audio, runtime acoustics, and pointer-drag visual
acceptance are still open.

Schema 3 also persists effect-source intent, bounded deterministic particle
emitters, and field-influence bindings. Every influence captures the exact
project field identity, revision, and digest; preview refuses stale or missing
field inputs and out-of-coverage sampling rather than substituting values. The
native authoring workspace can author these records and preview exact normal or
disabled states as a bounded local-space 2D point display. Preview output is
ephemeral and does not mutate the project or create a runtime pool. Wind,
thermal, and drainage use authored directional acceleration; avoidance reverses
that direction; turbulence adds bounded deterministic perturbation. This is not
a full vector-field solver and does not claim collision, fluid coupling, volume
rendering, or integration with the donor runtime. Schema-1 and schema-2
source documents migrate with empty new sections. Schema 4 adds source-only
vegetation scatter recipes over exact moisture, shade, disturbance, species
suitability, and elevation fields. The native panel authors the
recipe against vegetation-class assets, previews a bounded deterministic point
set, and marks it stale when its authoring or any source field changes. Missing,
wrong-semantic, stale, or uncovered field samples and candidate/output overflow
fail closed. The preview is not persisted as placement data and does not create
scene instances or invoke donor/runtime code. Schema-6 scatter recipes can use
either a vegetation-class asset or an exact Cartographer plant source
identity/revision/digest as their single species source. Preview requires a
bound recipe source to remain present and exact, and includes its receipt in
the preview digest; older recipes migrate as asset-backed. The native panel can
author either source mode. Schema-3 documents reopen with no scatter recipes.
Schema 5 adds typed branchable Safe Place and surface-state
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
a read-only terrain-preview operation; the native authoring workbench displays
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

Cartographer also owns a bounded `SignTextSource` and deterministic geometry
generator for physical signage. It produces separate panel and raised-letter
meshes in the Y-up X/Z authoring plane, with exact left/center/right layout,
line spacing, tracking, and bounded 5×7 block glyphs. Generated details expose
stable source-position glyph IDs, whitespace-delimited word IDs, and complete
face ranges for each glyph. Source revisions, tombstone lineage, canonical
snapshots, and generated-object receipts persist through `.carto`; human edits
and object creation use revision-checked journal commands. Motel 8, Pine Ridge
price-board, room-number-plate, and church-message-board fixtures exercise
deterministic generation, validation, application creation, and `.carto`
save/reopen. The Motel revision exercise also proves an existing object retains
its original source receipt after a wording update. The native
instrument offers source editing, a bounded wire preview, and explicit mesh
creation. The combined editable scene mesh carries a complete face-domain
`cartographer.material_slot` layer (1 = panel, 2 = raised lettering), so the
Material instrument can assign independent finishes without reconstructing the
text. Application acceptance verifies missing-slot and stale-revision rejection,
both assignments, `.carto` save/reopen with source receipt and face-slot
preservation, journal entries, and assignment undo/redo. Panel and lettering
are still one scene object. The integrated Motel 8 project fixture now saves
and reopens the exact revisioned sign source and generated-mesh receipt,
independent panel/lettering assignments, linear emissive factor and strength,
review-camera bookmark, marquee-light intent, and damage/repair state branches.
The native Material instrument can edit and commit source emissive factors;
this is source intent, not viewport emission output. Separate semantic child
objects, font assets/kerning, decals/SDF/emissive rendering, wear/damage material
variants, and screenshot-backed native gesture acceptance remain open.

## Acceptance gates

1. `cartographer_verification_donor_promotion_tests` passes deterministic
   receipt and ledger round trips.
2. Tampering, duplicate promotion IDs, abbreviated/full Git object IDs,
   repository-shaped path references, absolute paths, private/token-shaped
   text, missing absence reasons, unsorted evidence, and invalid promotion
   states fail closed.
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
   coverage includes schema-1 through schema-5 migration, schema-6 generic
   plant-source binding, exact semantic/revision/digest checks, deterministic
   preview/save-reopen parity, full-coverage refusal, density and candidate
   caps, disabled recipes, stale-field/source invalidation, and journaled
   add/undo/redo through the application/UI path.
   State branch acceptance covers schema-1 through schema-4 migration, parent
   ancestry and cycle rejection, exact Safe Place and surface-state targets,
   numeric and categorical before-values, baseline and sibling comparison,
   contested conflict marking, stale-source refusal, save/reopen parity, and
   project-command undo/redo.
7. `cartographer_sign_geometry_tests` proves bounded glyph generation,
   measured text fit (including tracking), alignment, deterministic word/glyph
   identities, and exact face-range coverage. `cartographer_sign_text_project_tests`
   proves source-lineage and receipt persistence/migration; the application
   suite exercises Motel 8, Pine Ridge, room-number, and church-message sign
   creation, stale-source refusal, save/reopen, undo/redo, and immutable
   historical receipts.
8. The existing public-boundary audit remains the release gate for tracked
   archives, specifications, credentials, workstation paths, and reachable
   history.
9. `cartographer_biological_authoring_tests` proves schema-1 migration without
   fabricated geometry, finite/range/overflow tessellation guards, deterministic
   editable proxy topology, exact source-revision validation, and stale-product
   refusal; `cartographer_biological_project_tests` proves profile save/reopen.
10. The explicit botanical runtime path is covered by
    `cartographer_private_runtime_client_tests`,
    `cartographer_generated_gltf_import_tests`, and the composed
    `cartographer_ui_tests`: no upload occurs before an explicit request;
    credentials and remote IDs stay out of request bodies and `.carto`; job
    revision/input/output digests are revalidated; unreceipted output and
    output bound to an outdated CONDITION authoring revision are rejected
    before project mutation; accepted output uses the human journaled import
    path and survives save/reopen plus undo/redo. These tests use a fixture
    transport and do not establish live-service availability.

## Explicit non-claims

This tranche does not claim game/runtime state, ownership of third-party geometry
implementations, deployed or available hosted service access, AI-driven mutation
authority, or autonomous promotion. It also does not
make a licensing or patentability determination; the receipt only preserves
the owner-reviewed basis supplied for a particular public decision.

## Cross-repository impact

Cartographer now has a narrow botanical runtime API client, an explicit
source-submission call, revision/digest-bound job and output receipts, and a
UI import boundary that admits generated glTF as a journaled human source
mesh. The project persists the source snapshot and input/output digests while
storing only a digest of the remote job identity; credentials, organization,
service URL, and remote artifact/job IDs are not serialized. Client
construction and project opening do not send data. The UI test composes submit,
poll, download, verification, import, save/reopen, and undo/redo through a
deterministic in-process transport; it does not call a live service. The
separate service and worker were tested in their own local worktrees, but no
deployment, production credential, or live endpoint is claimed here.

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
  additions. At this original checkpoint, native pixel-level drag acceptance
  was open after Windows capture failed twice. A later live observation
  verified grip drag, neighbor snap, and viewfinder pan, but no screenshot
  artifact or repeatable desktop gesture fixture was retained. Production
  source geometry and visual captures, and orthographic/perspective UI
  acceptance, remain unclaimed. No perspective or parent-local gizmo is claimed. Plan-view
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
  The native Material instrument imports bounded PNG/JPEG/TGA/DDS/EXR sources
  after container inspection, stores exact bytes in the project-local
  content-addressed blob tree, and creates a catalog record only after the
  application boundary re-reads and re-inspects the stored digest. Catalog
  records contain a project-relative digest locator and digest-derived generic
  display name; they do not persist the workstation path, original filename,
  or image-container metadata. A human can bind an imported source to the
  material base-color slot and choose source-default, linear, or sRGB
  interpretation. This is source intake and material intent only: there is no
  general pixel decoder, texture preview, GPU shader lowering, or live viewport
  shading. If the project command/journal commit fails after blob publication,
  an unreferenced content-addressed blob can remain; it is non-authoritative
  and currently has no project-local garbage collector.
  Safe deletion of disconnected supported nodes now records bounded tombstones;
  unsupported donor payloads remain undeletable until understood. Shader
  lowering, live viewport shading, automatic region authoring/discovery, and
  production material-board acceptance remain open.
- Audio/acoustic authoring: bounded source and zone intent records are persisted
  in the embedded authoring schema 3 and can be added through native project
  transactions. Bounded WAVE metadata inspection creates pathless,
  content-digest-bound source evidence, which remains unreviewed until an
  explicit action. Current reviewed clip evidence and owner assets are required,
  and stale evidence blocks publication. The native work surface projects
  attenuation rings and box/sphere zone outlines only when their authored owner
  asset maps to an existing scene object; these are visual guides, not runtime
  occlusion or acoustic simulation. Bounded transient sample decoding and
  audition, revision-bound radius/volume handles, and conservative overlap
  candidate preview are implemented. The synthetic Motel 8 integrated fixture
  checks the source-review gate; three scene bindings; UI-authored
  anchor/prefab/collection/kit records with revision receipts; transaction
  undo/redo; stale revision/source rejection; save/reopen; and package admission
  without embedding WAVE bytes or a workstation path. Native
  pointer-drag visual acceptance and production-recording audio validation
  remain open; runtime playback is intentionally out of scope.
- Effect authoring: native effect sources, bounded particle emitters, and
  exact field-influence bindings are persisted in embedded authoring schema 3. The UI
  generates deterministic local-space previews for exact normal/disabled
  states; stale/missing fields fail closed and preview leaves project revisions
  unchanged. No runtime pool, field-vector solver, collision, or rendered world
  integration is claimed. The application-backed Gas Station effect-source
  fixture now covers portable source/field records, deterministic active and
  disabled previews, stale-field refusal, and journal/save/reopen behavior;
  renderer-backed VFX presentation and production visual acceptance remain open.
- Terrain, procedural, state, and remaining greenfield gaps: separate
  reviewable tranches; terrain has the source/derived-product slice below but
  is not yet a full world-authoring workflow. A first independently authored
  generic sweep source now preserves a convex profile, explicit reference
  frame, taper/cap policy, and exact centerline identity/revision; bounded
  deterministic generation allocates native `EditableMesh` IDs. Project schema
  22 persists the recipe and monotonic source lineage through journaled
  transactions, validates the centerline binding, and blocks deletion of a
  referenced path. The native sweep instrument authors regular radial
  profiles, preserves custom profiles without flattening, and provides a
  source-bound, preview-only wireframe. Synthetic pipe, railing-part, and
  branch fixtures cover the primitive; project/application tests cover
  save/reopen, stale bindings, deletion/recreation, undo, and redo. This is not
  yet an accepted donor-kit promotion: material/UV transfer, reviewed
  donor-family parity, any governed derived-mesh placement workflow, and
  desktop visual acceptance remain open (see
  `docs/verification/procedural-sweep.md`).
  The protected procedural plant-growth implementation (generic species-source
  recipes, deterministic branch growth, and scatter-placement materialization)
  was excised from the public tree as protected field-growth content; the
  publication audit tripwires those names. The public CONDITION field-bound
  scatter preview keeps only its plant-source binding
  (identity/revision/digest) resolved against externally vouched source
  receipts, and the project schema no longer stores plant species sources,
  plant lineage, or plant instance receipts. Project schema 23 adds
  source-bound generic sign-text authoring and generated-mesh receipts,
  schema 24 persists road/centerline snapshots in road-surface object
  receipts, schema 25 persists biological authoring sources and lineage,
  and schema 26 adds biological proxy instance receipts.
- Greenfield geometry: a standalone bounded cubic path source/evaluator now
  supplies a shared curve primitive persisted in project schema v20. The
  native authoring workbench creates and edits up to 16 exactly joined cubic
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
  road mesh placement is now an explicit human command that rechecks current
  project, road, and centerline revisions/digests before producing editable
  road/shoulder geometry with material regions and world-scale UVs. Schema 24
  persists the bounded road and centerline snapshots plus generated-mesh digest
  in an object receipt; undo/redo and save/reopen are covered. The integrated
  Rural Road/Forest fixture also exercises terrain cut/fill, wind-vector
  preview, vegetation scatter, portable save/reopen, and excludes private product
  and runtime identity strings from the `.carto` payload. Terrain
  draping, frontage/driveway/sign generation, drainage geometry, moved-source
  relinking, orphan source-blob cleanup, side-by-side reimport review, and
  runoff/flow coupling remain open. Saved
  project OBJ/PLY/STL imports now bind their content-addressed source snapshot
  to the mesh asset and reimport changed geometry in place; the project source
  registry and reviewed plan-adoption core are implemented in schema 21, with
  limits recorded in [source asset registry](source-asset-registry.md) and
  [road surface preview](road-surface-preview.md).
  Terrain elevation source tiles, source-bound mesh,
  slope, passability, wet/dry derivations, and the explicit-frame hydro
  bathymetry adapter now have focused tests.
- Scene presentation: camera bookmarks and analytic light source intent are
  transactionally persisted and scene-object-bound. The native Inspector now
  keeps camera/light edits in an explicit revision-bound draft and commits the
  catalog through one undoable, journaled application action; stale project
  generation/revision drafts require reload. Preview rendering, photometric
  calibration, resolved reference images, and runtime switching are not
  claimed.
- Spatial-field authoring: a Cartographer-owned, bounded 2D scalar source now
  persists in schema v20 with deterministic bilinear sampling and
  revision-checked radial paint/erase/smooth operations. Dirty receipts bind
  both parent and resulting digests for safe incremental derivatives. The
  native authoring workbench supports additional fields, terrain tile
  identities, an editable heatmap, revision-bound brushes, read-only terrain
  analysis, a source-bound terrain-mesh overlay, and a capped wind U/V vector
  overlay on the central work surface. Vector preview requires exact matching
  source grids and suppresses stale receipts without persisting or mutating.
  General field graphs/composition, vector solving, runoff/hydrology simulation,
  and other non-terrain field overlays remain unclaimed.
- Public/private boundary: preserved; no attached archive or private runtime
  payload is included in the source tree.

## Continuation evidence — 2026-10-16

The biological authoring acceptance now includes generic canid and human-proxy
source-record fixtures. They round-trip structures, typed fields, attachments,
layers, authored stages, and representation receipts; source revision advance
preserves authored identity, while same-revision source replacement is
rejected. These fixtures are metadata/provenance evidence only. They do not
claim generated editable proxy meshes, native project transactions, sculpt or
multires integration, or visual acceptance.

At this 2026-10-16 checkpoint, the publication audit still reported protected
field-growth markers and the botanical source boundary was unresolved. That
historical result is superseded by the later source excision, narrow runtime
contract, and exact push-ref audit recorded in subsequent continuation
evidence. A local recovery-stash finding remains a local-only review item and
is not evidence that the content exists in a pushable ref.

## Continuation evidence — biological project persistence

Cartographer project schema 25 persists multiple biological authoring
documents in separate source and lineage tables. Admission is through
`ProjectTransaction`; the project stores the exact canonical biological bytes
and their SHA-256, checks embedded namespace/asset identity, preserves the
source's imported starting revision, requires exact-next revisions for edits,
and runs the biological reimport-continuity gate before publication. Deletion
requires the exact source revision and retains a tombstone. Schema-24 project
migration leaves biological tables empty rather than fabricating sources.
Canid and human-proxy source records now exercise `.carto` save/reopen and
journaled update/delete/recreate behavior.

The CONDITION donor repository was inspected read-only. It contains unrelated
local edits, so no donor files were modified. No reusable-code license was
found alongside the candidate generic botany implementation; the separate
CC0 human-data notice applies to the listed graphical assets, not automatically
to the surrounding source code. Accordingly, no donor implementation or data
was copied into Cartographer in this pass. The project-persistence slice is
a native integration of Cartographer's generic source contract, not a claim
that third-party or private runtime code was promoted.

Schema 2 now permits explicit, bounded ellipsoid proxy profiles on biological
structures. A clean-room compiler generates deterministic editable triangle/
quad meshes, binds each product digest to the canonical authoring digest and
source identity/revision, and rejects stale or altered products. Its generated
geometry is explicitly a coarse proxy; it does not establish anatomical
fidelity, sculptability, scene placement, or donor parity. Schema-1 migration
does not invent geometry. Project persistence tests also reopen the exact source
profile and regenerate its proxy.

`ApplicationSession` now accepts biological sources through a human-only
project command, exposes source metadata in snapshots, and returns source-bound
proxy previews only for the captured project revision. Undo and redo preserve
monotonic source lineage; preview does not change project revision, scene
objects, or journal history. Application tests cover initial import, an updated
profile, stale-preview refusal, undo/redo, and `.carto` reopen.

The CONDITION workbench now exposes a bounded source editor for selecting,
creating, and updating biological authoring records, with digest generation
from the canonical payload and an explicit read-only proxy wireframe preview.
The Vulkan/ImGui desktop target compiles. The full headless suite passes
116/116. This does not yet establish interactive GUI acceptance because no
native application window was exposed to the UI inspection tool in this run.

## Continuation evidence — source-receipted proxy materialization — 2026-10-16

Project schema 26 adds a required biological proxy instance receipt containing
the exact canonical source snapshot and digest, source namespace/asset/revision,
structure identity, compiler product digest, object identity, and mesh asset.
Admission recomputes the proxy product and verifies that it still matches the
receipt; project validation also requires the receipt to remain attached to the
same scene object and mesh while retained source lineage is at least as new as
the receipt's source revision. The serialized table and aggregate snapshot
bytes are bounded.

The application commit is explicitly human-only and fenced by project
revision, source revision, source-payload digest, structure, and preview product
digest. It recompiles before using the regular mesh command. Duplicate and
undo/redo preserve receipt identity and mesh linkage, and save/reopen round trips
the receipt. Tests reject stale project/source/payload/product inputs, corrupt
product digests, and a missing schema-26 receipt table.

The public project-bound SculptStack path preserves authored records and the
biological source receipt across save/reopen. The viewport remains on the base
mesh because private geometry evaluation is outside the public target graph.
This verification note no longer claims evaluation or hierarchy output.
The full headless suite passes after this integration. Still open: evidence-backed
anatomical fidelity and screenshot-backed interactive GUI acceptance. At this
checkpoint, the broad local-history audit included recovery refs and reported
protected markers; consult the newer exact-ref audit evidence below for push-scope
status. No donor runtime or donor repository identity was added to Cartographer by
this tranche. These gaps prevent claiming the donor-promotion series complete.

The synthetic CONDITION wildlife/human fixture now runs through the application-backed
UI fixture target. It creates three separate source records with fur, hair, and feather
layer semantics, materializes each through the project command path with undo/redo
checks, records one authored SculptStack, and verifies `.carto`
reopen plus source-payload negative-space assertions. It emits an end-to-end fixture
duration as a trace note rather than a performance benchmark. It deliberately makes no
claim of donor geometry parity or anatomical fidelity.

## Continuation evidence — Pickup source and consumer admission — 2026-10-16

`cartographer_condition_pickup_fixture_tests` now exercises the eighth named
CONDITION fixture through source export/import, content-addressed source storage,
metadata-only reimport, human-reviewed adoption, stale-plan refusal, and
undo/redo. Undo restores the exact prior source digest while advancing the
monotonic generation from 2 to 3; redo restores the reviewed digest at
generation 4. The fixture authors separate vehicle components, face material
regions and PBR materials, camera/light intent, source-bound wheel/tailgate
articulation, CONDITION variants and damage state, and verifies hinge preview
does not mutate project or operation history.

Stale CONDITION evidence blocks package construction. A human reconciliation
restores source-current publication, after which `.carto` save/reopen retains
project-relative source locators and exact authoring lineage. The reopened
package is admitted by the downstream consumer using the same authoring digest.
Serialized project and package bytes reject donor/runtime marker strings and
workstation paths. This is a synthetic coarse mechanical blockout; the reported
fixture wall time is trace data, not a performance benchmark. Visual capture,
production vehicle fidelity, and render timings remain unverified. This closes
the package handoff evidence for this fixture only; it does not complete the
cross-repository donor-promotion program.

## Continuation evidence — route landmark visibility — 2026-10-16

The authoring-observation kernel now evaluates landmark visibility from an
ordered set of explicit route stations. Each station receives an exact
point-sightline blocker receipt and cumulative route distance; the query
supports station-specific self-occlusion exclusions, caps sample count, route
length, per-sightline geometry, and total triangle work, and rejects stale or
over-budget results without returning a partial mask. The UI evaluates against
the exact current viewport snapshot and does not mutate project revision or
operation history. The Rural Road/Forest application fixture repeats the
query for deterministic parity and verifies stale-revision refusal after
undo. A bounded row-major landmark viewshed now applies the same exact
snapshot and fail-closed contract to caller-supplied target positions and
elevations. The native CONDITION workbench can build that grid from up to
32 × 32 exact vertices of a current terrain preview, binds terrain
identity/revision/digest separately from project revision, accepts an explicit
world-space observer, and draws a read-only visible/occluded overlay over the
X/Z work surface. Grid construction rejects altered samples and stale products;
the overlay disappears on project, terrain, or generation drift. The Rural
Road/Forest fixture checks deterministic grid receipts, visible-cell
aggregation, and stale refusal. No terrain heights are interpolated or
invented by this adapter. Continuous route interpolation, terrain resampling
between authored vertices, renderer visibility parity, screenshot-backed
gesture acceptance, and production-scene visual acceptance remain open.

## Continuation evidence — Pine Ridge price-board fixture — 2026-10-17

`cartographer_condition_authoring_ui_fixture_tests` now emits
`condition-pine-ridge-gas-price-board.carto` beside the integrated Motel 8
artifact. The fixture creates two editable instances from source revisions 1
and 2, checks both exact serialized snapshots and SHA-256 receipts, rejects
stale source creation without mutation, and assigns clean and weathered panel
and lettering materials to four face-region keys. It exercises assignment
undo/redo and native application save/reopen. A serialized negative-space
assertion rejects donor repository/commit and runtime-endpoint markers. The
fixture duration is trace output only, not a performance benchmark. No
renderer-backed price-legibility or screenshot claim is made. Pine Ridge
source-contract acceptance is advanced; the remaining named production
fixtures, visual captures, and donor-license classification remain open. The
botanical runtime boundary now has a locally tested explicit client and
receipt-checked import path; live service deployment and availability remain
unverified and are not implied.

## Continuation evidence — Safe Place state history — 2026-10-17

The same fixture runner emits `condition-safe-place-state-history.carto` with
nested privacy and wetness branches. `UiController::add_condition_state_branch`
now materializes each candidate against the immutable source baseline and its
parent branch before dispatch, rejecting a stale before-value before the
project command/journal can mutate state. The fixture verifies unchanged source
values, no revision/count change on rejection, branch comparison receipts,
save/reopen parity, and ordered undo/redo. It does not activate runtime state
or claim rendered temporal appearance. The donor-promotion program remains
incomplete; other named fixtures and screenshot-backed visual acceptance are
still required.

## Continuation evidence — General Store source-production fixture — 2026-10-17

`cartographer_condition_authoring_ui_fixture_tests` now emits current and
stale-source General Store `.carto` artifacts. The project binds its exterior,
sign backing, and interior CONDITION assets to exact scene object IDs; authors
a revision-1 physical frontage sign with independently assigned panel and
lettering materials; and retains authored review-camera and practical-light
intent. Three distinct synthetic WAVE clips become reviewed, content-digested,
pathless source evidence for refrigerator, freezer, and fluorescent audio
descriptors, with separate interior and cooler acoustic zones. There is no
runtime mixer or playback authority.

The fixture round-trips the current project through `ProjectDocument` and the
native application, checks sign source snapshot/digest and material/presentation
records, and exercises acoustic-zone undo/redo. Invalidating one reviewed clip
blocks package construction; the stale project remains stale after save/reopen;
undo restores the exact current evidence and redo re-applies the fail-closed
state. Negative-space checks reject workstation paths and donor/runtime marker
strings. Reported fixture duration is not a performance benchmark. Geometry is
a coarse synthetic blockout and the WAV data is a generated tone: this does
not establish visual fidelity, actual playback, acoustics, render timings, or
screenshot acceptance. The General Store source-contract gate advances, but
the donor-promotion program remains incomplete.

## Continuation evidence — Rural Road/Forest source-production fixture — 2026-10-17

`cartographer_condition_authoring_tests` now retains
`condition-rural-road-forest.carto` under
`build/desktop-vulkan-release/tests/condition-authoring-fixtures/` for each
fixture run. The project contains a revision-2 rural road with roadside-ditch
intent, a separate pedestrian trail/secondary path, a terrain tile, authored
forest scatter and wind fields, deterministic landmark visibility evidence,
and source-bound mesh receipts for both paths.

The fixture advances the primary road and centerline after generating road and
terrain previews, then proves the stale road commit is rejected without
revision/history/object mutation and that the old terrain preview no longer
matches current sources. Fresh products commit against current revisions and
survive `.carto` save/reopen; the artifact also survives ordered undo/redo
checks. Serialized bytes reject workstation temp identity and private product or runtime identity
markers. This is
structural source-production evidence: it does not claim a drainage solver,
visual capture, production-scene fidelity, or performance benchmark. The
cross-repository donor-promotion program remains incomplete.

## Continuation evidence — Conifer-family source-production fixture — 2026-10-17

The `cartographer_condition_authoring_tests` runner now emits current and
stale-source `condition-conifer-family` `.carto` artifacts under the build-tree
fixture directory. The fixture authors an asset-bound field-driven
scatter recipe (moisture, shade, disturbance, suitability, and terrain
elevation fields) and derives a deterministic, read-only placement preview
with exact field receipts and a preview digest.

The current artifact survives native application save/reopen, scatter
undo/redo, and preview regeneration after undo/redo. Both serialized
artifacts reject temporary source paths and private donor/runtime markers.
This establishes deterministic field-bound source lineage, not final
artist-authored species quality, production foliage rendering,
wind/collision, visual capture, or performance. The broader donor-promotion
program remains incomplete.

## Continuation evidence — Gas Station effect-source promotion — 2026-10-17

`cartographer_condition_authoring_ui_fixture_tests` now emits current and
stale-field `condition-gas-station-effects` `.carto` artifacts. The current
project contains three CONDITION effect sources for compressor steam, runoff,
and service-vehicle smoke; each has a bounded deterministic emitter and an
exact revision/digest-bound wind, drainage, or turbulence field. Active previews
are repeatable, disabled operating states are empty, and preview leaves project
revision and operation history unchanged. Save/reopen preserves the authored
records, field receipts, and preview digests. Revising the wind field blocks
the stale preview before and after save/reopen; journal undo/redo keeps field
revisions monotonic and does not silently revive the old binding. Serialized
artifacts reject donor/runtime identity markers. This closes source-contract
acceptance for the named synthetic VFX slice only; there is no runtime particle
pool, renderer integration, production VFX fidelity, screenshot, or benchmark
claim, so the donor-promotion program remains incomplete.

## Continuation evidence — occlusion-safe instrument capture and boundary scan — 2026-10-17

Workbench drag hit-testing now includes each native instrument's full window
bounds. When windows overlap, the topmost window owns the pointer: a lower
panel's visible drag region cannot capture input through the panel covering it.
The desktop supplies current ImGui window geometry and display order, and the
input fixture checks covered grips, overlapping grips, standard/narrow controls,
placement, snap math, and camera navigation. The native desktop and focused
input/UI/camera targets build and pass, but no targetable native application
window was available for screenshot-backed gesture acceptance.

The read-only public-boundary scanner now combines its secret, workstation-path,
and protected-content searches into one `git grep` invocation per historical
commit instead of three. All 18 adversarial Python audit tests pass, including
deleted-history detections and secret-value suppression. The current repository
audit still fails closed on the owner-protected field-growth implementation
markers in Cartographer source and tests; no green public-release clearance is
claimed. No donor runtime or donor repository identity was added by this work.

## Continuation evidence — source-bound curve visibility — 2026-10-17

The observation kernel now samples a current authored cubic centerline at a
bounded per-segment density, removes duplicate shared joints, and binds the
result to curve identity/revision/digest separately from the project/viewport
revision. Unit tests cover deterministic results, digest and revision drift,
altered sample positions and endpoint exclusions, invalid targets, and sample
budget boundaries. The Rural Road/Forest fixture checks exact source receipts,
deterministic repeated observations, stale expected-revision refusal, and no
project or operation-history mutation. The CONDITION workbench requires an
explicitly enabled target, renders sample sightlines as a read-only overlay,
and suppresses the overlay when the source, project, or project generation
drifts. This is geometry-only visibility against the current viewport scene;
terrain interpolation, rendered visibility parity, silhouette/sign/interaction
acceptance, and screenshot-backed visual review remain open. The broader
donor-promotion program remains incomplete.

## Continuation evidence — terrain-product resampling — 2026-10-17

The terrain viewshed adapter now produces its bounded uniform grid by sampling
the current `TerrainTileProduct` triangulation between vertices. Height is
piecewise planar using the product's actual `a-c-b` / `b-c-d` diagonal, not a
bilinear approximation, so analysis targets agree with the authored preview
mesh. The receipt binds terrain identity, revision, source digest, project
revision, and LOD step; validation recomputes every sample and rejects altered
targets, source drift, project drift, and a different derived LOD product.
Adversarial tests include a non-planar peak whose intermediate sample differs
from bilinear interpolation. The native workbench labels the analysis as a
sampled terrain surface and preserves its read-only behavior. This closes only
the between-vertex terrain-resampling gap for the native geometry viewshed;
renderer visibility parity, silhouette/sign/interaction acceptance, visual
captures, and the remaining donor-production gates are still open.

## Continuation evidence — Motel 8 durable current/stale fixture parity — 2026-10-17

The integrated Motel 8 fixture now saves its current source state after the
journaled invalidation/undo sequence and reopens that `.carto` through the
native application before producing the separate stale-source artifact. It
then redoes invalidation, saves the stale artifact independently, and verifies
that reopen remains fail-closed and non-publishable. This fixes fixture
production ordering only; journal-tail/snapshot validation is unchanged. The
focused `cartographer_condition_authoring_ui_fixture_tests` CTest passed. No
renderer, visual-fidelity, or donor-runtime claim is made, and the broader
donor-promotion program remains incomplete.

## Continuation evidence — explicit botanical runtime client and promotion boundary — 2026-10-17

The native CONDITION workspace exposes botanical recipe entry, an explicit
submit action, status refresh/output fetch, and a separate review/import action.
Configuration is read from process environment; values are not displayed. No
request is sent on client construction or project open, and submit is disabled
without a saved project, valid runtime configuration, and a current authoring
revision. The generated output remains untrusted until a human invokes import.

The UI promotion test composes the runtime client and native import boundary
with a deterministic in-process transport: explicit source submit, job polling,
input/output digest verification, output download, journaled import,
save/reopen, and undo/redo. Unreceipted and stale-revision outputs are rejected
before project mutation, including when CONDITION authoring advances while the
job is running. Project bytes retain the source and output provenance
but exclude remote job/artifact IDs, organization, credentials, and service
URL. The fixture does not call a deployed service and cannot establish hosted
availability or runtime security/performance.

At this 2026-10-17 checkpoint, the Conifer-family fixture proved deterministic
field-bound scatter preview and stale-source rejection only. The local native
botanical compiler, represented-source controls, and source-bound placement
commit described below had not yet been added. Live-service,
screenshot-backed GUI, production renderer fidelity, remaining named fixtures,
and donor-license review remained open.

## Continuation evidence — native botanical source and placement integration — 2026-10-18

Cartographer now has a local, deterministic botanical source compiler for the
hero/reduced/card/cluster/far-proxy representations. The desktop CONDITION
workspace exposes source/representation controls and stores the exact
project-local source-registry identity, generation, and digest in the scatter
recipe. A source-bound recipe uses that receipt as its sole species authority;
the legacy species-asset ID is zero.

The application binds preview identity to the enclosing project revision and
recomputes the preview at commit. Human-only commit validates the exact native
mesh receipt and root prototype, placement bounds and stable IDs, then creates
the complete instance batch with yaw/scale and per-object schema-v28 source,
project, authoring, field, and preview provenance. Stale revisions, mismatched
sources/prototypes, duplicate placements, invalid transforms, and batch
failure are rejected without retaining partial objects. The transaction is
undoable/redoable, persists through save/reopen, and is recorded in the native
operation ledger. The private runtime submit/refresh/import flow remains a
separate explicit operation; neither project open nor the local compiler calls
it, and the public project receipts contain no runtime endpoint, credential,
organization, or opaque remote job identity.

Evidence: `cartographer_condition_authoring_tests`,
`cartographer_application_tests`, `cartographer_botany_tests`, and
`cartographer_botany_ui_tests` passed; the Vulkan/ImGui `cartographer_desktop`
target configured, compiled, and linked; the C++ public-boundary test passed;
`tests/public_boundary_audit_tests.py` passed 25/25; and the exact-`HEAD`
public-boundary audit reported `PUBLIC_BOUNDARY_PASS`.

This advances the native source/placement slice only. Material-role transfer,
artist-reviewed botanical quality, visible rendered fidelity, GPU timing,
screenshot-backed gesture acceptance, live-service behavior, and completion
of the Conifer fixture remain unverified. The broader donor-promotion program
remains incomplete.

## Continuation evidence — source-bound botanical UV data — 2026-10-18

The local botanical compiler now attaches a normalized, corner-domain
`cartographer.uv0` set to hero, reduced, card, cluster, and far-proxy products.
Tube circumference and ellipsoid longitude wrap are represented as corner
seams without coordinates outside `[0,1]`. The version-2 botanical product
digest includes the UV-set descriptor and values; source validation compares
the complete attribute payload, so a structurally valid UV edit is rejected as
divergent from the exact source product.

Native source staging writes per-corner OBJ `vt` references. The bounded OBJ
importer resolves positive and relative indices, retains shared-vertex seams,
rejects malformed or partial UV coverage, and carries UVs through its existing
directed-edge repair. OBJ export retains the active render UV set while
triangulating polygons and reports that materials, extra UV sets, and custom
normals are not exported. The native path verifies UV equality before commit;
the save/reopen fixture checks exact UV values and active-set association.
OBJ source records identify importer version 2; PLY, STL, and generated-glTF
remain restricted to version 1.

Evidence: the isolated Release build compiled the botanical, I/O, application,
and UI targets. Seven focused CTests passed: botanical generation, native
botanical save/reopen and stale-source handling, OBJ UV seam/import/export and
repair boundaries, CONDITION authoring, application transactions,
generated-glTF import, and public-boundary C++. The adversarial Python
public-boundary suite passed 25/25. The current `HEAD` plus worktree scan and
ignore-policy check reported `PUBLIC_BOUNDARY_PASS` after the importer-version
gate change.

At this UV-only checkpoint, material-role transfer had not yet been verified.
The follow-on evidence below closes the face-region transfer and native catalog
binding slice; rendered foliage appearance, screenshots, GPU timing, and final
Conifer-scene fidelity remain open in the broader donor-promotion program.

## Continuation evidence — botanical material-role transfer — 2026-10-18

All five deterministic botanical products now carry a complete
`condition.material_region` face-domain layer. Stable region ID 1 means bark;
stable region ID 2 means foliage, including foliage cards, clusters, and the
far proxy. The canonical botanical digest now binds these IDs, role ranges, and
the complete face layer. Source validation rejects structurally valid but
altered region payloads.

Native OBJ staging writes the source marker and bounded per-triangle
Cartographer region records. The importer rejects duplicate, missing,
malformed, zero, or out-of-range IDs and restores the layer after polygon
triangulation. OBJ importer version 3 is admitted for OBJ only; PLY, STL, and
generated glTF remain version 1. Geometry, active UV, and material-region
digests must match before the native source-backed mesh is committed, and the
material digest is stored as its own source-domain receipt. Generic OBJ
materials and shader assignments remain intentionally outside this transfer.

The ordinary native material-catalog action accepts region ID 2. The
save/reopen fixture checks the exact region-to-material binding; undo removes
only that assignment and redo restores it. No renderer or special botanical
material authority was introduced.

Evidence: the focused six-test CTest set passed (botanical generation, native
botany UI, OBJ UV/material metadata, CONDITION authoring, application, and UI).
The adversarial public-boundary suite passed 25/25, the exact-`HEAD`
history/worktree plus ignore-policy audit reported `PUBLIC_BOUNDARY_PASS`, and
`git diff --check` passed. This closes only botanical role-ID transfer and
catalog persistence. It does not claim rendered foliage fidelity, shader
graph transfer, GPU performance, visual review, or complete Conifer acceptance.
