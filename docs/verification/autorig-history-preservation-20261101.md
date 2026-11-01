# Autorig authoring history preservation — source handoff

This continuation addresses a bounded AUT-004 / IK-control history gap on the
isolated autorig branch. It is not native editor, integrated runtime, temporal
trajectory, physical execution or full AUT-004 acceptance.

- Reaccepting a configured rig preserves byte-identical settings as a no-op;
  an altered replacement cannot silently discard them. Ordinary configuration
  cannot remove an authored chain. An explicit human historical-clear action
  requires a saved journal, loss acknowledgment, bounded justification, exact
  project generation/revision and record CAS. Its journal operation binds the
  asserted reason, record identity, prior revision and digest. The prior
  snapshot remains in authoring history; this is not physical rollback.
- A distinct historical view does not claim the source is current. The normal
  current read and switch proposal still require mesh readiness. Transaction-
  only clients have no stale historical-clear mutation; use of the human
  application boundary is required for that recovery path.
- Low-level `ProjectTransaction::accept_autorig` and
  `configure_autorig_ik_fk` now require caller-observed source namespace,
  project revision and record revision. This is a source API change; all
  in-repository callers must migrate. Checks cover an interleaved live-document
  revision change as well as the transaction's original and staged state.
  Configuration must begin in a fresh transaction before other staged edits.
  The transaction remains single-threaded and these tokens do not uniquely
  distinguish cloned/restored branches or authenticate a caller.
- Two independent source reviews examined preservation and historical clear;
  additional interleaved-live guards require integration review on the exact
  reconciled candidate. Tests for reaccept, stale CAS, clear, undo/redo,
  unsaved refusal and journal failure are authored but **not built or run**.
  The prior build-coordinator lease remains active; no competing build,
  integration claim or merge was taken by this continuation.

The user’s temporal-space scope distinguishes authored/rest, evaluated,
intended, predicted, observed and executed states, source/clock/frame lineage
and owner admission. This history path does not authenticate a physical event
or resolve world-space targets to control channels. Skinning weights are not
physical mass or gravity-dependent weight. Where relevant, use existing typed
material/construction/mass, field and simulation receipts under their owners;
do not infer physical state from geometry, labels or curve samples. Full
multi-chain arbitration, source-bound timeline/restore lineage and native,
independent-asset and runtime acceptance remain open.
