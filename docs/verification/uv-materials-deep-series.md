# UV and materials deep-series closure

This document records the bounded implementation slice from the supplied UV +
materials remediation series. It is an evidence map, not a claim that the
entire series is complete.

## Implemented in this slice

- `attributes::UvSetTable` gives corner-domain `vec2` layers explicit stable
  `UvSetId` identity, independent active-for-editing and active-for-render
  policy, lightmap-candidate metadata, layer validation, and consumer-rebind
  deletion protection.
- The glTF exporter now consumes the active corner-domain render UV set as
  `TEXCOORD_0`. It expands the derived render stream at `(face, vertex)` UV
  seams, validates finite coverage, and reports missing active render UVs per
  mesh instead of flattening authored seams into vertex-domain data.
- The exporter derives and emits glTF `TANGENT` frames for UV-bearing meshes
  whose triangles produce a valid frame. Degenerate authored UV triangles are
  omitted from tangent emission and reported explicitly per export; no tangent
  data is fabricated.
- A validated face-domain `condition.material_region` layer now survives glTF
  export as deterministic mesh extras containing source face IDs and region
  IDs. The exporter does not fabricate a `materials` array; visual material
  realization remains an explicit downstream responsibility.
- The Motel 8 invertebrate authored slice now has a deterministic UV atlas
  pass: the active render UVs for the three referenced meshes use padded,
  non-overlapping face islands with an explicit `tile_policy_version` of 2.
  This is a bounded authored result, not a claim that Cartographer has a full
  interactive unwrap and packing workflow.
- `render::TextureRef` is now the compatibility name for an authored texture
  binding. It carries UV-set identity, sampler identity, UV transform, channel
  mapping, color interpretation, and normal convention. Non-finite transforms,
  zero UV identities, and unknown enum values fail closed.
- The native D3D12 graphics executor accepts bounded consecutive SRV and
  sampler prefixes. The existing one-texture form remains valid when
  `sampled_texture_count` is zero; `sampled_sampler_count` defaults to its
  legacy one-sampler form. Saved-scene PBR binds independent samplers for its
  five material texture slots.
- The built-in D3D12 PBR shader binds the five `StandardMaterial` texture
  semantics: base color, metallic/roughness, tangent normal, occlusion, and
  emissive. It also carries tangent plus handedness in the derived vertex
  payload and has a bounded masked-alpha path.
- The root-constant budget is intentionally 224 bytes for this first native
  five-slot pass. Five SRV descriptor tables and one sampler table consume the
  remaining D3D12 root-signature budget; this is why the pass does not pretend
  that a 256-byte root-constant layout is valid. The saved-scene shader uses a
  separate 192-byte root block plus five sampler tables, moving per-draw
  camera/material factors into flat vertex attributes to stay within the same
  64-DWORD limit.

## Evidence

- `cartographer_uv_material_contract_tests`: UV identity/layer validation,
  active-set switching, consumer-rebind deletion rejection, explicit texture
  binding validation, GPU ABI packing, and glTF `TEXCOORD_0` seam expansion plus
  `TANGENT` emission, face-region extras, and fail-closed rejection of invalid
  face-region IDs.
- `motel_08_invertebrate_uv_atlas_pass.carto`: 660 reopened face islands across
  meshes 8, 9, and 13, with source object/geometry/material-region preservation
  and mesh 10 unchanged.
- `build/headless-release`: 20/20 CTest tests passed.
- `build/d3d12-headless-release`: 21/21 CTest tests passed on the local Quadro
  P5200. The native test compiled the updated HLSL through DXC at runtime and
  executed the five-slot material draw, indexed draw/readback, tone map,
  shadow, temporal, compute, 8K temporal/probe, and hidden swapchain paths with empty
  debug receipts.

## Deliberately still open

