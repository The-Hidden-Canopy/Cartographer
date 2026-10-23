# CONDITION source-authoring boundary

This verification note records the public Cartographer slice derived from the CONDITION source-authoring gap review.

## Public capability

Cartographer can author and persist a neutral `condition::AuthoringDocument` inside a `.carto` project. The document carries:

- reviewed source evidence and source-linked assets;
- Safe Place, familiarity, and view-condition metadata;
- projection and physical variant candidates with explicit mutation guards;
- temporal visual packages and surface-state records;
- interaction anchors, collections, prefabs, and region-kit manifests;
- revision-bound audio-source and acoustic-zone intent, referencing reviewed/current imported clip evidence without storing decoded samples or playback state;
- revision-bound effect-source, bounded particle-emitter, and exact spatial-field influence records, with deterministic local-space preview and explicit normal/disabled operating states;
- revision-bound vegetation scatter recipes bind exactly one vegetation-class asset or registered native botanical source (identity, generation, and digest), plus distinct moisture, shade, disturbance, vegetation-suitability, and elevation field receipts; deterministic bounded previews can be committed by a human as atomic, undoable native-mesh instance batches with durable project-revision and source/field provenance;
- branchable Safe Place and surface-history alternatives with exact before-values and conflict-aware comparison;
- deterministic serialization and SHA-256 package provenance.

The native shell exposes this through the CONDITION workspace. Most existing CONDITION records remain a read-only projection; interaction anchors, prefab assemblies, asset collections, and region-kit manifests can now be created with typed, revision-bound human actions, alongside the existing audio/acoustic, effect-source/emitter/field-influence, and vegetation-scatter additions. IDs are assigned by the application, prefab source revision is pinned to the exact authoring revision used to compose it, and each change goes through the project command/journal path. Bounded RIFF/WAVE intake validates supported uncompressed PCM/IEEE-float metadata and records a content digest plus a content-addressed logical locator. It does not persist the absolute file path or audio samples. Imported clips start unreviewed; an explicit review action is required before package publication. An operator can reselect a current clip source, have the bytes revalidated against the recorded digest, and audition a decoded three-second excerpt through the Windows desktop audio API. The absolute path and decoded PCM are held only in transient process memory and are stopped/released when requested or when the shell exits. Audio descriptors have a revisioned enabled/disabled state for fixture failure variants. The work surface draws source attenuation rings and acoustic-zone outlines only for authored assets mapped to existing scene objects. Min/max attenuation rings, sphere-radius handles, box-zone corner handles, and per-zone priority labels are interactive; drag previews commit through revision-checked project commands on release and cancel when the project snapshot changes. The CONDITION panel lists conservative bounded broad-phase overlap candidates and their authored priorities; this is advisory only and does not claim a runtime winner or acoustic simulation. The effect workbench makes a bounded deterministic 2D point preview in owning-asset local X/Z space; wind/thermal/drainage use authored directional acceleration, avoidance reverses the authored direction, and turbulence adds deterministic bounded perturbation. This is not a renderer-integrated particle system. Vegetation recipes bind exactly one source: either a project asset classified as vegetation or a generic Cartographer plant source bound by identity, revision, and digest. Their fields retain exact moisture, shade, disturbance, vegetation-suitability, and terrain-elevation semantics. The moisture field defines the candidate grid; every candidate must be covered by every bound field. Suitability score, seeded density, yaw, and scale yield a deterministic point preview with source receipts and a digest. Candidate and output limits fail closed; there is no partial/truncated preview. Preview does not mutate scene state, carries authoring and field receipts, and is hidden when its source or sampled fields change. A fixture can be loaded through the application session and exported as an admitted `.carto-condition` package.

