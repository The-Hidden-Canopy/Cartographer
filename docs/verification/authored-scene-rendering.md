# Authored-scene rendering continuation

This is an executable path from a saved Cartographer project to actual GPU
pixels. A code-only continuation now declares directional shadow execution,
but it is unbuilt and unrun. This is not photorealistic acceptance or completion
of the production render-graph, desktop viewport, Motel 8, or all-outstanding-
tasks goals.

## Capability delta and repair

The new optional `carto_d3d12_scene` library validates immutable render meshes,
current material sources, exact lookdev packages, full-width texture IDs,
source-bound mip artifacts, transforms, camera and work bounds before GPU
submission. It draws multiple submeshes/objects into depth-tested linear HDR,
applies ACES/sRGB exposure once, and reads back HDR, depth and display pixels.
The saved-project adapter consumes authored UV0, hard-edge normals, region
assignments, material variants and world transforms without changing the
document. A bound graph with constant/arithmetic nodes can feed supported
base-color, alpha, roughness, metallic, occlusion, emissive, clearcoat, and
clearcoat-roughness outputs into PBR material factors. The graph binding's exact revision and
digest must still match. Missing parameter/surface inputs, texture sampling,
graph-bound material variants, unsupported graph outputs, missing
UVs/materials/textures and stale/foreign products are refused explicitly.
Code-only V21/V22 continuations now execute straight-alpha blended submeshes
and source-bound vertex colors; centroid sorting, color-space interpretation,
native shader/pixel behavior, and acceptance remain unverified.

The PBR shader previously transformed normals with the model matrix. A native
comparison against equivalent geometry with transforms baked into its vertices
failed under nonuniform scale. The repair uses inverse-transpose cofactors and
reverses tangent parity for mirrored transforms, preserving the 232-byte root
constant and 52-byte vertex ABIs. The same GPU comparison now passes, including
a nonflat tangent-space normal map.

`cartographer_scene_render` loads `.carto` source, requires explicit camera,
source identity, extent and shader directory, and writes a new BMP plus JSON
receipt. The receipt binds source/request/shader/pixel/file digests, settings,
actual adapter identity and limitations. Reported wall time includes shader
compilation/resource creation/submission/readback; it is not GPU pass timing.
Host output payload bytes are sizes of returned arrays/encoded BMP, not peak
host or GPU memory. Existing output/receipt paths are refused.
Request digests identify inputs; baseline/cache comparisons must also bind
shader binaries and device evidence. `device_reported_driver` is the backend's
reported label, not an independently queried Windows driver-version receipt.

## Code-only direct-scene graph wiring (2026-10-26)

The authored-scene renderer now declares its HDR, depth, display and optional
caller-owned output resources in `GraphBuilder`. Its PBR, tone-map and
caller-target copy-source handoff pass order is compiled and checked as command
recording proceeds; resource transitions come from the declared pass usages.
Capture and direct-target receipts report the passes actually recorded. The
offscreen and caller-target regression assertions and CLI receipt contract were
updated alongside the implementation. None of these changes has been built or
run under the current instruction to code without testing.

This does not integrate the full editor application frame. The D3D12 shell now
graph-wires its scene-copy/background-clear/present path, while the native
shadow/IBL/temporal fixture remains a separate parity test. Native window
capture, scene-wide effects and performance acceptance remain outstanding.
The parent work-queue item remains started.

The D3D12 desktop loop now records scene-copy or background-clear and
backbuffer-present transitions from a compiled frame graph. Its frame recorder
checks pass dispatch and validates the command stream before submission. This
application-frame wiring has not been built or exercised on a native window.
The D3D12 shell's Home command now resets its camera controls to the originally
fitted scene view; source assertions cover bounded-state restoration, but are
unrun.

## Code-only material-graph wiring (2026-10-26)

`prepare_project_scene_render` now consumes a current graph binding through the
existing deterministic CPU evaluator and copies supported output values into
PBR factors before compiling the lookdev artifact. The graph digest is part of
the derived material identity. A changed graph behind an old binding fails as
stale; unprovided parameter/surface values and reachable texture samples fail
through the evaluator. Output channels without a matching PBR field and
graph-plus-variant precedence are explicitly unsupported. Each material graph
is evaluated once per project preparation and reused across its submeshes; the
sum of graph source nodes evaluated in one preparation is capped at 262,144.
The saved project remains read-only.

Changed in this continuation: `libs/d3d12/src/project_scene_renderer.cpp`,
`libs/d3d12/include/carto/d3d12/scene_renderer.hpp`, and
`tests/d3d12_scene_renderer_tests.cpp`. The test source now replaces the old
blanket graph-refusal assertion with prepared-factor, stale-binding,
missing-input, and unsupported-output cases. Documentation also changed in
this ledger, `docs/verification/material-graph-authoring.md`,
`docs/capability-matrix.md`, and the ignored local queue. No production schema,
journal contract, renderer shader ABI, or dependency changed. Rollback only
these implementation/API-comment, test, and documentation hunks; preserve the
earlier renderer work in the shared dirty tree.

The native scene-renderer regressions now cover successful factor wiring,
stale graph binding refusal, missing-parameter refusal, and unsupported-output
refusal. They were authored but not built or run in the current code-only
sprint. This does not establish
GPU shader lowering for graph nodes, texture sampling, native viewport refresh,
or graph-based scene pixels.

## Code-only saved-material texture source wiring (2026-10-26)

The D3D12 desktop and capture CLI now call
`load_project_scene_textures` before preparing the saved-project render. It
collects texture references from visible material assignments and resolved
variants, verifies each exact project-relative content-addressed blob and its
SHA-256, re-inspects format and dimensions against the current catalog record,
decodes PNG/JPEG through the shared bounded Windows Imaging Component path or
TGA through the existing decoder, and generates semantic sRGB/linear/normal
mip chains. `SceneRenderRequest` then receives only those current project
pixels. Missing/corrupt/stale sources, unsafe locators, unsupported DDS/EXR
pixel decoding, conflicting semantic use of one asset, and aggregate mip data
above 512 MiB fail closed.

The CLI's optional `--texture` input remains accepted only when its full
derived mip artifact digest exactly matches the chain generated from the
current project bytes. A matching source digest alone can no longer substitute
for source pixels. A saved-project regression was authored for verified TGA
decoding, semantic mip binding, render-request integration, cross-semantic
reuse, missing project blobs, and tampered source bytes. These changes have not been built or run; no
native texture render or acceptance receipt is claimed.

The execution results below predate this graph-wiring continuation and do not
verify the current source after these edits.

## Execution evidence

- Before the shader repair, the native scene test passed project preparation,
  admission and scene execution, then failed the nonuniform/mirrored normal
  comparison. After repair its four test groups passed directly.
- The CLI acceptance script rendered a saved 14-object synthetic room at
  640x400 on the NVIDIA Quadro P5200. Two separate executions produced identical
  pixel, bitmap and request hashes. Source bytes were unchanged; receipt hashes,
  shader count, draw count and exact BMP length matched. Existing-output and
  invalid-camera attempts were refused without changing the existing image or
  publishing an invalid capture.
- The actual bitmap was opened and visually inspected. It depicts the authored
  floor, walls, bed, pillows, furniture and lamp. It visibly remains a blockout.
  The capture and receipt are retained in the ignored build evidence directory;
  there is no committed portable visual baseline or production-scene approval.
- Initial foundation CTest passed 3/3 (21.86 seconds total): scene renderer 4.54 s,
  saved-project CLI 3.96 s, existing D3D12 regression suite 12.55 s. The latter
  executed all 16 existing native groups. Final native debug-state assertions
  passed. Scoped diff checks passed. Current tree/index and selected `HEAD`
  publication audit with ignore-policy checking returned `PUBLIC_BOUNDARY_PASS`;
  other history refs, forks and consumers were not audited.
- Later graph, texture and desktop changes required a fresh source build.
  `repo-rendering-boundaries.md` records the subsequent reproduced failures,
  V2 repairs, direct-target pressure tests and current verification limits.

Reproduce with the D3D12/CLI/tests options enabled:

```text
cmake --build build/d3d12 --target cartographer_scene_render cartographer_d3d12_scene_renderer_tests cartographer_d3d12_tests
ctest --test-dir build/d3d12 -R "^(cartographer_d3d12_scene_renderer_tests|cartographer_scene_render_cli_tests|cartographer_d3d12_tests)$" --output-on-failure
cartographer_scene_render input.carto new-output.bmp --identity review-scene --shaders libs/d3d12/shaders --camera 5 3.1 5.5 -0.3 0.8 -1 --size 640 400
```

Bound textures load from `.carto-assets/source-blobs` automatically. Optional
`--texture <asset-id> <serialized-mip-file>` artifacts must exactly match the
current source-derived mip artifact; they cannot supply replacement pixels.

## Repair ledger

- Changed files: `libs/assets/CMakeLists.txt`,
  `libs/assets/include/carto/assets/texture_image_decoder.hpp`,
  `libs/assets/src/texture_image_decoder.cpp`,
  `apps/desktop/reference_image_decoder.cpp`, `apps/desktop/CMakeLists.txt`,
  `libs/d3d12/src/project_scene_textures.cpp`,
  `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/src/project_scene_renderer.cpp`,
  `libs/d3d12/CMakeLists.txt`, `apps/d3d12_desktop/main.cpp`,
  `apps/cli/scene_render.cpp`,
  `libs/d3d12/shaders/cartographer_pbr.hlsl`,
  `libs/d3d12/CMakeLists.txt`, `apps/cli/scene_render.cpp`,
  `apps/cli/CMakeLists.txt`, `tests/d3d12_scene_renderer_tests.cpp`,
  `tests/run_scene_render_cli_tests.cmake`, the scene-test registration hunks in
  `tests/CMakeLists.txt`, this ledger, acceptance matrix and ignored local queue.
  Other existing shared-workspace changes are outside this continuation.
- Prior verified boundaries, all predating graph wiring: stale
  project/mesh/material content, source identity swaps, absent
  assignments/UVs/textures, wrong texture semantic,
  tampered mips, duplicate/low-word-aliased texture IDs, duplicate objects,
  singular/unstable transforms, float32 world-geometry collapse, invalid cameras,
  nonfinite values, allocation bounds, missing shaders, masked alpha, draw-order
  independent depth, camera-dependent pixels, CPU tone-map parity, caller-owned
  resources and clean native debug state. Save/reopen preserves request identity.
- Governance preserved: no authoring transition or domain mutation is added;
  the const project adapter and source-byte checks prove read-only preparation.
  No journal/event writes are required or fabricated for GPU capture. No AI
  publication or human approval bypass, role/org authority, schema migration,
  credentials, network calls or cross-repository edits are introduced. Org scopes,
  role tiers, justifications, invalid domain transitions and datetimes are not
  inputs to this derived renderer; their application boundaries are unchanged.
- Not run for graph and source-texture wiring: any build, test, lint, or native GPU/render
  interaction. Not run for the wider renderer: full repository suites,
  Vulkan/full desktop visual interaction,
  clean-machine installation, downstream consumers, sustained/8K scene workloads,
  and RegOS/IDA_Training/hidden_canopy_bot/Ask_IDA suites. The generic `backend`,
  `ida_core` and `frontend` directories do not exist in this C++ workspace.
- Remaining risks: fixed 0.03 ambient fallback when no SH input is supplied;
  optional diffuse SH remains unverified; the new scene shadow pass has no
  execution evidence; no specular environment/reflection lighting,
  antialiasing, temporal accumulation, sculpt/
  deformation evaluation or transparent output. DDS/EXR decode and shared
  Windows codec linkage are unverified after the current edits. Capture synchronously allocates
  temporary GPU resources and compiles shaders. Declared payload limits are not
  reservations or peak-memory guarantees. BMP/receipt publication is a two-file
  operation and an I/O failure can leave a partial derived output. The opt-in
  D3D12 desktop project view now consumes the renderer through a direct GPU
  target, but full render-graph and native-window acceptance remain open.
  Shader repair affects all D3D12 PBR
  users; existing native shader regressions must pass.
- Rollback: remove only the new scene API/implementation/CLI/tests and their
  target/registration hunks; restore only the shader normal/tangent repair
  hunks. Preserve unrelated edits in shared CMake/docs files. No project/schema,
  journal, database or source migration needs reversal. Derived captures may be
  retained as evidence.
- Intentionally rejected: generated illustrations as renderer evidence, hidden
  default materials or stale texture fallback, treating fixed ambient as IBL,
  claiming a GPU benchmark from host wall time, broad authoring refactors, and
  accepting caller mip pixels from source-digest metadata alone. Production or
  photorealistic gates are not closed from this synthetic room.

## Cross-product trace and promotion

Direct impact is limited to Cartographer's optional D3D12 rendering and CLI.
Indirect impact is a reviewable, source-bound image path for future consumer
acceptance. The shared contracts touched are existing render mesh, lookdev,
material/mip and shader ABIs; new capture API/receipt types are additive. Saved
project and consumer schemas are unchanged. No direct changes were made to
RegOS, IDA_Training, hidden_canopy_bot or Ask_IDA. Their adapters must retain
source/receipt identities and require attributed consumer tests before accepting
these images as external/native/production evidence.

Dataset progress: new synthetic geometry and adversarial regression pressure.
Training/prompt progress: none. Capability progress: bounded authored-scene GPU
capture and corrected transformed normal maps, with shadow execution authored
but unverified. Promotion progress: local native foundation evidence only.
Remaining promotion tests include real-scene material and texture fidelity,
shadow readback/visual acceptance, IBL, native desktop camera/presentation
acceptance, portable image baselines, measured sustained workloads and consumer
acceptance.

## Code-only directional shadows (2026-10-26)

The scene renderer now derives a directional orthographic projection from the
current transformed vertices, records an alpha-tested caster pass into the
compiled graph, and samples its 1024-square-or-smaller color depth map with 3x3
PCF in a scene-specific PBR shader. The graph includes caster/PBR texture reads
and per-object vertex/index buffer reads, and dispatches texture and buffer
transitions at first use. Offline captures add a `scene.capture-readback` pass
that transitions HDR, camera depth, and display sources to copy-source before
the readback helpers issue staging copies. Caster cutouts use the current
base-color texture and alpha factor. A precomputed
light-space vertex attribute keeps this scene shader's texture table within
D3D12's 64-DWORD root-signature limit. The
shared `cartographer_pbr.hlsl` and `cartographer_shadow.hlsl` public ABIs remain
unchanged. Request identity advances to V3, and receipts bind six shader
digests so pre-shadow captures cannot be reused as current results.

Added source assertions for `scene.shadow`/`scene.capture-readback` pass order
and the V3/six-shader CLI receipt contract; removed a duplicate render-graph
key from the CLI receipt and added a uniqueness assertion. The expanded
68-byte scene vertex layout is
included in the bounded mesh-upload calculation. No build, shader compilation,
test, GPU execution, image inspection, or display interaction was performed.
This code is unverified and
does not close native shadow quality or overall render-graph acceptance. One
shadow map does not provide cascades/contact shadows, and a very large scene
extent can reduce effective shadow resolution. No environment/reflection or
temporal pass was added.

Source review found that the shadow pixel shader divided rasterized
`SV_POSITION.z` by pixel-stage reciprocal `SV_POSITION.w`, corrupting the
stored shadow depth. It now writes raster depth directly to both color and
hardware depth. This correction is source-only; no shader compile or shadow
readback has confirmed it.

## Code-only diffuse environment lighting (2026-10-26)

The scene request accepts either a bounded order-2 real SH interchange file
with nine RGB triples for Lambert-convolved diffuse irradiance normalized by
pi, or an opaque 2:1 equirectangular PNG/JPEG/TGA panorama. The file loader
hashes exact SH bytes; direct C++ request construction must supply its own
source digest. The panorama path hashes exact image bytes, requires an explicit
linear or sRGB interpretation, converts sRGB to linear light, and integrates
the image over pixel solid angles into the same SH basis (+Y up, +Z at image
center, +X right). Both paths are bounded; images are limited to 256 MiB,
16,777,216 pixels, and 16,384 pixels per dimension. Transparent images,
unsupported pixel decoders, non-2:1 layouts, unknown color spaces, malformed SH
files, and unbounded coefficients refuse. PNG/JPEG/TGA image input is 8-bit
LDR; the Radiance RGBE path is documented below. CPU coefficient derivation is
synchronous and bounded by the source/pixel caps, but its runtime and peak
memory have not been measured. The CLI and source-backed D3D12 desktop accept
`--diffuse-irradiance-sh <file>` or
`--environment-equirectangular <image> --environment-color-space linear|srgb`.
Request identity binds the source digest, derived coefficients, and image color
space; capture/target receipts expose the environment digest and optional color
space.