The series remains incomplete for interactive seam painting, island
extraction, native-verified repacking, directional stretch analysis,
comprehensive UDIM/lightmap workflows, and topology-aware UV repair. The native
inspector reports edge-connected islands and can now submit a packed UV set as
a separate project edit; it does not extract island geometry. Its bounded
area-equivalent texel-density estimate uses an explicit uniform tile-resolution
assumption and does not measure directional stretch or read texture assets. The
bounded face-island pass is intentionally not a replacement for those tools.
Tangent generation and derived glTF export now exist, but hard-normal splitting,
topology-aware invalidation, and automated
repair of degenerate authored UV triangles remain open. Texture decode/residency,
MaterialGraph, OpenPBR/MaterialX, baking, IBL lookdev, color management, and
broader authored material realization remain open. The current native pass
uses a bounded five-table descriptor layout rather than a bindless or
streaming material system.

## Code-only continuation — 2026-10-25

The native UV instrument now has a saved-project inspector. It can load the
selected mesh's catalog of at most 256 UV sets and report internal UV seams, faces
whose UV bounds span integer tile boundaries, degenerate UV triangles, and
unique overlapping inter-face pairs. Reports require a clean saved project and
are bound to its generation and revision, selected object, mesh asset and mesh
revision, and UV-set ID. The overlap scan is capped at one million candidate
pairs; a capped report labels its overlap count as a lower bound. The edge
preview is sampled independently and is not presented as a complete rendered
atlas. This code was not built or tested, and no native interaction or visual
acceptance evidence was collected. The same instrument now permits one selected
face corner to be edited by a human through a project-generation/revision and
mesh-revision bound application action and the existing undoable project-mesh command. It
checks the selected face/corner, expected source UV, finite coordinate bounds,
and locked shared instances, and records exact before/after coordinates in the
project operation. A save is required before refreshing the durable report for
another edit. A focused source test covers invalid coordinates, stale project
generation/revision, mesh-revision and corner values, wrong-face refusal, Agent-task refusal,
successful history admission, and save/reopen undo/redo; it was not run. This
edit path was not built. Native interaction and multi-tile texture baking remain
open.

The inspector also reports edge-connected UV island count. It joins two mapped
faces only when they share a topological edge and both endpoint UV coordinates
agree under the seam diagnostic tolerance; faces touching only at one vertex
remain separate. Face storage is bounded by the existing corner ceiling. The
UV material contract suite now covers contiguous faces, a UV seam split,
vertex-only contact, and triangle-count refusal. These tests were authored but
not run, and the desktop was not built.

Code-only integration continuation (2026-10-25): the inspector now retains
deterministically ordered face membership for each current UV island, keyed for
display by its lowest face ID. From one selected face, the native instrument can
select the complete island through a single atomic application action. The
action binds project generation/revision and mesh revision, rejects empty,
duplicate, missing, stale, or over-65,536 face requests without changing
selection, and changes editor selection only; it does not write the project,
journal, or undo history. Application and UV-contract cases were authored but
not built or run. Native interaction remains unverified.

The same batch-selection path is now wired to the inspector's overlap and
cross-tile diagnostics. Overlap selection says explicitly when a capped scan
provides only observed lower-bound faces. These native controls remain
unverified; the atomic application action cases cover their shared admission
boundary, not button click-through.

The same inspector now estimates per-triangle area-equivalent pixels per world
unit from the stored source geometry and UV triangles. The user must provide
tile width and height in pixels; dimensions are bounded to 16,384 per axis and
are explicitly labeled as uniform assumptions for every integer UV tile. The
report excludes and counts degenerate UV triangles, degenerate surface
triangles, and surface-area overflow, and displays a min/max range rather than
claiming a directional-stretch or texture-asset measurement. The UV material
contract suite includes a known-density fixture and missing-resolution refusal;
it was authored but not run. No native build or interaction acceptance was
performed.

The native editor also provides a human-only seam alignment action. With one
face selected, it can copy both UV endpoint coordinates from the adjacent face
to the selected face as one command. It binds project generation/revision,
mesh revision, edge and face identities, and all four source endpoint values;
boundary edges, locked shared instances, stale snapshots, and already aligned
edges refuse. The project-mesh command records one history/journal operation
for both endpoints. A history/persistence fixture covers stale endpoint and
mesh revisions, wrong selected face, stale generation, boundary-edge and
already-aligned refusal, save/reopen, undo, and redo; it was authored but not
run. This is seam alignment only; it does not unwrap, pack, or extract islands.

### BakeGraph tile preflight wiring (2026-10-26, code-only)