The `plant_source` field is a revision/digest-bound binding. Local previews
resolve it against the project source-asset registry and require a
Cartographer-native botanical mesh. The desktop CONDITION workspace exposes
local source/representation controls and explicit mesh compilation. Selecting
a native source makes its receipt the sole species authority (the legacy
species-asset ID is zero). A human-only commit binds the exact project revision
and preview digest, creates one bounded mesh-instance batch, and records v28
per-object provenance; stale project/source/field evidence, a wrong prototype,
invalid or duplicate placements, or partial batch failure is rejected. Private
runtime submission, job refresh, and human-reviewed glTF import (when
configured) remain separate explicit operations. No service request occurs on
project open; serialized data contains no service credentials, organization,
endpoint, or opaque remote identifiers. No live-service availability or
production-renderer fidelity is claimed.

CONDITION assets can be bound to an existing scene object from the native
authoring instrument. This is an explicit human-only project command guarded by
the current project and authoring revisions; the binding is journaled and
undoable. The CONDITION viewport also exposes the ordinary scene-object
translation gizmo for a selected unlocked root object: Ctrl-drag uses the
0.25 m grid, Shift-drag requests a revision-bound geometry snap, and a plain
drag remains unsnapped. Left-drag on empty Model/CONDITION canvas pans the
viewfinder; object and gizmo hits remain routed to selection and authoring.
This makes authored source and zone guides reachable from the native work
surface without granting a runtime or AI mutation path.

Workbench instruments expose a dedicated grip that captures the native pointer
before viewport input arbitration. A drag can cross the full desktop work area,
then settle to an admissible work-surface edge/neighbor snap target or store the
instrument by release over either side rack. Empty-canvas pan remains separate
from object, gizmo, instrument, and active placement gestures. Live native
acceptance on 2026-10-17 verified a 20-pixel grip drag, neighbor snapping beside
the Material instrument, and reversible Pan View movement. The temporary panel
and view offsets were restored without saving project data. This was observed
from live screenshots, but no screenshot artifact was retained and the
acceptance is not yet a repeatable automated desktop fixture.

The native editor also provides read-only point and sampled-route landmark
visibility against the exact viewport scene. Route stations are explicit and
revision-bound; the query returns a blocker receipt per station, rejects stale
or over-budget work atomically, and never mutates the project or operation
journal. The Rural Road/Forest fixture exercises deterministic visibility
parity and stale-revision rejection after a source transaction. A bounded
row-major landmark viewshed can be derived from a current terrain preview by
selecting only exact terrain-product vertices (up to 32 × 32 samples); each
grid captures terrain identity/revision/digest separately from the project
revision used for sightlines. The CONDITION workbench accepts an explicit world
XYZ observer, evaluates against the exact current viewport scene, and draws
green-visible/amber-occluded cells as a read-only work-surface overlay. Field,
project, or project-generation drift suppresses the overlay. No terrain height
is interpolated or invented by this adapter. This is geometric authoring
visibility, not renderer visibility, perspective pixels, or a production
heatmap capture. A caller-targeted authoring-observation projection accepts
caller-supplied target identities and semantic kinds, applies an explicit
kind filter and camera view cone, and runs revision-bound point visibility
only for eligible targets. It is a structural authoring report, not runtime
perception, silhouette fidelity, sign readability, interaction reachability,
or pixel/render truth.

Interaction-anchor, prefab, collection, and region-kit creation use explicit human-only commands.
Both commands check the current project and authoring revisions, validate the
complete updated authoring document, assign IDs at the application boundary,
and commit through the same journaled sidecar command. Prefabs capture the
pre-mutation authoring revision as their source revision. Stale revisions,
missing asset/variant references, malformed anchor geometry, duplicate or
unsorted prefab members/sockets, invalid collection or region-kit references,
invalid coverage values, and caller-supplied IDs are rejected without changing
project state. Snapshots expose read-only projections for the native authoring
controls. Collections support asset and nested-collection membership; region
kits support asset/prefab membership, required classes, coverage targets, and
validation requirements. Native edit controls preserve record identities and are
bound to the project and authoring revisions captured when an edit begins. A
stale draft must be canceled and reloaded before it can be submitted. Deletion
requires a second confirmation and revalidates the complete document, so records
with live references cannot be removed.

## Governance and failure behavior

