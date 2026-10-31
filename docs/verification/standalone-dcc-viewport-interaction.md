# Standalone DCC viewport interaction verification

`cartographer_viewport_interaction_tests` proves the bounded interaction
contract with CPU-reference evidence:

- malformed hit identity, ambiguous components, non-finite depth, and zero
  normals reject;
- picks ignore stale source revisions, backfaces, occluded hits, and depth
  outside the requested window unless the explicit option permits them;
- nearest selection is deterministic and independent of input order for the
  same depth/identity tie rules;
- box selection rejects invalid coordinates, deduplicates objects, and keeps
  component identity distinct;
- drag capture transitions through captured/dragging/completed states;
- cancellation is explicit;
- a source revision change during capture fails closed without mutating the
  session into a completed state.
- perspective and orthographic marker projection return viewport-local
  coordinates; behind-camera and off-viewport markers are distinguished, and
  invalid viewports/non-finite points reject;
- numeric translation preview preserves the two unlocked coordinates, applies
  unit-aware relative/absolute input only on the locked world axis, rejects
  stale revisions and malformed expressions, and returns a candidate only on
  explicit confirmation; cancellation clears the modal session.

The test is structural and CPU-reference evidence only. Native GPU picking,
high-DPI input, tablet pressure, device-loss behavior, and interactive timing
remain unverified until a native desktop acceptance harness records them. The
numeric session emits a candidate transform and does not itself dispatch a
project mutation.

`cartographer_desktop_workbench_input_tests` also exercises the docking
adapter's saved panel tree and monitor-recovery geometry: a valid negative-
origin monitor is preserved, a useful visible strip is not moved, off-screen
windows are recovered, oversized windows retain a draggable strip, and missing
or malformed monitor data leaves the saved position unchanged. This does not
verify real Win32/Vulkan tear-out, mixed-DPI input, or unplug/replug behavior;
those remain interactive acceptance gates.

## Source-only perspective viewport integration (2026-10-25)

The desktop source now connects `EditorCamera` to a plan/perspective viewport
toggle, orbit tool, pan, dolly, framing, world-to-screen projection, X/Z
translation handles, screen-ray ground placement, and mesh-drop placement. Scene
triangles are sorted far-to-near by average camera depth for the ImGui painter
path. Perspective triangles are clipped against a distance-scaled near plane;
generated fan triangles preserve barycentric correspondence to the original
source corners, and authored edge segments are clipped while retaining their
source edge identities. Surface-hit snapshots carry perspective view depth and
use perspective-correct source barycentrics, so sculpt/decal anchors
interpolate against the captured source mesh rather than the projected screen
triangle. Perspective selection uses the bounded visible
surface cache for frontmost object/face picks and rejects occluded vertex/edge
candidates. Regular edge selection and loop-cut seed picking share the same
perspective-depth visibility check, and loop-cut picking rejects a stale
project-generation snapshot before dispatch. This is code wiring only; the task
remains started pending compilation and native observation. In the perspective
workspace, the legacy ImGui selection fallback is disabled so a rejected native
pick cannot be re-dispatched from unoccluded screen-space candidates; the
loop-cut hover preview also hides occluded edges. Sculpt/knife captures bind
the camera parameters, project generation/revision, and viewport rectangle, and
cancel if that view state changes mid-gesture.

Changed source: `apps/desktop/main.cpp`, `libs/editor/include/carto/editor/editor_camera.hpp`,
`libs/editor/src/editor_camera.cpp`, `libs/editor/include/carto/editor/viewport_interaction.hpp`,
`libs/editor/src/viewport_interaction.cpp`, `libs/ui/include/carto/ui/ui.hpp`, and
`libs/ui/src/ui.cpp`. Regression cases were authored in
`tests/editor_camera_tests.cpp`, `tests/viewport_interaction_tests.cpp`, and
`tests/ui_tests.cpp`; none were run for this source-only sprint.
The capture camera comparison, segment interval planner, and reciprocal-depth
edge visibility predicate now live in `apps/desktop/studio_viewport_capture.hpp`;
the desktop shell calls these same helpers. Adversarial cases were added to
`tests/editor_camera_tests.cpp` for camera changes, non-finite and over-budget
stroke gaps, and visible versus occluded edge depth. They were not run. Direct
loop-cut seed context and target admission now also use shared policy helpers,
so the desktop route and regression cases share the same ignore/keep/cancel/
admit decisions. Added cases cover wrong context, stale snapshots, speculative
preview, modifiers, viewport misses, ambiguous or invalid edge data, missing
source identity, hidden/locked owners, and an occluded perspective edge. These
cases were not run. The legacy-selection fallback predicate has focused
plan/perspective/non-workspace cases. The helpers do not construct a native
window or drive the complete ImGui pointer event route.
The selected-ref public-boundary audit returned `PUBLIC_BOUNDARY_PASS` for the
current tree and `HEAD` plus `origin/main`; other refs were not included. It
does not verify compilation, test behavior, or native rendering.
The UI surface sampler now has a source-only regression for clipped-vertex
barycentric composition and refusal of a non-normalized source mapping. It does
not exercise desktop triangle clipping or clipped edge rendering, and was not
run.

The code keeps project mutation on existing human command routes and binds
camera-framed scene state to the captured project generation/revision. No
project camera record, schema, journal, permission, organization, or AI mutation
authority was added. No external repository or cross-product contract was
changed.

