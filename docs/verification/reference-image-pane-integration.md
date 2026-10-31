# References pane integration

## Repair ledger

- **Files changed:** `apps/desktop/main.cpp`, this ledger, and the ignored
  local work queue.
- **Capability gained:** the desktop References pane now reports the saved
  project plane count and enabled count, and opens the existing Inspector
  entry point for reference image and view-set controls.
- **Boundary improved:** the pane no longer describes implemented
  project-local reference images as future work. It labels viewport drawings
  as UI overlays, not depth-tested scene geometry, and identifies the
  selection needed to reach the existing Inspector controls.
- **Governance preserved:** the pane reads the current snapshot catalog and
  changes only transient workbench visibility. Import, view edits, and source
  decoding continue through their existing project and source-admission paths.
  No persisted schema or authority boundary changed.
- **Tests added or updated:** none; this is a narrow desktop handoff change.
- **Tests run:** none, per the user's instruction.
- **Tests not run:** all builds, desktop/UI suites, native interaction, source
  import, and visual review.
- **Risks:** the desktop source is uncompiled. The existing reference editor
  remains inside Inspector > Scene presentation and requires a selected scene
  object; the new pane explains that dependency instead of duplicating its
  editing controls.
- **Rollback:** remove `draw_reference_images_instrument`, restore the
  placeholder call, and remove this ledger and its work-queue note. No project
  data migration or persisted-state rollback is needed.
- **Rejected:** duplicating reference editing controls, treating enabled planes
  as proof of decoded/current viewport pixels, or claiming image overlays are
  depth-tested geometry.

## Cross-product and promotion trace

Direct impact is Cartographer's desktop navigation for reference images.
RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA have no direct or indirect
code, API, schema, dataset, evaluation, or deployment changes. Dataset and
training progress are unchanged; capability progress is source-only; promotion
remains open pending compilation and native interaction evidence.