- Source authoring is committed through the existing project command boundary and remains revision-bound.
- Within one project namespace, a changed imported authoring document must advance the embedded authoring revision by exactly one; same-revision, skipped, and stale replacement files fail without changing project state.
- Source invalidation advances the embedded authoring revision before the command is journaled; undo/redo restore the corresponding revision and source status together. Revision exhaustion fails without a partial source update.
- Package publication is rejected when source evidence is stale, unreviewed, invalid, or has a dependent source-revision mismatch.
- Presentation-only variants cannot declare collision, navigation, or interaction impact.
- The package stores logical source locators only; absolute paths, parent traversal, drive-qualified paths, secrets, and runtime telemetry are not accepted.
- Package admission verifies the serialized bytes and publication digest before a consumer can use the package.
- Audio sources require a real imported-asset evidence record and an existing owner asset; invalid ranges, missing/wrong-kind clip references, and non-canonical zone shapes fail before the project command. Stale clip evidence blocks publication.
- Effect sources require existing owner assets and distinct normal/disabled state labels. Emitters cap rate, lifetime, duration, timestep, and total preview population; field bindings require a safe project-local identity, current non-exhausted source revision, nonzero digest, and unit direction. Preview rejects absent or mismatched project fields, unknown states, out-of-coverage samples, excessive time, and non-finite outputs; it never substitutes a default field or changes project/authoring revisions.
- Vegetation recipes require exactly one source binding: either an existing vegetation-class asset or an externally vouched generic plant-source receipt bound by identity, revision, and canonical digest. Five distinct exact-semantic field identities, bounded spacing/density/scale/threshold controls, and an explicit output cap are also required. Preview rejects missing, stale, wrong-semantic, malformed, or out-of-coverage fields; a candidate grid above 65,536 samples or an output above the recipe cap fails rather than truncating. A disabled recipe produces an empty derived preview, and preview alone never inserts scene instances. This scatter path does not generate plant geometry or commit placements; generated geometry enters through the separate explicit runtime-client and digest/revision-verified glTF import boundary.
- State branches require exact existing source records, supported typed properties, a current parent branch, and a before-value equal to that source or inherited parent value. Safe Place metrics and surface occupancy/maintenance/damage/wetness can be authored as review alternatives; comparisons mark contested sibling changes and never overwrite the source baseline or activate runtime state.
- WAVE intake is capped at 64 MiB, 30 minutes, eight channels, and 8–192 kHz; malformed RIFF framing, inconsistent rates/alignment, compressed/extensible formats, and non-finite float samples fail before any project mutation. The content digest binds exactly the bytes inspected; no source path or sample payload is serialized.
- Audition reopens a user-selected local file only after source identity lookup, the same 64 MiB WAVE validation, and an exact SHA-256 match. Decoding is limited to three seconds and 16 MiB of transient 16-bit PCM; multichannel sources are bounded downmixes. No local path, excerpt, or decoded sample enters the project, sidecar, package, or network path. Windows playback is explicitly a clip-audition utility, not a project mixer or runtime audio authority.
- Reviewing imported evidence is a separate revision-bound project command. It cannot review stale evidence, and undo restores the prior unreviewed state.
- Consecutive CONDITION sidecar edits can be undone/redone in history order while project revisions remain monotonic; the sidecar byte precondition prevents undo from overwriting a different authoring document.
- Acoustic radius and zone-volume edits require the exact authoring revision captured at gesture start, reject stale or invalid edits without partial change, and are undoable/redoable through the same sidecar command path.
- Scene-object binding requires the exact project and authoring revisions plus an existing asset/object pair. Missing targets and stale snapshots fail closed; unchanged binding is a no-op; accepted changes use the project command journal and undo/redo.
- CONDITION runtime behavior, player telemetry, private donor services, and game-owned state are intentionally outside this public module.

## Persistence and compatibility