The renderer uploads the coefficients as a 3x3 RGBA32F texture and evaluates
diffuse SH per pixel. The material table now uses seven sampled textures; its
56 DWORD constants plus seven texture tables and one sampler table use the
entire 64-DWORD D3D12 root-signature budget. Source review found the native
parameter/range backing arrays still sized for six textures; they now fit seven
SRV tables, the sampler table and root constants. If the input is absent, the
shader retains the explicitly reported fixed ambient approximation. This adds
diffuse environment lighting only: there is no specular environment prefilter
or reflection.

V5 request identity separates these inputs from prior captures and records
image color-space interpretation. CLI and desktop options reject conflicting
input kinds and require the image color space. Loader, color conversion,
aspect/alpha refusal, over-bound coefficient, native-pixel, digest, duplicate-
option, and receipt assertions were authored but not run. No build, test,
shader compilation, GPU execution, image inspection, or desktop interaction was
performed. The code remains unverified; SH parity, image quadrature, the exact
64-DWORD pipeline, and the D3D12 device's seven-texture binding path need
acceptance evidence.

## Code-only Radiance RGBE environment input (2026-10-26)

The existing equirectangular loader now also accepts bounded Radiance RGBE
panoramas with `32-bit_rle_rgbe` headers. The source digest binds exact file
bytes; dimensions remain capped at 16,384 per axis, 16,777,216 pixels, and
256 MiB, with a 64 KiB aggregate header cap. `EXPOSURE` and `COLORCORR`
multipliers are applied with finite bounds;
non-unit `GAMMA`/`PIXASPECT`, any explicit `PRIMARIES` field, unsupported header fields,
malformed modern RLE packets, malformed legacy pixels/runs, non-2:1 layouts,
and trailing data refuse.
The loader treats RGBE samples as linear and requires the existing explicit
color-space option to be `linear`. It integrates rows directly, retaining only
one decoded scanline in addition to the bounded input bytes.
The native environment chooser includes `.hdr`, and the scene-render CLI and
D3D12 desktop usage text identify Radiance RGBE as a supported input. The
extension filter is convenience only; loader admission uses the file signature
and format header.

No test source was added or run, and no build, shader compile, GPU execution,
pixel inspection, or native review was performed. The parser, header transform
handling, scanline orientation, and SH output remain unverified. This adds
linear HDR diffuse input only; explicit primaries, unsupported Radiance transforms,
EXR and specular prefiltering remain unsupported. The desktop temporal preview
was wired later under V29 as a bounded source-only path; its native behavior is
not established by this RGBE continuation.

- **Files changed:** `libs/d3d12/src/environment.cpp`,
  `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `apps/desktop/main.cpp`, `apps/cli/scene_render.cpp`,
  `apps/d3d12_desktop/main.cpp`, this ledger, and the local work queue.
- **Capability gained:** bounded Radiance RGBE panorama bytes can supply
  digest-bound diffuse irradiance to the existing D3D12 scene path.
- **Governance preserved:** the loader is read-only, refuses unsupported color
  transforms, and does not infer external asset provenance or mutate a project.
- **Risks:** parser and SH math are uncompiled and untested; RGBE color-space
  assumptions and header multiplier interpretation still need fixture and
  GPU acceptance.
- **Rollback:** remove the RGBE branch/helpers and restore the 8-bit-only API
  comment; no saved schema or project data changed.
- **Rejected:** clamping unsupported color transforms, treating malformed or
  unknown header fields as neutral, or labeling RGBE radiance as sRGB.
- **Cross-product trace:** direct impact is Cartographer's D3D12 scene
  environment loader. No RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA
  contract changed; no dataset or evaluation artifact changed. No deployment
  occurred.
- **Required follow-up:** add RGBE fixtures for constant radiance, signed axis
  orientations, exposure/color correction, malformed and over-bound packets,
  unknown headers, and stale source bytes; then build the D3D12 scene path,
  inspect derived coefficients/pixels, and review a native scene capture.

### Radiance legacy RGBE compatibility continuation (2026-10-26; code-only)

Removed the accidental eight-pixel minimum from the bounded 2:1 image
admission. Scanlines with the modern marker continue through the bounded
per-channel RLE decoder; other streams use the legacy four-byte RGBE path,
including bounded previous-pixel run expansion across row boundaries. This
allows small valid flat RGBE panoramas and legacy-encoded images without
allocating a full decoded image. Legacy runs require a prior literal pixel and
are bounded by the remaining image texels; malformed/truncated streams and
trailing bytes still refuse. No test source was changed or run, and no build,
GPU execution, coefficient inspection, or native review was performed.

- **Files changed:** `libs/d3d12/src/environment.cpp`, this ledger, and the
  ignored local work queue.
- **Capability gained:** small flat RGBE panoramas and bounded legacy RGBE
  repeat encoding can supply the existing digest-bound diffuse SH input.
- **Boundary improved:** image dimensions remain under the existing dimension,
  pixel, byte, and exact-2:1 limits; legacy runs cannot refer to a missing pixel
  or expand beyond the image.
- **Governance preserved:** loading stays read-only and source identity still
  hashes the exact input bytes; unsupported color transforms remain refused.
- **Tests:** not run, per the code-only sprint instruction. Required follow-up
  includes small flat fixtures, legacy runs crossing a row, zero/oversized run
  cases, truncation, and modern RLE parity, followed by build and native image
  review.
- **Risks:** the revised decode branch is source-reviewed only; legacy run
  semantics, SH values, and compatibility with real Radiance producers remain
  unverified.
- **Rollback:** restore the width minimum and the modern-RLE-only row loop.
- **Rejected:** rejecting all legacy streams or allocating a full decoded image
  to simplify the parser.
- **Cross-product trace:** direct impact is Cartographer's D3D12 environment
  loader. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are
  unchanged; dataset, training, deployment, and promotion progress are
  unchanged.

Changed in this continuation: the scene environment API/integration, bounded
equirectangular decoder and SH derivation, scene PBR shader, D3D12 graphics
texture-table bound, scene CLI, D3D12 desktop options, fixture/tests, and these
ledgers. No saved project schema, journal, authoring state, or cross-product
API changed. Rollback only the V5 equirectangular derivation/color-space
identity, V4 optional SH path and seven-texture table, their CLI/desktop wiring,
corresponding assertions, and ledger paragraphs; preserve all unrelated dirty
work.

Files changed: renderer API/implementation, two scene-only HLSL programs,
scene-renderer and CLI receipt assertions, queue, acceptance matrix, and this
ledger. No saved project, schema, journal, or authoring state changed. Rollback
only the scene shadow pass, V3 identity/shader digest additions, assertions,
and corresponding ledger paragraphs; retain all unrelated dirty work.

### V6 request-bound spatial FXAA wiring (2026-10-26; code-only)

The D3D12 tone-map pass now has a request-bound FXAA mode that evaluates
luminance and directional filtering on tone-mapped display colors. Saved-scene
and desktop requests default to FXAA; the capture CLI accepts
`--antialias fxaa|none`, the D3D12 desktop shell toggles it with F2, and the
main desktop's offline Scene preview has an explicit checkbox whose mode is
part of preview freshness.
Request digests bind the selected mode, and image and GPU receipts report it.
The algorithm identity is V6. Scene-linear HDR and
depth capture remain unchanged. This is spatial filtering only; temporal
accumulation, specular environment lighting, full desktop graph parity, and
visual/performance acceptance are still open. No test source was added or
changed, and no build, test, shader compilation, GPU execution, or visual
review was run at the user's request. The new shader path is unverified and may
increase per-pixel cost on high-resolution captures. Rollback the V6 request
field/digest identity, tone-map filtering, CLI option and receipt metadata, and
this ledger entry; preserve earlier D3D12 work.

### V7 source-bound panorama reflection wiring (2026-10-26; code-only)

Eligible 2:1 LDR panorama loads now retain a color-correct mip chain up to
4,194,304 decoded pixels. The scene renderer packs the signed diffuse-SH
coefficients into the existing RGBA32F environment texture and uploads the
panorama mip chain to that same resource. The PBR shader samples the reflected
view direction at a roughness-selected mip for base specular and clearcoat,
starting at mip one because mip zero stores SH. This adds environment-colored
reflections without another SRV, root constant, saved-project field, or journal
mutation. Request identity and material-texture cache identity bind the mip
artifact digest. Larger LDR panoramas and Radiance inputs preserve their prior
diffuse-SH behavior and do not claim specular contribution.

The mips use the existing area-box filtering in equirectangular space. The
renderer does not yet perform GGX convolution or a split-sum BRDF lookup, and
the V7 path did not decode HDR radiance pixels for reflections. The receipt
limitations name those bounds. This code-only change has not been built, run,
shader-compiled, rendered, or visually reviewed. Source tests were added for
eligible mip derivation, stale-source refusal, and environment-color changes
reaching metallic pixels with diffuse SH cleared. GPU upload alignment,
shader sampling, color-space correctness, performance, and visible quality
remain unverified. No product data, authoring authority, or downstream API
changed; RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA remain untouched.

### V8 linear Radiance reflection mip wiring (2026-10-26; code-only)

Eligible 2:1 Radiance RGBE inputs now retain canonical linear RGB mip levels
through the existing t6 environment slot. Mip generation applies spherical
pixel-overlap weights, preserves odd-size tails and radiance above one; artifact
identity binds the exact float pixels and source digest. The renderer validates extents,
finite nonnegative values, linear interpretation, and the 4,194,304-pixel / 64
MiB source-mip bounds before upload. The GPU RGBA32F upload remains capped at
128 MiB. Valid inputs above the reflection pixel or 1e12 radiance bounds omit
HDR mips and retain diffuse SH if its coefficients pass validation. Explicit
Radiance primaries and globally over-budget images are refused. The V8 request
identity changes with the HDR artifact digest.

Source regression code covers RGBE decoding, above-one linear values, stale
source and artifact identity, and an HDR reflection changing native metallic
pixels while the cache switches source. No builds, tests, shader compilation,
GPU execution, visual review, or frame-time measurement were run at the user's
request. RGBE orientation/exposure behavior and the float upload/shader path
remain unverified. No saved project or downstream schema changed; RegOS,
IDA_Training, hidden_canopy_bot, and Ask_IDA remain untouched. Dataset and
training progress are unchanged, capability progress is code-only, and
promotion remains open.

### Equirectangular reflection seam wiring (2026-10-26; code-only)

The D3D12 PBR shader now performs explicit bilinear environment sampling with
wrapped longitude and clamped latitude, then blends adjacent roughness mip
levels. This removes the panorama's false hard edge at longitude 0/1 while
keeping the SH payload in mip zero excluded from reflection samples. Added a
native regression source with contrasting first/last panorama columns and
metallic views immediately on both sides of the seam. No build, shader
compilation, test, GPU run, or visual review was performed at the user's
request.

- **Files changed:** `libs/d3d12/shaders/cartographer_scene_pbr.hlsl`,
  `tests/d3d12_scene_renderer_tests.cpp`, this ledger, and the ignored local
  work queue.
- **Capability gained:** roughness-selected reflections interpolate across
  the horizontal panorama boundary instead of clamping at separate edges.
- **Governance preserved:** this is derived rendering only; source digest,
  project, authoring, journal, receipt and promotion authority are unchanged.
- **Tests added:** native source compares center HDR pixels for symmetric
  directions around the seam. **Run:** none. Shader compilation, supported
  device execution, and captured image review remain outstanding.
- **Risks:** HLSL integer-coordinate sampling and its GPU cost are unverified;
  the area-filtered mip chain present at this point was superseded by the
  source-only V9 GGX derivation below, which remains unverified.
- **Rollback:** restore `SampleLevel` with the clamp sampler and remove the
  seam fixture/assertions and this section.
- **Rejected:** changing panorama orientation or filtering the source bytes to
  hide the seam; longitude periodicity belongs at the sampling boundary.
- **Cross-product trace:** Cartographer's D3D12 shader only. RegOS,
  IDA_Training, hidden_canopy_bot and Ask_IDA APIs/data/deployments are
  unchanged; dataset/training progress is unchanged, capability is code-only,
  and promotion remains open.

### V9 shared GGX-prefiltered environment mip wiring (2026-10-26; code-only)

LDR and linear Radiance inputs now share a bounded canonical linear-float mip
chain. Mip zero keeps the decoded panorama except for the existing packed SH
patch; later levels use 32 Hammersley samples from a GGX importance distribution
and N·L-weighted radiance. The request/cache artifact digest binds source bytes,
input color-space interpretation, algorithm sample count, dimensions, and
pixels. The shader selects the mip from roughness, manually wraps longitude,
and diverts samples touching the SH patch to mip one. The request algorithm is
V9. There is no split-sum BRDF LUT.

CPU regression source now covers constant LDR/HDR across roughness levels, a
bright odd-size source tail, source/artifact identity, and legacy RGBE parity
with the migrated chain. The CLI receipt source expects V9.
No builds, tests, shader compilation, rendering, or review were run at the
user's request. Worst-case prefilter time, HLSL behavior, upload correctness,
and visual quality remain unverified. No saved project or downstream product
schema changed. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA remain
unchanged; dataset/training progress is unchanged, capability is code-only,
and promotion remains open.

### V10 transient material-graph input wiring (2026-10-26; code-only)

The saved-scene adapter now accepts typed transient inputs for bound material
graphs only when their graph revision and canonical digest match the current
saved source. Supported graph outputs become PBR factors, and the input digest
participates in material and request identity. The desktop forwards values only
after the CPU evaluation receipt validates against the saved graph; the
scene-render CLI accepts repeated typed parameter and surface inputs and
records their source/input digests in the receipt. Editing a desktop input
invalidates the prior capture. No graph inputs are saved, and the live
Vulkan material viewport remains unevaluated. Aggregate input validation is
capped at 262,144 values across graph bindings. Missing/mistyped or
output-unreachable inputs, stale/foreign sources, unused graph bindings, graph
texture samples, graph-plus-variant combinations, and unsupported outputs fail
closed.

- **Files changed:** D3D12 adapter API and implementation, desktop and CLI
  input wiring, scene-renderer/CLI regression sources, and verification ledgers.
- **Capability gained:** explicit graph inputs can drive supported PBR outputs
  in the offline saved-scene D3D12 preview and CLI capture.
- **Boundary improved:** preview identity records the exact graph and input
  values; stale or irrelevant input bindings cannot be silently accepted.
- **Governance preserved:** this is a transient read-only evaluation with no
  project, journal, schema, or promotion mutation.
- **Tests added:** source cases cover parameter and surface input flow, request
  identity changes, CLI receipt digests, missing/unreachable values, and
  foreign/stale source bindings. **Run:** none at the user's instruction; no
  build, shader compile, capture, or review was run.
- **Risks:** compile correctness and actual desktop/GPU behavior remain
  unverified; graph textures, durable graph parameters, and live viewport
  shading remain unsupported.
- **Rollback:** remove the optional graph-input map and desktop forwarding,
  revert the algorithm identity and corresponding tests, and remove this entry.
- **Rejected:** saving preview values into authored material state, defaulting
  absent inputs, or widening live viewport claims.
- **Cross-product trace:** direct impact is Cartographer's offline D3D12
  preview and CLI capture; no RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA
  contract or data change. Dataset/training progress is unchanged, capability
  progress is code-only, deployment is unverified, and promotion remains open.
  Required follow-up: build, targeted graph/scene/CLI tests, shader compilation,
  and native saved-scene capture review.

### V11 durable material-graph inputs (2026-10-26; code-only)

Material catalog schema v4 stores typed per-material parameter and surface
values alongside each graph source pin. The human-only application action
requires exact project/material/graph revision and digest evidence, validates
supplied keys against reachable input nodes, advances material revision, and
commits through the existing journaled catalog path. Partial input maps remain
authored data; evaluation refuses any required values still missing. Catalog
versions 1 through 3 remain readable with empty binding input sets.

The saved-scene adapter now defaults to durable binding values. A
revision/digest-pinned per-request input map replaces the complete saved set
for that capture and does not mutate project state. Both paths bind input digests into derived
material and request identity. Stale graph pins, wrong types, unreachable
values, texture samples, variants, and unsupported output channels still fail
closed. The renderer source and regression cases were authored but not built,
run, or visually reviewed. The Vulkan material viewport remains unevaluated.

### V12 ignored material texture controls (2026-10-26; code-only)

The scene-capture request now refuses bound material texture settings the
current shader does not execute: non-UV0 sets, non-default sampler IDs,
non-identity transforms, non-default channel mappings, color interpretations
that conflict with the slot semantic, and OpenGL-Y normal maps. Existing
explicit UV0, identity transform, default channel/sampler, semantic color-space,
and DirectX-Y bindings retain the current path. This makes the current source
boundary honest while the shader ABI lacks per-slot control wiring.

- **Files changed:** `libs/d3d12/src/scene_renderer.cpp`,
  `tests/d3d12_scene_renderer_tests.cpp`, this ledger, the capability matrix,
  and the local work queue.
- **Capability gained:** none in rendered functionality; unsupported authored
  material intent now receives an explicit refusal instead of a misleading
  capture.
- **Boundary improved:** non-default UV, sampling, transform, channel,
  interpretation, and normal-convention data cannot silently disappear in the
  D3D12 path.
- **Governance preserved:** source materials and project state remain
  read-only; the adapter keeps exact source/digest admission and does not
  synthesize texture state.
- **Tests added:** source regressions cover each ignored setting and preserve
  the existing default base-color texture path. **Run:** none at the user's
  instruction; no build, test, shader compile, render, or visual review was
  performed.
- **Risks:** the new request checks and authored regressions are uncompiled and
  unrun; existing projects using these controls will now fail scene capture
  until the shader implements them.
- **Rollback:** remove the texture-control helper and its request-validation
  call, then remove the added source regressions and this V12 ledger entry.
- **Rejected:** silently ignoring settings, applying a partial UV transform,
  guessing sampler behavior, or marking source bindings as fully rendered.
- **Cross-product trace:** Cartographer's offline D3D12 capture only; no
  RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA contract/data changed.
  Dataset and training progress are unchanged; capability work is code-only,
  deployment is unverified, and promotion remains open. Follow-up: compile and
  run scene-render tests, then implement per-binding UV/material-control shader
  wiring and inspect actual capture pixels.

**Superseded by V27 for channel mappings:** the scene adapter now lowers
supported per-slot RGBA selection, one-minus, and constant-zero/one selectors
into the scene vertex stream, and the PBR shader applies them to base color,
metallic/roughness, normal, occlusion, and emissive samples. The generic
material compiler remains strict; the D3D12 adapter normalizes only its
internal compile copy and retains the exact authored source digest. Conflicting
color interpretations and unsupported normal conventions remain refused.

### V13 masked alpha coverage mips (2026-10-26; code-only)

The verified project-texture loader derives a quantized source-alpha threshold
from each resolved masked material's alpha cutoff and base-color alpha factor,
then preserves covered-texel ratio through generated mips. A texture shared by
masked materials with incompatible effective thresholds is refused. Opaque
materials keep ordinary averaged alpha mips; zero-cutoff masked materials need
no coverage adjustment. The scene capture algorithm identity advances to V11
so receipts from this revised render preparation remain distinguishable. No
source, catalog, or mip-artifact format changes.

- **Files changed:** `libs/d3d12/src/project_scene_textures.cpp`,
  `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `tests/d3d12_scene_renderer_tests.cpp`,
  `tests/run_scene_render_cli_tests.cmake`, this ledger, and the local queue.
