# Integrated texturing workflow

Work is isolated in `.worktrees/texturing-workflow`, started at `f749a04` and
rebased onto `173a3c0` from `integration/staging-20261112`. The primary checkout
and other worktrees were not edited. No network access, dependency installation,
deployment, or schema migration is part of this work.

## Inspection before changes

| Area | Existing implementation | Workflow gap addressed here |
| --- | --- | --- |
| Material truth | `MaterialCatalog`, `StandardMaterial`, create/update/mesh and face assignment actions; journaled catalog/document commands | Expose existing mapping data consistently and apply finished edits immediately |
| UVs | Planar projection, triangle relaxation, island packing, unique lightmap candidates, seam/coordinate commands, UV inspector | Reuse these commands; exercise a generated UV set on an existing OBJ |
| Texture assets | Container inspection, bounded decoding, content-addressed exact source storage, stable-ID reimport | Retain filenames; join generated outputs and separate PBR maps to the same catalog |
| Baking | CPU/D3D12 tiles, AO, normal, bent normal, height, curvature, thickness, world position, surface IDs, immutable receipts | Explicitly adopt supported AO/tangent-normal outputs; never mutate project truth from a worker |
| Shading | CPU oracle and native D3D12 authoring renderer; base color, packed metallic/roughness, normal, AO, emissive, scalar optical V5 | Add mapping/channel/color-space controls and disclose unsupported paths |
| Interchange | glTF/GLB embedded images and loss diagnostics; engine-neutral prepared products used by VANTA adapters | Exercise the existing routes with generated catalog maps, without a parallel exporter |
| AI | Reviewed native proposals, scoped Agent Workbench material-region and lightmap-UV actions | Read-only typed material creation/edit/mesh-or-face assignment review, followed by explicit human acceptance |

Branch inspection used local history only. `codex/material-transmission-v5`
has three commits beyond this baseline: studio lights/environment changes,
CPU anisotropic sampling, and non-mipmapped-sampler/clearcoat import with loss
diagnostics. They were not silently merged. The CPU sampler part of `df811a3`
was subsequently adapted after the strict GPU parity test exposed the existing
CPU/shader filtering mismatch. The studio-light, atlas and lighting-upload
changes were not imported. Standard PBR map controls and the
validated scalar Material V5 work are already integrated in the baseline.
`integration/preserved-material-v5-20261116` has no additional commits beyond
this baseline. Other worktrees remain untouched.

## Operator path

1. Save a project, import an existing mesh, and select its object. Use the UV
   instrument to generate/project/pack a UV set; inspect the actual UVs.
2. Open the dockable/detachable Material instrument. Create a material and
   assign it to the selected mesh or faces. Shared mesh instances still share
   face slots; existing locked-instance checks remain in force.
3. Import maps. The source filename and exact bytes are retained. Bind base
   color, packed metallic/roughness, tangent normal, AO and emissive sources.
   Separate roughness and metallic images can be explicitly packed from their
   red channels, with no implicit resampling or color conversion.
4. Expand Mapping and channels for UV identity, sampler identity, offset,
   scale, pivot, rotation, channel mapping and color-space interpretation.
   Keep all maps in one rendered material on the same authored UV set.
5. Live apply commits one reversible update when the current edit finishes.
   It retains dirty/stale drafts and stops automatic retries after a refused
   commit. Manual Apply remains available. Enable Rendered viewport in the
   work-surface toolbar to inspect PBR shading.
   With Live apply off, Review source changes stages an immutable proposal;
   compare its saved/proposed values and supply a reason before accepting it.
   Human and AI hosts use `UiController::begin_texturing_review` and
   `commit_texturing_review`; no raw AI mutation authority is added.
6. Bake using the existing UV tools. The material instrument can adopt the
   last completed bake or a published receipt manifest. Only current,
   same-project, single-tile AO or complete tangent-normal outputs are
   admitted. Recipe, artifact, receipt and human reason remain in immutable
   source blobs; adoption and map binding use one catalog command.
   Explicitly packing/adopting the same generated map also restores missing
   derived image bytes from verified inputs while retaining its identity and
   lineage. Existing corrupt blobs are refused rather than overwritten.
   Reimport of a generated record is refused because it would falsely attach
   the old bake/packing lineage to new pixels. Import and bind a new source.
7. Save/reopen and export through the existing glTF/GLB or prepared-product
   routes. Preserve `.carto-assets` with a flat project. glTF is a rendering
   interchange format and does not preserve the complete Cartographer history
   or bake lineage.
   Neutral prepared catalogs retain source provenance references; they do not
   bundle the full authoring journal or every referenced bake receipt. Keep the
   source project/assets or migrated package for complete authoring lineage.
   Save As to another directory copies managed source/lineage blobs before
   publishing the project; external texture locators must be managed first.