### Loop-cut seed admission policy ledger (2026-10-26, code-only)

- Files changed: `apps/desktop/studio_viewport_capture.hpp`,
  `apps/desktop/main.cpp`, `tests/editor_camera_tests.cpp`, this file, and
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- Capability gained: the production loop-cut pointer route delegates its
  context and target decisions to pure shared policy helpers, preserving the
  established retry-versus-cancel outcomes while making occlusion and stale
  input boundaries regression-addressable.
- Invariants preserved: selection still dispatches `SelectEdgeAction`, then
  rechecks generation, revision, mode, owner, and exact selected edge before
  invoking the existing human loop-cut action. The helper cannot mutate the
  project or dispatch tools.
- Tests authored: context changes, stale project snapshot, unavailable cache,
  speculative preview, modified click, outside viewport, miss, ambiguity,
  invalid hit, missing source edge, hidden/locked owner, frontmost admission,
  occluded edge refusal, and depth diagnostic preservation.
- Tests run: none. The user requested a code sprint without testing; no build,
  test, lint, or native interaction was performed.
- Public/private audit: `PUBLIC_BOUNDARY_PASS` for the current tree and selected
  refs `HEAD` and `origin/main`. The optional ignore-policy probe was blocked
  by sandbox permission to write its temporary `.gitignore`; the boundary scan
  passed when rerun without that probe.
- Risks: the desktop source is uncompiled. The helper cases exercise the exact
  production decisions but do not simulate ImGui/Win32 event delivery, cache
  construction, actual surface resolution, or dispatch sequencing.
- Rollback path: restore the inline loop-cut admission branches in
  `main.cpp`, remove the two decision helpers and their regression cases, and
  remove this ledger. Preserve the existing camera and depth helpers and other
  work in the shared files.
- Intentionally rejected: reporting pure policy tests as a complete native
  pointer-router test or changing project mutation and tool-dispatch authority.
- Cross-product trace: Cartographer only; no RegOS, IDA_Training,
  hidden_canopy_bot, or Ask_IDA contract, data/evaluation, or deployment state
  changed. Dataset, training, and promotion progress are unchanged.

Risks remain: this is not depth-buffered rendering. Average-depth painter order
can misorder intersecting or concave surfaces. Near-plane clipping and hit
correspondence are source-only changes and remain unbuilt and unverified.
Other derived preview overlays are not yet occlusion-tested. Sculpt and knife captures check the
scene-wide frontmost surface at each event and sample the interpolated screen
segment between events at no more than one-pixel spacing. Each segment is
bounded to 64 samples and one million triangle checks; larger/high-density gaps
cancel the stroke. This cannot reconstruct an OS-coalesced curved path between
reported positions. Perspective selection
stops when the visible hit cache exceeds its triangle cap. Same-object shared
face boundaries resolve deterministically for object-level surface, component,
and edge visibility, while
cross-object equal-depth surfaces remain ambiguous. No native screenshot,
gesture, DPI, resize, or restart acceptance is claimed. The focused tests/build
remain unverified by request.

Rollback is selective: remove near-plane clipping by restoring whole-triangle
visibility and identity-only projected hits. To remove the broader perspective
path, remove its toolbar/input/projection route in `apps/desktop/main.cpp`,
restore scalar-only transform projection in the editor interaction session, and
remove `studio_viewport_capture.hpp`, its target listing, and the matching
regression cases. Preserve existing unrelated edits in these shared files. A
full GPU renderer and claiming native acceptance before evidence were
intentionally rejected for this slice.

### Viewport capture policy extraction ledger (2026-10-26, code-only)

- Files changed: `apps/desktop/studio_viewport_capture.hpp`,
  `apps/desktop/main.cpp`, `apps/desktop/CMakeLists.txt`,
  `tests/editor_camera_tests.cpp`, this file, and
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- Capability gained: desktop stroke captures and edge picks share explicit,
  separately callable camera-state, bounded-sampling, and perspective-depth
  decisions; regression cases exercise those exact helpers.
- Invariants preserved: project generation/revision and scene-hit admission
  remain in the desktop route; the helper does not mutate project state or
  grant selection/authoring authority.
- Tests added: camera-change invalidation, non-finite and over-budget gaps,
  triangle-check budget, reciprocal-depth interpolation, and nearer-surface
  occlusion; plan/perspective/non-workspace legacy-picker admission.
- Tests run: none. No build, test suite, or native interaction was run at the
  user's request.
- Public/private audit: `PUBLIC_BOUNDARY_PASS` for the current tree and
  selected refs `HEAD` and `origin/main`; other refs were not included.
- Risks: helper coverage does not exercise Win32 event routing, loop-cut
  dispatch, painter rendering, GPU depth, or native gestures. Source changes
  are uncompiled.
- Rollback path: restore the three inline decisions in `main.cpp`, remove the
  helper and its CMake entry, remove the matching regression cases, and remove
  this ledger. No project data or serialization changed.
- Intentionally rejected: claiming event-router or rendering acceptance from
  pure helper regressions, and adding GPU behavior without a verifiable
  rendering contract.
- Cross-product trace: direct impact is Cartographer only. No RegOS,
  IDA_Training, hidden_canopy_bot, or Ask_IDA contract, dataset, evaluation,
  deployment, or promotion state changed.