- **Capability gained:** masked base-color textures retain authored cutout
  coverage through the loader's generated mip chain.
- **Boundary improved:** shared texture uses with incompatible masked cutoffs
  refuse instead of silently selecting one material's coverage rule.
- **Governance preserved:** exact project-local source bytes and digest remain
  required; mip generation is derived and does not mutate project state.
- **Tests added:** source cases cover sparse alpha through lower mip levels,
  unchanged opaque filtering, and incompatible cutoffs. **Run:** none at the
  user's instruction; no build, test, render, or visual review was performed.
- **Risks:** no compilation or pixel execution evidence; only the shared
  source-loader path receives this adjustment, and native material render
  acceptance remains open.
- **Rollback:** remove the requested alpha-threshold tracking and enablement in
  the loader, remove its source regression, and remove this V13 ledger entry.
- **Rejected:** using one default cutoff, mutating source alpha in storage, or
  claiming unrelated source-pixel and shader paths have passed.
- **Cross-product trace:** Cartographer's D3D12 source loader only. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA contracts/data are unchanged;
  dataset/training progress is unchanged, capability is code-only, deployment
  is unverified, and promotion remains open. Follow-up: build and run texture
  mip/scene tests, then inspect alpha-masked capture pixels at multiple
  distances.

### V14 CLI camera view inputs (2026-10-26; code-only)

`cartographer_scene_render` now accepts optional camera-up, vertical field of
view, and near/far clip values. Omitted options keep the previous defaults.
Receipts serialize the actual camera-up vector alongside the already-recorded
field of view and clip range; all four values already participate in the
request digest. The CLI delegates range and degeneracy checks to the existing
scene-render request validation before it stages output.

- **Files changed:** `apps/cli/scene_render.cpp`,
  `tests/run_scene_render_cli_tests.cmake`, this ledger, the capability matrix,
  and the local work queue.
- **Capability gained:** offline captures can represent camera views beyond
  the CLI's fixed-up/default-projection view, with matching receipt provenance.
- **Boundary improved:** duplicate controls, parallel up vectors, out-of-range
  field of view, and inverted clip ranges have authored refusal cases that
  require no image or receipt publication. A changed view must change the
  request digest.
- **Governance preserved:** scene loading remains read-only, source identity
  and digest admission are unchanged, and the renderer remains the authority
  for camera validation. Camera inputs remain part of capture identity.
- **Tests added:** receipt checks for every camera-up component and the
  projection/clip values, changed-view digest coverage, plus duplicate and
  invalid-input refusal cases. **Run:** none at the user's instruction; no
  build, test, render, or visual review was performed.
- **Risks:** the CLI and CMake acceptance source are uncompiled/unrun; native
  camera behavior and projection conventions have no new capture evidence.
- **Rollback:** remove the three CLI options, duplicate `--camera` guard, and
  dynamic camera-up receipt field, then remove their CLI acceptance cases and
  this V14 entry.
- **Rejected:** changing default camera behavior, accepting last-wins duplicate
  options, or claiming a native camera review from source-level wiring.
- **Cross-product trace:** Cartographer's offline scene-render CLI only.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts/data remain
  unchanged; dataset/training progress is unchanged, capability is code-only,
  deployment is unverified, and promotion remains open. Follow-up: build and
  run the CLI scene-render acceptance, then compare receipt values to a native
  capture and inspect the resulting view. Parent remains started.

### V15 texture-free capture without UV0 (2026-10-26; code-only)

Saved-scene preparation no longer requires UV0 when every assigned material is
texture-free. The render-mesh packer retains its strict one-argument behavior;
the added overload requires one validated material-constants value per
submesh and permits neutral GPU UV/tangent inputs only when every texture ID is
zero. Current material artifacts are source-validated before the draw package
reaches the renderer. Textured meshes without authored UV0 still refuse.

- **Files changed:** `libs/production/include/carto/production/render_mesh.hpp`,
  `libs/production/src/render_mesh.cpp`,
  `libs/production/src/lookdev_draw_package.cpp`,
  `libs/d3d12/src/project_scene_renderer.cpp`,
  `libs/d3d12/src/scene_renderer.cpp`,
  `tests/production_closure_tests.cpp`,
  `tests/d3d12_scene_renderer_tests.cpp`, this ledger, the capability matrix,
  and the local work queue.
- **Capability gained:** a saved scene with solid-color materials can reach
  capture without authored UV coordinates.
- **Boundary improved:** texture-free input is admitted with unused neutral
  vertex values; any texture-bound draw still requires authored UV0 and its
  tangent frame. The existing strict packer call remains strict.
- **Governance preserved:** no project attributes or source meshes are
  modified, no UV data is claimed as authored, the GPU vertex layout and
  render-mesh artifact format are unchanged, and textured source admission
  remains fail-closed. The original one-argument packer API stays strict; the
  additive overload requires per-submesh material evidence rather than a
  caller-set boolean.
- **Tests added:** default packer refusal and explicit texture-free packing,
  saved-scene acceptance without UV0 for untextured materials, and refusal for
  textured material evidence with missing UV0. **Run:** none at the user's
  instruction; no build, test, render, or visual review was performed.
- **Risks:** the C++ and saved-scene integration paths are uncompiled/unrun;
  only the no-texture case is admitted, and GPU output remains unreviewed.
- **Rollback:** restore the adapter's unconditional UV0 requirement and strict
  packer calls, then remove the material-evidence overload, regression cases,
  and this V15 entry.
- **Rejected:** filling missing UV coordinates for textured materials, or
  weakening the general PBR upload API's default strictness or trusting an
  unchecked caller boolean as proof that a mesh is texture-free.
- **Cross-product trace:** Cartographer's saved-scene production/D3D12 path
  only. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts/data are
  unchanged; dataset/training progress is unchanged, deployment is unverified,
  and promotion remains open. Follow-up: build and run production-closure and
  scene-render suites, capture a texture-free UV-less fixture, and separately
  confirm the textured refusal. Parent remains started.

### V16 opt-in GPU timing receipt (2026-10-26; code-only)

`cartographer_scene_render --gpu-timing` now requests graphics-queue timestamp
queries around the scene command stream. The readback buffer is imported into
the render graph and transitions to copy-destination through a recorded
`scene.gpu-timing-resolve` pass. The capture refuses before artifact publication
if timing is requested on an asynchronous path or the graphics queue does not
expose timestamp queries. When requested and resolved, the receipt includes the
timestamp frequency and rounded elapsed nanoseconds. The interval covers the
recorded scene graph passes; it excludes the timing-resolve pass, host setup,
subsequent pixel readback, and any presentation after the submission. Timing
is opt-in and is excluded from request identity because it does not change
rendered pixels. The existing three-argument C++ API remains available through
an uninstrumented overload; timing uses an additive four-argument overload.

- **Files changed:** `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/scene_renderer.cpp`, `apps/cli/scene_render.cpp`,
  `tests/run_scene_render_cli_tests.cmake`, this ledger, the capability matrix,
  the acceptance matrix, and the local work queue.
- **Capability gained:** native scene captures can carry measured queue-time
  evidence separately from host render wall time.
- **Boundary improved:** timing is absent when not requested, present with
  query frequency and resolved GPU timestamps when requested, or the capture
  refuses; no CPU duration is relabeled as GPU time.
- **Governance preserved:** timestamps describe only the submitted graphics
  queue interval, readback is ordered by the render graph, source/pixel request
  identity remains unchanged, and no state or project data is mutated.
- **Tests added:** CLI acceptance source checks the untimed null receipt,
  requested timing fields/frequency/pass, request and pixel identity parity,
  and duplicate-option refusal. **Run:** none at the user's instruction; no
  build, test, render, or timestamp readback was performed.
- **Risks:** production code and acceptance source are uncompiled/unrun; queue
  timestamp behavior and receipt serialization have no fresh native evidence.
- **Rollback:** remove the opt-in API argument and timing fields, timestamp
  buffer/graph pass/readback, CLI option and receipt fields, CLI acceptance
  cases, and this V16 entry.
- **Rejected:** inferring GPU time from `render_wall_time_ns`, reporting a
  fallback/estimated value, or making timing instrumentation change capture
  identity.
- **Cross-product trace:** Cartographer's D3D12 scene-render path only. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA contracts and datasets are
  unchanged; dataset and training progress are unchanged, capability work is
  source-only, deployment and promotion remain unverified. Follow-up: build
  and run the D3D12 scene and CLI acceptance suites, then compare the receipt's
  queue interval with a native capture. Parent remains started.

### V17 shared material UV scale/offset (2026-10-26; code-only)

The saved-scene D3D12 path now carries an axis-aligned UV scale and offset in
both PBR and alpha-shadow root constants. Its scene-only material adapter
packs transform-independent GPU factors while binding the artifact to the
original material digest; generic material compilation remains strict. The
shader applies the same
`(uv - pivot) * scale + pivot + offset` mapping before every material sample,
including the base-color alpha test used to build the shadow map. A request is
admitted only when all bound texture slots share the same transform. Nonzero
rotation and per-slot conflicts remain explicit unsupported inputs; existing
UV-set, sampler, channel-map, color-interpretation, and normal-convention
refusals remain intact. The render algorithm fingerprint advances to V12.

- **Files changed:** `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/src/project_scene_renderer.cpp`, both scene PBR/shadow shaders,
  `tests/d3d12_scene_renderer_tests.cpp`, the CLI acceptance source, this
  ledger, the capability and acceptance matrices, and the local work queue.
- **Capability gained:** saved-scene D3D12 captures can execute shared
  axis-aligned texture scale/offset with authored pivot semantics.
- **Boundary improved:** requests carrying rotation, float32-overflowing values,
  or incompatible transforms across material slots still refuse before GPU
  submission; varied texture fixtures cover direct and cached pixel parity.
- **Governance preserved:** source, material, and texture inputs remain
  revision/digest bound; no project mutation or fallback texture is introduced;
  shadow alpha sampling uses the same admitted transform as PBR sampling.
- **Tests added:** prepared identity and transform validation, rotation,
  float32 overflow and per-slot conflict refusal, plus direct/cached textured
  pixel and request identity parity. A source case also checks that the
  generic material compiler still refuses transforms while the D3D12 adapter
  binds the original material digest.
  **Run:** none at the user's instruction; no build, shader compile, test,
  render, or capture was performed.
- **Risks:** CPU/HLSL constant-buffer agreement and visual sampling remain
  unverified; common-transform admission is intentionally narrower than the
  authoring model.
- **Rollback:** restore the V11 shader ABI and algorithm fingerprint, revert
  the UV constant packing and shader sampling changes, and remove the authored
  acceptance cases and this V17 entry.
- **Rejected:** silently dropping UV transforms, weakening the shared material
  compiler for every renderer, applying per-slot transforms through one shared
  constant, or approximating rotation with scale/offset.
- **Cross-product trace:** Cartographer's saved-scene D3D12 renderer only.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA remain untouched;
  dataset/training progress is unchanged; this is source-only capability work,
  deployment and promotion remain unverified. Follow-up requires running the
  scene-render tests, shader/runtime compile, and a native pixel capture.
  Parent status stays started.

### V18 per-texture affine UV streams (2026-10-26; code-only)

V13 replaces the shared UV constants with separate transformed UV ranges in
per-object vertex/index buffers. Each of the five material texture slots now
has its own affine mapping, including rotation and pivot. PBR samples use the
matching slot coordinate; the shadow pass uses the base-color coordinate for
the same alpha test. PBR root constants remain 224 bytes (56 DWORDs), leaving
the existing 64-DWORD signature budget for seven sampled textures and the
sampler table. Generic material compilation remains strict; the scene adapter
preserves the exact authored material digest. Requests refuse absent authored
UV0 for textured materials and transformed float32 overflow. Other UV sets,
samplers, channel maps, color interpretations, and unsupported normal
conventions remain unsupported.