Generated sign meshes encode panel and raised-lettering faces in stable
`cartographer.material_slot` regions 1 and 2. The native Material instrument
can bind each independently; the application acceptance fixture verifies
revision rejection, durable `.carto` assignments, source-receipt preservation,
and undo/redo. The Material instrument also edits revision-bound linear
emissive factors and strength as source data; it does not claim viewport
emission output. The integrated Motel 8 fixture reopens the exact sign source
snapshot/digest, both material assignments, emissive source values, review
camera bookmark, marquee-light intent, and sign damage/repair branch lineage.
Panel and lettering remain one scene object rather than separate child objects.

`ProjectDocument` schema 28 stores the `CONDITION_AUTHORING` sidecar record (introduced in schema 14), revisioned sign-text source definitions, retained source lineages, generated sign-mesh receipts, bounded botanical-generation receipts, and durable field-driven vegetation placement receipts. A placement receipt records the exact native plant source generation/digest, enclosing project revision, CONDITION authoring revision/digest, preview digest, all five field receipts, and committed transform/suitability. Schema 28 requires its receipt table even when empty; schemas 1–27 remain readable without synthesizing placements. The botanical generation receipt binds the canonical source snapshot and input/output digests while keeping service credentials, organization identity, and raw job IDs out of `.carto`. The embedded CONDITION authoring codec is schema 8 and continues to read schemas 1–7: earlier schemas retain their prior empty-section migrations, schema 6 adds exact generic plant-source identity/revision/digest bindings, and schema 7 adds articulation graph/limit sources. Schema 8 binds each articulated moving part to a scene object, mesh asset, and exact mesh revision. Schema-7 assemblies reopen with no part bindings for repair, but are non-current and cannot be published until explicitly rebound; no mesh or runtime pose is silently inferred. Earlier supported project schemas remain readable through the existing compatibility path; schema 14+ documents require the CONDITION record, including an explicit `NONE` record for projects without CONDITION authoring.

Field-driven vegetation now has an explicit local workflow: compile a native
plant representation, bind its exact source receipt to a scatter recipe, review
the deterministic preview, then commit that exact preview as one human-only,
revision-checked undoable batch of mesh instances. The application binds the
preview digest to the enclosing project revision; field, authoring, or unrelated
project mutations make the captured preview stale. Each instance retains a
durable source/field/transform receipt. Private-worker submission remains a
separate explicit action and is never a local-generation fallback. This path
now preserves the local compiler's normalized corner UVs through OBJ staging
and `.carto` save/reopen. This does not establish material-role transfer,
rendered vegetation appearance, collision behavior, or performance acceptance.

The acceptance tests cover fixture round-trip, illegal physical/presentation mutations, stale-source and stale-file rejection, revision exhaustion, audio-source/zone round-trip and hostile references, persisted enabled/disabled source variants, WAVE metadata inspection and malformed-input rejection, bounded PCM excerpt conversion, pathless content provenance, review gating, stale acoustic-edit rejection, zone geometry edit/undo/redo, revision-bound anchor/prefab/collection/region-kit creation, assigned identity and source-revision behavior, hostile references, stale revision rejection, save/reopen, undo/redo, and non-reuse after undo, effect schema-1/schema-2, vegetation schema-1 through schema-5 migration, schema-6 generic plant-source binding, schema-7 unbound-articulation migration, schema-8 part/mesh-revision binding, stale-binding publication refusal, source tamper/missing/revision rejection, bounded native hinge-pose preview with no project/journal mutation, and state schema-1 through schema-4 migration, adversarial effect/vegetation/state bounds and references, deterministic normal/disabled and field-driven placement previews, stale/missing/wrong-semantic/out-of-coverage field rejection, no-truncation limits, state before-value drift and contested sibling comparison, nested branch ancestry/cycle rejection, preview non-mutation, `.carto` save/reopen, sidecar command history, undo/redo, UI workspace availability and scene-translation workspace policy, package publication, and consumer admission. The native shell target compiles anchor, prefab, collection, and region-kit composition controls, CONDITION scene translation and snapping, interactive source/zone handles, advisory overlap/priority display, effect and vegetation preview canvases, field-bound scatter authoring, Safe Place branch creation/tree comparison, and digest-verified Win32 clip audition. A live workbench drag/snap/pan gesture was visually observed on 2026-10-17, but no screenshot artifact or repeatable automated desktop gesture fixture is retained; production-fixture visual/audio acceptance remains unverified. Collision/layout review, foliage rendering, and full visual production acceptance remain unverified. There is no project mixer, acoustic simulation, runtime effect pool, spatial runtime playback, state-driven runtime switching, or game-runtime claim. These are source-authoring and preview fixtures; no production visual-fidelity claim is implied.

