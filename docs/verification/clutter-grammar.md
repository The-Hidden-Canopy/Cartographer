# Deterministic vegetation clutter grammar

## Source and preview boundary

The public vegetation scatter recipe is Cartographer's current authored
clutter grammar: one source recipe selects one vegetation asset or one exact
botanical source receipt, binds moisture, shade, disturbance, vegetation
suitability, and elevation field receipts, and defines spacing, seed, density,
suitability threshold, scale range, enabled state, and a maximum instance
count. The same deterministic production is evaluated at each bounded grid
cell. Stable placement IDs, seeded density, yaw, and scale are derived from
recipe and cell identity.

Preview is an ephemeral derived value. It binds the authoring digest, project
and application revisions, the botanical source receipt when that recipe
option is used, and every field identity, revision, semantic, and digest.
Candidate and output limits fail closed; no
partial preview is returned. Missing, wrong-semantic, stale, or out-of-coverage
fields reject preview generation. Repeating the same source inputs yields the
same placements. Preview generation does not mutate the project.

Placement enters the scene only through the explicit human
`commit_condition_vegetation_preview` action. The commit rechecks the current
project generation/revision, source and field receipts, and target object
before creating the batch. Accepted placements receive durable source and
preview provenance and use the project command/journal path; undo and redo
preserve that history. Stale previews, invalid targets, and duplicate commits
are rejected without partial scene mutation.

This contract is a single-species vegetation scatter grammar. It does not claim
an unrestricted multi-asset rule language, automatic runtime spawning, or
simulation behavior.

## Acceptance evidence

Current-source run (2026-10-20):

```text
cmake --build build/headless-release --target cartographer_condition_authoring_tests --parallel 4
  succeeded (171/171 build steps)

ctest --test-dir build/headless-release --output-on-failure -R '^cartographer_condition_authoring_tests$'
  passed (1/1)
```

The passing executable calls these acceptance cases:

- `vegetation_scatter_round_trips_and_derives_deterministic_read_only_preview`
- `vegetation_scatter_preview_rejects_stale_missing_wrong_semantic_and_unbounded_inputs`
- `ui_vegetation_recipe_commits_reopens_undoes_and_rejects_stale_sources`
- `condition_rural_road_forest_workflow_is_revision_bound_and_portable`

The run emitted 13 existing non-fatal `-Wmissing-field-initializers` warnings
in `libs/application/src/application.cpp`; the condition-authoring target
compiled and linked successfully.