- **Files changed:** `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/src/project_scene_renderer.cpp`,
  `libs/d3d12/shaders/cartographer_scene_pbr.hlsl`,
  `libs/d3d12/shaders/cartographer_scene_shadow.hlsl`,
  `tests/d3d12_scene_renderer_tests.cpp`, this ledger, the capability and
  acceptance matrices, and `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- **Capability gained:** saved-scene captures can render rotation, pivot,
  scale, and offset independently for each supported material texture slot.
- **Boundary improved:** per-slot transform disagreements are executed rather
  than rejected or silently flattened; UV overflow still fails before GPU
  submission. Per-draw ranges share object-level buffers, and the payload bound
  includes packed mesh inputs plus worst-case transformed vertex/index data.
- **Governance preserved:** source/material/texture digests remain authoritative;
  no source mutation, fallback texture, or generic compiler relaxation was
  introduced. The 64-DWORD root signature limit is preserved.
- **Tests added/updated:** prepared transform identity, rotated pivot behavior,
  independent base-color/metallic-roughness transforms, normal-map UV/tangent
  frame effects, transformed pixels in direct and cached render paths, and
  float32-overflow refusal. **Run:** none
  per the user's instruction; no compile, test, render, or capture was run.
- **Risks:** CPU stream packing, HLSL semantics, tangent-frame adjustment,
  cache invalidation, and visual sampling are uncompiled and unverified;
  vertices are duplicated across draws and rely on the bounded upload estimate.
- **Rollback:** restore V12's shared UV constants and shader sampling, restore
  the V12 algorithm fingerprint, revert the per-draw stream packing/bindings,
  and remove this entry and the corresponding source acceptance cases.
- **Rejected:** adding more root constants past the existing signature budget,
  widening the generic material compiler, discarding per-slot transforms, or
  approximating rotation with axis-aligned scale/offset.
- **Cross-product trace:** Cartographer D3D12 only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA are unchanged; dataset/training progress is
  unchanged, capability progress is source-only, deployment and promotion
  remain unverified. Follow-up requires building the renderer/shaders, running
  scene and CLI acceptance, and reviewing a native capture. Parent remains
  started.

**Capability Delta**

- **Boundary improved:** each admitted texture slot now carries its own affine
  UV coordinates, including rotation and pivot; float32 overflow and unsupported
  binding controls still fail closed.
- **Governance preserved:** authored digests, generic compiler strictness,
  source immutability, and the human approval boundary are unchanged.
- **Tests:** added and updated above; none run by instruction.
- **Rejected options:** larger root constants, universal compiler relaxation,
  or silently shared/flattened UV transforms.
- **Remaining risk:** CPU/HLSL ABI and rendered behavior remain unverified.
- **Promotion status:** dataset unchanged; training unchanged; capability source
  work only; promotion remains unverified.

### V19 OpenGL Y+ normal-map wiring (2026-10-26; code-only)

The saved-scene D3D12 path now executes both authored tangent-space normal
conventions. OpenGL Y+ bindings reverse the per-draw tangent handedness after
the normal-map UV affine is applied; DirectX Y- retains the existing path. The
scene-only material adapter normalizes the convention only in its temporary
generic compiler input and restores the exact authored material digest on the
resulting artifact. The generic material compiler remains strict.
The scene-render algorithm fingerprint advances to V14, and the CLI receipt
acceptance expectation follows that identifier.

- **Files changed:** `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/shaders/cartographer_scene_pbr.hlsl`,
  `tests/d3d12_scene_renderer_tests.cpp`,
  `tests/run_scene_render_cli_tests.cmake`, this ledger, the capability and
  acceptance matrices, and the local work queue.
- **Capability gained:** scene captures can render normal maps authored with
  OpenGL Y+ convention alongside DirectX Y- maps.
- **Boundary improved:** the convention is preserved through current material
  identity and affects each draw's tangent frame; unsupported convention data
  attached to non-normal texture slots still refuses.
- **Governance preserved:** scene preparation remains read-only; material
  source digest and revision are retained; generic material compilation is not
  relaxed; no source pixels or project records are rewritten.
- **Tests added/updated:** saved-project preparation and artifact identity
  cover OpenGL admission while generic compilation still refuses it; native
  source cases compare OpenGL and DirectX request and pixel identities with a
  transformed normal-map UV frame and verify cached pixels against a fresh
  render after the convention changes. **Run:** none at the user's instruction;
  no build, test, shader compile, render, or capture was performed.
- **Risks:** tangent handedness and shader pixel output are uncompiled and
  unverified; existing source acceptance does not establish visual correctness.
- **Rollback:** restore the DirectX-only scene-binding refusal, restore the
  V13 algorithm fingerprint and matching CLI receipt expectation, then remove
  convention propagation, regressions, and this V19 entry. No project schema
  or saved data changed.
- **Rejected:** flipping normal source pixels, changing the generic compiler's
  contract, or claiming native acceptance without rendering evidence.
- **Cross-product trace:** Cartographer D3D12 only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts and datasets remain unchanged.
  Dataset/training progress is unchanged; capability progress is source-only;
  deployment and promotion remain unverified. Follow-up: build and run the
  scene and CLI suites, compile shaders, and compare a native normal-map capture.

**Capability Delta**

- **Boundary improved:** both supported Y conventions now produce separately
  identified per-draw tangent frames, including when a normal UV affine is
  applied.
- **Governance preserved:** exact material source lineage, read-only scene
  preparation, generic compiler strictness, and the human publication boundary
  remain intact.
- **Tests:** saved-project source identity and direct rendered-pixel cases
  added/updated; none run by instruction.
- **Rejected options:** rewriting pixels, universal compiler relaxation, or
  silently treating Y+ as Y-.
- **Remaining risk:** shader/tangent ABI, runtime behavior, and native visual
  parity remain unverified.
- **Promotion status:** dataset unchanged; training unchanged; capability source
  work only; promotion remains unverified.

### V20 shared authored sampler wiring (2026-10-26; code-only)

The saved-project D3D12 adapter now maps current project sampler intents into
validated native sampler descriptors. A draw may use one shared non-default
sampler across its bound material texture slots; different sampler IDs within
one draw remain explicitly unsupported because the current pipeline binds one
sampler table per draw. The same selected sampler is bound for PBR and masked
base-color shadow sampling. Descriptor fields and identities participate in
request and material-texture cache digests. Sampler resources follow the cached
material-resource lifetime. The scene-render algorithm advances to V15.

- **Files changed:** `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/scene_renderer.cpp`, `libs/d3d12/src/device.cpp`,
  `libs/d3d12/src/project_scene_renderer.cpp`, `libs/gpu/src/gpu.cpp`,
  `tests/d3d12_scene_renderer_tests.cpp`,
  `tests/run_scene_render_cli_tests.cmake`, this ledger,
  `docs/capability-matrix.md`, `docs/verification/acceptance-matrix.md`, and
  the ignored local work queue.
- **Capability gained:** saved-project captures execute the authored repeat,
  mirrored-repeat, or clamp-to-edge address modes and nearest/linear
  minification, magnification, and mip filtering represented by a sampler
  intent. The D3D12 native filter mapping now preserves all eight point/linear
  combinations, including for other device users that create comparison
  samplers. The scene renderer refuses comparison-sampler use.
- **Boundary improved:** missing descriptors, unused direct-request
  descriptors, invalid sampler enum values, and multiple sampler IDs across
  one draw refuse before submission; comparison descriptors also refuse
  because scene shaders do not perform comparison samples. Sampler changes
  alter request/cache identity; the test source compares out-of-range
  repeat/clamp pixels and cached results with fresh renders.
- **Governance preserved:** project and material identities remain bound to
  their exact source digests; scene adaptation changes no saved state; the
  generic material compiler remains strict; custom handles retire with their
  owning cached resources; no fallback or guessed sampler is introduced.
- **Tests added/updated:** project-intent mapping and material-digest
  preservation, direct request identity and descriptor-boundary cases,
  per-draw conflict refusal, native min-versus-mag sampling, and repeat/clamp
  cached pixel parity. **Run:**
  none at the user's instruction; no build, test, shader compile, render, or
  capture was performed.
- **Risks:** C++ mapping, cache lifetime, sampler table binding, and native
  pixel behavior are uncompiled and unverified. The pipeline still cannot use
  different sampler IDs for separate texture slots in one material draw.
- **Rollback:** restore the V14 algorithm identifier and CLI expectation,
  remove request sampler descriptors and their digest/validation logic,
  restore the previous D3D12 filter conversion, remove project-intent mapping
  and draw bindings, then remove these source cases and this V20 entry. No
  project schema changed.
- **Rejected:** silently use the default sampler, flatten multiple sampler
  intents to one guessed descriptor, relax the generic material compiler, or
  change project-authored sampler records.
- **Cross-product trace:** Cartographer D3D12 only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts and data remain unchanged;
  dataset/training progress is unchanged, capability work is source-only,
  deployment and promotion remain unverified. Follow-up: build and run scene
  and CLI acceptance, then review a native capture. Parent remains started.

**Capability Delta**

- **Boundary improved:** the saved-scene path maps each authored material-slot
  sampler independently and preserves request/cache identity for assignments
  and repeat/clamp/filter changes.
- **Governance preserved:** current material/project digests, read-only
  preparation, bounded native descriptor validation, and the human approval
  boundary remain intact.
- **Tests:** source acceptance added/updated; none run by instruction.
- **Rejected options:** default-sampler fallback, sampler flattening, generic
  compiler relaxation, or claims of native pixel acceptance.
- **Remaining risk:** implementation and native rendering remain unverified.
- **Promotion status:** dataset unchanged; training unchanged; capability source
  work only; promotion remains unverified.

### V21 per-texture sampler wiring (2026-10-26; code-only)

Saved-project scene rendering now accepts a distinct current sampler intent for
each bound material texture slot. The D3D12 pipeline descriptor declares a
bounded consecutive sampler prefix, while its zero value retains the legacy
single-sampler graphics path. The scene PBR shader binds s0..s4 to base color,
metallic/roughness, normal, occlusion, and emissive textures; alpha-shadow
sampling still uses the base-color sampler. To keep the root signature under
64 DWORDs, the scene constant block is 192 bytes and the per-draw camera,
clearcoat, surface factors, and occlusion-map flag use flat vertex attributes.
The mesh cache key now includes camera position because the upload carries it.
Request identity includes sampler-to-slot assignment.

- **Files changed:** `libs/gpu/include/carto/gpu/rhi.hpp`,
  `libs/gpu/src/gpu.cpp`, `libs/d3d12/src/device.cpp`,
  `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/src/project_scene_renderer.cpp`,
  `libs/d3d12/shaders/cartographer_scene_pbr.hlsl`,
  `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `tests/d3d12_scene_renderer_tests.cpp`,
  `tests/device_tests.cpp`,
  `tests/run_scene_render_cli_tests.cmake`,
  `docs/capability-matrix.md`, `docs/verification/uv-materials-deep-series.md`,
  and this ledger.
- **Capability gained:** separate authored sampler descriptors are carried
  from each project material slot into its native PBR sampler register.
- **Boundary improved:** mixed sampler IDs in one draw are accepted only when
  every bound non-default sampler has a current descriptor; sampler
  assignments are bound into request identity, and missing/unused descriptors
  remain refused.
- **Governance preserved:** source material digests remain exact, project
  preparation remains read-only, generic material compilation stays strict,
  and sampler resources retain their cache-owner lifetime.
- **Tests added/updated:** direct request acceptance for independent slot
  samplers and identity changes when two assignments are swapped; a native
  render case varies only metallic/roughness addressing and checks changed
  pixels plus fresh/cached parity. **Run:** none per the user instruction; no
  build, test, shader compile, render, or capture was performed.
- **Risks:** root-signature creation, vertex interpolation qualifiers,
  per-slot sampler binding, cache invalidation, and resulting native pixels
  remain uncompiled and unverified. Per-draw upload size increases by 48 bytes
  per generated vertex and remains covered by the existing 512 MiB bound.
- **Rollback:** restore the V15 algorithm fingerprint and CLI expectation;
  revert the sampler-prefix field and D3D12 root/binding changes; restore the
  previous PBR root constants and vertex payload; then remove the per-slot
  acceptance source and this entry. No saved-project schema changed.
- **Rejected:** clamp all slots to the base-color sampler, silently map
  missing descriptors to defaults, exceed the 64-DWORD root-signature budget,
  or relax the generic material compiler.
- **Cross-product trace:** Cartographer D3D12 only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts/data are unchanged; dataset and
  training progress are unchanged, capability work is source-only, deployment
  is unverified, and promotion remains open. Follow-up: build and run the
  scene-render and D3D12 suites, then inspect a native capture. Parent remains
  started.

### V22 authored UV-stream selection (2026-10-26; code-only)

The saved-project D3D12 adapter now resolves each bound material slot's authored
UV-set identity to one of the two coordinate streams represented by the
current render-mesh artifact. It selects the normal-map set as the primary
stream so generated tangents use the matching coordinates, and binds that
identity through request and mesh-cache digests. Missing UVs and objects that
need more than two distinct texture UV sets refuse. Because the mesh artifact
contains one tangent frame per vertex, normal maps must share the primary UV
set; requests that need multiple normal-map UV sets fail closed instead of
reusing a mismatched tangent basis. No project schema changed.

- **Files changed:** `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/src/project_scene_renderer.cpp`,
  `tests/d3d12_scene_renderer_tests.cpp`,
  `tests/run_scene_render_cli_tests.cmake`, `docs/capability-matrix.md`,
  `docs/verification/uv-materials-deep-series.md`, and this ledger; the local
  queue records the continuation in `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- **Capability gained:** material textures can sample either explicitly mapped
  source UV stream, and the project adapter chooses tangent UVs from authored
  normal-map intent.
- **Boundary improved:** stale/missing stream assignments, more than two
  distinct sets, and multiple normal-map UV identities fail closed; UV-set
  assignments participate in request and mesh-cache identity.
- **Governance preserved:** project preparation remains read-only, source
  material digests remain exact, generic material compilation stays strict,
  and no saved-project schema or authority changed.
- **Tests added/updated:** direct request acceptance for mapped UV1 and request
  identity; direct refusal for normal maps on a secondary stream without a
  matching tangent frame; render acceptance compares pixels when a base-color
  slot selects UV1. **Run:** none at the user's instruction; no build, test,
  shader compile, render, or capture was performed.
- **Risks:** C++ integration, artifact stream alignment, shader interpolation,
  tangent selection, cache identity, and changed pixels remain uncompiled and
  unverified. More than two distinct UV sets and more than one normal-map UV
  set per object remain unsupported.
- **Rollback:** restore the V17 fingerprint and CLI expectation and remove the
  primary-tangent UV refusal plus its source regression to undo this boundary
  correction. To remove the broader UV-stream feature, restore V16 and remove
  object stream identities, project UV selection, per-slot stream lookup,
  digest/cache fields, and associated acceptance/documentation.
- **Rejected:** silently substitute UV0 for missing or unrepresented sets,
  use one tangent basis for different normal-map UV sets, expand the persisted
  mesh schema during this bounded continuation, or claim native pixel
  acceptance from authored source cases.
- **Cross-product trace:** direct impact is Cartographer D3D12. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA contracts and data are unchanged;
  dataset and training progress are unchanged, capability progress is
  source-only, and deployment and promotion remain unverified. Follow-up:
  build and run focused scene request/project/CLI suites, then inspect a native
  UV1 capture and real saved-scene material assignments. Parent remains
  started.

**Capability Delta**

- **Capability gained:** saved-scene material sampling resolves an authored
  texture UV-set ID to either source-bound vertex stream in the current artifact.
- **Boundary improved:** missing UVs, unsupported stream counts, and tangent
  basis mismatches are explicit failures rather than guessed UV0 sampling.
- **Governance preserved:** source identity, read-only preparation, strict
  generic material compilation, and human approval boundaries are unchanged.
- **Tests:** added direct acceptance/refusal and pixel-difference source cases;
  none run by instruction.
- **Rejected options:** UV0 fallback, multiple normal UVs over one tangent
  basis, persisted schema expansion, and claims of rendered acceptance.
- **Remaining risk:** implementation and native rendering are unverified; the
  mesh artifact currently exposes at most two UV streams.
- **Promotion status:** dataset unchanged; training unchanged; capability
  source work only; promotion remains unverified.

### V23 bounded multi-stream authored UV wiring (2026-10-26; code-only)

The earlier two-stream ceiling is removed for material sampling. Derived
render-mesh artifact v4 carries up to eight authored UV streams, with extra
stream identities stored once per artifact and corner-split coordinates stored
per vertex. The compiler includes every selected stream in seam splitting,
canonical validation, serialization, and artifact identity. The D3D12 project
adapter chooses a primary stream, a secondary stream, and bounded additional
streams from the actual material-slot assignments; all five material texture
slots can then resolve their own UV identity. Normal maps still require the
primary tangent stream, and v1-v3 artifact decoding remains supported. The
saved-project schema is unchanged. A follow-up static review moved the total
stream-count check into v4 size admission and decoder header handling, before
reserving stream IDs or vertex storage.

- **Files changed:** `libs/production/include/carto/production/render_mesh.hpp`,
  `libs/production/src/render_mesh.cpp`,
  `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/src/project_scene_renderer.cpp`,
  `tests/production_closure_tests.cpp`,
  `tests/d3d12_scene_renderer_tests.cpp`,
  `tests/run_scene_render_cli_tests.cmake`, `docs/capability-matrix.md`,
  `docs/verification/uv-materials-deep-series.md`, and this ledger; the local
  queue records the continuation in `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- **Capability gained:** saved scenes can resolve material texture UV IDs across
  up to eight source-bound UV streams, including more than the legacy UV0/UV1
  pair, while retaining seam-correct derived vertices.
- **Boundary improved:** missing, aliased, duplicate, non-finite, and over-bound
  streams fail closed. Normal maps cannot sample with a tangent basis built
  from another stream. Artifact byte and scene payload limits include extra UV
  values.
