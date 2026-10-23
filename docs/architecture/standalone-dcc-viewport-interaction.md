# Standalone DCC viewport interaction boundary

Cartographer's native DCC needs a shared interaction contract instead of
separate picking logic in each shell or tool. The public editor layer now
provides `ViewportHit`, `ViewportPickSettings`, `pick_viewport_hit`,
`select_viewport_box`, and `ViewportInteractionSession`.

The contract is intentionally backend-neutral:

- a hit carries object/component identity, screen position, world position and
  normal, depth, face orientation, occlusion state, and the source revision;
- a CPU reference picker applies the same revision, backface, occlusion, depth,
  object/component, and nearest-hit rules a native identity picker must obey;
- box selection returns deterministic identity-bound records without mutating
  `ProjectDocument` or editor selection state;
- drag capture owns only pointer state and refuses document revision changes;
- completion and cancellation are explicit, so a modal tool can produce one
  command on confirmation and no command on cancellation.

`EditorCamera::project_marker()` now supplies deterministic viewport-local
marker coordinates for orthographic and perspective views, with explicit
front-facing and viewport-inclusion flags. It does not guess near/far clipping.
`ViewportNumericTransformSession` provides revision-bound, world-axis-locked
translation preview and explicit confirmation using the bounded numeric
expression parser. Preview and confirmation return a candidate transform; the
caller still must dispatch it through the authoritative application command
boundary. Cancellation returns no candidate.

The shell remains responsible for GPU readback, pointer-device translation,
rendering the projected marker, and dispatching an admitted selection or tool
command. The editor contract is the shared seam between those concerns and the
authoritative application boundary.

In the Win32/Vulkan desktop shell, detached ImGui viewports use the owning
platform window for pointer translation and capture; their layout is stored in
the user-local `workspace-docking.ini`, never in `.carto`. After native platform
windows update, a detached viewport with no useful intersection against any
connected monitor work area is moved to the nearest work area. A viewport
already showing a useful strip is not moved; oversized viewports retain a
draggable strip rather than being silently resized. This recovery changes
presentation only and does not change project or selection authority.

This slice does not claim GPU identity-picking performance, tablet support,
near/far clipping, a rendered gizmo, or a complete transform gizmo. Those are
consumers of this contract and require their own native receipts and
acceptance fixtures. The monitor-recovery geometry has headless coverage, but
actual OS-window tear-out, mixed-DPI movement, and display-removal recovery
still require interactive desktop acceptance.

The contract also keeps public Cartographer independent of private project
integrations, runtime code, and AI mutation paths.
