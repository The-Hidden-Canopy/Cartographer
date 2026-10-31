# Current-repository rendering boundary check

The check began on local `main` at `1f84ed6`, with 106 modified tracked files
and 55 untracked files. These counts were a point-in-time observation, not a
clean-tree claim. The existing passing native binaries predated the newer
project-view, material-graph and texture-loader sources. Unrelated shared
worktree changes were preserved; no remote fetch, push, commit or cross-repo
mutation was performed.

## Findings, minimal repairs and proof

| Finding | File/function | Product consequence and repair | Proof |
| --- | --- | --- | --- |
| New target path did not compile | `scene_renderer.cpp`, `render_scene_impl` | A plain `TextureHandle` was still treated as a `Result`. Use the handle directly for display readback; preserve the new caller-owned GPU path. | Current-source build reproduced `no member named 'value'`; the corrected scene, CLI, native desktop and existing D3D12 targets all built. |
| Desktop graph source identities exceeded the 128-character artifact bound | `project_scene_renderer.cpp`, `prepare_project_scene_render` | A valid desktop identity plus exact graph digest could never reach rendering. Hash the entire over-bound context with a domain separator and retain a material/graph identity prefix. Short valid identities stay unchanged; no source component is truncated. | Regression first failed with the actual desktop identity shape. Repaired tests prove admission, bounded identity, deterministic requests and distinct full project contexts; existing stale-graph refusal remains. |
| Accepted graph occlusion did not affect ambient pixels without an image map | `scene_renderer.cpp`, material descriptor preparation | A neutral white texture erased the admitted scalar. Derive an exact float32 constant texture from the source value, reuse it across equal factors and set shader map strength to one. Preserve authored image-map strength behavior and immutable material products. | The native-only regression first failed after the direct-target tests passed. Repaired GPU tests check linear HDR pixels for graph values 0, 0.25 and 1, plus the unchanged zero-strength bound-map behavior. |

These repairs preserve source fidelity, lineage and explicit unsupported-input
refusal. They do not introduce RegOS domain transitions into a renderer.

At this checkpoint the corrected host interpretation was identified by
`carto.d3d12.scene-direct-capture.v2`. The later code-only shadow continuation
below advances the current identifier to V3; the execution evidence above
predates that continuation and does not verify it.

## Execution evidence

- The full scene executable passed its seven source/admission/native groups.
- CTest passed 3/3 in 26.22 seconds: scene renderer 8.01 s, CLI 5.41 s and
  existing D3D12 regressions 12.14 s. Source verification, save/reopen,
  immutable project bytes and clean native debug/device state remained covered.
- The freshly rebuilt platform-neutral viewport-input suite also passed 1/1
  (0.10 s test, 0.18 s total), including the newer Home/reset behavior.
- Added target tests render into caller-owned BGRA8, sample that texture through
  a GPU identity probe and compare every channel with offline capture within
  one quantization step. Changed camera views change target pixels. Null/stale
  handles, wrong format/extent, missing usage, forbidden states and a false
  fresh-target initialization claim are refused without submission. Successful
  and failed calls preserve caller ownership. The bounded capture/target pass
  lists are checked against actual readback, not used as full-graph evidence.
- Native desktop source compiled and linked. Its read-only project-view CLI
  launched the exact rebuilt executable and exposed the expected window at
  DPI 96. The first capture contained unrelated occluding content; activation
  and refreshed window selection did not yield a valid capture. Retries ended
  with `FrameArrived timed out: timed out waiting on channel`. No displayed-
  scene, orbit, zoom, resize, DPI-change or human-visible quality acceptance is
  claimed. The test-owned viewer was retired before rebuilding a frame-loop
  edit that arrived after that executable was compiled. That later desktop
  frame-loop build is pending; its native window behavior is unverified.
- The worktree-wide whitespace check passed. Final publication audit: pending.

## Repair ledger and capability delta