## Explicit limits

- The ordinary workspace is not texture shaded. Native rendered inspection
  currently pauses viewport mesh gestures until restart; panel commands still
  work. This pre-existing input-correlation guard is preserved.
- Live apply takes effect on edit completion, not on every slider sample.
- Native material programs support one UV identity per material, bounded
  texture counts/memory and 4096 map extents. Numeric UV/sampler controls do
  not manufacture UV sets or sampler records.
- Existing UDIM UV generation, packing and tile baking remain intact.
  Multi-tile material sampling/adoption is not implemented; no first-tile
  fallback is used.
- Height/displacement, bent normal, thickness, curvature, world position and
  surface-ID bake products retain their existing output workflows. They are
  not falsely assigned to unsupported shading slots. Scalar V5 optical
  parameters work; V5 transmission/thickness/anisotropy textures remain
  unsupported by the authoring renderer.
- Source admission is not pixel-decoding evidence. Unsupported DDS/EXR and
  other decoder variants produce explicit viewport/export errors. PFM is
  scene-linear RGB for supported base-color/emissive paths.
- glTF reports/refuses unsupported normal-frame conversions and color-space
  overrides. This work does not expand external VANTA runtime admission.
- Native packed lookdev rejects custom UV/sampler/transform/channel settings,
  interpretation overrides and OpenGL normal convention. The existing neutral
  prepared path can retain the exact source catalog and texture products
  without a packed lookdev realization. That publication is not evidence of
  VANTA rendering or runtime admission.
- Material review currently covers create/update and mesh/face assignment.
  It does not add autonomous texture import, baking, or broader Agent
  Workbench effects. Existing scoped UV/material-region recipes remain.

## Verification and repair ledger

The focused native integration executable uses the bundled, authored
`examples/first-block/first-block.obj`, not a UI-only demonstration. Test
images are explicitly synthetic. It leaves inspectable `.carto`, original
images, bake receipts/blobs, CPU frames, package and interchange outputs in
the build's `texturing-evidence` directory.

A passing CPU image comparison is not GPU or manual visual evidence.

Changed areas: Material instrument and mapping presentation; shared
application actions/review and catalog commits; source provenance retention
and package copying; focused integration test and CMake registration.

Governance path: human action admission -> validated typed action -> existing
command bus -> application mutation journal -> catalog/document truth.
Reviews and background bakes do not mutate the live project. Stale or foreign
sources and invalid receipts fail closed. No RegOS org/role/transition APIs
exist in this C++ module; project namespace, session and revisions are the
applicable scope boundaries.

Rejected options: a parallel material model or demo; merging unverified
branch work; fabricated shader support for height/UDIM/optical maps; automatic
background material changes; a direct AI-to-document mutation route; implicit
resampling/color conversions; weakening the viewport input guard.

Rollback: revert the isolated diff or discard this worktree. New assets are
immutable content-addressed files and can remain as harmless cached blobs.
Existing schema versions and project records remain readable. Undo restores
the prior catalog for each accepted material/packing/adoption command.

Cross-product trace: direct impact is Cartographer only. RegOS, IDA_Training,
hidden_canopy_bot and Ask_IDA were not accessed or changed. No shared API,
dataset, training run, deployment or promotion was modified. Capability
evidence comes from the tests and visual observations listed here, not from
a dataset/training/promotion claim.

## Change ledger

| Files | Concrete change |
| --- | --- |
| `apps/desktop/main.cpp`, `studio_texture_binding.hpp` | Finished-edit live apply, mapping/channel controls, linear roughness/metallic packing, explicit bake adoption, material proposal review/acceptance; Material workspace opens its existing instrument |
| `libs/application/include/carto/application/application.hpp`, `src/material_texturing.cpp`, `src/application.cpp`, `CMakeLists.txt` | Validated adoption/packing actions; session-bound read-only reviews and human acceptance; filename intake; immutable generated-source policy; journaled catalog commands; Save As copies managed sources |
| `libs/project/include/carto/project/material_catalog.hpp`, `src/material_catalog.cpp`, `src/package.cpp` | Validate generated-map provenance digests, retain them in source-blob inventory, copy them during package migration |
| `libs/ui/include/carto/ui/ui.hpp`, `src/ui.cpp` | Source filename and bake-manifest adapters; shared review/acceptance API with operation/problem feedback |
| `libs/io/src/gltf_export.cpp` | Permit explicitly material-bound UVs without a separate active-render flag; still reject unresolved authored UVs |
| `libs/production/src/authoring_material_surface.cpp`, `tests/authoring_surface_tests.cpp` | Match the existing GPU anisotropic sampler and add an independent thin-footprint numerical check; retain the intended isotropic mip check |
| `tests/texturing_workflow_tests.cpp`, `tests/CMakeLists.txt` | Real-mesh workflow and refusal/rollback boundary coverage |
| This document | Branch inventory, limits, verification, lineage and rollback ledger |

