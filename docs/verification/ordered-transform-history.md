# Ordered project transform history

Prefab placement acceptance exposed a history failure: undoing a child transform
advances the project revision, so the preceding root-transform command rejected
its own undo even after the child's edit had been restored.

Initial transform execution still requires the captured project revision and
exact source local transform. An admitted command's undo/redo requires its exact
expected target transform, captures the current project revision before checking
that state, and applies through `set_object_transform_if_revision`. Unrelated
authored data is retained. A conflicting target edit is refused without changing
the document or consuming the history entry.

## Evidence

`cartographer_tests` passes the new ordered two-object undo/redo regression,
unrelated-object preservation, conflicting redo refusal, and initial deferred
admission refusal after an unrelated revision change. Existing conflicting-target
undo and deferred-execution refusal tests remain enabled. The prefab acceptance
suite exposed the old consecutive-undo failure before this repair; its full
placement acceptance is recorded separately.

## Repair ledger

- Files: `libs/editor/include/carto/editor/command_bus.hpp`,
  `libs/editor/src/command_bus.cpp`, `tests/test_main.cpp`, this note.
- Capability: consecutive transform commands can undo and redo in history order
  while project revisions remain monotonic.
- Invariants: captured initial admission, stable target identity, exact target
  state, conditional revision mutation, command history, journal and rollback.
- Tests: focused core command suite passed; prefab/application integration
  results are recorded in the prefab snapshot verification note.
- Risks: an admitted command can replay across unrelated project revisions only
  when its target state matches exactly. This relies on existing stable scene
  object identities; it does not permit reuse of a deleted object's identity.
- Rollback: revert this transform command change; no schema or migration.
- Rejected: rewinding revisions, removing the revision guard, overwriting a
  contested target, or suppressing the failing undo acceptance case.