- **Governance preserved:** no project schema or source authority changed;
  preparation remains read-only, source digests remain exact, and existing
  v1-v3 render-mesh artifacts remain decodable.
- **Tests added/updated:** artifact v4 serialization/reopen and v3 compatibility;
  extra-stream seam splitting and malformed metadata refusal; direct D3D12
  additional-stream identity/pixel cases; project preparation with three
  different material-slot UV assignments; missing UV refusal remains covered.
  The malformed metadata source cases also cover total stream overflow before
  the decoder reads the declared ID list.
  **Run:** none at the user's instruction; no build, test, shader compile,
  render, or capture was performed.
- **Risks:** artifact encoding compatibility, allocation bounds, seam
  canonicalization, tangent selection, cache invalidation, and native pixels
  remain unverified. Eight streams are the format ceiling; normal maps on one
  object still share one primary tangent UV set.
- **Rollback:** restore render-mesh artifact format v3 and its prior serializer,
  compiler options, and vertex layout; restore the V18 scene fingerprint/V4
  mesh-cache domain and CLI expectation; remove extra-stream scene selection,
  tests, and these documentation entries. Continue decoding v1-v3 as before.
- **Rejected:** expand the saved-project schema, collapse distinct UV IDs into
  one guessed stream, discard corner seams, or generate a false tangent basis
  for normal maps assigned to secondary/additional UV sets.
- **Cross-product trace:** direct impact is Cartographer production artifacts
  and D3D12 rendering. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA are
  unchanged; dataset/training progress is unchanged, capability progress is
  source-only, and deployment and promotion remain unverified. Follow-up:
  build/run the production-closure and D3D12 scene/project/CLI suites, inspect
  v3 artifact reopening, and review a saved-scene capture using more than two
  UV sets. Parent remains started.

**Capability Delta**

- **Capability gained:** up to eight identity-bound source UV streams reach
  saved-scene material sampling through the derived artifact and D3D12 draw
  stream generation.
- **Boundary improved:** unsupported/missing data is refused; UV seams and
  stream identities remain explicit in derived artifact identity.
- **Governance preserved:** project state remains read-only and schema-neutral;
  source digests, strict generic material compilation, and human approval
  boundaries remain unchanged.
- **Tests:** artifact, project-preparation, direct-request, and pixel cases
  authored; none run by instruction.
- **Rejected options:** UV0 fallback, seam flattening, serialized project
  migration, and mismatched normal-map tangent bases.
- **Remaining risk:** no compilation or native evidence exists; format v4 and
  the expanded UV interpolation remain unverified.
- **Promotion status:** dataset unchanged; training unchanged; capability
  source work only; promotion remains unverified.

### V24 split-sum specular environment integration (2026-10-26; code-only)

The D3D12 reflection path now applies a deterministic 128x128 split-sum GGX
BRDF lookup, generated once per process from 256 Hammersley samples per texel.
Its scale and bias terms weight the prefiltered environment for the view angle,
material roughness, and F0; clearcoat uses the same lookup at its own roughness.
The lookup uploads through a new t7 resource when specular environment mips are
present. D3D12 graphics pipelines now admit eight sampled texture tables, with
root-table storage sized for the expanded prefix; the scene PBR signature uses
61 DWORDs of its 64-DWORD limit. The renderer algorithm advances to V20, and
the in-memory material texture cache namespace advances to v4. No saved-project
schema changed.

- **Files changed:** D3D12 scene renderer/header, PBR shader, D3D12 pipeline
  descriptor implementation, generic pipeline validation regression,
  D3D12 scene-render regression, CLI receipt expectation, capability matrix,
  this ledger, and the ignored local work queue.
- **Capability gained:** roughness-selected environment reflections now include
  the integrated view/roughness/F0 response instead of multiplying by a
  Schlick Fresnel term alone.
- **Boundary improved:** the LUT is generated from bounded fixed dimensions and
  samples, bound only when the corresponding specular environment is present,
  and included in the render graph's sampled-resource usages. Without specular
  mips, t7 aliases the valid environment resource and the shader does not read
  it.
- **Governance preserved:** environment pixels and project sources remain
  read-only; no saved data, authority, or downstream contract changed. The
  versioned request identity and shader digests identify the new rendering
  semantics.
- **Tests added/updated:** generic descriptor validation now covers an
  eight-texture/five-sampler prefix and rejects an overlong sampler prefix. A
  native scene case compares smooth and rough metallic reflections under the
  same constant environment. The CLI receipt expects V20. **Run:** none at the
  user's instruction; no build, test, shader compile, render, or capture ran.
- **Risks:** HLSL compilation, D3D12 root-signature creation, upload alignment,
  cache reuse, LUT values, and visible quality are unverified. The 256-sample
  CPU integration cost and resulting image quality have not been measured.
- **Rollback:** restore V19 and the previous PBR shader's Schlick reflection
  weighting; remove t7/LUT upload and graph binding; restore the D3D12 sampled
  texture ceiling and root arrays; revert the regressions and this entry. No
  saved-project migration is involved.
- **Rejected:** continuing with a Schlick-only environment multiplier,
  describing that approximation as split-sum integration, or weakening the
  explicit 64-DWORD root-signature limit.
- **Cross-product trace:** direct impact is Cartographer D3D12 rendering.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA are unchanged; dataset
  and training progress are unchanged, capability progress is source-only, and
  deployment and promotion remain unverified. Follow-up: compile the shader,
  run the D3D12 scene/device/CLI suites, and inspect metallic and clearcoat
  captures against a fixed environment. The parent render-graph task remains
  started.

**Capability Delta**

- **Capability gained:** prefiltered environment reflections use split-sum
  BRDF scale/bias for base metal/dielectric and clearcoat response.
- **Boundary improved:** missing specular mips do not trigger LUT sampling or a
  fabricated environment contribution.
- **Governance preserved:** scene preparation remains read-only and source
  digests remain bound; no project schema changed.
- **Tests:** source coverage authored; none run per instruction.
- **Rejected options:** Schlick-only reflection multiplier and exceeding the
  root-signature budget.
- **Remaining risk:** no compile, GPU, cache, or visual evidence exists.
- **Promotion status:** dataset unchanged; training unchanged; capability
  source work only; promotion remains unverified.

### V25 straight-alpha blended submesh wiring (2026-10-26; code-only)

The D3D12 scene path now admits authored blended materials, draws opaque and
masked materials first, then sorts transparent submeshes back-to-front using
the centroid of their indexed world-space vertices. A dedicated pipeline uses
straight-alpha color blending, depth testing, and disabled depth writes.
Blended submeshes are omitted from the opaque shadow map so they cannot claim
solid occlusion. The renderer algorithm advances to V21; no saved-project
schema or material ABI changed.

- **Files changed:** D3D12 scene-renderer header/implementation, scene-render
  CLI receipt expectation, D3D12 scene regression source, capability matrix,
  this verification ledger, and the ignored local work queue.
- **Capability gained:** source-bound alpha-blended materials render after
  opaque/masked geometry with back-to-front composition in scene-linear HDR.
- **Boundary improved:** transparent draws test against opaque depth without
  replacing it; invalid source index ranges refuse; blended surfaces do not
  become fabricated opaque shadow casters.
- **Governance preserved:** material artifacts and request digests continue to
  bind current authored alpha mode and source identity; rendering is read-only
  and no project schema or cross-product contract changed.
- **Tests added/updated:** direct request validation now admits and identity-
  binds blended mode. A native pixel case submits near/far transparent draws in
  reverse order and checks back-to-front HDR composition plus opaque-depth
  preservation. The CLI receipt expectation advances to V21. **Run:** none at
  the user's instruction; no build, test, shader compile, render, or capture
  ran.
- **Risks:** C++ compilation, D3D12 blend state, native depth behavior, and the
  pixel case are unverified. Centroid sorting is approximate for large or
  intersecting submeshes; fractional transparent shadowing is not implemented.
- **Rollback:** restore V20 and the prior single PBR pipeline; reinstate
  blended-material refusal; remove draw ordering and shadow exclusion, revert
  the CLI/test changes, and remove this entry. Preserve unrelated workspace
  changes.
- **Rejected:** rendering blended input as opaque, allowing blended surfaces
  to overwrite depth, claiming solid blended shadow occlusion, or expanding
  the material schema to represent runtime sort state.
- **Cross-product trace:** direct impact is Cartographer D3D12 rendering.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA are unchanged; dataset
  and training progress are unchanged, capability progress is source-only, and
  deployment and promotion remain unverified. Follow-up: compile and run the
  focused scene/device/CLI suites, inspect overlapping and intersecting
  transparent fixtures, and review a native capture. Parent render-graph task
  remains started.

**Capability Delta**

- **Capability gained:** saved-scene draws support straight-alpha composition
  in linear HDR with opaque depth retained.
- **Boundary improved:** transparent draws sort back-to-front and cannot write
  depth or silently cast fully opaque shadows.
- **Governance preserved:** source-bound material identity remains intact;
  there is no persistence migration or authority change.
- **Tests:** boundary and pixel regressions authored; none run per instruction.
- **Rejected options:** opaque fallback, transparent depth writes, and fake
  solid shadows.
- **Remaining risk:** native ordering and blend pixels are unverified;
  intersecting surfaces and fractional shadows need additional work.
- **Promotion status:** dataset unchanged; training unchanged; capability
  source work only; promotion remains unverified.

### V26 authored vertex-color scene wiring (2026-10-26; code-only)

The D3D12 scene vertex stream now carries source-bound RGBA vertex colors; a
mesh without an authored color supplies white. The PBR shader multiplies
base-color RGB and alpha by the interpolated vertex value. Masked PBR discard
and shadow alpha tests use the same vertex alpha. The renderer algorithm
advances to V22. No project schema, material ABI, or authoring authority
changed.

- **Files changed:** D3D12 scene-renderer header/implementation, PBR and shadow
  shaders, scene CLI receipt expectation, D3D12 scene regression source,
  capability matrix, this verification ledger, and the ignored local work queue.
- **Capability gained:** saved-scene vertex and corner color attributes now
  influence base PBR shading, blended opacity, and masked cutouts.
- **Boundary improved:** source artifact validation retains bounded finite
  colors; absent color remains neutral white; masked vertex alpha discards from
  both visible color and the opaque shadow map.
- **Governance preserved:** vertex colors remain part of the derived
  render-mesh artifact and request identity. Rendering remains read-only, with
  no schema, RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA contract change.
- **Tests added/updated:** request validation now admits and identity-binds
  authored color data. A native regression checks RGB tint response and alpha
  discard/depth preservation. The CLI receipt expectation advances to V22.
  **Run:** none at the user's instruction; no build, test, shader compile,
  render, or capture ran.
- **Risks:** HLSL interface compilation, native interpolation, input-layout
  offsets, color-space interpretation, and pixel results are unverified. The
  source attribute does not carry a color-space tag; values are consumed as
  stored with no conversion.
- **Rollback:** restore V21 and the prior 148-byte upload layout, remove PBR
  and shadow color attributes/multiplication, reinstate vertex-color refusal,
  revert the CLI/tests, and remove this entry. Preserve unrelated workspace
  changes.
- **Rejected:** silently ignoring color layers, treating absent values as black,
  or changing saved-project schema to duplicate data already present in the
  render-mesh artifact.
- **Cross-product trace:** direct impact is Cartographer D3D12 rendering.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA remain unchanged; dataset
  and training progress are unchanged, capability progress is source-only, and
  deployment and promotion remain unverified. Follow-up: compile both scene
  shaders, run scene/device/CLI suites, and inspect vertex tint, alpha mask, and
  shadow pixels on a native device. The parent render-graph task remains
  started.

**Capability Delta**

- **Capability gained:** authored vertex color reaches saved-scene PBR RGB and
  alpha behavior.
- **Boundary improved:** missing colors stay neutral; masked alpha is applied
  consistently to color and shadow rejection.
- **Governance preserved:** artifact/source identity remains bound; no project
  mutation or authority change.
- **Tests:** request and native pixel cases authored; none run per instruction.
- **Rejected options:** ignore colors, default to black, or duplicate persisted
  color data.
- **Remaining risk:** native interface, interpolation, and pixel behavior are
  unverified.
- **Promotion status:** dataset unchanged; training unchanged; capability
  source work only; promotion remains unverified.

### Desktop renderer capability-label wiring (2026-10-26; code-only)

Updated the Scene instrument's environment description to match the current
renderer: roughness-selected 32-sample GGX-prefiltered mips use the split-sum
BRDF lookup. The previous label said that lookup did not exist, which became
stale after V24.

- **Files changed:** `apps/desktop/main.cpp`, this ledger, and the ignored local
  work queue.
- **Capability gained:** the desktop now describes the active environment
  reflection path accurately.
- **Governance preserved:** no renderer behavior, project state, material
  schema, or downstream contract changed.
- **Tests:** none run or added; this is a UI-description correction.
- **Risks:** no build or native visual review was run, so the label's display
  remains unverified.
- **Rollback:** restore the prior Scene instrument string and remove this note.
- **Rejected:** leaving a stale limitation in the UI or claiming capture/quality
  acceptance from this wording change.

Direct impact is Cartographer's D3D12 Scene instrument. RegOS, IDA_Training,
hidden_canopy_bot, and Ask_IDA are unchanged. Dataset, training, deployment,
and promotion progress are unchanged.

### Desktop scene-capture timing and device metadata wiring (2026-10-26; code-only)

Added an opt-in timing overload for cached D3D12 scene renders. The desktop
requests timestamps only when the graphics queue reports a nonzero timestamp
frequency, then carries the renderer's device identity, shaded draw count,
compiled pass names, antialiasing mode, and optional GPU pass duration into the
preview tooltip. The duration is labeled with its actual scope: it excludes
host setup and pixel readback. Timing remains outside request identity.

- **Files changed:** D3D12 scene-renderer header/implementation, desktop preview
  metadata and tooltip, D3D12 scene regression source, this ledger, and the
  ignored local work queue.
- **Capability gained:** a saved-scene preview can expose measured GPU pass
  timing and the renderer/device evidence already returned by the capture.
- **Boundary improved:** unsupported timestamp queries remain optional and do
  not fabricate a duration or change output identity.
- **Tests:** authored conditional native coverage for timestamp frequency,
  stable request identity, and unchanged pixels. No build, test, shader compile,
  GPU run, or native review was performed at the user's request.
- **Risks:** overload wiring, timestamp query behavior, and tooltip display are
  uncompiled and unverified; timing-enabled failure can reject a preview when a
  device advertises timestamps but a query fails.
- **Rollback:** remove the timing overload, restore the desktop's untimed cached
  call and adapter-only tooltip, and remove its conditional regression.
- **Rejected:** timing via host wall-clock duration presented as GPU time, or
  substituting synthetic values when hardware timestamps are unavailable.

Direct impact is Cartographer D3D12 scene rendering and the desktop preview.
RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA are unchanged; no serialized
schema or downstream contract changed. Dataset, training, deployment, and
promotion progress are unchanged; native renderer acceptance remains open.

### Transient key-light and exposure controls (2026-10-26; code-only)

The saved-scene preview now exposes bounded, transient directional key-light
direction/radiance and exposure controls. These values flow into the existing
source-bound render request and request digest but are never written to the
project. Preview freshness now binds a render-settings generation, so changing
the light or exposure invalidates already displayed pixels.

- **Files changed:** `apps/desktop/main.cpp`,
  `apps/desktop/studio_viewport_capture.hpp`,
  `tests/editor_camera_tests.cpp`, this ledger, and the ignored local work queue.
- **Capability gained:** users can tune direct illumination and exposure in the
  D3D12 saved-scene preview without changing authored project state.
- **Boundary improved:** zero/non-finite light direction, invalid radiance, or
  out-of-range exposure disables rendering; stale captures cannot survive a
  light/exposure change.
- **Governance preserved:** controls are explicitly preview-only, and all
  render input remains bound by the existing request digest. No project schema
  or mutation path changed.
- **Tests:** authored a stale render-settings binding regression. No build,
  test, shader compile, GPU run, or native UI review was performed at the user's
  request.
- **Risks:** UI wiring and freshness behavior are uncompiled and unverified;
  broad light/exposure ranges may produce visually poor, though bounded,
  captures.
- **Rollback:** remove the transient controls and state, restore the fixed
  default light/exposure, and revert the preview settings-generation binding
  and its regression.
- **Rejected:** persisting preview lookdev values into the project without an
  authored command, or leaving old pixels visible after render settings change.

