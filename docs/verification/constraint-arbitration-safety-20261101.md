# Constraint arbitration safety — source handoff

This is a bounded correctness repair to the existing CPU constraint pipeline,
not a complete multi-chain solver or runtime acceptance. It preserves the
documented stage/dependency/priority model and does not infer physical
hardness, observed contact, material behavior or runtime execution.

- Equal-priority ready constraints now use ascending stable ID as documented;
  per-target groups preserve dependency-aware order. Inactive, superseded,
  later-stage or interleaved prerequisites that cannot be honored refuse.
- Groups with conflicting pairwise arbitration declarations refuse instead of
  silently choosing one. Blend arbitration refuses until its final-pose
  residual and group semantics can be validated; this deliberately changes the
  previous CPU behavior and does not remove the broader product requirement.
- Active referents are checked before arbitration, including those that would
  be superseded. Missing joints and unresolved control references refuse.
- Final IK-pin, contact and orientation residuals are measured against the
  returned world pose; a prior solver failure is not promoted. Superseded
  contacts remain unverified. `all_converged` is not a full admission decision:
  callers must inspect limit violations and the remaining evidence separately.
- A Jacobian chain already satisfying its target now returns rest locals for
  absent sparse overrides rather than throwing. Orientation comparison uses
  the shortest quaternion hemisphere and rejects an unsupported target/source
  dependency that cannot be corrected by this local evaluator.

The source cases in `constraint_arbitration_safety_tests.cpp` and the updated
blend expectation in `pose_tests.cpp` were authored and reviewed but not
compiled or run: the recorded build-coordinator lease remains active. The
integration owner must run both registered suites on the exact reconciled
candidate after obtaining the build slot. Existing full-body, overlapping-
chain, mixed rigid/organic, prioritization, space-switch and hard/soft contact
requirements remain open; refusing ambiguous cases is not their implementation.
Scenario/branch/session/restore lineage, exact time-bound receipts, material
and physical state, native workflows, and VANTA runtime authority remain
separate gates. A numerical residual is modeled CPU output, not observation
or proof of a physical action.
