# Supported IK/FK key acceptance - source handoff

This is an uncompiled source continuation of `af76a5f` on
`codex/autorig-durable-acceptance-20261031` (original base `33ba165`). It
does not close the native or runtime acceptance rows. No integration claim,
merge, build lease, deployment or test execution is claimed.

The implemented case is one three-joint FK-to-IK graph at the accepted rig's rest pose,
with an identity current scene root and identity local control display
transforms, the default coordinate policy, isolated graph-world target-point,
pole-direction and mode controls, and exactly seven step tracks. The project
and scene revisions, scene source object and mesh identity, geometry digest,
rig row/revision/digest, rest graph, control graph, limits, source namespace,
mode, frame and exact tick are checked. Multi-chain, nonidentity frame,
follow-space, layered motion, arbitrary posed FK, and conflicting controls
fail closed. Source namespaces and asset provenance do not authenticate a
person or establish physical state.

An explicit human adoption copies one detached workspace into the project;
the detached source and its separate journal are not written or synchronized.
The project's v39 workspace snapshot is the sole source for this supported
project workflow. The accepted rig and workspace changes are in one
`ReplaceProjectDocumentCommand` and one project journal snapshot, with the
existing application rollback and undo/redo conventions. There is no sequential
cross-journal commit. The project codec checks a bounded payload, digest and
unkeyed envelope; the application validates the actual workspace on use.

The application derives the source rest pose and target/pole channel values;
the caller does not supply a source pose or an approval boolean. The move-only
session review exposes the match and proposal digest, and acceptance reruns
the calculation against the same current project and compares the exact
proposal and resulting workspace before consuming the token. Seven exact-tick
keys are written together. A CPU consumer samples the owned tracks, decodes
the controls and runs the two-bone solver; acceptance checks that this
consumer reproduces the reviewed pose and the source-side positions and
orientations within the specified bounds. This is authored/modelled state,
not runtime execution or observed physical evidence.

Legacy temporal channels and motion documents are refused by this supported
bridge, as is a non-classic C++ global locale. No old locale-dependent channel,
clip, graph or proposal digest is silently rebound. A separate migration and
owner-approved reference update remains necessary for those sources. The
workspace's public mutable store is not used for live acceptance: the bridge
decodes a private project snapshot and stages a replacement. Explicit adoption
is a project-local fork and does not assert an exclusive lock on the detached
workspace.

## Reviews and integration evidence required

Two final source review passes were performed by the implementing worker:
one traced authority, scene/source/frame pins, matched pose and human token;
the second traced v38/v39 serialization, atomic history, undo/redo, replay
and the source tests. These are not independent external reviews. They
identified the need to bind the scene object and geometry, verify the actual
key consumer, checksum the workspace revision envelope, quarantine old locale
paths and reject an oversized project snapshot; the source includes those
checks. Sol must independently review the reconciled candidate.

The recorded build-coordinator lease remains active. No compiler, build,
CTest, migration run, UI execution, or runtime acceptance was performed in
this continuation. Test source is registered for
`cartographer_application_ik_fk_key_acceptance_tests` and
`cartographer_control_workspace_project_format_tests`; existing schema
fixtures were updated for v39. Sol should build and run those targets, the
prior autorig/application, pose, workspace, project migration and journal
regressions, then verify the reviewed source/mode/tick/frame refusal, stale
token after edit/restore, failed journal publication, actual key values,
undo/redo, save/reopen, digest tamper and quarantined-locale cases on the
exact reconciled candidate. The source fixtures are not proof they pass.

`AUT-004`, `IK-001`, `IK-004`, `ANI-002` and `UI-003` retain their native
review, editable controls, arbitrary-chain, timeline and visual requirements.
The new UI-controller methods expose the host workflow; no desktop widget or
native click-through is implemented here. Independent assets, multi-chain
arbitration, orientation/position continuity beyond this supported case,
external runtime admission and CPU/GPU parity remain separate gates.

Collision and exchange work uses separate branches. Sol must reconcile the
small shared application/UI header and CMake hunks; this lane adds no collision
proposal method or generic mutable project accessor.

## Source change counts

Counts against `af76a5f` are added/deleted lines. New files were untracked at handoff.

| Path | Added | Deleted | State |
| --- | ---: | ---: | --- |
| `docs/format/cartographer-project-v39.md` | 27 | 0 | new |
| `docs/verification/autorig-key-acceptance-closeout-20261101.md` | 109 | 0 | new |
| `libs/application/CMakeLists.txt` | 2 | 0 | tracked |
| `libs/application/include/carto/application/application.hpp` | 86 | 0 | tracked |
| `libs/application/src/application.cpp` | 1 | 1 | tracked |
| `libs/application/src/ik_fk_key_admission.cpp` | 411 | 0 | new |
| `libs/project/include/carto/project/project.hpp` | 21 | 1 | tracked |
| `libs/project/src/project.cpp` | 99 | 0 | tracked |
| `libs/ui/include/carto/ui/ui.hpp` | 14 | 0 | tracked |
| `libs/ui/src/ui.cpp` | 38 | 0 | tracked |
| `tests/CMakeLists.txt` | 10 | 0 | tracked |
| `tests/accepted_autorig_project_tests.cpp` | 9 | 5 | tracked |
| `tests/application_ik_fk_key_acceptance_tests.cpp` | 364 | 0 | new |
| `tests/autorig_ik_fk_project_tests.cpp` | 4 | 2 | tracked |
| `tests/character_records_project_tests.cpp` | 32 | 14 | tracked |
| `tests/condition_prefab_placement_tests.cpp` | 3 | 1 | tracked |
| `tests/control_workspace_project_format_tests.cpp` | 34 | 0 | new |
| `tests/project_authoring_standards_tests.cpp` | 12 | 4 | tracked |
| `tests/project_source_asset_registry_tests.cpp` | 6 | 2 | tracked |
| `tests/rig_module_project_tests.cpp` | 8 | 4 | tracked |
| `tests/sweep_source_project_tests.cpp` | 5 | 1 | tracked |
| `tests/unadmitted_mode_key_project_format_tests.cpp` | 8 | 4 | tracked |