Changed files for this continuation are
`libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
`libs/d3d12/src/scene_renderer.cpp`,
`libs/d3d12/src/project_scene_renderer.cpp`, `apps/cli/scene_render.cpp`,
`tests/d3d12_scene_renderer_tests.cpp`,
`tests/run_scene_render_cli_tests.cmake`, this ledger, the related verification
notes/acceptance matrix and the ignored queue's evidence paragraph. The existing
presentation, graph dispatch and texture loader work was verified/preserved,
not authored as part of these repairs. Only the local build cache was changed
to enable the existing D3D12 desktop target; no tracked CMake option was changed.

Capability gained: desktop-sized graph identity contexts can render; admitted
scalar occlusion now survives all the way to native pixels; the newer direct
GPU target path has adversarial ownership/state/pixel evidence. The source and
shader material/vertex ABIs remain unchanged. Additional constant textures are
derived GPU resources, not invented authored texture assets or fallback source
images.

Governance preserved: project preparation/rendering is read-only and tests
compare canonical project bytes. No source-state transition, journal/domain
event write, permission/org authority or human publication boundary was added.
Cross-org roles, justification and datetime rules are not renderer inputs;
foreign/stale source scopes and generations remain explicitly tested. No schema
migration, secret, network, deployment or external-account action was added.

Not run: the full repository suite, full Vulkan/ImGui desktop validation,
clean-machine packaging, live native gestures/DPI, sustained real-scene/8K
workloads, external consumer acceptance, and RegOS/IDA_Training/
hidden_canopy_bot/Ask_IDA suites. Native capture failure prevents the visual
gate; source inspection or offscreen pixels are not substitutes.

Remaining risks: the renderer retains fixed ambient lighting when no SH input is
provided and only has a single directional shadow map; cascades/contact shadows,
specular environment filtering/reflections, temporal accumulation and AA remain
open. The later V4 diffuse-SH source path is code-only and unverified.
Each distinct unbound occlusion factor can add a temporary float texture/upload
buffer; equal values are reused, but declared payload bounds are not GPU-memory
reservations. Long identities now use a documented hash recipe, so trace tools
must reconstruct the full context rather than expect raw names in the artifact.
Consumers must recognize the current renderer version and not reuse old cache
entries. Existing two-file
BMP/receipt publication can still leave a partial derived output on I/O failure.

Rollback: revert only the handle fix, bounded-identity fallback, scalar-occlusion
GPU preparation, V2/receipt hunks and these new tests/notes. Preserve unrelated
concurrent changes in the same files. No saved-project, database or journal
rollback is needed. Historical V1 captures may be retained as historical evidence.

Intentionally rejected: trusting an older green binary as current-source proof,
truncating lineage, accepting white fallback instead of the source occlusion
value, disabling failing regressions, rewriting the broader RHI before checking
existing work, and counting a wrong-surface/failed capture as native acceptance.

## Cross-product trace and promotion

Direct changes are confined to Cartographer rendering, tests and ledgers; there
were no direct changes to RegOS, IDA_Training, hidden_canopy_bot or Ask_IDA. The
shared consumer contract now has source-only V3 shadow execution, V4 optional
diffuse-SH lighting, and V5 equirectangular SH derivation with explicit color
space. Request digests, six shader digests, recorded-pass names, and optional
environment digest/color-space receipt fields carry that lineage. Project
schemas and the other products' APIs are unchanged. The accepted captures still
identify V2, so consumers must keep V2 evidence separate from unverified
V3/V4/V5 output and check source, digest and renderer version before adopting
it. Required follow-up includes V3/V4/V5 cache separation, real-scene baselines, full
texture/material graph semantics, native gesture/presentation capture,
sustained workloads and consumer/deployment acceptance.

Dataset progress: synthetic adversarial regression cases, with no external
dataset ingestion. Training/prompt progress: none. Capability progress: the
bounded corrections and native tests above; later V3/V4/V5 additions are
unverified. Promotion progress: local native V2 foundation evidence;
photorealistic quality and the outstanding queue items remain unproven/open.

## Code-only solution-file boundary continuation (2026-10-26)

The publication audit now scans `.sln` and `.slnx` files for the same known
private production implementation references it checks in CMake, MSBuild
project, and source build files. Its reachable-history marker also recognizes the private
Sculpt evaluation source path, so the content scan does not skip a solution
entry that only names that implementation path. Regression source covers the
solution reference in the current tree, index, and reachable history. No audit
or test was run during the user's no-test sprint instruction.

- **Files changed:** `scripts/public_boundary_audit.py`,
  `tests/public_boundary_audit_tests.py`, `docs/public-private-boundary.md`,
  this ledger, and the ignored local work queue.
- **Capability gained:** a checked-in Visual Studio solution cannot silently
  add the protected Sculpt evaluation implementation without the publication
  check detecting it.
- **Invariants preserved:** the scan remains read-only; it reports file/path
  and category without printing protected source content. No private source,
  build target, runtime, artifact, or secret was added.
- **Tests authored:** current, staged, and history cases for an MSVC solution
  reference. **Run:** none.
- **Not run:** the Python audit tests, full publication audit, build, or any
  product suite.
- **Risks:** this remains token-based detection for known private interfaces;
  it is not a complete parser for every solution/project format or a content
  classifier for renamed private algorithms.
- **Rollback:** remove `.sln` from the scanned build-file suffix set, remove
  the history marker and regression, and revert only this documentation.
- **Rejected:** treating a filename extension as a violation by itself or
  broadening the scan to generic words such as `private` that would flag
  documentation and allowed client code.

**Cross-product trace:** impact is Cartographer's publication audit only.
RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA code, schemas, data,
evaluations, and deployment are unchanged. Dataset/training progress is
unchanged; capability progress is source-only; promotion remains unadvanced.

## Code-only directional shadow integration (2026-10-26)

The authored-scene path now derives a bounded directional orthographic view
from all transformed mesh vertices, renders an alpha-tested caster pass into a
1024-square-or-smaller color depth map with a hardware depth attachment, and
uses a 3x3 depth comparison filter in the scene PBR shader. The graph orders
`scene.shadow`, `scene.pbr`, tone mapping, offline capture-source readback, and
any caller-target/desktop handoff.
It declares the shadow caster's base-color reads, PBR material texture reads,
and per-object vertex/index buffer reads. Copy-destination to shader-read and
undefined to vertex/index transitions are dispatched at each resource's first
graph use.
Each PBR vertex carries a precomputed light-space position. The later V4
diffuse-SH continuation uses a seven-texture PBR root signature at exactly 64
of D3D12's 64 DWORDs without changing the
shared 52-byte `cartographer_pbr.hlsl` ABI. The separate scene shaders include
the extra vertex data and shadow sampling. Masked base-color alpha is applied
in both the caster and shaded passes.

The shadow checkpoint advanced scene identity to V3/six shader digests.
Assertions
now require the shadow graph pass and the CLI receipt contract expects six
shader digests and four offline scene passes, including capture readback-source
state. These edits are code-only: no shader
compile, build, test, GPU execution, image inspection, or native desktop check
was run. Shadow accuracy, graph scheduling and the changed receipt are not
accepted until those gates are exercised. A later source review corrected the
shadow pixel shader to store rasterized `SV_POSITION.z` directly; shader
compilation and GPU readback remain open. The one-map method has no cascades or
contact shadows and can lose distant shadow coverage in a scene with a very
large world-space extent.

The subsequent V4 source continuation accepts an optional nine-coefficient
Lambert diffuse SH artifact, hashes the exact input file, includes the input
and coefficients in the request digest, and records the environment digest in
capture receipts. The PBR pass reads a 3x3 uploaded coefficient texture; the
CLI and source-backed D3D12 desktop can load the same bounded interchange file.
It retains the fixed ambient approximation only when no SH input is supplied.
This does not add specular IBL or environment prefiltering.

The later V5 source continuation derives the same order-2 diffuse SH from an
opaque, bounded 2:1 equirectangular PNG/JPEG/TGA image. It requires an explicit
linear or sRGB interpretation, records that choice in request identity and
receipts, and hashes the exact source image bytes. Its 8-bit decoder, angular
quadrature, D3D12 pipeline, CLI and desktop wiring have not been built,
shader-compiled, tested, or visually accepted; synchronous preprocessing cost,
HDR decode, and specular IBL remain open. The parent render-graph task remains
started.

The touched CLI receipt serializer also emitted the same `render_graph_passes`
JSON key twice. It now emits the array once, with a source assertion that
rejects duplicate field text. The pass data is unchanged. This check was
authored but was not run under the no-test instruction.

Changed files: `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
`libs/d3d12/src/scene_renderer.cpp`,
`libs/d3d12/shaders/cartographer_scene_pbr.hlsl`,
`libs/d3d12/shaders/cartographer_scene_shadow.hlsl`,
`apps/cli/scene_render.cpp`, `tests/d3d12_scene_renderer_tests.cpp`,
`tests/run_scene_render_cli_tests.cmake`, the local work queue,
`docs/verification/authored-scene-rendering.md`,
`docs/verification/repo-rendering-boundaries.md`, and
`docs/verification/acceptance-matrix.md`. The shared PBR/shadow shader ABIs,
project schema, journal, authoring state and material-source contracts are
unchanged. Rollback only these V3/shadow hunks and the two scene-only shader
files; preserve all unrelated shared workspace changes.

