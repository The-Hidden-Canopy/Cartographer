# SculptStack layer instrument

**Status:** Code wiring added; UI and persistence acceptance remain open.

## Change

The Layers instrument now reads the current `UiSnapshot` SculptStack for the
selected mesh and exposes the existing visibility, weight, and ordering
commands. The Sculpt tools pane calls the same view helper. Every edit still
passes the snapshot's project revision and generation through the existing
`ApplicationSession` dispatch actions.

The instrument explicitly scopes these controls to SculptStack. It does not
claim general workspace-layer, compositing, or visibility authority. Outside
the Sculpt workspace or without a selected mesh stack, it shows that boundary
instead of synthesized layer data.

## Repair ledger

- **Files changed:** `apps/desktop/main.cpp`, this ledger, and the local-only
  `docs/CARTOGRAPHER_WORK_QUEUE.local.md`.
- **Capability gained:** the Layers instrument can inspect and edit authored
  SculptStack layer visibility, weight, and order through the existing human
  command path.
- **Invariants preserved:** edits remain revision/generation-bound; the
  application actions retain persistence, journaling, undo/redo, and AI
  proposal refusal behavior. No project schema or cross-product contract
  changed.
- **Tests run:** none, per the user's instruction not to test this sprint.
- **Tests not run:** `cartographer_ui_tests`, desktop build, and native UI
  interaction/reopen acceptance.
- **Risks:** the new panel and Sculpt tools pane expose the same current stack
  while both are open. Their snapshots refresh through the normal UI loop;
  native interaction has not been reviewed.
- **Rollback:** remove `draw_sculpt_layer_stack` and `draw_layers_instrument`,
  restore the prior Sculpt tools layer block, and restore the Layers
  placeholder dispatch in `apps/desktop/main.cpp`.
- **Rejected:** inventing a generic workspace-layer model or routing changes
  outside the existing revision-bound application actions.

## Required acceptance

Run the existing SculptStack UI action cases, then use the desktop UI with a
saved project containing multiple SculptStack layers. Verify visibility,
weight, and ordering edits, action receipts, undo/redo, save/reopen, and the
non-Sculpt/empty-stack boundary. No such acceptance is claimed here.

RegOS, IDA_Training, `hidden_canopy_bot`, and Ask_IDA are not affected. Dataset,
training, capability-promotion, and deployment state did not change.
