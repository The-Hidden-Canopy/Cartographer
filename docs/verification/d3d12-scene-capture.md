# D3D12 saved-scene capture

`cartographer_scene_render` is an offline capture tool built when the
`carto_d3d12_scene` target is available. It loads a saved `.carto` document,
prepares an explicit, revision-bound scene request, renders it on the selected
D3D12 adapter, and writes a BMP plus a JSON receipt. The caller must supply a
stable project identity, shader directory, camera, and extent. Referenced texture
bytes are verified and loaded from the saved project's content-addressed store;
optional mip artifacts must match that exact current derivation. Missing bound
textures fail closed.

The receipt binds the source revision and digest, request digest, rendered
pixel digest, BMP digest, shader digests, adapter identity, camera, light, draw
count, and optional environment digest and image color space. The environment
digest, all nine coefficients, and any image color-space interpretation also
participate in the request digest. The loader computes the digest from exact SH
file or panorama bytes; direct C++ request construction supplies its own
provenance digest. It also records `render_wall_time_ns`, measured around the
synchronous `render_scene` call, and `host_output_payload_bytes`, the logical
sizes of the returned HDR, depth, display-RGBA, and encoded BMP buffers. These
are host-side measurements. They do not claim GPU timestamps, peak host memory,
total GPU allocation, or a benchmark of sustained frames.

The renderer is bounded direct-light capture. V3 adds one directional shadow
map, V4 adds a precomputed order-2 Lambert diffuse SH input, and the code-only
V5 continuation derives those coefficients from an explicit-color-space 8-bit
equirectangular image. V6 adds request-bound FXAA after tone mapping, with an
explicit off mode. V7 adds source-bound roughness-selected reflections from
eligible panorama mip chains. V8 extends the same path to linear Radiance RGBE
inputs while preserving values above one. V9 replaces area-filtered environment
mips with a bounded 32-sample GGX prefilter. V10 binds explicit transient
material-graph inputs to saved-scene request identity. The original V10 status
statement was that V3 through V10 had not been built or accepted. Later source
continuations add GGX-prefiltered reflections and a split-sum BRDF lookup (V20),
then saved-scene inputs through V24. The current continuation work has not been
built, run, or accepted. When environment input is absent, the renderer retains
its reported fixed ambient approximation; without eligible decoded panorama
mips it evaluates diffuse environment lighting only. The reflection path uses
source radiance in mip zero (apart from the packed SH patch) and GGX-prefiltered
levels for increasing roughness. The code-only V25 continuation adds an
explicit caller-bound temporal HDR resolve; the D3D12 desktop still has no
motion-vector producer or interactive history owner, and native acceptance is
open. Transparent output remains unsupported.
Spatial FXAA is implemented but unverified and is identified in
request/image/receipt data. The
renderer requires authored UV0 and explicit material-region assignments;
unsupported graph operations and other unsupported inputs refuse instead of
receiving fallback data. A successful capture is not a live viewport, full
application render-graph parity, or production acceptance result.

No build, test, render, or device measurement was performed for the 2026-10-26
receipt-metric continuation or the publication change below. The CLI now writes
both artifacts into a unique sibling staging directory, publishes the receipt
first, then publishes the BMP as the completion marker. A BMP publication error
attempts to remove the already-published receipt, and temporary files are
cleaned up on ordinary success or failure. Two-file publication is not
crash-atomic: a process or machine failure between the two renames can leave a
receipt without a BMP. Consumers must still validate both artifacts and the
receipt's BMP digest. Cleanup removes only the two staging paths owned by this
capture, then removes the staging directory if empty.

### Publication repair ledger

- **Capability gained:** ordinary write or publication failures no longer leave
  a BMP that appears complete without its receipt; temporary output is grouped
  and cleaned up through one staging directory.
- **Boundary improved:** the BMP is the final publication marker. If its rename
  fails after receipt publication, the CLI attempts receipt rollback.
- **Governance preserved:** the receipt is derived from the exact rendered
  image, source/request digests, and current device report. No project source,
  journal, renderer authority, or human approval path changes.
- **Tests:** added CLI assertions that successful captures leave no staging
  entries and a pre-existing receipt is preserved while blocking output.
  Run: none, at the user's request. Not run: all builds, tests,
  shader compilation, GPU capture, and device acceptance.
- **Risks:** two filesystem renames cannot make a receipt/BMP pair crash-atomic;
  a crash between them can leave receipt-only state. Staging cleanup is
  best-effort if the filesystem itself refuses removal.
- **Rollback:** revert the CLI staging/publish helper, its staging assertion,
  and this ledger paragraph; no serialized project data or schema changed.
- **Rejected:** writing the BMP first (can leave an unreceipted image) and
  claiming two-file crash atomicity (not provided by the filesystem API).
- **Cross-product trace:** direct impact is Cartographer's offline D3D12 capture
  path. No RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA code/API/data
  changed. Dataset and training progress are unchanged; capability progress is
  code-only; promotion progress remains open pending builds and native
  acceptance.

## Desktop project-view wiring (2026-10-26, code-only)

`cartographer_d3d12_desktop` accepts an optional saved-project view mode:

```text
cartographer_d3d12_desktop.exe --project <saved-project.carto> --shaders <shader-directory>
```

Optional environment inputs are `--diffuse-irradiance-sh <file>` or
`--environment-equirectangular <image> --environment-color-space linear|srgb`.
Image decoding requires a bounded opaque 2:1 panorama.

Add `--debug-layer` to request D3D12 debug validation on systems with the
Windows Graphics Tools feature installed.

It loads the saved document read-only, prepares the existing bounded scene
request, frames visible mesh bounds for the current window aspect, and calls
`render_scene_to_texture` when the view changes or the window resizes. The
renderer tone-maps directly into a persistent caller-owned BGRA8 GPU texture;
the renderer checks the target's actual registered descriptor, and the desktop
copies that texture to each acquired DXGI back buffer. It does not read the
display image back to the CPU or upload it again. The existing
no-argument clear-only shell and its orbit, keyboard, wheel, and DPI input path
remain available. No project, journal, or authoring state is modified.

This wires direct GPU rendering into visible desktop presentation. The scene
renderer compiles its bounded scene-pass graph, and the desktop dirty-frame path
records its presentation handoff graph; these are not full application
render-graph parity. Project rendering still uses synchronous queue
submissions, recompiles its temporary resources on a view change, and refuses
unsupported graph operations, unavailable bound texture inputs, and other
unsupported inputs. Code-only scene-render V21/V22 wiring now executes
straight-alpha blended draws with submesh-centroid back-to-front sorting and
source-bound vertex colors, including vertex alpha in masked color/shadow
discard. Intersecting transparency remains approximate and fractional
transparent shadows are unsupported. A supported hardware/window run is still
needed to prove compilation, native
resource states, resize behavior, displayed pixels, and debug-layer
cleanliness. The desktop checks device receipts after scene rendering and
back-buffer copy; `--debug-layer` makes the native layer opt-in. No build, test,
or native window run was performed for this wiring continuation.

## Required follow-up

- Build `cartographer_scene_render` and run the D3D12 scene-renderer tests.
- Build `cartographer_desktop` with `CARTO_ENABLE_D3D12` both off and on; run
  the saved-source button against a supported project with and without a
  selected panorama, confirming the image, color-space interpretation,
  provenance tooltip, and debug receipts.
- Exercise dirty/unsaved state, on-disk revision mismatch, unsupported source
  textures, camera changes, viewport resize, and project replacement; verify
  refusal or cache retirement without project/journal mutation.