Cross-product impact is limited to Cartographer's derived image and receipt.
Indirect consumers must treat V3 as a distinct cache identity and attribute
the new shadow shader digests. No direct RegOS, IDA_Training,
hidden_canopy_bot or Ask_IDA change was made. Dataset/training progress is none;
capability progress is authored but unverified shadow execution; promotion
progress remains gated on fresh compilation, GPU readback, visual acceptance,
real-scene fidelity and sustained performance evidence.

## Dirty-frame graph composition (2026-10-26, code-only)

When camera or viewport input changes the saved-scene view, the D3D12 desktop
now records scene PBR, tone mapping, cached-scene copy, and backbuffer present
state in one compiled graph and one synchronous submission. The graph imports
the current swapchain backbuffer and checks its extent, format, and copy
destination usage. The scene target remains caller-owned and ends in
`copy_source`; the backbuffer ends in `present`, after which the shell invokes
DXGI `Present`. Unchanged frames continue to copy the cached scene through the
existing two-pass frame graph. The authored-scene pipeline still has no scene
shadows, IBL/reflections, temporal accumulation, or AA.

- Files changed: `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`,
  `libs/d3d12/src/scene_renderer.cpp`, `apps/d3d12_desktop/main.cpp`,
  `tests/d3d12_scene_renderer_tests.cpp`, this ledger, and the local work queue.