Direct impact is Cartographer's D3D12 Scene instrument. RegOS, IDA_Training,
hidden_canopy_bot, and Ask_IDA remain unchanged. Dataset, training, deployment,
and promotion progress are unchanged; rendered quality acceptance remains open.

### Preview input identity hardening (2026-10-26; code-only)

Displayed saved-scene previews now compare captured key-light direction,
radiance, and exposure against the current transient inputs in addition to the
settings generation. This prevents stale pixels from being treated as current
if a generation value is reused or wraps. Non-finite captured or current values
refuse the match. The source regression covers exact equality, each changed
input, and non-finite values; no build or test was run.

- **Files changed:** desktop preview helper and integration, editor-camera test
  source, this ledger, and the ignored local work queue.
- **Capability gained:** preview freshness is bound to the render values that
  produced the pixels, with a direct input comparison alongside the generation
  marker.
- **Governance preserved:** preview values remain transient and the project,
  journal, renderer authority, and serialized contracts are unchanged.
- **Risks:** the helper and desktop call site are uncompiled and unexecuted;
  exact floating-point equality assumes the preview stores the original inputs,
  which it does today.
- **Rollback:** remove the direct settings matcher/call and its regression.
- **Rejected:** relying on a wrapping generation token as the sole proof that
  the currently displayed capture uses current settings.

Direct impact is the Cartographer D3D12 saved-scene preview. RegOS,
IDA_Training, hidden_canopy_bot, and Ask_IDA are unchanged. Dataset, training,
deployment, and promotion progress remain unchanged.

### Saved-project vertex-color adapter wiring (2026-10-26; code-only)

The direct D3D12 renderer and production mesh compiler already carry authored
RGBA vertex/corner color, but the saved-project adapter rejected every
`color4` attribute before compiling. The adapter now selects the only
vertex- or corner-domain `color4` layer when exactly one exists and passes it
through the standard render-mesh compilation path. No color layer preserves the
existing implicit white default. Multiple color layers and unsupported domains
fail closed instead of silently choosing or ignoring authored data.

- **Files changed:** project-scene renderer adapter, D3D12 scene-renderer test
  source, this ledger, and the ignored local work queue.
- **Capability gained:** authored project colors can reach the existing PBR
  vertex stream and its masked-alpha/shadow path.
- **Boundary improved:** ambiguous layers and non-vertex/corner color domains
  are rejected; preparation remains read-only and source-bound.
- **Governance preserved:** no mutation, schema, renderer ABI, or authority
  change. Cartographer only; no RegOS, IDA_Training, hidden_canopy_bot, or
  Ask_IDA contracts changed.
- **Tests:** authored coverage for color transfer, unchanged uncolored default,
  ambiguity refusal, and wrong-domain refusal. No build, test, shader compile,
  GPU render, or native visual acceptance was run at the user's instruction.
- **Risks:** the adapter change and authored cases are uncompiled. Existing
  shader interpolation and color-space limitations remain.
- **Rollback:** restore the saved-project color4 refusal and remove the new
  adapter regression and ledger entry.
- **Rejected:** selecting the first sorted color layer, discarding extra color
  channels, or converting values without an authored color-space contract.

Dataset, training, deployment, and promotion progress remain unchanged; native
color and shadow acceptance remains open.

### Authored project camera capture wiring (2026-10-26; code-only)

The direct scene-render CLI now accepts an explicitly selected saved
perspective camera. Resolution uses the current scene-object world translation
and rotation, ignores object scale, and carries the saved field of view and clip
range into the existing bounded renderer. Orthographic cameras, missing camera
records, conflicting camera sources, and projection overrides fail closed.
Receipts identify whether the camera was explicit or authored and, for saved
cameras, retain object ID, authored identity, and revision. Source-level unit
and CLI integration cases were added; no build, test, GPU capture, or native
visual acceptance was run at the user's request.

- **Files changed:** D3D12 scene-renderer API and project adapter, scene-render
  CLI, D3D12 scene-renderer test source, CLI fixture, this ledger, and the
  ignored local work queue.
- **Capability gained:** deterministic command-line captures can use a camera
  authored in the saved project, with receipt provenance tied to that record.
- **Boundary improved:** camera source ambiguity, unsupported projection, and
  projection overrides are rejected; resolution reads the current project and
  does not mutate it.
- **Governance preserved:** no project schema, authoring state, domain event,
  authority, or renderer ABI changed. Cartographer only; no RegOS,
  IDA_Training, hidden_canopy_bot, or Ask_IDA contracts changed.
- **Tests:** source coverage added for world-pose/projection resolution,
  missing-camera and orthographic refusal, CLI receipt provenance, conflicting
  source refusal, and saved-projection override refusal. No build, test,
  shader compilation, GPU capture, or native review was run.
- **Risks:** authored source and CLI paths remain uncompiled; native orientation
  and visual framing have not been exercised.
- **Rollback:** remove the authored-camera resolver and CLI option, its source
  regressions/fixture, and this ledger entry.
- **Rejected:** silently approximating orthographic cameras, applying explicit
  camera fields over saved projection data, or labeling the saved camera as
  runtime-authored state.

Direct impact is Cartographer's D3D12 scene-capture CLI. RegOS, IDA_Training,
hidden_canopy_bot, and Ask_IDA are unchanged. Dataset, training, deployment,
and promotion progress remain unchanged; native camera/render acceptance is
open.

### Desktop authored-camera preview wiring (2026-10-26; code-only)

The desktop Scene preview now offers an explicit saved-camera selector. Its
default remains the current perspective viewport camera. Selecting a saved
perspective camera resolves the saved scene object's current world pose and
the authored field of view and clip range through the same D3D12 resolver as
the capture CLI. Orthographic entries are visible as unsupported and cannot be
selected. The capture freshness key includes the selected camera identity,
revision, projection values, and world transform; project-generation changes
clear the selection. Preview metadata exposes the authored object and revision
when one was selected. Authored-camera pixels appear in the Scene instrument,
keeping them separate from the interactive viewport and its editor-camera
overlays. This path renders the saved project only and adds no authoring
mutation.

- **Files changed:** desktop camera-binding helper and Scene preview wiring,
  desktop camera-binding regression source, this ledger, and the ignored local
  work queue.
- **Capability gained:** an operator can inspect saved-scene rendering from an
  explicitly authored perspective instead of only the transient editor view.
- **Boundary improved:** missing, stale, non-finite, orthographic, or
  out-of-render-bounds sources cannot reuse prior preview pixels; changing the
  selected camera invalidates the capture.
- **Governance preserved:** camera selection is transient and read-only; no
  project record, journal event, schema, AI route, or approval authority changes.
- **Tests:** source cases cover current authored identity/revision, repeated
  binding, stale-generation and missing-source refusal, orthographic refusal,
  direct-render clip-bound refusal, and the explicit viewport-camera path. No
  build, test, shader compile, GPU capture, or native review was run at the
  user's instruction.
- **Risks:** the desktop integration and tests are uncompiled. Native camera
  orientation/framing, Scene-instrument composition, and project reload
  behavior still need execution evidence.
- **Rollback:** remove `studio_scene_camera.hpp`, the desktop selector and
  binding fields, its source regression cases, and this continuation. The
  standalone capture CLI resolver remains available.
- **Rejected:** auto-selecting the camera marked active for review, silently
  substituting the editor camera for a selected stale camera, or approximating
  orthographic projection.

Direct impact is Cartographer's D3D12 desktop preview. RegOS, IDA_Training,
hidden_canopy_bot, and Ask_IDA contracts/data remain unchanged. Dataset,
training, deployment, and promotion progress remain unchanged; native saved-
camera acceptance remains open.

## Code-only authored-light adapter wiring (2026-10-26)

`prepare_project_scene_render` now resolves visible project-authored directional
lights from their current scene-object world orientation and linear color /
intensity. Co-directional sources are combined within a bounded set and replace
the transient caller key light. A saved source known to be dark or zero-radiance
suppresses that fallback. Projects with no visible authored lights preserve the caller's
explicit key-light behavior. Active point, spot, area, divergent directional,
switchable/intermittent, temperature-tagged, and shadow-disabled intent fails
closed because the existing GPU path cannot represent those semantics. The
capture algorithm identifier advances to V23 so receipts distinguish this
project-light lowering behavior.

- **Files changed:** project-scene renderer adapter, renderer algorithm
  identifier, D3D12 scene-renderer test source, CLI receipt-version assertion,
  this ledger, capability matrix, and the ignored local work queue.
- **Capability gained:** saved-scene previews consume an explicitly supported
  subset of authored project lighting instead of ignoring it.
- **Boundary improved:** unsupported or ambiguous visible lighting cannot be
  rendered with the unrelated transient key light; known-dark sources do not
  reactivate that fallback. Preparation remains read-only and the canonical
  project digest continues to bind all source records.
- **Governance preserved:** no project mutation, audit/journal event, schema,
  renderer ABI, authority, or cross-product contract changed.
- **Tests:** source cases cover authored pose and photometry, no-light fallback,
  active point refusal, switchable-state refusal, known-dark and zero-radiance
  handling, temperature/shadow-intent refusal, and divergent directional
  refusal; the CLI source expects receipt algorithm V23. No build, test, shader
  compile, GPU capture, or native review was run at the user's instruction.
- **Risks:** authored-direction convention and intensity interpretation have
  not been confirmed against a native visual capture; this adapter is
  uncompiled. The renderer still has a single directional-light/shadow path.
- **Rollback:** revert only the `resolve_project_authored_light` adapter,
  its source regressions, and this ledger/capability/work-queue note.
- **Rejected:** summing lights with different directions, approximating local
  lights as a directional key, simulating unresolved fixture state, or silently
  retaining transient lighting when authored active sources are unsupported.

**Cross-product trace:** Direct impact is Cartographer's saved-project D3D12
preview; indirect impact is limited to callers preparing a project render that
contains visible authored lights. No shared contract is touched. API/schema
risk is limited to this internal adapter's uncompiled behavior; public API and
project schema are unchanged. Dataset/eval risk is none because no dataset or
evaluation changed. Deployment risk is none because no package or deployment
configuration changed. Required follow-up tests are the D3D12 scene-renderer
source suite, GPU pixel comparison for orientation/radiance, and native preview
review that distinguishes supported directional from refused local lighting.

Promotion status—dataset: unchanged; training: unchanged; capability:
source-only directional-light wiring; promotion: not advanced, pending compile,
GPU and native visual evidence.

## Code-only authored point/spot PBR wiring (2026-10-26)

The saved-project adapter now resolves visible steady point and spot lights
from current world transforms, source color/intensity, range, and spot cones.
The D3D12 PBR path uploads at most 64 local lights as four RGBA32F texels per
row at `t8`; the compiled render graph declares the upload resource and PBR
read. The shader applies a quadratic range fade and smooth spot-cone cutoff.
These local lights are unshadowed by contract. Sources requesting local
shadows, color-temperature conversion, unresolved fixture state, or area
emitters fail closed. The existing shadowed directional path remains separate.
Receipts use algorithm V24, and request/cache identity binds the local source
records.

- **Files changed:** scene-render request ABI and algorithm identifier,
  project-light adapter, D3D12 request validation/digest/cache/GPU upload and
  render-graph wiring, graphics SRV table bound, PBR shader, native D3D12 and
  adapter regression sources, CLI receipt-version assertion, this ledger,
  capability matrix, and the ignored local work queue.
- **Capability gained:** current saved point/spot sources can contribute to
  the native PBR term; fixture position, range, radiance, and spot cone remain
  bound to the exact request.
- **Boundary improved:** local shadow intent and unsupported area/temperature/
  state semantics cannot be silently approximated; zero/dark sources do not
  reactivate transient key light. Local source count, enums, pose, range,
  radiance, and cones are bounded and validated.
- **Governance preserved:** project preparation/rendering remain read-only;
  source digest, request digest, algorithm V24, and render-graph transitions
  retain lineage. No project schema, mutation, journal, authority, or shared
  cross-product contract changed.
- **Tests:** source cases cover point/spot source mapping, world direction,
  cones, request digest changes, unsupported shadows/area/state/temperature,
  and GPU pixel response/cutoff. The device validation source now admits nine
  sampled textures; the CLI expects V24. No build, test, shader compile, GPU
  render, or native review was run at the user's instruction.
- **Risks:** HLSL compilation, root-signature cost, RGBA32F row upload and graph
  transitions are unverified. The quadratic range fade is an explicit lookdev
  model; intensity has no calibrated photometric units. Area lights and local
  shadows remain unsupported.
- **Rollback:** revert only the V24 request fields and GPU/shader/table
  changes, associated regression and CLI expectation changes, and this
  continuation's documentation; retain the earlier V23 directional path.
- **Rejected:** treating local sources as directional keys, synthesizing area
  emitters, rendering local sources without requested shadow behavior, or
  claiming calibrated photometry from the stored intensity field.

**Cross-product trace:** Direct impact is Cartographer's D3D12 saved-scene
preview; indirect impact is limited to callers preparing requests with local
project lights. No shared contract is touched. API/schema risk is confined to
the internal request and renderer ABI; persisted project schema and CLI
receipt shape are unchanged apart from the renderer algorithm value. Dataset/
evaluation risk is none; no training data or evaluator changed. Deployment
risk is none; no package or deployment configuration changed. Required
follow-up tests are the D3D12 scene suite, root-signature/resource-state debug
run, point/spot pixel and range/cone comparisons, cache parity, and native
preview review on representative authored fixtures.

Promotion status—dataset: unchanged; training: unchanged; capability:
source-only local-light wiring; promotion: not advanced, pending compile,
GPU and native visual evidence.

### Code-only desktop camera refit on resize (2026-10-26)

The D3D12 desktop previously changed the scene target extent after a resize but
kept the camera distance chosen for the initial aspect ratio. Portrait or
narrow windows could crop geometry that fit at startup. A platform-neutral
helper now computes a sphere-fit distance from the current extent and vertical
field of view. Initial framing and `WM_SIZE` share that helper; resize also
updates the derived far plane. The camera target and user orbit/zoom state stay
unchanged, while the precomputed scene bound avoids re-reading project data.

- **Files changed:** D3D12 scene-camera fit helper, standalone D3D12 desktop
  wiring, viewport-input regression source, this ledger, and the ignored local
  work queue.
- **Capability gained:** a saved-scene desktop preview refits its camera when
  the viewport aspect changes, keeping the bounded scene sphere visible.
- **Boundary improved:** invalid/zero extents, nonpositive or nonfinite bounds,
  invalid FOV, and invalid margin values produce no fitted distance; no stale
  initial-aspect distance is silently retained after a successful resize.
- **Governance preserved:** camera fit and extent changes are transient view
  state. No project, journal, schema, source identity, or authority path is
  mutated.
- **Tests:** source cases cover a portrait-versus-landscape distance change,
  margin behavior, and invalid radius, extents, and FOV. **Run:** none, per the
  user's instruction. **Not run:** all builds, tests, shader compiles, GPU
  execution, and native resize/capture review.
- **Risks:** implementation and test source are uncompiled. The fit uses a
  conservative sphere from the initial visible-mesh bounds; it does not
  re-evaluate geometry if the saved source changes while the app is open.
- **Rollback:** revert the helper, desktop calls/state, source regressions, and
  this ledger entry; the project format and serialized data are unaffected.
- **Rejected:** retaining the startup distance across aspect changes, or
  recomputing geometry bounds from project assets during every resize.

**Cross-product trace:** direct impact is Cartographer's standalone D3D12
saved-scene preview. No RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA code,
API, or data changed. Dataset/training/deployment state is unchanged;
capability is source-only; promotion remains open pending native resize and
render acceptance.

### Code-only local-light float-table boundary hardening (2026-10-26)

The local-light upload now normalizes spot emission direction in double
precision before writing its RGBA32F table. This keeps large but finite input
magnitudes from overflowing shader float normalization. Request validation
already rejects zero/nonfinite spot directions, unknown kinds,
unrepresentable positions/radiance, malformed cones, invalid ranges, duplicate
source IDs, and tables above 64; additional adversarial source cases cover
those boundaries.
The native spot pixel source also compares direction magnitudes that differ by
1e300 and expects identical pixels. No test, build, shader compile, or GPU run
was performed.

- **Files changed:** D3D12 local-light upload; scene renderer request-boundary
  and pixel regression sources; this ledger and the local work queue.
- **Capability gained:** spot directions are normalized before float upload,
  keeping magnitude-only changes from altering or invalidating shader math.