- Capture a reviewable authored project on a supported adapter and retain the
  BMP, matching receipt, and exact command/input assets.
- Compare the recorded host wall time and payload bytes with any separately
  measured GPU time and allocation data; do not substitute one for the other.
- Integrate the bounded scene and desktop frame graphs with the application's
  long-lived render scheduler/resources before claiming full render-graph
  parity; project-view rendering still uses synchronous submissions.
- Build the D3D12 desktop and run a supported native window through project
  load, orbit/zoom, resize, display capture, and debug-receipt review.
- Add and run native target-render coverage for stale handles, wrong format or
  extent, missing usage, invalid initial state, target ownership, and equality
  with the offline capture within the expected BGRA8 quantization bound.

## Subsequent repository verification (2026-10-26)

The earlier code-only statements above describe their original continuations.
The later current-repository check reproduced and repaired a display-handle
compile error, an over-bound desktop graph identity and scalar occlusion values
that failed to reach native pixels. Scene, CLI and existing D3D12 CTests passed
3/3 (26.22 s total); the newly rebuilt Home/reset/input suite separately passed
1/1. Added direct-target tests cover all pixels within a BGRA8 quantization step,
source identity, recorded pass names, caller ownership, wrong descriptors,
invalid resource states and stale/null handles. These local target-render
follow-ups have execution evidence now.

The earlier verified captures identify
`carto.d3d12.scene-direct-capture.v2` and bind that corrected host
interpretation in the request digest. A subsequent code-only continuation
advances the current scene renderer to V3, adds a shadow pass, declares mesh
vertex/index buffers in its scene graph, and adds an offline
`scene.capture-readback` source-state pass; that source has not been built or
run. A later V4 source continuation adds digest-bound optional diffuse
irradiance SH input to the CLI; it also remains unbuilt and unrun. The
V5 source continuation adds opaque 2:1 equirectangular PNG/JPEG/TGA conversion
to SH, explicit color-space lineage, and CLI/desktop wiring; it too remains
unbuilt and unrun. The
preceding evidence therefore covers V2 only. JSON records the
actually recorded standalone scene passes. This is bounded graph/target
evidence, not proof of all application, upload, shadow, environment or temporal
work.

## Cartographer Vulkan viewport preview (2026-10-26, code-only)

When `CARTO_ENABLE_D3D12` is enabled, the Cartographer desktop's Scene
instrument now offers a one-shot “Render saved scene into viewport” action.
It requires a saved, unchanged project, a matching on-disk project revision,
and the active perspective camera. The action loads current project texture
sources, prepares the existing read-only D3D12 scene request, and caps preview
resolution at 2048 pixels on the longest edge and 4,194,304 pixels total. It
uploads the returned display-encoded RGBA8 pixels to a separate Vulkan texture
and draws that image behind the existing authoring overlays. Hover details show
the source, request, and pixel digests, adapter name, and renderer limitations.
The cached image is cleared if the project generation/revision/path, dirty
state, camera, environment selection, or viewport extent no longer matches the
capture. The Scene instrument also offers an optional panorama selector and
requires the user to declare linear or sRGB interpretation. The selected
opaque 2:1 PNG/JPEG/TGA source flows through the bounded environment loader
into the D3D12 request's diffuse SH input; its digest and color-space
interpretation are reflected in the request and preview tooltip. Changing or
clearing the panorama invalidates the previous image. This adds diffuse
environment lighting to the preview; it does not add specular environment
reflections or HDR panorama decode.

This is a user-triggered saved-source still preview. It does not continuously
render the live editor state, mutate or save the project, provide full
application render-graph parity, or establish photorealistic or production
acceptance. Unsupported saved-scene inputs still fail closed through the
existing adapter. The synchronous render, panorama decode, and Vulkan upload
can block the UI. The exact frame, resize behavior, shader discovery in installed layouts,
device/debug receipts, and pixel display have not been exercised.

Vulkan reference-image and scene-preview textures now enter a retirement list
when controls replace or clear them. The list retains every image and ImGui
descriptor referenced by draw data for the current frame, including multiple
changes before submission. After queue submission/presentation, or when a
minimized/out-of-date swapchain skips submission, the renderer waits for device
idle and releases the retired descriptors and images. Shutdown drains the list
after its idle wait and before the ImGui Vulkan backend stops.
This avoids destroying a resource still referenced by a not-yet-submitted ImGui
draw list. The wait can stall the UI, and the lifetime path has not been built
or exercised on a Vulkan device.

Repeated explicit captures now reuse a lazily created debug-enabled D3D12
device owned by the desktop session. Device-creation diagnostics refuse the
capture; any later debug receipt or device-loss result discards the cached
device, and a new device is created only after a subsequent explicit user
request. Scene render resources remain per-capture temporaries, and this does
not integrate the scene passes into the application's persistent render graph.
No repeated-capture, recovery, or device-lifetime execution was performed.

### Viewport wiring ledger

- **Files changed:** `apps/desktop/CMakeLists.txt`, `apps/desktop/main.cpp`,
  `apps/desktop/studio_viewport_capture.hpp`, `tests/editor_camera_tests.cpp`,
  and this verification record. The local-only queue continuation is in
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- **Capability gained:** users can request a bounded D3D12 render of the
  current saved project and see its pixels in Cartographer's existing Vulkan
  viewport, with explicit source/request/pixel identity and optional diffuse
  environment lighting from a color-space-declared panorama. Repeated explicit
  captures reuse one healthy session-owned D3D12 device.
- **Boundary improved:** dirty or unsaved projects, non-perspective cameras,
  stale on-disk revisions, stale project/camera/environment/viewport bindings,
  unavailable source textures, unsupported scene inputs, and D3D12 debug
  receipts refuse or retire the preview. The feature is compile-time optional.
- **Governance preserved:** project loading, texture admission, scene
  preparation, and rendering are read-only. The preview owns a derived GPU
  texture only; it has no project/journal mutation or promotion path.
- **State transition path:** none; the operation is a derived preview and
  changes no project state.
- **Audit/domain event path:** none; no authored or domain mutation is
  attempted.
- **Permission/org-scope path:** not applicable to this standalone local
  preview; it introduces no remote or org-scoped action.
- **Human approval boundary:** the user explicitly clicks the preview button;
  no capture is automatically promoted or accepted.
- **Tests:** added a platform-neutral stale-binding regression for missing or
  dirty projects, wrong camera/source, stale project/environment generations
  and revision, changed camera/extent, zero extent, and non-finite extent.
  Updated: `tests/editor_camera_tests.cpp`. Run: none, at the user's request.
  No dedicated regression exercises Vulkan retirement or the session-owned
  D3D12 device lifecycle; those need native execution evidence.
  Not run:
  all builds, tests, shader compilation, device execution,
  native-window interaction, and screenshot review.
- **Risks:** rendering and panorama decoding are synchronous and may stall the
  UI. Releasing retired Vulkan textures waits for device idle and can also
  stall on replacement. Keeping a D3D12 device alive retains adapter resources
  for the desktop session. The displayed pixels can become stale if an external
  process rewrites a project while preserving its revision or replaces the
  selected panorama at the same path; the displayed digests identify the exact
  loaded sources.
  Vulkan sampling/color presentation and shader path discovery remain unproved
  until exercised on the target installation.
- **Rollback:** remove the conditional desktop link/definition, the
  `Rgba8Texture` scene-capture slot, retirement list, and upload/display path, the pure binding
  helper and regression, the session-owned D3D12 device field, and these ledger
  sections. No project schema or saved project data changed.