The separate `cartographer_condition_authoring_ui_fixture_tests` target generates
a deterministic one-second synthetic PCM tone and a saved `.carto` scene with
digest-bound reviewed clip evidence, one audio source, and one acoustic zone.
It also creates a source-driven Motel 8 sign, separate panel and lettering
materials, project-owned presentation intent, and alternative damage/repair
states. It verifies WAVE metadata, scene binding, sign source receipt and
material/presentation/state save-reopen, package admission, and that neither
the workstation path nor sample bytes enter the project. This is repeatable QA
data, not production audio or runtime acoustic validation. ImGui interaction
tests verify the visible drag grip and MOVE/PLACE control at standard and narrow
panel widths; screenshot-backed native pointer-drag acceptance is still
unverified because this run had no targetable Cartographer desktop window.

The same fixture runner now emits a second durable `.carto` artifact for the
Pine Ridge gas-price board. It contains two editable sign instances generated
from exact source revisions 1 and 2, preserves each canonical text snapshot and
digest, rejects creation from a stale revision without project mutation, and
assigns clean versus weathered panel/lettering materials to four explicit
face-region bindings. Assignment undo/redo and native application save/reopen
are checked. The serialized fixture rejects donor repository/commit and runtime
endpoint markers. Its elapsed time is fixture trace data, not a benchmark. It
does not prove rendered price readability, visual fidelity, GPU cost, or donor
runtime independence by itself; those remain separate acceptance gates.

The runner also emits `condition-safe-place-state-history.carto` with nested
privacy and wetness alternatives. Before a state-branch command is journaled,
the UI now materializes the candidate against its source baseline and parent
ancestry; a mismatched before-value is rejected without a project revision or
history change. The artifact checks that the authored Safe Place/surface
baseline remains unchanged, comparison digests are stable for the saved
revision, and both branches survive save/reopen and ordered undo/redo. This is
reviewable authored state only; it does not activate runtime state or prove
rendered temporal appearance.

The same runner emits `condition-general-store.carto` and a separate
`condition-general-store-stale-audio.carto`. The current artifact binds three
CONDITION assets to exterior, sign-backing, and interior scene identities;
adds a synthetic exterior/interior blockout and a revision-1 generated
frontage sign; assigns separate panel and lettering materials; and stores
review-camera and practical-light intent. Three distinct synthetic WAVE
sources are imported as digest-bound, pathless evidence and attached to three
audio descriptors and two acoustic zones. The test saves and reopens the
current project, then invalidates one reviewed clip, confirms package
construction fails closed, saves/reopens that stale state, and verifies
ordered undo/redo restores and reapplies currentness. The serialized `.carto`
artifacts are checked for workstation paths and donor/runtime markers. This
is a coarse synthetic source fixture: it does not prove production store
fidelity, spatial playback, acoustic simulation, rendered sign legibility,
GPU performance, or screenshot-backed visual acceptance.

`cartographer_condition_authoring_tests` also emits a durable Rural Road/Forest
`.carto` artifact in the build-tree fixture directory. It binds a revision-2
primary road and roadside-ditch intent, a distinct pedestrian trail, the exact
terrain tile, authored vegetation and wind fields, and deterministic landmark
visibility. The test invalidates an earlier road/terrain preview by advancing
the road and centerline revisions; stale road realization is rejected without
project/history mutation, and current road/trail meshes plus scatter placements
survive save/reopen and undo/redo. The serialized artifact is checked for
temporary source identity and private donor/runtime markers. It remains a
synthetic structural fixture: no drainage solver, rendered capture, or timing
claim is made.

