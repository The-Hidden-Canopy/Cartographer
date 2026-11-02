# Cartographer project schema v39

Schema v39 adds a mandatory `CONTROL_WORKSPACE` table with zero or one
digest-bound project-owned character workspace snapshot. A v38 project reads
with no snapshot; a v39 project must contain the table, and an older outer
schema carrying it is refused. The project codec checks the bounded bytes,
record revision, payload digest and an unkeyed envelope checksum; the
application checks the workspace codec and
its current scene, rig, graph and control bindings before using it.

Adoption is an explicit human action that copies a detached workspace into
project history. It does not write, synchronize, or revoke the detached
workspace or its journal. The project copy is the source for this supported
workflow. The checksum detects accidental changes but does not authenticate
an author or establish currentness. Its initial import and each accepted key edit use a single project
snapshot command and the project journal; undo and redo restore that snapshot
as authoring history, not as physical reversal.

The currently supported switch is one three-joint FK-to-IK graph in the default
coordinate policy, from accepted graph rest, with an identity scene root,
isolated graph-world target-point and pole-direction controls, and seven
exact-tick step tracks. The application samples those tracks and solves the
result for CPU preview. It refuses legacy temporal channel or motion-document
payloads and non-classic C++ global locale because their older graph or
temporal digests cannot be silently reinterpreted. No locale migration is
performed. Multi-chain, follow-space, nonidentity root, layered motion,
runtime execution and physical observation remain outside this contract.