The real-mesh test exposed the glTF guard bug; it was repaired in the
exporter's existing validation layer. Desktop inspection exposed the missing
Material workspace visibility hook; it was repaired in the existing workspace
switch. GPU shader code was preserved; the CPU reference sampler was corrected
to follow its already implemented filtering contract. The viewport status row
and Retry control now retain a stable layout so frame completion cannot resize
and invalidate the request it just completed.

Remaining risks: recipe/receipt digests establish integrity and freshness, not
authenticated execution; generated blobs can remain unreferenced after a
failed commit; flat-project source assets must travel with the project; C++
hosts must rebuild for the extended action variant. No persisted schema or C
SDK ABI was changed. Bake freshness is checked at adoption; later mesh/UV edits
do not automatically rebake maps or turn historical receipt metadata into live
freshness evidence. Large production assets and destination runtime admission
need separate qualification.

## Executed verification

Windows Release build with Clang 22.1.8, Ninja, Vulkan desktop UI and D3D12
readback support. `cartographer_desktop` and every named test executable built.
`git diff --check` passed.

**Final result on the rebased branch: 20/20 targeted tests passed in 45.10
seconds.** This is not the full 273-test suite. Exact final build/test logs are
`build/texturing/acceptance-final-build.log` and `acceptance-final-tests.log`.

- New `cartographer_texturing_workflow_tests`: passed under CTest. The final
  combined run took 7.89 seconds. It imports the actual bundled First Block
  OBJ, creates UVs and a new material, preserves a UTF-8 image filename, imports
  and packs maps, reviews and accepts the edit, assigns the mesh, renders it,
  bakes/adopts AO and tangent normal, checks undo/redo, saves/reopens, migrates
  a package, performs Save As, exports glTF and GLB, reimports GLB, and verifies
  neutral prepared publication on re-read. Source/round-trip CPU display
  pixels match exactly for this fixture after explicitly configuring the same
  consumer light; glTF's profile does not export the authored lighting.
  A second GLB round trip includes the adopted tangent-normal bake and verifies
  appearance within one display byte for float32/display quantization. The
  missing packed-map case regenerates exact bytes under the same source ID;
  fresh replacement pixels cannot reuse a generated map's old provenance.
- Existing passing tests: `authoring_viewport`, `authoring_material`,
  `authoring_texture_snapshot`, `material_catalog`, `texture_channel_pack`,
  `texture_source`, `material_artifact`, `bake_graph`,
  `bake_tile_incremental`, `lightmap_uv`, `prepared_scene`,
  `uv_material_contract`, `gltf_scene_import`, `gltf_material_export`,
  `material_assignment_integration`, `authoring_shader_compile`,
  `authoring_surface`, `authoring_gpu_viewport`, `agent_workbench`
  (each named `cartographer_<name>_tests`).
- `cartographer_authoring_gpu_viewport_tests`: initially failed at
  `GPU/CPU HDR BRDF parity`. Investigation found that the existing shader took
  up to eight anisotropic taps while the CPU used one isotropic lookup. After
  adapting the branch's CPU sampler fix, this unchanged strict GPU test and
  `cartographer_authoring_surface_tests` **both passed** (13.92 seconds total).
  No tolerance was relaxed. The new numerical case independently expects .25
  for eight taps across the black/white ramp; a single center tap gives zero.
- After the UTF-8 intake fix, six affected workflow/import/export/material
  tests were rebuilt and rerun: **6/6 passed**, 55.77 seconds total. See
  `build/texturing/closeout-build.log` and `closeout-tests.log`.
- The earlier targeted results in `build/texturing/regression-tests.log`
  record the original GPU failure. Its successful repair is recorded in
  `parity-fix-tests.log`; subsequent workflow/render checks are in
  `render-integration-tests.log`. The final 20-test run supersedes those
  intermediate results.

Adversarial assertions include missing justification, foreign project/mesh
identity, wrong session/generation, stale material and receipt, consumed or
canceled review, corrupt artifact bytes, unsupported tile, missing UV set,
missing texture pixels, and generated-source reimport with false lineage.
Refused adoption preserves canonical document, history count and journal
bytes. Background bake leaves project truth untouched; acceptance appends a
durable mutation. Normal convention and exact adopted pixel/UV-row order are
checked. There is no org/role-tier or naive-datetime model in this module;
project namespace/session/revision and human admission are the actual tested
boundaries.