The same test target emits current and stale-source Conifer-family `.carto`
artifacts. The fixture records an asset-bound field-driven scatter recipe
(moisture, shade, disturbance, suitability, and terrain elevation) and derives
a deterministic, read-only placement preview with exact field receipts and a
preview digest. The current artifact survives native application save/reopen,
scatter-recipe undo/redo, and preview regeneration after undo/redo. A
source/field revision change rejects stale previews without project mutation.
That scatter fixture remains source/preview evidence; it does not itself produce
a production species asset, renderer-fidelity, or visual-performance claim.

Separately, the native `carto_botany` generator now derives a deterministic
branch graph and hero, reduced, card, cluster, and far-proxy meshes from a
revision-bound botanical source. It records source, graph, and product digests,
stable logical branch identities, bounded geometry, and bark/foliage material
regions. `UiController::create_native_botanical_mesh` sends a chosen result
through the existing OBJ importer and source registry, commits one source-linked
mesh, and rejects stale source revisions or OBJ round-trip digest changes.
`cartographer_botany_tests`, `cartographer_botany_ui_tests`, and
`cartographer_obj_uv_tests` cover deterministic normalized corner UVs,
wrap-seam behavior, UV-payload tampering, OBJ positive/relative index import,
polygon-triangulation export, topology-repair association, commit,
save/reopen, undo/redo, reimport lineage, and stale-source refusal. The native
OBJ source receipt uses importer version 3; PLY, STL, and generated glTF remain
version 1. The botanical source also carries stable bark/foliage face-region
IDs and a separate material-domain digest. The native app verifies geometry,
UV, and material-region equality before commit. Region IDs survive project
save/reopen and can be bound through the existing material catalog; catalog
assignment is undoable/redoable. OBJ carries region IDs, not material assets or
rendered shader assignments. No rendered capture, production species fidelity,
or performance acceptance is claimed.

The native fixture runner also emits current and stale-field
`condition-gas-station-effects` `.carto` artifacts. Three authored sources and
bounded emitters bind to exact wind, drainage, and turbulence field revisions
and digests. Deterministic previews cover the active compressor, runoff, and
vehicle states; the paired disabled states remain empty. Preview calls leave
project revision and operation history unchanged. Save/reopen preserves the
exact preview digests and field receipts; a subsequent wind-field edit blocks
the prior binding after save/reopen, undo, and redo without revision rewind.
This is source and preview acceptance only: it does not add a runtime particle
pool, renderer integration, or production VFX fidelity claim.

## Botanical material-role transfer

All five native botanical representations now expose stable face-domain
material regions: bark is region ID 1 and foliage (including card, cluster,
and far-proxy forms) is region ID 2. The botanical product digest binds the
region-role records and the complete `condition.material_region` face layer;
all faces must be covered exactly once. The source verifier rejects altered
region IDs or ranges.

Version-3 native OBJ staging writes the role records plus one bounded
Cartographer face-region record per triangulated face. The importer refuses
duplicate, incomplete, zero, out-of-range, or malformed region metadata. The
native commit compares canonical geometry, UV, and material-region digests
before mutation and persists the material digest in the source-domain receipt.
The ordinary material-catalog action binds the stable foliage region ID to a
catalog material; `.carto` reopen preserves that binding and undo/redo changes
only the assignment.

Evidence: six focused CTests passed: `cartographer_botany_tests`,
`cartographer_botany_ui_tests`, `cartographer_obj_uv_tests`,
`cartographer_condition_authoring_tests`, `cartographer_application_tests`,
and `cartographer_ui_tests`. The public-boundary Python suite passed 25/25;
the exact-`HEAD` history/worktree and ignore-policy audit reported
`PUBLIC_BOUNDARY_PASS`; `git diff --check` passed. These tests establish
authoring/catalog transfer only. OBJ does not transfer material assets or
shader graphs, and rendered appearance, artist review, GPU timing, and final
Conifer-scene acceptance remain open.
