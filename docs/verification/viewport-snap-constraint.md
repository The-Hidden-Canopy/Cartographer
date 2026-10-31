# Viewport snap constraint control

**Status:** Code and UI wiring added; native interaction and build acceptance
remain open.

## Scope

The Constraint instrument now controls the cross-axis alignment tolerance used
by the existing viewport geometry-snap preview. The default remains 0.05 m;
the UI bounds the setting to 0.001–0.5 m. Ctrl-grid snapping retains precedence
over Shift geometry snapping, and the chosen X/Z gizmo axis still controls the
movement direction.

The tolerance is session-local UI state. A drag captures its value at start and
uses the same value for preview and completion, so changing the instrument
cannot silently change an active drag's commit rule. A transform is still
committed only through the existing application command path. This is not a
general relational-constraint solver.

## Repair ledger

- **Files changed:** `apps/desktop/main.cpp`,
  `libs/editor/include/carto/editor/viewport_interaction.hpp`,
  `libs/editor/src/viewport_interaction.cpp`,
  `tests/viewport_interaction_tests.cpp`, this ledger, and the local-only
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- **Capability gained:** users can tune X/Z cross-axis alignment for existing
  revision-bound viewport geometry snaps.
- **Boundary improved:** strict, default, nonfinite, negative, and over-bound
  tolerances are represented in source regression cases; invalid API inputs
  refuse before preview or mutation.
- **Governance preserved:** previews remain transient; source revisions and
  snap identity checks remain in place; final transforms use the current
  application command path. No schema or authority changed.
- **Tests added:** focused alignment and tolerance-boundary cases in
  `viewport_transform_snap_policy_is_shared_and_source_bound`.
- **Tests run:** none, per the user's instruction not to test this sprint.
- **Tests not run:** `cartographer_viewport_interaction_tests`, desktop build,
  and native Shift/Ctrl interaction.
- **Risks:** the public C++ helper signature changed, so dependent binaries
  need rebuilding. The tolerance is not persisted and resets to 0.05 m when
  the desktop process starts. Native UX and drag behavior are unverified.
- **Rollback:** remove the optional tolerance argument and instrument state,
  restore the fixed 0.05 m value in the snap helper, restore the Constraint
  placeholder, and remove the added source cases.
- **Rejected:** adding a general constraint graph, changing acquisition radius
  or hysteresis policy, or making this view imply broader constraint-solving
  support.

## Required acceptance

Build the editor and desktop targets, run the focused viewport interaction
suite, then use the native viewport to check X and Z drags at strict/default
tolerances, Ctrl-grid precedence, and identical preview/commit settings.
None of those acceptance gates is claimed here.

Direct impact is Cartographer only. RegOS, IDA_Training, `hidden_canopy_bot`,
and Ask_IDA are unchanged. Dataset and training progress did not change;
capability progress is code-only; deployment and promotion remain open.