The saved UV inspector now calls the production `enumerate_bake_tiles` function
for the selected mesh and UV set. It displays the exact ordered tile
coordinates accepted by the CPU normal-map scheduler, or that scheduler's
refusal reason for empty, out-of-range, or cross-boundary coverage. A refusal
leaves seam, overlap, island, and density diagnostics available. This is a
read-only preflight: it does not compile pixels, access a cache, or publish an
artifact. The UV material contract target links `carto_production` to satisfy
the shared scheduler dependency. No build or tests were run; native rendering
and click-through remain unverified. Desktop bake execution and multi-tile bake
acceptance remain open.

## Sprint repair ledger — code only, 2026-10-25

**Capability gained:** the saved UV report now includes edge-connected island
count and a per-triangle area-equivalent texel-density range. A human can edit
one selected UV corner or align both endpoints of a shared seam through normal
project history.

**Boundary improved:** UV seams split island connectivity; faces meeting at a
single vertex do not merge. Density requires explicit tile width and height,
rejects zero or excessive inputs, counts degenerate/unavailable triangles, and
does not infer texture dimensions or directional stretch. Edit actions reject
stale project/mesh/corner snapshots, malformed values, locked shared owners,
and nonhuman callers.

**Governance preserved:** coordinate and seam edits remain human-only and enter
through `SetProjectMeshCommand`, retaining project revision, command history,
journal, undo, and redo. Diagnostics are read-only; density controls are
assumptions, not live asset claims. The Cartographer internal application/UI
action contract changed, but no permission, organization, network API, or
serialized project schema contract changed. There is no cross-product mutation
path.

**Tests:** authored UV-edit and seam-alignment history/persistence adversarial
coverage plus UV-inspector island, density, invalid-resolution, and
triangle-budget cases.
Run: none, by the current no-test sprint instruction. Not run: all builds,
tests, native interaction, rendering, and multi-tile bake acceptance.

**Rejected:** direct native mesh writes; treating default resolution as
detected asset state; calling area-equivalent density directional stretch;
extracting or repacking islands from a diagnostic count; promoting this code
without build, test, and native evidence.

**Risks and rollback:** the header-only inspector and native UI changes are
unbuilt, so compile and UI integration defects may remain. Density assumes a
uniform resolution for every integer UV tile and reports an area-equivalent
range only. Roll back only the UV inspector/action/UI/test/documentation hunks;
preserve unrelated dirty work in shared files.

**Cross-product trace:** direct impact is Cartographer only. Experimental
Internal RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA were not changed;
no cross-product API/schema, dataset, training, deployment, or release contract
was touched. Dataset progress: unchanged. Training progress: unchanged.
Capability progress: code expanded, unverified. Promotion progress: unchanged;
acceptance remains open.

### Code-only continuation — UV island selection (2026-10-25)

**Capability gained:** the UV inspector's edge-connected island report now
provides stable current face membership. The native instrument can select that
island, reported overlap faces, and cross-tile faces through one batch action.
When overlap work is capped, the UI labels its selectable faces as partial.

**Boundary improved:** the action is bounded to 65,536 faces and checks the
captured project generation, project revision, mesh revision, object, duplicate
IDs, and existence of every face. It builds a candidate editor selection and
publishes it only after every face passes.

**Governance preserved:** island selection remains ephemeral editor state. It
does not mutate geometry, project revision, command history, or journal. The
existing source-bound UV report remains the only source of island membership.

**Tests:** authored UV-island membership assertions and application happy-path,
stale-snapshot, wrong-object, duplicate, missing-face, and over-budget cases.
Those action cases cover the common selection boundary, not native button
click-through. Run: none, per the no-test sprint instruction. Not run: build,
test, native interaction, and display acceptance.

**Rejected:** repeated per-face dispatches that could leave a partial
selection; persisting selection state; treating capped overlap findings as
complete; changing UV coordinates or topology; implying island extraction or
repacking.

**Risks and rollback:** code is unbuilt; the new action and desktop adapter may
contain integration defects. Roll back only the UV island report/action/UI/test
hunks and this ledger entry.

**Cross-product trace:** direct impact is Cartographer. RegOS,
IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are untouched. Dataset,
training, deployment, and promotion progress are unchanged.