- Capability gained: a camera/resize frame now submits scene rendering and the
  application copy/present-state handoff together.
- Boundary improved: frame-target alias, extent, format, and usage mismatches
  refuse before submission; cached scene and swapchain ownership remain
  separate.
- Governance preserved: scene source preparation is read-only; no project or
  journal mutation, fallback pixels, or new authoring authority was added.
- Tests added: exact graph pass order and one-submission assertions, copied
  output comparison against the offscreen capture, and invalid target
  refusal/ownership cases. Authored but not run.
- Tests run: none, under the code-only sprint instruction.
- Tests not run: D3D12 build or tests, full desktop build, debug-layer run,
  native presentation/capture, gesture/DPI checks, and sustained performance.
- Risks: the unbuilt graph pass order and resource-state contract may expose a
  compile or native validation error. DXGI `Present` remains a separate shell
  call after the graph's present-state transition. Clean frames still use the
  cached-scene copy graph.
- Rollback: remove the frame-handoff overload, graph imports/passes, dirty-frame
  dispatch change, regression, and this continuation; retain existing direct
  target and cached-copy paths.
- Rejected: rendering the scene on every idle frame, removing the cached scene
  texture, or claiming the graph transition alone is a completed DXGI present.
- Cross-product trace: Cartographer D3D12 only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts and datasets are unchanged. The
  additive C++ API needs consumer rebuilds; no project schema changed.

Dataset progress: none. Training/prompt progress: none. Capability progress:
code-only dirty-frame composition. Promotion progress: unchanged; build, native
presentation, image quality, and performance gates remain open.
