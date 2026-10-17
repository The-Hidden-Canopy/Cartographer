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

The test is structural and CPU-reference evidence only. Native GPU picking,
high-DPI input, tablet pressure, device-loss behavior, and interactive timing
remain unverified until a native desktop acceptance harness records them.
