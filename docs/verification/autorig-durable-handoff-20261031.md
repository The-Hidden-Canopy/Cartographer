# Autorig durable acceptance handoff — 2026-10-31

## Status and lineage

Draft implementation on `codex/autorig-durable-acceptance-20261031`, based on
`33ba1650c8ea2e42d9f010e0395b150b162a1400` (Cartographer main). The graph
adapter and historical review-record codec were selectively ported from
`b37ae936dde61ad1e77471010157e74be37f4e9e` and
`e6662bac1752b6e3345db75bf48f6e2b1c2737ce`, respectively, then reviewed and
adapted on this branch. Neither historical commit supplied a complete
durable rig and transferred weights. No merge or integration-queue ownership
is part of this work.

The implementation is **not a complete persistent IK/FK rig**: no authored
chain-to-control mapping, switch semantics, or solver configuration is stored.
It is also not a claim of native workflow or asset acceptance. See
[schema v36](../format/cartographer-project-v36.md) for the proposed format.

## Implementation boundary

- A reviewed candidate and current staged mesh are checked before conversion.
  The caller supplies durable joint IDs and final parent-local transforms;
  their composed positions must agree with reviewed anchors. Candidates with
  omitted parts or mechanical ports are refused.
- A canonical record retains the review assertion, selected-candidate pins,
  mapping and graph. The bundle additionally stores the `RigDefinition` and
  control graph, `JointLimitSet`, complete transferred per-vertex binding,
  layer metadata and receipt. Graph, limit, control, symmetry, mesh and weight
  references are cross-checked before staging.
- One revisioned schema-v36 row is staged through `ProjectTransaction` and
  published by its journaled commit. Public typed decode and an explicit
  current-project readiness check provide a read path after reopen.
- Existing IK, FK and skinning algorithms can operate on explicitly supplied
  compatible data. This does not persist the authoring configuration needed
  for an IK/FK switch.

## Focused verification

- `cmake --preset default -G Ninja` configured successfully (exit 0).
- `cmake --build --preset default --target cartographer_autorig_graph_handoff_tests cartographer_autorig_pose_skin_tests cartographer_accepted_autorig_project_tests cartographer_rig_record_tests cartographer_transferred_weight_record_tests -j 4` passed (exit 0). The project test was rebuilt after its failure fixture was corrected (exit 0).
- `ctest --test-dir build/default --output-on-failure -R '^(cartographer_autorig_graph_handoff_tests|cartographer_rig_record_tests|cartographer_transferred_weight_record_tests|cartographer_autorig_pose_skin_tests|cartographer_accepted_autorig_project_tests)$'` passed 5/5 (exit 0, 2.09 s). Earlier runs exposed an invalid review-assertion token and a journal-limit fixture that prevented beginning the transaction; both fixtures were fixed and rerun.
- `cmake --build --preset default --target cartographer_character_records_project_tests cartographer_project_authoring_standards_tests cartographer_sweep_source_project_tests cartographer_rig_module_project_tests cartographer_project_source_asset_registry_tests cartographer_condition_prefab_placement_tests -j 4` passed (exit 0). Warnings came from unchanged source files.
- `ctest --test-dir build/default --output-on-failure -R '^(cartographer_character_records_project_tests|cartographer_project_authoring_standards_tests|cartographer_sweep_source_project_tests|cartographer_rig_module_project_tests|cartographer_project_source_asset_registry_tests|cartographer_condition_prefab_placement_tests)$'` passed 6/6 (exit 0, 8.66 s).

These focused CPU tests do not establish native editor, independently
authored asset, actual application undo/redo, or full runtime acceptance.
The delivering worker's response records the resulting commit, if any.

## Limits and manual gates

- External source content digest and source-binding provenance are supplied
  by the caller. Current staged mesh revision and geometry are checked, but
  external bytes and reviewer identity are not authenticated.
- Consumers must invoke `validate_accepted_autorig_current` before use. A
  subsequent source edit or removal does not itself mark the stored layer
  stale. Cold decode proves internal consistency only.
- Sparse correspondence leaves unlisted target vertices rigid. The transfer
  record does not independently prove the supplied source binding corresponds
  to an available source mesh.
- The bundle's 64 MiB aggregate bound is not a guarantee of journal
  admission: the journal includes the whole project and framing. A near-bound
  commit can fail and must not publish partial state.
- Author, persist, validate and integrate an explicit IK/FK chain, controls,
  switch and solver configuration before calling the rig complete. Verify
  both switch directions and position/orientation behavior with a real asset.
- Manually exercise review, selection, explicit final authoring, commit,
  save/reopen, freshness refusal after mesh modification, skin deformation,
  and IK/FK switching in the native editor on independently authored assets.
  Confirm migration, journal undo/redo and project-size behavior in the
  integration environment. Focused CPU tests do not establish these gates.

Rollback is to leave this branch unmerged or revert the integration commit;
preserve the base checkout and other lanes' work. Existing v36 files should be
backed up before running an older reader that does not support the new schema.

## Per-file line counts

Compared with the base revision; added files were untracked at handoff preparation.

Tracked changes (added / deleted):

- `docs/architecture/character-production.md`: +8 / −7
- `docs/capability-matrix.md`: +1 / −1
- `libs/character/CMakeLists.txt`: +3 / −0
- `libs/character/include/carto/character/autorig_analysis.hpp`: +6 / −0
- `libs/character/src/autorig_analysis.cpp`: +19 / −0
- `libs/project/CMakeLists.txt`: +3 / −3
- `libs/project/include/carto/project/project.hpp`: +15 / −1
- `libs/project/include/carto/project/transaction.hpp`: +21 / −0
- `libs/project/src/project.cpp`: +208 / −0
- `libs/project/src/transaction.cpp`: +45 / −0
- `libs/weights/CMakeLists.txt`: +1 / −0
- `tests/CMakeLists.txt`: +25 / −0
- `tests/character_records_project_tests.cpp`: +40 / −16
- `tests/condition_prefab_placement_tests.cpp`: +5 / −1
- `tests/project_authoring_standards_tests.cpp`: +11 / −3
- `tests/project_source_asset_registry_tests.cpp`: +6 / −4
- `tests/rig_module_project_tests.cpp`: +9 / −8
- `tests/sweep_source_project_tests.cpp`: +2 / −2

New, untracked files (added / deleted):

- `docs/format/cartographer-project-v36.md`: +48 / −0
- `docs/verification/autorig-durable-handoff-20261031.md`: +119 / −0
- `libs/character/include/carto/character/autorig_graph.hpp`: +40 / −0
- `libs/character/include/carto/character/autorig_graph_record.hpp`: +40 / −0
- `libs/character/include/carto/character/rig_record.hpp`: +18 / −0
- `libs/character/src/autorig_graph.cpp`: +97 / −0
- `libs/character/src/autorig_graph_record.cpp`: +159 / −0
- `libs/character/src/rig_record.cpp`: +161 / −0
- `libs/project/include/carto/project/accepted_autorig.hpp`: +24 / −0
- `libs/project/src/accepted_autorig_internal.hpp`: +11 / −0
- `libs/weights/include/carto/weights/transfer_record.hpp`: +34 / −0
- `libs/weights/src/transfer_record.cpp`: +153 / −0
- `tests/accepted_autorig_project_tests.cpp`: +139 / −0
- `tests/autorig_graph_handoff_tests.cpp`: +141 / −0
- `tests/autorig_pose_skin_tests.cpp`: +173 / −0
- `tests/rig_record_tests.cpp`: +107 / −0
- `tests/transferred_weight_record_tests.cpp`: +119 / −0