- **Boundary improved:** zero, nonfinite, duplicate, over-count, invalid-range,
  negative-radiance, reversed-cone, and point-with-cone records are refused at
  the request boundary.
- **Governance preserved:** no project mutation, schema change, fallback light,
  audit path, or authority change; exact request values remain identity-bound.
- **Tests:** new adversarial source cases cover request validation and an
  extreme-magnitude spot pixel comparison. No tests or builds were run at the
  user's instruction.
- **Risks:** GPU equivalence and HLSL behavior remain unverified until the
  source suite and native renderer run.
- **Rollback:** revert only CPU direction normalization and these regression/
  ledger additions; the V24 local-light path remains otherwise intact.
- **Rejected:** passing unbounded direction magnitude directly to float shader
  math or silently accepting a zero spot axis.

**Cross-product trace:** direct impact is Cartographer D3D12 scene rendering;
no RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA contract is touched.
Dataset/training/deployment state is unchanged. Promotion remains unadvanced.

### Code-only effective-light capture receipt wiring (2026-10-26)

The scene-render CLI receipt previously serialized the original `--light`
argument even when saved-project preparation replaced it with an authored
directional source or zeroed it for local sources. The receipt now records the
effective `SceneRenderRequest::light` and the exact point/spot source records
used by the request, including object ID, world position, linear radiance,
range, and spot direction/cones. It labels local lighting as unshadowed with a
quadratic range fade and explicitly says the linear scale is not calibrated
lumens. Render limitations carry that same statement into the desktop Scene
preview, which displays capture limitations. The CLI regression fixture
supplies saved point and spot sources while also passing a conflicting
transient light, then checks the request-side receipt values. These tests were
authored but not run.

- **Files changed:** scene-render CLI receipt generation; D3D12 render
  limitations; CLI fixture and receipt assertions; scene-renderer regression
  source; this ledger and the local work queue.
- **Capability gained:** a saved-scene capture receipt can now identify the
  effective point/spot settings and their saved source object IDs that produced
  its request digest.
- **Boundary improved:** a superseded transient key value is no longer
  presented as the effective rendered key; unsupported photometric certainty
  is not claimed.
- **Governance preserved:** capture remains read-only and source/request
  digests remain unchanged; no project schema or authority path changed.
- **Tests:** CLI source fixture asserts authored point/spot identities,
  point position/radiance/range, spot direction/cones, unshadowed model, and
  the zeroed effective key when a conflicting transient `--light` is supplied.
  Renderer source also asserts the local-light limitation is present in the
  capture result. **Run:** none, per the user's instruction.
- **Risks:** CLI receipt consumers that enforce an exact key set may need to
  accept the additive local-light fields. Rendering, JSON parsing, and the
  fixture command are uncompiled/unrun.
- **Rollback:** restore receipt output to the caller argument and remove the
  new metadata fields, fixture, assertions, and this ledger entry.
- **Rejected:** copying the CLI input into a receipt after project preparation,
  or claiming local intensity is physically calibrated.

**Cross-product trace:** direct impact is Cartographer's offline D3D12 capture
CLI. No RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA contract/data is
touched. Persisted schema and deployment are unchanged; dataset/training stay
unchanged and promotion remains unadvanced. Follow-up is the CLI integration
suite and native capture receipt review after the no-test instruction is
lifted.

### Code-only explicit temporal resolve wiring (2026-10-26)

The D3D12 scene renderer now compiles and records the existing temporal compute
shader as a render-graph pass between PBR and tone mapping. The request accepts
caller-owned RGBA32F history/output and optional RG32F motion textures, binds
their exact extents/formats/usages and declared states, and includes history,
prior-request, source, and motion provenance assertions plus feedback/reset
flags in request identity. First-frame and camera-cut requests copy current HDR
through the same pass. Nonfinite or off-frame reprojection and nonfinite
history pixels resolve to current HDR at those pixels. The resolved texture is
read for tone mapping and HDR capture; image/receipt output-state metadata tells
callers how to reuse their own output texture.

For valid in-frame reprojection, history RGB is clamped per channel to the
finite current-frame 3x3 HDR neighborhood before feedback blending. This limits
stale color at disocclusions while preserving the current sample. Out-of-frame
coordinates still resolve directly to current HDR; the shader does not clamp
coordinates to frame edges.

The caller must declare the actual supported initial state of each input
texture, provide exact history and motion content digests, bind history to the
same project revision/digest, bind motion to the same previous request, and
track the output's final resource state. The graph transitions declared input
states to shader-read for the resolve. These digests are attestations: the
renderer can validate texture metadata and request consistency, but it cannot
authenticate GPU contents. No
static-motion or stale-history fallback is inferred. The D3D12 desktop has no
motion-vector producer wired to this optional API, so interactive temporal
accumulation remains open.

- **Files changed:** D3D12 scene-render request/receipt API and graph
  execution,
  temporal HLSL, scene-render CLI receipt schema, scene-renderer and CLI source
  regressions, this ledger, the D3D12 capture ledger, and the ignored local
  work queue.
- **Capability gained:** external D3D12 callers can execute reprojection and
  feedback blending over explicit scene-linear prior-frame pixels, then retain
  the resolved GPU texture for their next frame.
- **Boundary improved:** missing history and camera cuts reset to current HDR;
  foreign-source history, motion tied to another previous request, missing
  provenance, aliased resources, invalid feedback, wrong formats/extents,
  nonfinite flow/history, and off-frame reprojection are refused or resolve to
  current HDR without sampling stale edge pixels. In-frame history is bounded
  by the local finite current-frame neighborhood to reduce disocclusion ghosts.
- **Governance preserved:** saved project state is read-only; render-graph
  resource transitions remain explicit; input/output handles remain
  caller-owned; no authorization, promotion, or mutation authority changes.
- **Tests:** authored request-boundary and native temporal integration cases
  cover first-frame/camera-cut reset, prior-frame blending, digest identity,
  output-state receipt, stale/foreign identities, aliases, wrong extents and
  formats, feedback bounds, and per-pixel neighborhood-clamped history.
  **Run:** none, per the user's instruction.
  **Not run:** builds, all test suites, HLSL compilation, D3D12 execution,
  desktop interaction, 8K timing/memory, and visual review.
- **Risks:** source is uncompiled. The neighborhood clamp adds up to eight
  texture reads per output pixel; its 8K cost is unmeasured. Native shader
  descriptor behavior and resource-state transitions remain unverified. GPU
  content digests are caller
  assertions, and the desktop supplies no motion vectors or history. A caller
  can still misstate a digest; no authenticated GPU readback is performed.
- **Rollback:** revert only V25 temporal request/graph/shader/receipt changes,
  associated source regressions, and this ledger entry; V24 lighting and prior
  render paths remain intact.
- **Rejected:** inferring zero motion, silently accepting stale history,
  clamping off-frame reprojection onto edge pixels, or claiming the viewport
  now performs temporal accumulation.

**Cross-product trace:** direct impact is Cartographer's optional D3D12 scene
capture API and receipt. The additive JSON output-state field and seven-entry
shader digest list are the shared Cartographer API/receipt contract touched;
CLI consumers enforcing an exact schema may need updates. RegOS,
IDA_Training, hidden_canopy_bot, and Ask_IDA have no direct or indirect code,
API, schema, dataset, evaluation, or deployment changes. Dataset progress:
unchanged. Training progress: unchanged. Capability progress: source-only and
unverified. Promotion progress: unchanged; native shader, producer, desktop,
and acceptance evidence remain required.

### Code-only finite shading-input guards (2026-10-26)

The scene PBR shader now uses guarded normalization for transformed vertex
normals/tangents and pixel-stage tangent, normal, view, light, half-vector, and
environment directions. A zero or non-finite sampled tangent-space normal
falls back to the geometric normal; a collapsed camera vector and degenerate
tangent/half-vector use finite, deterministic directions. These fallbacks
prevent undefined `normalize(0)` results from contaminating shaded pixels and
do not modify project or source data. Directional shadow projection also
returns unshadowed for non-finite clip coordinates, and malformed local-light
table rows are skipped before their values enter BRDF arithmetic.

- Files changed: `libs/d3d12/shaders/cartographer_scene_pbr.hlsl`, this ledger,
  and the ignored local work queue.
- Capability gained: degenerate per-pixel shading vectors resolve through
  explicit finite fallbacks instead of relying on undefined shader math.
- Boundary improved: zero-length, non-finite, or overflowed normalization
  inputs cannot directly propagate NaNs through the shading normal and BRDF
  vectors; invalid shadow projections and local-light rows cannot reach
  texture-coordinate conversion or lighting arithmetic.
- Governance preserved: source preparation and project state remain
  read-only; the fallback affects derived pixels only and adds no authority,
  schema, audit, or mutation path.
- Tests added/updated/run: none. Builds, HLSL compilation, shader reflection,
  GPU render, and native visual review were not run under the code-only
  instruction.
- Risks: shader syntax, compiler lowering, and pixels are unverified. The
  fallback keeps rendering finite if malformed vertex data reaches the shader;
  it does not repair or certify that source data.
- Rollback: restore the direct normalization expressions and remove this
  continuation; no ABI, shader resource layout, or serialized format changed.
- Rejected: allowing zero normal-map samples, coincident camera fragments,
  degenerate half-vectors, or malformed packed light values to produce
  undefined shader values; changing or rewriting the authored texture/mesh to
  hide such cases.
- Cross-product trace: Cartographer D3D12 renderer only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts/data are unchanged. API/schema,
  dataset/evaluation, and deployment risks are unchanged; promotion remains
  unadvanced.
- Capability delta: dataset progress unchanged; training progress unchanged;
  capability progress is source-only and unverified; promotion progress is
  unchanged.
- Required follow-up: compile the PBR shader and renderer, then capture zero
  and non-finite normal-map samples, a camera-coincident fragment, and a
  degenerate tangent fixture on D3D12.

### Code-only constant subpixel temporal reprojection (2026-10-26)

The temporal request now accepts an explicit uniform current-to-history offset
in pixel units. It may stand alone for caller-authored jitter or combine with a
per-pixel motion texture; the renderer never infers the offset from the camera,
scene, or texture contents. The offset and its presence are included in request
identity, and it must bind to the same prior request as the history. Camera-cut
resets, zero-motion assertions, missing history, nonfinite offsets, offsets
outside the frame bound, and mismatched prior-request identities refuse.

The temporal shader adds the constant offset to each supplied motion vector and
bilinearly samples the prior RGBA32F history at floating-point pixel
coordinates. Out-of-frame coordinates still resolve to the current HDR pixel;
the finite 3x3 neighborhood clamp remains in place. The renderer algorithm
identity advances to V26. This adds a precise input primitive for static-scene
jitter and uniform screen-space motion, but does not create or own the desktop's
history or camera/object motion vectors.

- **Files changed:** D3D12 temporal request API, request validation and digest,
  temporal HLSL, renderer algorithm identity, scene-render CLI algorithm
  expectation, temporal request/native integration regression source, this
  ledger, the D3D12 capture ledger, and the local work queue.
- **Capability gained:** callers can reuse history under explicit fractional
  pixel offsets instead of quantizing all motion to integer pixels.
- **Boundary improved:** constant offsets are finite, frame-bounded,
  prior-request-bound, and mutually exclusive with the zero-motion assertion;
  reprojection uses bilinear taps and still refuses off-frame reuse rather
  than extending stale edge colors.
- **Invariants preserved:** request and source provenance remain explicit;
  caller textures remain caller-owned; no motion is inferred; graph resource
  states and history/output alias checks remain authoritative; saved project
  data is read-only.
- **Tests authored:** request admission/refusal and digest sensitivity for
  constant offsets; native integration source compares a quarter-pixel result
  against a CPU bilinear reference and keeps the integer motion-vector case.
  **Run:** none, at the user's instruction. **Not run:** build, HLSL compile,
  renderer/CLI tests, D3D12 execution, desktop integration, 8K measurement, or
  visual review.
- **Cross-product trace:** direct impact is Cartographer's D3D12 scene-render
  request and algorithm identity. No RegOS, IDA_Training, hidden_canopy_bot,
  or Ask_IDA code/data contract changed. Dataset/eval progress and deployment
  state are unchanged; API risk is limited to callers that hard-code the
  V25 algorithm string or temporal request layout.
- **Risks:** source and HLSL are uncompiled; bilinear edge behavior, request
  compatibility, and shader performance are unverified. A caller can still
  misstate that a uniform offset matches its history; digests are assertions,
  not authenticated GPU content.
- **Rollback:** revert the `constant_motion_pixels` API/validation/digest,
  bilinear temporal shader path, V26 identity and test-source updates, and this
  continuation; retain the prior V25 integer-vector path.
- **Rejected:** deriving jitter from guessed camera state, silently treating
  missing vectors as zero, clamping out-of-frame history, or claiming the
  desktop now owns temporal history.
- **Required follow-up tests:** build the D3D12 renderer and CLI; run the
  temporal request and native resolve suites; compile HLSL; compare fractional
  X/Y offsets, integer texture motion, finite-neighborhood clamping, and
  off-frame fallback on D3D12; then connect a revision-bound desktop history
  owner and review resize/camera-cut behavior.
- **Promotion status:** dataset progress unchanged; training progress
  unchanged; capability progress is source-only and unverified; promotion
  progress unchanged.

## Authored texture channel mappings (2026-10-26; code-only)

The saved-scene D3D12 path now executes all valid authored `ChannelMapping`
selectors for base-color RGBA, metallic and roughness, tangent-space normal,
occlusion, and emissive RGB. Red/green/blue/alpha selection, one-minus
selection, and constant-zero/constant-one selectors are carried as bounded
per-draw vertex values and applied after texture sampling. Identity mappings
preserve the previous shader result. The generic GPU material compiler remains
strict; the D3D12 scene adapter normalizes its temporary compilation copy and
keeps the original material source digest and request identity. Masked
base-color materials that select a variable source channel other than alpha
for output alpha still refuse: current alpha-coverage mip generation only
preserves the source alpha channel. Constant-zero/constant-one output alpha is
admitted because its coverage does not depend on filtered source values.

The request digest includes the authored selectors, the mesh-cache identity
includes the selectors embedded in its vertex stream, and the reported scene
algorithm advances to V27. Source regressions now cover project-adapter
preservation, fail-closed masked-alpha coverage admission, and per-slot
native-pixel/cache parity for all five material textures, including inversion
and constants. They also cover fail-closed variable-channel masked alpha and
constant-alpha admission. These regressions were not run.

- **Files changed:** D3D12 scene-render header/adapter, PBR and shadow HLSL,
  scene-renderer tests, CLI algorithm expectation, capability matrix, this
  ledger, and the local work queue.
- **Capability gained:** supported material channel mappings now affect
  authored-scene shading instead of causing refusal or disappearing before
  texture sampling.
- **Boundary improved:** request/cache identity follows the exact selector
  values, and unsupported interpretations/conventions still fail closed.
- **Governance preserved:** project state and source textures remain
  read-only; no authored material value is rewritten, and the generic material
  compiler retains its strict contract.
- **Tests authored:** saved-project mapping preservation plus native output and
  cache parity for base color, metallic/roughness, normal, occlusion, and
  emissive slots. **Run:** none at the user's instruction.
- **Not run:** builds, unit/CLI suites, HLSL compilation/reflection, D3D12
  execution, and visual capture review.
- **Risks:** the new vertex signature and shader code are uncompiled. Added
  per-vertex selector data increases transformed vertex upload size by 48
  bytes; native input-layout support, shader behavior, and performance remain
  unverified. The receipt algorithm identifier changes from V26 to V27, so
  consumers that pin that value need an update. Masked materials using a
  variable non-alpha source channel remain unsupported until mip coverage
  preservation follows that mapping.
- **Rollback:** revert the V27 selector packing, shader application, cache and
  request identity additions, source regressions, algorithm identity, and this
  ledger entry; V26 history resolve and earlier texture behavior remain intact.
- **Rejected:** retaining blanket channel-map refusal, applying only base-color
  channels, or changing the generic material compiler's admission policy.
- **Cross-product trace:** Cartographer D3D12 renderer only. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA APIs/data are unchanged;
  dataset/training progress is unchanged, capability is source-only, deployment
  is unverified, and promotion remains open.
- **Required follow-up:** build and run D3D12/CLI scene suites, compile and
  reflect the PBR shader, exercise all five slots on D3D12, inspect rendered
  captures, and record GPU/memory impact. The renderer parent remains started.

## Saved orthographic camera rendering (2026-10-26; code-only)

