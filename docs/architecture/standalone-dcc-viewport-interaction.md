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

The shell remains responsible for projection, GPU readback, pointer-device
translation, and dispatching an admitted selection or tool command. The editor
contract is the shared seam between those concerns and the authoritative
application boundary.

This slice does not claim GPU identity-picking performance, tablet support,
ray generation, or a complete transform gizmo. Those are consumers of this
contract and require their own native receipts and acceptance fixtures.

The contract also keeps public Cartographer independent of CONDITION, VANTA,
private runtime code, and AI mutation paths.
