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