- **Rejected:** describing the still image as a live viewport, promoting it as
  full render-graph parity, or claiming visual acceptance without an actual
  native capture and review.
- **Capability Delta:** dataset progress is unchanged; training progress is
  unchanged; capability progress is source-only; promotion progress is
  unchanged and remains open pending native evidence.
- **Cross-product trace:** direct impact is Cartographer only. No RegOS,
  IDA_Training, hidden_canopy_bot, or Ask_IDA code/API/data changed. Dataset
  and training progress are unchanged; this is code-only capability progress;
  promotion remains open pending native build, render, display, resize, and
  visual acceptance evidence.

The project-view executable was built and launched, but native captures showed
unrelated occluding content or failed with `FrameArrived timed out`; live scene,
gesture, resize and DPI acceptance remains open. A subsequent frame-loop source
edit was newer than that executable and is being rebuilt separately. Exact
scope, repairs, risks and rollback are recorded in
`repo-rendering-boundaries.md`.

## Shared D3D12 graph dispatch (2026-10-26, code-only)

The authored-scene renderer and D3D12 desktop frame recorder now use one
`RenderGraphDispatch` implementation for compiled pass order, resource-state
mapping, transition emission, and pass-name receipts. Callers still own the
commands that implement each pass. The dispatcher preflights every resource
usage before appending that pass's transitions, refuses unbound resources and
wrong-kind stage bindings (including non-buffer vertex inputs and buffers in
color/depth/present stages), and requires all compiled passes to be recorded in
order before submission. Initial states must be known and compatible with the
bound handle kind. Backend dispatch also refuses stage/access pairs
that D3D12 cannot represent with the available states, including writable
vertex inputs, read-write copy/depth/present usages, read-only color outputs,
and invalid access enum values. The authored-scene path supplies its mesh
vertex and index bindings and initial states; the desktop frame path supplies
its scene texture and acquired back buffer bindings.

This removes two copies of D3D12 graph sequencing and barrier mapping. It does
not add persistent transient-resource allocation, asynchronous scheduling,
multi-frame resource ownership, or full application render-graph parity. The
scene renderer still creates temporary resources and submits synchronously;
the viewport preview still crosses D3D12 and Vulkan through a synchronous
readback/upload boundary.

### Shared-dispatch ledger

- **Files changed:** `libs/d3d12/include/carto/d3d12/detail/render_graph_dispatch.hpp`,
  `libs/d3d12/src/render_graph_dispatch.cpp`, `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/CMakeLists.txt`, `apps/d3d12_desktop/main.cpp`,
  `tests/d3d12_render_graph_dispatch_tests.cpp`, `tests/CMakeLists.txt`, and
  this record. The local-only queue continuation is in
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- **Capability gained:** both D3D12 graph clients now record transitions and
  pass-order evidence through the same implementation.
- **Boundary improved:** wrong pass order, omitted passes, unbound resources,
  invalid handles/access modes/initial states, unsupported stage/access pairs,
  and resource-kind mismatches fail before that pass appends any transitions.
- **Governance preserved:** this remains derived rendering. It does not mutate
  project, journal, domain, or promotion state; the explicit preview action
  remains the human boundary.
- **Tests:** authored source-level cases for compiled order, transition states,
  vertex/index distinctions, pass receipts, wrong order, unbound resources,
  atomic refusal of an unsupported vertex binding, stage/access mismatch, and
  invalid access/stage enums, unknown initial states, and states incompatible
  with their handle kinds. Run: none, at the user's request. Not run: all
  builds, tests, shader compilation, D3D12 device runs, desktop interaction,
  and visual review.
- **Risks:** semantic mapping is still a fixed D3D12 mapping rather than a
  general backend scheduler. The helper does not retain resources across frames;
  the code and tests have not been compiled or executed.
- **Rollback:** remove the shared-dispatch header/source and test target, restore
  the two callers' local transition/order dispatch, and remove this ledger
  section. No project schema or saved data changed.
- **Rejected:** claiming full render-graph parity or asynchronous scheduling
  from shared barrier code alone.
- **Capability Delta:** dataset progress is unchanged; training progress is
  unchanged; capability progress is code-only and unverified; promotion
  progress remains open.
- **Cross-product trace:** direct impact is Cartographer only. No RegOS,
  IDA_Training, hidden_canopy_bot, or Ask_IDA code/API/data changed. Dataset and
  training progress are unchanged; promotion remains open pending compilation,
  tests, supported-device execution, and native display review.

Next code-side work remains the long-lived render scheduler/resource lifetime
integration. Native builds and dispatch tests are still required before
acceptance.

## Persistent scene program, mesh, and material cache wiring (2026-10-26, code-only)

`SceneRenderCache` now reuses compiled shaders, pipelines, sampler, immutable
mesh vertex/index uploads, verified source textures, neutral factor textures,
and diffuse SH input across saved-scene renders through the offscreen capture
and BGRA8 direct-target APIs. The integrated scene/copy/present path uses the
same BGRA8 cache, and the D3D12 desktop session owns it beside its device.
Program identity binds the device, exact shader-source digests, and output
format. Mesh identity binds source revision/digest, validated draw-package and
mesh digests, object transforms, and light direction. Material-texture identity
binds current source and mip digests plus the exact SH coefficients and
interpretation. Camera and extent changes retain these immutable inputs;
source, transform, light, or material-input changes rebuild the affected entry.
The desktop object order destroys the cache before its swapchain and device.
Cached resources are marked in use through submission and become reusable
after the synchronous wait. Scalar AO textures, shadow/HDR/display targets, and
other per-render resources remain temporary.

- **Files changed:** `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/scene_renderer.cpp`, `apps/d3d12_desktop/main.cpp`,
  `tests/d3d12_scene_renderer_tests.cpp`, and the local queue.
- **Capability gained:** repeated desktop renders and captures can retain the
  shader program, mesh upload buffers, and verified material textures across
  requests, including camera/extent updates, instead of recreating them for
  every render.
- **Boundary improved:** changing devices, shader bytes, output format, scene
  source, mesh/package identity, transforms, light direction, source texture
  bytes/mips, or SH coefficients invalidates the affected cached resources;
  cache ownership is tied to the device lifetime.
- **Governance preserved:** this only renders caller-supplied saved-scene data.
  It does not mutate a project, journal, domain state, or promotion state, and
  the render remains behind the explicit desktop action.
- **Tests:** authored cache reuse assertions for offscreen pixels, camera
  changes, mesh-transform/light invalidation, texture-byte and SH invalidation,
  repeated BGRA8 direct targets, integrated handoff, and output-format switching.
  Run: none, at the user's request. Not run: builds, tests, shader compilation,
  D3D12 execution, desktop interaction, or visual review.
- **Risks:** caller must destroy or reset the cache before its device. The
  render path still validates scene meshes and reads/hashes shader sources on
  each request, and waits synchronously; no scheduler, render-target pool,
  multi-frame resource allocator, or measured speedup is provided. One cache
  retains the current scene's mesh buffers and source mips until reset or key
  replacement; the validated combined mesh/mip upload budget is up to 512 MiB,
  plus small neutral/SH textures. Scalar AO factors remain transient. The
  changes have not been compiled or exercised.
- **Rollback:** remove `SceneRenderCache` and its cached overloads, restore the
  D3D12 desktop call to `render_scene_to_texture_with_frame_handoff`, and
  remove the cache assertions and this ledger section. No project schema or
  saved data changed.