Saved orthographic cameras now keep their projection type and authored vertical
extent through the project adapter, scene request digest, D3D12 projection
matrix, desktop Scene selector, and CLI receipt. The renderer builds a bounded
aspect-correct orthographic matrix with the existing D3D depth interval; it
does not approximate the view with perspective. Perspective remains the default
for transient editor and explicit CLI cameras. Orthographic extents above the
direct renderer's 1e8 bound fail closed. The project schema and authored camera
state are unchanged. This supersedes the earlier camera-capture entries'
orthographic-refusal limitation; those entries describe the prior V27 source.
The CLI also accepts an explicit `--projection orthographic` with a bounded
`--orthographic-height`; saved-camera projection overrides remain refused.

- **Files changed:** scene-renderer API/projection and request identity,
  project-camera adapter, desktop camera binding and selector, CLI receipt,
  D3D12 and desktop regression sources, CLI receipt regression, capability
  matrix, this ledger, and the ignored local queue.
- **Capability gained:** saved orthographic review cameras render through the
  direct D3D12 path and desktop Scene preview, and explicit CLI cameras can
  request an orthographic view with an authored world-space height.
- **Boundary improved:** projection mode and orthographic height participate
  in request/cache identity; invalid enum values, nonpositive extents, and
  out-of-bound saved extents refuse. Camera provenance remains explicit.
- **Governance preserved:** rendering reads the current project and scene-object
  transform without changing authored camera or project state. No project
  schema, event, mutation, authority, or cross-product contract changed.
- **Tests authored:** resolver and request identity coverage, invalid projection
  and extent refusal, native pixel coverage comparing depth-invariant
  orthographic size against perspective shrinkage, desktop camera-binding
  freshness identity, and CLI render/receipt provenance for saved and explicit
  orthographic cameras plus override/misconfiguration refusal. **Run:** none at
  the user's instruction.
- **Not run:** builds, unit/CLI suites, HLSL compilation, D3D12 execution,
  desktop display capture, GPU timing, and visual review.
- **Risks:** matrix layout and depth behavior, desktop selection, and receipt
  compatibility are source-only and unverified. The request digest and scene
  algorithm identity advance to V28; consumers pinning V27 need to accept the
  new algorithm. CLI receipts add `camera_projection` and
  `orthographic_height`; existing fields remain present.
- **Rollback:** remove the projection enum/extent and matrix branch, restore
  orthographic refusal in the project and desktop camera adapters, restore V27
  digest/receipt identity and selector behavior, remove the explicit CLI
  projection options, revert the authored source regressions, and remove this
  ledger entry. No saved project migration is required.
- **Rejected:** converting orthographic height to a guessed field of view,
  changing saved camera values, or broadening projection support beyond the
  two project-authored modes.
- **Cross-product trace:** direct impact is Cartographer D3D12, its Scene
  preview, and capture CLI. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA
  APIs/data are unchanged. Dataset/training progress is unchanged; capability
  progress is source-only; deployment is unverified; promotion remains open.
- **Required follow-up:** build the D3D12 renderer, CLI, and desktop; run the
  scene, CLI, and desktop camera-binding suites; inspect projection/depth
  readbacks and saved orthographic-camera captures at multiple aspect ratios;
  then record device and visual evidence. The production render-graph parent
  remains started.

## Desktop temporal accumulation wiring (2026-10-26; code-only)

The standalone `apps/d3d12_desktop` Scene preview now owns two caller-allocated
RGBA32F resolve textures and alternates them as explicit history/output. It jitters perspective
or orthographic projection by a bounded Halton sample, binds the exact current
to-history offset between adjacent samples, and computes each history content
digest from finite GPU readback pixels. The previous completed request digest
and exact source revision/digest accompany history. A stable request identity
with temporal inputs and jitter removed resets the accumulation after camera,
resize, source, or render-setting changes. Accumulation stops at 16 samples.
The two textures are capped at 256 MiB total; larger previews keep the existing
unjittered non-temporal path. The renderer algorithm identity advances to V29.

- **Files changed:** D3D12 scene request/projection/digest, desktop preview
  ownership/scheduling/readback, renderer regression sources, capability
  matrix, this ledger, and the ignored local work queue.
- **Capability gained:** a static authored scene in the standalone D3D12 desktop
  window can accumulate up to
  16 jittered scene-linear samples using caller-owned history with content and
  request provenance derived from completed GPU work.
- **Boundary improved:** camera/settings/resize changes reset history; offsets
  are bounded and bind previous-sample provenance; non-finite or wrong-extent
  readback cannot become history; large previews stay on the prior path under a
  fixed texture-memory bound.
- **Governance preserved:** the project remains read-only. No zero-motion claim
  is made: the desktop supplies only the known jitter delta and invalidates
  history whenever the rendered camera/request changes. It adds no mutation,
  authoring authority, persistence, or cross-product contract.
- **Tests authored:** bounded jitter validation, request-identity binding, and
  native pixel response to jitter. **Run:** none at the user's instruction.
- **Not run:** builds, D3D12/desktop tests, HLSL compilation, GPU execution,
  frame timing, display capture, and visual review.
- **Risks:** the desktop now performs a synchronous RGBA32F history readback
  after each asynchronous render, which can stall the preview and use transient
  host memory. Projection signs, fence/state interaction, allocation behavior,
  shader stability, and native appearance remain unverified. The 256 MiB cap
  covers the two GPU history textures, not transient readback staging.
- **Rollback:** remove the request jitter field/projection/digest changes,
  restore algorithm V28, remove desktop history ownership and repeated sample
  scheduling, remove the jitter regressions, and remove this ledger entry.
- **Rejected:** asserting zero motion, guessing history content digests,
  reprojecting camera motion without vectors, or allocating unbounded history.
- **Cross-product trace:** Cartographer D3D12 and its desktop preview only.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA APIs/data are unchanged;
  dataset/training progress is unchanged; capability is source-only; deployment
  is unverified; promotion remains open.
- **Required follow-up:** compile the D3D12 and desktop targets; run renderer
  validation/native tests; compile and reflect the PBR and temporal shaders;
  inspect camera reset, resize, fallback, and 16-sample captures; and measure
  readback latency and GPU/host memory. Parent acceptance remains open.

## Main desktop offline temporal sample accumulation (2026-10-26; code-only)

When enabled in the main desktop Scene instrument, the saved-scene D3D12
preview now renders 2–16 fixed-scene samples. The first sample starts from a
camera-cut reset; later samples use the shared bounded Halton projection
jitter, ping-pong caller-owned RGBA32F textures, the exact previous output
readback digest, and the prior request/source identity. Current-to-history
offsets are derived from adjacent known jitter values. GPU timing, when
available, sums the per-sample scene-graph intervals. The default remains one
unjittered render. The control labels this as a synchronous capture and does
not claim animated-object motion or general temporal reprojection.

- **Files changed:** shared D3D12 temporal-sampling helper, standalone D3D12
  desktop helper reuse, main desktop Scene preview, renderer source regression,
  capability matrix, this ledger, and the ignored local queue.
- **Capability gained:** the main desktop's offline saved-scene preview can
  accumulate a bounded set of jittered samples through the existing temporal
  resolve graph.
- **Boundary improved:** sample count is 2–16; two RGBA32F history textures
  share the 256 MiB cap; each history digest comes from the exact finite HDR
  readback; each step carries prior request and source provenance; and the
  source project remains read-only. Existing one-sample behavior is unchanged
  by default.
- **Governance preserved:** no project mutation, source persistence, authority
  expansion, or external product/API/dataset change is introduced. No moving
  geometry, camera animation, or object-motion vector source is claimed.
- **Tests authored:** shared sampling determinism/bounds, adjacent jitter-offset
  direction, exact two-texture memory limit, empty extent refusal, plus the
  existing renderer request-identity and native-pixel source coverage. **Run:**
  none at the user's instruction. No desktop-specific integration test was
  added.
- **Not run:** builds, D3D12 renderer/desktop suites, shader compilation,
  device execution, display capture, visual review, or GPU/host-memory timing.
- **Risks:** a selected 16-sample capture performs 16 synchronous scene renders
  and readbacks on the UI thread; this may stall the main desktop. Shader,
  resource-state, texture lifetime, request-chain, and native visual behavior
  remain source-only. The 256 MiB bound covers only the two temporal textures;
  renderer staging and the current sample's CPU readbacks are outside it. The
  prior CPU result is released before each subsequent sample.
- **Rollback:** remove the main desktop temporal checkbox/sample loop and its
  preview freshness/sample-count fields; restore the local jitter and memory
  helpers in the standalone D3D12 desktop; remove the shared sampling header,
  its source regressions, and this ledger update. No project migration is
  needed.
- **Rejected:** changing the default preview behavior, averaging display-encoded
  pixels on the CPU, retaining history across separate captures, or claiming
  object motion without a producer.
- **Cross-product trace:** Cartographer D3D12 and its main desktop preview only.
  RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA APIs/data are unchanged;
  dataset/training progress is unchanged; capability progress is source-only;
  deployment and promotion remain unverified.
- **Required follow-up:** compile the D3D12 renderer and both desktop targets;
  run focused shared-sampling and renderer tests; compile/reflect the temporal
  shader; exercise 2- and 16-sample requests, failure cleanup, and preview
  invalidation; inspect native output; and measure UI-thread latency and GPU/
  host memory. The D3D12 production parity parent remains started.

## Code-only bounded inverse-square local-light continuation (2026-10-27)

The scene PBR shader now attenuates authored point and spot radiance by inverse
square world-space distance, clamped at a 1 cm near-field floor, and uses a
quartic smooth range cutoff. Request validation caps each source radiance
channel at `1.0e20`; the renderer algorithm identity advances to V30 so cached
captures cannot reuse output from the previous quadratic-fade shader. The
desktop and capture receipt limitation now describes the unshadowed model,
near-field floor, quartic cutoff, and uncalibrated linear units.

- **Files changed:** D3D12 scene-render request/validation identity, PBR local
  light shader, native renderer regression source, capture CLI receipt and its
  source assertions, this ledger, and the ignored local work queue.
- **Capability gained:** saved point/spot sources use bounded inverse-square
  distance attenuation in the scene PBR path.
- **Boundary improved:** source radiance is capped; near-field distance is
  floored at 1 cm; the range cutoff reaches zero smoothly at the authored
  range. Source/request identity advances to V30.
- **Governance preserved:** capture remains read-only and request-bound; no
  project mutation, audit path, or authority change was introduced.
- **Tests authored:** source cases compare point-light contribution at 2 m and
  4 m, require finite and stronger near-field output under the floor, require
  no direct contribution beyond range, retain spot contribution/cutoff cases,
  reject radiance above the bound, and assert the CLI receipt reports the
  active attenuation model. **Run:** none, at the user's instruction.
- **Not run:** C++ builds, HLSL compile/reflect, D3D12 tests, GPU execution,
  native capture, or visual review.
- **Risks:** the code and HLSL are uncompiled; attenuation behavior, output
  bounds, adapter compatibility, and native pixel comparisons are unverified.
  Point/spot shadows and photometric calibration remain unsupported.
- **Rollback:** restore the quadratic range attenuation and its prior
  limitation text, remove the radiance cap and V30 identity bump, revert the
  associated source cases, and remove this ledger/queue continuation.
- **Rejected:** claiming lumens or calibrated physical output, adding local
  shadowing in this slice, or keeping the old lookdev-only quadratic model
  while describing it as inverse-square lighting.

**Cross-product trace:** direct impact is Cartographer's saved-scene D3D12
preview and capture CLI. Indirect impact is limited to receipt consumers and
desktop users who see the local-light limitation string. No shared contract
with RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA is touched. The internal
renderer request algorithm advances to V30; the existing CLI
`local_lighting_model` receipt adds attenuation and floor fields, and its
`range_fade` value changes, so consumers that enforce the former exact object
shape need review. Dataset/eval risk is none; no dataset or evaluator changed.
Deployment risk is none; no package or deployment configuration changed.
Capability progress is source-only and promotion remains open. Required
follow-up tests are D3D12 request-validation and native pixel comparisons for
near/far/range/spot cases, plus CLI receipt parsing; compile the D3D12 renderer
and compile/reflect the PBR shader before reviewing captures on representative
saved scenes.

## Code-only desktop environment-source label (2026-10-27)

The saved-scene preview tooltip now labels the environment digest as an
“Environment source” instead of “Diffuse environment.” The digest identifies
the source used for both diffuse SH and, when present, the GGX-prefiltered
specular mip chain; the limitation list continues to state which paths were
active.

- **Files changed:** `apps/desktop/main.cpp`, this ledger, and the ignored local
  queue.
- **Capability gained:** the preview tooltip names the digest without implying
  that only diffuse environment lighting is active.
- **Governance preserved:** this is presentation text only; render requests,
  receipts, source state, and project authority do not change.
- **Tests:** none added or run; desktop build and native tooltip review remain
  unverified at the user's instruction.
- **Risk:** the label is not verified in a built desktop binary.
- **Rollback:** restore the previous tooltip label and remove this continuation.
- **Rejected:** deriving environment capabilities from the source label; the
  renderer's explicit limitation list remains the evidence for active features.
- **Cross-product trace:** Cartographer desktop only. Other product contracts,
  datasets, evaluations, deployments, and promotion state are unchanged.
- **Required follow-up:** build the desktop target and inspect the tooltip with
  diffuse-only SH and with an equirectangular environment that also supplies
  specular mips.

## Code-only single-spot shadow wiring (2026-10-27)

The existing bounded shadow map can now be selected for one saved spotlight.
Its world position, emission direction, range, and outer cone produce a
perspective shadow projection; the existing scene shadow pass rasterizes that
projection, and the local-light shader samples the map only for the source
whose authored `casts_shadows` flag is set. The saved-scene adapter carries
that flag into request identity, the GPU local-light table, preview limitations,
and capture receipts. A capture refuses point-light shadows, multiple local
shadow casters, unsupported narrow/short spotlight projections, or simultaneous
key-directional and spotlight shadows because the renderer owns one map.

- **Files changed:** D3D12 scene-render request/adapter/cache/shader, scene
  capture CLI receipt, request/native pixel/CLI regression sources, this
  ledger, capability matrix, and the ignored local work queue.
- **Capability gained:** one authored spotlight can cast depth-tested shadows
  through the existing 3x3 PCF scene map, including saved-project captures.
- **Boundary improved:** point shadows, a second local caster, a simultaneous
  nonzero key light, ranges below 0.02 m, and outer cones below 0.01 rad refuse
  before submission; local-light and transformed-vertex cache identities bind
  the shadow source inputs.
- **Governance preserved:** rendering reads the saved project and changes only
  derived pixels/receipts. No project mutation, schema, authority, audit path,
  or human-approval boundary changed.
- **Tests authored:** request identity and validation for one spotlight,
  point/multiple/combined-key refusal, stable projection bounds, saved adapter
  transfer, an occluded receiver pixel, capture limitation reporting, and the
  CLI receipt's active shadow flag/policy. **Run:** none at the user's request.
- **Not run:** C++ build, HLSL compile/reflect, D3D12 tests, CLI capture, GPU
  execution, desktop preview, visual review, or performance measurement.
- **Risks:** projection conventions, depth precision, 3x3 PCF, HLSL bindings,
  mesh-cache invalidation, and device execution are source-only. The capture
  algorithm advances to V31; the local-light receipt's `shadowed` value now
  reflects actual request intent and the policy field names the single-spot
  bound. Consumers that assume all local lights are unshadowed need to review
  the updated receipt.
- **Rollback:** restore V30 and the old local-light shader/table meaning,
  disable authored spotlight shadow transfer, restore the previous cache
  identity domains and receipt policy, and remove this continuation's source
  cases and ledger/queue notes. Saved projects need no migration.
- **Rejected:** mapping a point light through a spot projection, silently
  choosing which of multiple authored shadow casters wins, or reusing the one
  map for key and local lights in the same capture.
- **Cross-product trace:** direct impact is Cartographer's D3D12 saved-scene
  preview and capture CLI. The API/schema surface is limited to internal
  algorithm identity and the existing CLI receipt fields. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA are unchanged; dataset/eval and deployment
  progress are unchanged. Capability progress is source-only; promotion remains
  open.
- **Required follow-up:** build renderer, CLI, and desktop; run request, adapter,
  pixel, and CLI receipt suites; compile/reflect both scene shaders; inspect
  point, unshadowed spot, and shadowed spot captures on representative saved
  projects; record device and performance evidence. The production rendering
  parent remains started.