### Code-only continuation — UV island packing command (2026-10-25)

**Capability gained:** the existing deterministic island packer is now wired
from the UV instrument into the normal human application path. A selected mesh
can pack one authored UV set into a separate non-active set with bounded
resolution, padding, tile count, density policy, and optional quarter-turn
rotation. The source set remains intact; the resulting mesh change is one
`SetProjectMeshCommand` with a digest-derived journal operation, undo, and redo.

**Boundary improved:** packing refuses stale project generation/revision or
mesh revision, a non-selected owner, missing source UV set, hidden or locked
targets, any locked shared-mesh instance, invalid IDs/settings, and nonhuman
mutation. The reusable mesh builder validates the artifact against its exact
source and refuses missing corner identities or conflicting output layers.
The output never silently becomes the active edit or render set.

**Governance preserved:** the kernel builds an immutable packed mesh candidate;
application admission performs current-source checks and commits via the
existing command, journal, and project history. No project schema change or
cross-product mutation path was added.

**Tests:** authored application and UI coverage for stale generation/revision,
wrong owner, missing source set, raw AI refusal, one-step history, preserved
source set, non-active output set, undo/redo, save/reopen, and operation
lineage. Run: none, per the active user instruction. Not run: build, tests,
native click-through, and multi-tile bake acceptance.

**Rejected:** replacing the source UV set, activating output implicitly,
applying the artifact through a direct project transaction in the UI path,
and treating packing as a texture bake or full UV acceptance.

**Risks and rollback:** new production/application/UI integration is unbuilt and
may contain compile or integration defects. No serialized schema was added;
the output is stored in the existing mesh attribute and UV-set payload. Roll
back only the new packed-mesh helper, action/dispatch route, UV instrument
controls, authored regressions, and this section.

**Cross-product trace:** direct impact is Cartographer only. RegOS,
IDA_Training, hidden_canopy_bot, and Ask_IDA remain untouched. Dataset,
training, deployment, and promotion progress are unchanged; capability progress
is code-only and unverified.

### Desktop target source-list wiring (2026-10-26, code-only)

Added `uv_inspector.hpp` to the explicit source list for the desktop executable.
`main.cpp` already includes the header; the source-list entry keeps it visible
in generated IDE projects with the other native instrument headers. This is
project wiring only and does not alter behavior or the build graph's linked
libraries.

**Tests and verification:** no build or tests were run. The file inclusion and
CMake target declaration were inspected in source. Native UI acceptance remains
open.

**Risks and rollback:** generator-specific project visibility remains
unverified. Remove the one `uv_inspector.hpp` source-list entry to roll back.
No serialized data, public API, dependency, or mutation boundary changed.

**Rejected:** treating CMake source-list visibility as evidence that the UV
instrument compiles or works in the native editor.

### Saved-scene D3D12 authored UV stream mapping (2026-10-26, code-only)

Material texture slots resolve their authored UV-set IDs to the primary or
secondary coordinate stream carried by the derived render-mesh artifact.
Project preparation selects the normal-map UV set for tangent generation and
binds stream identities into the request and mesh-cache digests. The artifact
currently supports two distinct UV sets. One tangent frame is available per
vertex, so every normal map on an object must use the primary stream; the
renderer refuses multiple normal-map UV identities rather than sampling with
a tangent basis derived from another set. Direct request, UV1 pixel-effect, and
unsupported-boundary cases are authored but were not built or run. This is
source-only progress; native shader and visual acceptance remain open.

### Bounded additional UV streams for saved-scene rendering (2026-10-26, code-only)

Render-mesh artifact v4 now carries up to eight identity-bound UV streams while
preserving v1-v3 decoding. Additional coordinates participate in corner seam
splitting, canonical artifact validation, serialization, and byte accounting.
The project adapter maps each material slot to a matching stream, including
sets beyond UV0/UV1. One tangent frame remains available per vertex, so normal
maps must use the primary stream. Production artifact, scene request, saved
project, malformed-input, and pixel-effect regression cases were authored but
not built or run. This supersedes the earlier two-stream ceiling; the feature
is source-only until native build and capture review.