- **Rejected:** process-global caching, reuse across devices or shader/output
  changes, asynchronous submission, and claiming full render-graph parity from
  program reuse alone.
- **Capability Delta:** dataset progress is unchanged; training progress is
  unchanged; capability progress is code-only and unverified; promotion
  progress remains open.
- **Cross-product trace:** direct impact is Cartographer. No RegOS,
  IDA_Training, hidden_canopy_bot, or Ask_IDA code/API/data changed. Dataset and
  training progress are unchanged; promotion remains open pending compilation,
  native execution, lifetime recovery, and visual acceptance.

The D3D12 parity parent remains started. Device lifetime, debug-layer
cleanliness, shader/source/mip/SH invalidation, geometry/transform/light
invalidation, format switching, desktop resize and camera interaction, and any
performance effect need execution evidence.

## Bounded D3D12 scene-target reuse (2026-10-26, code-only)

`SceneRenderCache` now owns a single dimension-keyed set of internal HDR,
depth, shadow-color, shadow-depth, and (when capturing offscreen) display
targets. Repeated requests at the same extent and shadow-map size reuse those
attachments. The cache estimates their logical texel storage and retains the
set only when that estimate is at most 128 MiB; larger requests use the
existing per-render resource path. A resolution change replaces the one cached
set. Scene color/depth states are carried between graph dispatches and are
updated only after the synchronous submission wait succeeds. A submission or
wait failure leaves the set marked uncertain; after queue-idle recovery it is
discarded and recreated before reuse.

This adds attachment lifetime reuse to the existing program/mesh/material
cache. It remains single-threaded and synchronous, and does not add an
asynchronous scheduler, multi-frame allocator, or full application graph
parity. The 128 MiB value is a logical texel-byte estimate, not a driver-reported
physical allocation bound.