Final automated artifacts are in
`build/texturing/tests/texturing-evidence/texturing-11408282665700/`: source
images/blobs, `.carto` and journal, AO/normal bake publications, saved-as copy,
package, glTF/GLB, reimported project, CPU PPM frames and neutral prepared
publication, including `with-baked-normal.glb` and its reimported project.
`normal-bake.png` is a lossless conversion of the CPU PPM for
inspection, not a native GPU screenshot.

## Visual verification and acceptance limit

Using the computer-use skill, an isolated native desktop instance reopened
the generated scene. Observed the Material workspace opening its instrument,
texture filenames and generated-map records, persisted material/map bindings,
UV set/sampler IDs, offset `(0.125, 0.25)`, scale/pivot and channel controls.
A native rotation step changed `0.0000` to `0.0100` with Live apply: the
document advanced from revision 19 to 20 and Undo became available. Ctrl+Z
restored `0.0000` at revision 21; Ctrl+S saved the restored state. This was
performed only in a generated QA project. The separate active renderer
worktree window was not operated on.

CPU images were visually inspected and show the mapped block. Initial native
inspection exposed a delivery loop: the completion status/Retry controls
changed canvas height and therefore invalidated the completed request. After
giving those controls stable layout, native readback reached **Ready** and
displayed the actual checker texture. Frame all produced an inspectable mesh.
Changing base-color UV rotation from `0` to `1.5708` through the Material panel
advanced revision 19 to 20 and visibly rotated the native texture from its
original pattern to a red-left/blue-right pattern. Undo at revision 21 restored
the original mapping and native appearance; Ctrl+S saved it. No restart or
external application was needed for this material edit. This is visual
evidence for this fixture, not a performance measurement or blanket native
acceptance. The factor-only workspace was not used as texture-render evidence.
Docking/detaching uses existing infrastructure; its drag lifecycle was not
requalified.

Not run: the entire 273-entry CTest suite, large/production-asset performance
workloads, external VANTA runtime admission, all UDIM/decoder variants, and an
all-mouse authoring walkthrough. The supplied RegOS/IDA Python and frontend
npm commands do not match this C++ checkout and were not run. No dependency
installation, migration or deployment was performed.

## Capability delta and remaining integration work

Capability gained: the existing authoring/catalog/asset paths now join imported
maps, explicit linear metallic/roughness packing, receipt-bound AO/normal bake
adoption, reviewed commands, live finished-edit apply, persistence and
interchange. Explicit material UV binding can reach glTF without inventing an
active-render UV flag. Generated lineage survives inventory, package migration
and Save As. Missing generated image bytes can be restored by an explicit,
verified regeneration rather than returning success for an absent asset.

Boundary improved: stale/foreign/corrupt bake inputs and stale/consumed proposal
transitions refuse without a project/history/journal mutation; no worker or
raw AI proposal gains direct mutation authority; missing pixels and unsupported
UDIM/packed-lookdev paths remain visible failures.

Governance preserved:

- State path: existing command bus and catalog/document commands.
- Audit path: existing `execute_command` / mutation-journal machinery.
- Scope path: project namespace, mesh identity/digest/revision, session and
  generation pins; existing assignment locks/face validation.
- Human approval: explicit action admission, and justified acceptance of a
  read-only proposal. No broader Agent Workbench effect was granted.

The primary checkout advanced externally to `173a3c0` while this work started
on `f749a04`. That commit modifies `libs/ui/src/ui.cpp` as well as Agent
Workbench code/tests. The isolated feature commit was rebased cleanly onto
`173a3c0`, preserving the newer subtree-translation work. The combined native
build and 20 targeted tests validate that integration; final results are
recorded below. No primary-checkout file was replaced or branch switched.

Required follow-up: broaden visual coverage to face assignment and full
save/export round trips on the native path; qualify native packed lookdev mappings or retain its explicit
refusal; test destination VANTA decoding/admission with a supported profile.

Cross-product impact: direct changes are only in Cartographer. No RegOS,
IDA_Training, hidden_canopy_bot or Ask_IDA repository was modified or tested;
their indirect consumer impact is unverified. Shared contracts touched are
Cartographer's additive C++ action/review API and generated provenance
conventions. Existing glTF/prepared schemas stay in use. C++ consumers need a
rebuild; no new serialized project schema is required. Eval pressure improves
through these regression fixtures; no production dataset or training run was
changed. Deployment/promotion requires the native and destination checks above.

Dataset progress: regression fixtures improved; no training dataset edited.
Training/prompt progress: none.
Capability progress: command/data/persistence/interchange evidence above.
Promotion progress: the fixture's native material-edit appearance and strict
GPU parity checks pass; broader production/native and destination acceptance
still require the follow-up checks above. The newer primary subtree-translation
commit is retained in the feature branch's ancestry.