- **Files changed:** `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `tests/d3d12_scene_renderer_tests.cpp`, and the local queue.
- **Capability gained:** repeated same-size saved-scene renders can reuse
  internal HDR, depth, shadow, and display attachments while preserving the
  prior capture output.
- **Boundary improved:** extent changes rebuild the single retained target
  set; requests exceeding the logical memory cap stay temporary; failed
  submissions invalidate uncertain state before a later reuse.
- **Governance preserved:** rendering remains read-only over saved project
  inputs, uses the existing explicit preview/capture action, and adds no
  project, journal, domain, or promotion mutation.
- **Tests:** added source assertions comparing display, linear HDR, and depth
  pixels across repeated cached renders, camera changes, and extent changes
  against fresh captures. Run: none, at the user's request. Not run: builds,
  tests, shader compilation, D3D12 execution, desktop interaction, or visual
  review.
- **Risks:** the source has not been compiled or exercised. The logical budget
  cannot account for driver allocation alignment; reset/destruction still
  requires the device to be alive. This improves repeated synchronous renders
  but does not demonstrate frame-time or memory-performance gains.
- **Rollback:** remove `SceneRenderTargets`, target estimate/pool selection,
  graph initial/final state wiring, and the corresponding parity assertions;
  restore per-render attachment creation and remove this ledger section. No
  project schema or saved data changed.
- **Rejected:** reusing targets after an uncertain submission without a queue
  wait, adding unbounded size-keyed targets, or claiming an asynchronous render
  scheduler from synchronous target reuse.
- **Capability Delta:** dataset progress is unchanged; training progress is
  unchanged; capability progress is code-only and unverified; promotion
  progress remains open.
- **Cross-product trace:** direct impact is Cartographer only. No RegOS,
  IDA_Training, hidden_canopy_bot, or Ask_IDA code/API/data changed. Dataset and
  training progress remain unchanged; promotion remains open pending native
  builds, D3D12 lifetime execution, resize/recovery, and visual acceptance.

## D3D12 scalar AO constant path (2026-10-26, code-only)

Unbound scalar ambient-occlusion values now travel in the existing material
root-constant word. The high bit marks a bound occlusion map; the shader clears
that bit to recover its authored strength and samples the map. An unbound value
uses the same neutral AO texture as other missing material maps and applies the
constant directly. This removes a staging buffer and committed 1×1 texture for
each distinct scalar factor while preserving bound-map strength semantics,
including zero strength. The PBR root-constant count remains 56 DWORDs, within
the current D3D12 root-signature budget.

- **Files changed:** `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/shaders/cartographer_scene_pbr.hlsl`,
  `tests/d3d12_scene_renderer_tests.cpp`, and the local queue.
- **Capability gained:** scenes with many distinct scalar AO factors avoid
  allocating a per-factor upload buffer and texture.
- **Boundary improved:** sign-bit encoding distinguishes scalar AO from a
  bound map without inventing a source texture or growing the 64-DWORD-limited
  root signature. Negative zero is canonicalized before tagging.
- **Governance preserved:** authored factors and bound texture sources remain
  unchanged; unavailable bound sources still fail closed; only derived draw
  constants change.
- **Tests:** existing scalar-factor pixel cases cover 0, 0.25, and 1.0; added
  source assertions cover a fully occluding map and a bound map with zero
  strength. Run: none, at the user's request. Not run: shader compilation,
  builds, tests, GPU execution, or visual review.
- **Risks:** shader/C++ encoding agreement is source-reviewed only; the DXC
  layout and device pixel behavior have not been exercised. The bit tag relies
  on the existing validated nonnegative AO range.
- **Rollback:** restore per-factor AO texture staging and neutral-map selection,
  revert the PBR shader's sign-bit decoding, and remove the added assertions and
  ledger section. No project schema or saved data changed.
- **Rejected:** adding another root-constant DWORD (the existing PBR signature
  already consumes the 64-DWORD limit with seven SRV tables and a sampler) and
  retaining a texture for every distinct scalar value.
- **Capability Delta:** dataset progress is unchanged; training progress is
  unchanged; capability progress is code-only and unverified; promotion
  progress remains open.
- **Cross-product trace:** direct impact is Cartographer's D3D12 scene renderer.
  No RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA code/API/data changed.
  Dataset and training progress remain unchanged; promotion remains open
  pending shader compilation, tests, native execution, and visual acceptance.

## Bounded asynchronous D3D12 frame scheduling (2026-10-26, code-only)

Added a single-threaded `D3D12FrameScheduler` that bounds tracked graphics
submissions to the swapchain buffer count and waits for the oldest fence only
when a slot is needed. Cached scene handoff now has an asynchronous submission
entry point that returns its fence serial; the D3D12 desktop tracks both scene
handoff and frame-composition submissions and drains them at shutdown. Existing
synchronous capture APIs continue to wait. After successful submission,
temporary and invalidated cache handles use the device's fence-keyed deferred
destruction path; uncertain submission failures retain the queue-idle cleanup.
All reused scene resources remain ordered on the graphics queue.

Fixed-storage continuation (2026-10-26, code-only): the scheduler now keeps its
at-most-eight submission serials in an inline ring instead of a dynamically
growing deque. Tracking a successful GPU submission therefore performs no
allocation after queue submission; completion retirement and idle drain reset
the bounded slots. This narrows the post-submit failure surface under host
memory pressure without changing the scheduler's single-threaded contract.

- **Files changed:** `libs/d3d12/include/carto/d3d12/frame_scheduler.hpp`,
  `libs/d3d12/src/frame_scheduler.cpp`, `libs/d3d12/CMakeLists.txt`,
  `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/scene_renderer.cpp`, `apps/d3d12_desktop/main.cpp`,
  `tests/d3d12_tests.cpp`, this file, and the local queue.
- **Capability gained:** desktop scene and composition work can keep up to the
  two swapchain frames in flight instead of waiting after each submission.
- **Boundary improved:** the frame queue has a fixed capacity, serials must be
  tracked monotonically, device loss refuses new reservations, and resize or
  shutdown still drains GPU work before native resource teardown.
- **Governance preserved:** rendering remains read-only; the scheduler grants
  no project or authoring mutation authority.
- **Tests added:** native regression source covers invalid frame limits,
  reservation/cancel behavior, refusal to drain an untracked reservation,
  monotonic serial tracking, bounded successive submissions, and idle drain.
  **Run:** none. Builds, tests, shader compilation, GPU execution, and native
  review were not run, per the code-only instruction.
- **Risks:** C++ API/layout changes, the fixed ring indexing, and asynchronous
  lifetime transitions are uncompiled and unexercised. The scheduler regression
  source is also uncompiled and unexercised. The async scene API
  requires callers to track its serial and order subsequent use on the same
  graphics queue. No performance gain is claimed; `Present` and swapchain
  resize may still block.
- **Rollback:** restore the synchronous scene handoff and per-frame wait,
  remove `D3D12FrameScheduler` and its build entry, and revert the async receipt
  serial path. Keep the existing synchronous capture API and deferred device
  destruction.
- **Rejected:** unbounded submissions, concurrent cache use, cross-queue
  resource reuse without synchronization, and removing the synchronous capture
  contract.
- **Capability Delta:** dataset progress is unchanged; training progress is
  unchanged; capability progress is code-only and unverified; promotion
  remains open.
- **Cross-product trace:** direct impact is Cartographer's D3D12 desktop and
  renderer. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA code/API/data
  are unchanged; dataset and training progress remain unchanged.

## V7 source-bound panorama reflection wiring (2026-10-26, code-only)

Eligible 2:1 LDR panorama loads now retain a color-correct mip chain when the
decoded image is at most 4,194,304 pixels. The renderer packs the nine signed
diffuse-SH coefficients into mip zero of the existing RGBA32F environment
texture and uploads the panorama mips into that same texture. Its PBR shader
uses reflected view directions and material roughness to select from mip one
onward; clearcoat uses its own roughness. This adds a source-image reflection
signal without extending the seven-SRV PBR table or changing the root-constant
ABI. Request and material-cache identities bind the mip artifact digest. The
receipt limitations state that this is an equirectangular area-box mip
approximation, without GGX convolution, a split-sum BRDF LUT, or HDR specular
input. Radiance files and panorama images above the mip-payload cap continue to
provide their existing diffuse-SH path and are reported as diffuse-only.

- **Files changed:** `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/environment.cpp`, `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/shaders/cartographer_scene_pbr.hlsl`,
  `tests/d3d12_scene_renderer_tests.cpp`,
  `docs/verification/authored-scene-rendering.md`,
  `docs/verification/acceptance-matrix.md`, this ledger, and the local queue.
- **Capability gained:** eligible current panorama pixels can contribute
  roughness-dependent reflection color to metallic and clearcoat materials.
- **Boundary improved:** panorama source digest, explicit color-space
  interpretation, opaque alpha, derived mip artifact, dimensions, and payload
  limits are validated; mismatched-source mip chains refuse. Larger/HDR inputs
  retain diffuse-only behavior with that limitation disclosed.
- **Governance preserved:** no saved project, journal, authoring state, or
  publication authority changed. The renderer remains read-only and consumes
  caller-supplied source evidence.
- **Tests added:** source assertions for LDR mip derivation, stale-mip refusal,
  and red/blue environment changes reaching metallic pixels with SH zeroed.
  **Run:** none, per the user's instruction. Builds, shader compilation, GPU
  execution, visual review, and performance acceptance were not run.
- **Risks:** the RGBA32F staging path can use up to 128 MiB; panorama mips are
  filtered in lat-long image space and are not physically convolved. HLSL
  sampling, upload footprints, actual pixel contribution, color-space parity,
  and memory behavior remain unverified.
- **Rollback:** revert the V7 environment mip field/identity, environment
  upload, reflection shader terms, source/GPU assertions, and this ledger
  section; retain the earlier V3–V6 renderer work and unrelated workspace edits.
- **Rejected:** adding an eighth sampled resource to the 64-DWORD-limited PBR
  root signature, silently interpreting HDR as LDR, treating diffuse SH as
  specular reflection, or claiming GGX-quality prefiltering from box mips.
- **Direct impact:** Cartographer's D3D12 environment renderer and capture
  receipt. **Indirect impact:** a richer source-bound capture may support later
  scene review, but no consumer now treats it as accepted evidence.
- **Shared contract touched:** additive `SceneRenderEnvironment` mip input,
  V7 request digest, existing t6 RGBA32F environment slot, render-graph texture
  transitions, and existing receipt limitation strings. **API/schema risk:**
  the C++ request struct layout and request identity changed; no saved-project
  schema or consumer wire schema changed, and dependent native builds remain
  unverified.
- **Dataset/eval risk:** no dataset or eval examples were changed; evaluation
  coverage is source-only until the new regressions run. **Deployment risk:**
  HLSL runtime compilation and installed shader lookup have not been verified.
- **Required follow-up tests:** build the D3D12 library, CLI, and desktop;
  run scene-renderer, capture-CLI, and D3D12 CTests; compile the PBR shader and
  inspect GPU debug receipts; exercise small/maximum accepted panorama sizes,
  linear/sRGB sources, diffuse-only and specular paths, cache invalidation,
  capture readback, and the native desktop preview; review captures and frame
  time on the target adapter. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA
  remain unchanged; dataset and training progress are unchanged; capability
  progress is code-only and unverified; promotion remains open.

## V8 linear Radiance reflection mip wiring (2026-10-26, code-only)

The bounded Radiance RGBE decoder now retains canonical linear RGB mip levels
for eligible 2:1 sources of at most 4,194,304 pixels. Each mip is area-filtered
by spherical pixel overlap, including the full tails of odd-size extents, and
the exact float payload is bound to the source digest by a canonical artifact
digest. The existing RGBA32F environment upload accepts these values directly,
preserving radiance above one. The same t6 shader sampling, material roughness
selection, render-graph transition, and versioned cache invalidation path is
used. Valid inputs above the reflection pixel or radiance bound omit HDR mips
and use diffuse SH when its coefficients remain within their bound. Explicit
Radiance primaries and globally over-budget images are refused; HDR input must
remain explicitly linear.

- **Files changed:** `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/environment.cpp`, `libs/d3d12/src/scene_renderer.cpp`,
  `apps/desktop/main.cpp`, `tests/d3d12_scene_renderer_tests.cpp`,
  `tests/run_scene_render_cli_tests.cmake`,
  `docs/verification/authored-scene-rendering.md`,
  `docs/verification/acceptance-matrix.md`, this ledger, and the local queue.
- **Capability gained:** eligible Radiance environments can contribute
  roughness-selected HDR reflections to metallic and clearcoat materials while
  retaining scene-linear values above one.
- **Boundary improved:** source digest, linear color-space interpretation,
  canonical mip digest, ordered mip extents, finite/nonnegative radiance, and
  pixel/payload bounds are validated. Stale or modified float mip content
  refuses until its artifact digest is refreshed.
- **Governance preserved:** no project, journal, authoring state, saved schema,
  publication authority, or cross-product API changed.
- **Tests added:** CPU source assertions cover decoded RGBE values, source and
  artifact identity, incorrect sRGB interpretation, and an odd-sized panorama
  whose bright final column must survive downsampling; native regression source
  checks above-one reflections and cached source invalidation. The CLI receipt
  regression now expects the V8 algorithm identity. **Run:** none, per the
  user's instruction. Builds, shader compilation, GPU execution, and visual
  review were not run.
- **Risks:** Explicit Radiance primaries remain rejected; the 1e12 reflection
  bound and lat-long area filtering are approximations, not GGX convolution.
  Decoder orientation, float upload footprints, color preservation, cache
  replacement, runtime shader sampling, and frame cost remain unverified.
- **Rollback:** revert the V8 HDR mip type/digest, Radiance mip generation,
  upload branch, regression source, and this section; retain V7 LDR reflections
  and all earlier renderer work.
- **Rejected:** quantizing HDR into RGBA8, silently interpreting Radiance as
  sRGB, using SH coefficients as specular data, or claiming physically
  convolved reflections from area-filtered mips.
- **Direct impact:** Cartographer's D3D12 environment loader, renderer, and
  receipt identity. **Indirect impact:** richer derived captures may inform
  later consumer review, but no consumer acceptance was changed. Shared contract
  is the additive C++ `SceneRenderEnvironment` mip input and V8 request/cache
  identity; no project or consumer schema changed. Dataset/eval progress is
  unchanged, deployment remains unverified, and capability is code-only.
  Required follow-up: D3D12 library/CLI/desktop builds; scene, CLI and backend
  CTests; RGBE orientation/exposure/color-correction cases; maximum-size and
  out-of-bound radiance cases; shader compilation; cache replacement; GPU debug
  receipts; native capture/color review and frame-time measurement. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA remain unchanged; promotion is
  still open.

### Legacy Radiance RGBE boundary regressions (2026-10-26; code-only)

Added scene-renderer regression source for flat legacy RGBE images below the
modern scanline width, a 127-pixel legacy repeat crossing scanline boundaries,
and malformed repeat-without-literal, overrun, truncation, and trailing-data
payloads. The crossing-run fixture compares all generated HDR mip levels and
the diffuse SH coefficient against the same image encoded with modern
per-channel RLE. No production decoder change was needed in this continuation.

- **Files changed:** `tests/d3d12_scene_renderer_tests.cpp`, this ledger, and
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- **Capability gained:** the existing legacy RGBE path has source coverage for
  small flat inputs, image-wide repeat semantics, and malformed boundaries.
- **Governance preserved:** decoding remains bounded, read-only, linear, and
  bound to exact input bytes; no project, journal, schema, or authority changes.
- **Tests added:** legacy flat/scanline-crossing and malformed-stream cases.
  **Run:** none, per the code-only request. No build, test, shader compile,
  GPU execution, or native review was performed.
- **Risks:** the fixtures and decoder have not been compiled or run; real
  producer compatibility and parser behavior remain unverified.
- **Rollback:** remove the fixture helper and cases and this ledger entry.
- **Rejected:** changing the decoder without a reproduced failure, accepting
  malformed/trailing payloads, or treating authored regressions as acceptance.
- **Cross-product trace:** Cartographer D3D12 only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts and data are unchanged. Dataset and
  training progress are unchanged; capability is code-only and unverified;
  promotion remains open.

## V9 shared GGX-prefiltered environment mip wiring (2026-10-26, code-only)

Eligible LDR and linear Radiance sources now derive one canonical float RGB
chain. Mip zero retains decoded source radiance; each later mip uses 32
Hammersley/GGX importance samples at its roughness level and N·L-weighted
reflection radiance. The shared chain binds the source-byte digest, the declared
input color-space interpretation, fixed sample count, mip extents, and exact
linear float pixels into request and material-cache identity. The environment
upload remains RGBA32F in the existing t6 slot; its 3x3 mip-zero SH payload is
avoided by a shader fallback to mip one when a bilinear footprint overlaps it.
The algorithm identity is V9. The prefilter has no split-sum BRDF LUT.

- **Files changed:** `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/environment.cpp`, `libs/d3d12/src/scene_renderer.cpp`,
  `libs/d3d12/shaders/cartographer_scene_pbr.hlsl`, `apps/desktop/main.cpp`,
  `tests/d3d12_scene_renderer_tests.cpp`,
  `tests/run_scene_render_cli_tests.cmake`, this ledger, the acceptance matrix,
  and the ignored local work queue.
- **Capability gained:** eligible LDR and HDR panoramas now provide
  source-bound, roughness-dependent GGX reflection levels instead of using
  area-box mips for specular response.
- **Boundary improved:** LDR and HDR share linear float validation, exact
  source/color-space binding, complete canonical extents, and a 64 MiB mip
  payload bound. The algorithm version and artifact digest distinguish these
  derived levels from V7/V8 area-filtered results.
- **Governance preserved:** this remains derived offline rendering; it adds no
  project mutation, saved schema, authoring authority, or promotion decision.
- **Tests added/updated:** source regressions cover constant HDR preservation
  at each roughness, source/artifact identity, HDR/legacy parity, odd-source
  hotspot input, LDR interpretation, and the V9 CLI receipt identity. **Run:**
  none, per the user's instruction. No build, test, shader compile, GPU render,
  native preview, or visual review was performed.
- **Risks:** the prefilter is CPU-generated and its worst-case time and memory
  cost are unmeasured. The 32-sample approximation, HLSL sampling/fallback,
  upload footprints, shader compilation, reflections, color response, and
  desktop label remain unverified; no split-sum BRDF LUT is present.
- **Rollback:** restore the V8 HDR-only mip API and area-filter builders,
  restore the V7 LDR mip path and V8 algorithm identity, revert the shader and
  desktop label, remove the migrated regression changes, and delete this V9
  entry. Keep prior independent work intact.
- **Rejected:** retaining area filtering while labeling it GGX, quantizing HDR
  to RGBA8, adding an extra SRV/root parameter, or representing source mips as
  accepted visual quality.
- **Impact and follow-up:** direct impact is Cartographer's D3D12 loader,
  offline renderer, CLI receipt identity, and desktop preview wording. Indirect
  impact is limited to future review of derived captures. The shared contract
  is the additive C++ environment input and V9 request/cache identity; no
  project or consumer API/schema changed. Dataset/eval progress is unchanged,
  deployment remains unverified, and promotion remains open. Required follow-up
  is D3D12 library/CLI/desktop builds; scene, CLI, and D3D12 CTests; shader
  compilation; bounded maximum-size timing/memory measurements; cache
  replacement; GPU receipts; and native capture/color review.

## V10 transient material-graph input wiring (2026-10-26, code-only)

The saved-scene adapter accepts explicit typed graph inputs bound to the exact
current graph revision and canonical digest. It evaluates the graph with those
values, lowers supported PBR outputs into material factors, and folds the graph
and evaluation-input digests into material identity. The desktop only supplies
the input set after its CPU evaluation receipt validates against the saved
graph; changing graph inputs clears the previous capture. The scene-render CLI
accepts repeatable typed parameter and surface input options, pins them to the
loaded graph revision/digest, and records those digests in its JSON receipt.
Use `--graph-parameter graph-id name scalar value` or provide four components
after `color`; `normal` takes three. `--graph-surface-input` uses the same type
and component format with a surface name such as `authored_mask`.
Values remain transient and do not modify the project, material catalog, or
journal. Missing or mistyped values, output-unreachable input values,
stale/foreign graph sources, unused graph bindings, texture samples,
graph-plus-variant combinations, and unsupported outputs fail closed. The live
Vulkan material viewport remains unevaluated.

- **Files changed:** `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/project_scene_renderer.cpp`, `apps/desktop/main.cpp`,
  `apps/cli/scene_render.cpp`, scene-renderer and CLI regression sources, this
  ledger, the material-graph verification ledger, the acceptance matrix, and
  the ignored local queue.
- **Capability gained:** explicit parameter/surface values can reach supported
  graph-bound materials through desktop preview or the scene-render CLI.
- **Boundary improved:** input values require exact graph-source revision and
  digest, contribute to render identity, and cannot be silently ignored when
  no visible bound material consumes them.
- **Governance preserved:** graph evaluation and preview are read-only;
  transient inputs are not persisted, no journal/domain event is emitted, and
  no new authoring, AI, or promotion authority is introduced.
- **Tests added/updated:** source cases cover supplied values reaching PBR
  factors, changed values changing request identity, foreign/stale graph
  bindings, CLI receipt digests, missing values, and output-unreachable values.
  **Run:** none, per the user's instruction. No build, test, shader compile,
  GPU capture, or native review was performed.
- **Risks:** desktop gating, graph receipt validation, input digest identity,
  saved-scene material lowering, renderer output, and error presentation remain
  unverified. Inputs are transient and the saved-scene renderer supports only
  its documented CPU-evaluable graph subset.
- **Rollback:** remove the optional graph-input argument and adapter evaluation
  binding, revert desktop receipt forwarding and feedback wording, restore the
  prior algorithm identity, remove the regression source and this V10 ledger
  entry, while keeping earlier environment work intact.
- **Rejected:** persisting these values into project/material state, inventing
  defaults for missing values, ignoring stale or unused bindings, widening the
  live Vulkan renderer, or claiming unrun source cases as acceptance.
- **Cross-product trace:** direct impact is Cartographer's saved-scene D3D12
  adapter, desktop offline preview, and CLI capture. Indirect impact is limited
  to capture consumers. The shared contract is an additive C++ request input and V10
  request/material identity; there is no project/API schema change. Dataset
  and training progress are unchanged; deployment and promotion remain open.
  Required follow-up is D3D12 library/CLI/desktop compilation, graph and scene
  renderer tests, CLI receipt tests, shader compilation, and a native saved-scene
  capture review.

### Aggregate graph-input work bound (2026-10-26, code-only)

The saved-scene adapter caps the combined number of caller-supplied parameter
and surface values across graph bindings at 262,144, matching its aggregate
graph-node evaluation budget. It counts the whole input map before resolving
graph identities, so unknown graph keys cannot force unbounded prevalidation
work. This guard is source-only; no build, test, render, or device review was
performed.

- **Files changed:** `libs/d3d12/src/project_scene_renderer.cpp`,
  `tests/d3d12_scene_renderer_tests.cpp`, this ledger, the acceptance matrix,
  and the ignored local work queue.
- **Capability gained:** transient graph input validation has an explicit
  aggregate work ceiling tied to the renderer's existing graph evaluation cap.
- **Boundary improved:** more than 262,144 values fail with
  `validation_failed` before graph lookup or per-binding validation; the count
  check subtracts from the remaining budget to avoid size overflow.
- **Governance preserved:** graph evaluation remains read-only and exact-input;
  no source, project, journal, permission, or promotion state changes.
- **Tests added:** source regression constructs 262,145 values across bounded
  per-graph maps and expects an early validation refusal. **Run:** none, per the
  user's instruction. Builds, all tests, shaders, GPU captures, and visual
  review remain unrun.
- **Risks:** the new guard and its large boundary fixture are not compiled or
  executed; future graph input APIs must preserve this aggregate limit.
- **Rollback:** remove the aggregate prepass and its fixture, then remove this
  ledger entry; prior exact-source and per-input validation remain intact.
- **Rejected:** relying on the per-graph map limit alone, which still permits
  millions of values across 1,024 bindings, or claiming source assertions as
  passing runtime evidence.
- **Cross-product trace:** direct impact is Cartographer's saved-scene D3D12
  adapter. No RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA contract/data
  changed; dataset/training progress is unchanged, deployment is unverified,
  and promotion remains open. Follow-up requires D3D12 scene/CLI tests, builds,
  shader compilation, and saved-scene capture review.

### V11 durable material-graph inputs (2026-10-26; code-only)

Material catalog schema v4 stores typed parameter and surface values on a
material's exact graph binding. The human-only application action validates
project revision, material revision, graph identity/revision/digest, and input
key/type compatibility before the existing journaled catalog commit. The
saved-scene adapter uses those durable values by default; explicit
revision/digest-pinned request inputs override them for that capture and keep
their input digest in material/request identity. Catalog v1-v3 readers
migrate with empty graph-input maps. Regression source covers v4 round trip,
v3 migration, invalid-subset refusal, project-diff visibility, human-only UI
admission, saved-input rendering, and per-request override identity. No tests,
builds, shader compilation, captures, or native review were run at the user's
request. Old binary readers and GPU behavior remain unverified; no viewport or
texture-sampling acceptance is claimed.

### DPI-normalized viewport orbit (2026-10-26; code-only)

Pointer deltas in the native D3D12 shell are converted from physical client
pixels to 96-DPI logical pixels before changing orbit angles. A monitor DPI
transition cancels an active orbit drag after applying the suggested window
bounds, avoiding a camera jump when client-coordinate scale changes mid-gesture.

- **Files changed:** `libs/d3d12/include/carto/d3d12/viewport_input.hpp`,
  `apps/d3d12_desktop/main.cpp`, `tests/d3d12_viewport_input_tests.cpp`, this
  ledger, the capability matrix, and the local work queue.
- **Capability gained:** orbit sensitivity remains consistent across supported
  monitor DPIs, and a DPI migration cannot apply a discontinuous drag delta.
- **Boundary improved:** authored input coverage compares equal logical motion
  at 96 and 144 DPI; the native shell ends an active captured drag when DPI
  changes.
- **Governance preserved:** camera motion remains transient presentation state;
  saved project content and application authority do not change.
- **Tests added:** platform-neutral equal-logical-motion regression. **Run:**
  none at the user's instruction; no build, test, or native monitor migration
  was performed.
- **Risks:** the changed input class and Win32 message path are uncompiled and
  unrun; touch/pen input and native multi-monitor behavior remain unreviewed.
- **Rollback:** restore raw pointer-pixel deltas and remove the DPI-transition
  drag cancellation, then remove the regression and this ledger entry.
- **Rejected:** increasing sensitivity with monitor DPI or retaining a drag
  across a change in client-coordinate scale without rebasing its origin.
- **Cross-product trace:** Cartographer's D3D12 desktop only. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA contracts/data are unchanged;
  dataset/training progress is unchanged, deployment is unverified, and
  promotion remains open. Follow-up: compile and run input tests, then exercise
  orbit, capture loss, resize, and monitor-DPI migration on the native shell.

### D3D12 scheduler unknown-serial refusal (2026-10-26; code-only)

The device now exposes the high-water mark of serials returned by successful
submissions. `D3D12FrameScheduler::track_submission` refuses a future serial
that the device has not returned, while retaining its reservation so the
caller can cancel or retry with the actual result. The scheduler regression
source exercises this refusal before tracking the real serial.

- **Files changed:** `libs/d3d12/include/carto/d3d12/device.hpp`,
  `libs/d3d12/src/device.cpp`,
  `libs/d3d12/include/carto/d3d12/frame_scheduler.hpp`,
  `libs/d3d12/src/frame_scheduler.cpp`, `tests/d3d12_tests.cpp`, this ledger,
  and the ignored local work queue.
- **Capability gained:** the frame scheduler rejects future/unissued serials
  before they can occupy a bounded in-flight slot or fail at a later wait.
- **Boundary improved:** zero, unreserved, nonmonotonic, and above-high-water
  serials are refused; a rejected serial does not consume the reservation.
- **Governance preserved:** this affects GPU submission bookkeeping only; no
  project mutation, audit authority, saved schema, or cross-product contract is
  changed.
- **Tests added:** scheduler source asserts the initial serial is zero, tracks
  the device's returned serial, refuses the next unissued value, and then
  successfully tracks the actual serial under the same reservation. **Run:**
  none, per the no-test instruction.
- **Risks:** source and fixture are uncompiled and unexecuted. The monotonic
  high-water check cannot prove serial provenance when callers pass a value
  from another device whose numeric value falls within this device's range;
  callers must track the serial returned by the same device.
- **Rollback:** remove the getter/high-water field and scheduler guard, restore
  the prior test fixture, and remove this section and its queue note.
- **Rejected:** waiting on an invalid serial to discover the error after the
  scheduler is already full, or claiming this high-water check authenticates
  cross-device provenance. Temporal resolve wiring also remains open: the
  authored-scene request has no producer-bound motion field or persistent
  history/reprojection contract, so asserting zero motion or inventing history
  validity would misstate the current inputs.
- **Cross-product trace:** direct impact is Cartographer D3D12. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA are unchanged; dataset/training
  progress is unchanged, capability is source-only, deployment is unverified,
  and promotion remains open. Follow-up: build and run `cartographer_d3d12_tests`,
  then exercise async submit/wait and teardown on a supported device.

### Code-only 8K request bounds and readback lifetime (2026-10-26)

The scene request previously rejected any extent above 16,777,216 pixels,
blocking the queue's 8K renderer target before device execution. The request
contract now permits at most 16384 pixels per axis and 8192×4320 total pixels,
which includes 7680×4320 UHD and 8192×4320 DCI extents. Synchronous capture
converts and releases the large display readback before requesting HDR and
depth readbacks, reducing simultaneous host copies. Direct-to-texture rendering
continues to avoid host image readback. These bounds do not guarantee that a
particular adapter has enough memory or can sustain an 8K frame.

- **Files changed:** scene-render request bounds and public constants, readback
  lifetime order, scene-renderer boundary source, this ledger, and the ignored
  local work queue.
- **Capability gained:** valid scene requests can describe standard 8K UHD and
  DCI-sized frames; display conversion releases its temporary pixels before
  HDR/depth readback begins.
- **Boundary improved:** per-axis and total-pixel caps reject larger requests;
  a source regression checks the 8K edge, an over-limit extent, and the axis cap.
- **Governance preserved:** no scene, project, receipt schema, or promotion
  authority changes; the request digest continues to bind the exact extent.
- **Tests:** source covers request validation and digest behavior. **Run:** none,
  per the user's instruction. **Not run:** builds, all tests, GPU allocation,
  native 8K rendering, performance measurement, and visual review.
- **Risks:** request admission does not guarantee resource availability. Full
  synchronous 8K capture still returns large HDR, depth, and display buffers;
  device/host memory and timing are unmeasured.
- **Rollback:** restore the 16M-pixel request limit and prior readback order,
  remove the bound assertions and this ledger entry; no saved data changes.
- **Rejected:** treating successful request validation as proof of 8K device
  support or performance.

**Cross-product trace:** Cartographer D3D12 only. RegOS, IDA_Training,
hidden_canopy_bot, and Ask_IDA are unchanged; dataset/training/deployment state
is unchanged. Capability is source-only; 8K and promotion evidence remain open.

## Caller-bound temporal resolve wiring (2026-10-26; code-only)

Algorithm V25 adds the existing temporal compute shader to the scene graph
between PBR and tone mapping. Callers may provide explicit prior scene-linear
history, current-to-history motion vectors, and a caller-owned resolved HDR
output. History is tied to the current project source revision/digest; motion
is tied to the same previous request identity. Callers declare each supported
input's initial state; graph transitions and descriptor usage are validated
against D3D12 texture descriptors. The output state is returned in
image/receipt metadata. First frames and camera cuts run the pass with history
disabled and copy current HDR.
Motion that is nonfinite or maps outside the frame, and nonfinite history
pixels, resolve to current HDR rather than clamped stale edge data. Valid
in-frame history RGB is additionally clamped to the finite 3x3 current HDR
neighborhood before feedback, limiting disocclusion ghosts without extending
edge colors across the frame boundary.

No content authentication is implied: history and motion digests are caller
attestations. The D3D12 desktop still has no motion-vector producer or history
owner connected to this optional contract. Temporal capture and the shader
integration remain unverified until a build, renderer suite, HLSL compile,
native D3D12 run, and desktop producer path are exercised.

- **Governance:** project data remains read-only; resource state transitions
  remain graph-dispatched; caller textures remain caller-owned. No authority,
  promotion, or cross-product contract changed.
- **Tests authored:** request provenance/reset/refusal cases, D3D12 reset and
  history-blend source cases, resource extent/format checks, and CLI receipt
  schema expectation. **Run:** none by user instruction.
- **Risks:** source and shader remain uncompiled; the neighborhood clamp adds
  up to eight texture reads per pixel and its 8K cost is unmeasured. Exact
  receipt schemas now include one temporal output-state field and seven shader
  digests. Native GPU
  content/state correctness, memory, performance, and display quality remain
  open.
- **Rollback:** revert the V25 temporal API/graph/shader/receipt/test changes
  and this section; keep V24 local-light behavior and prior renderer work.
- **Rejected:** treating hashes as authenticated texture reads, inventing
  motion/history, or claiming interactive D3D12 parity.

**Cross-product and promotion trace:** Cartographer only. RegOS,
IDA_Training, hidden_canopy_bot, and Ask_IDA are unchanged. Dataset/training
progress is unchanged; capability progress is source-only; promotion remains
open. Required follow-up: D3D12 and CLI tests, shader compilation, native
motion/history producer wiring, resize/8K device checks, and visual review.

## Constant subpixel temporal motion (2026-10-26; code-only)

V26 extends caller-bound temporal resolve with an optional uniform
current-to-history pixel offset. It is digest-bound to the request, tied to the
same previous request as history, and may combine with the per-pixel motion
texture. The shader now samples history bilinearly at floating-point
coordinates; out-of-frame reprojected coordinates still resolve to current
HDR. The caller must supply the offset explicitly. No desktop motion producer
or history owner was added.

Regression source covers finite/frame bounds, prior-frame identity, zero-motion
and camera-cut conflicts, request digest sensitivity, and quarter-pixel
integration against a CPU reference. No tests, build, HLSL compile, or native
render ran under the user's instruction. Risks include shader syntax and
device behavior, unmeasured bilinear cost at 8K, and caller misstatement of
uniform motion. Rollback is limited to the V26 constant-motion API, shader,
algorithm identity, and related source regressions; V25 per-pixel motion remains
the fallback. Cartographer's D3D12 API is the only contract touched; the four
other products and datasets are unchanged. Native desktop history ownership,
camera cuts, device execution, and visual acceptance remain open.
