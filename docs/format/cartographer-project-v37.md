# Cartographer project schema v37

Schema v37 retains the revisioned `ACCEPTED_AUTORIGS` table introduced in
[v36](cartographer-project-v36.md) and permits a version-two nested accepted
autorig bundle containing explicit IK/FK authoring records. The v37 reader
accepts canonical v1 and v2 bundles; v1 contains no IK/FK records. Older
projects through v36 remain readable and are written as v37 on a later save.
The v37 reader refuses a v2 nested bundle labeled as an older outer schema.
Readers limited to v36 must refuse the newer outer schema; a v1-only bundle
reader also refuses v2. Preserve backups when exchanging projects with older
software.

A nonempty configuration serializes the nested bundle as v2. It contains one
to 128 chain records, sorted by root, mid and effector joint IDs without
duplicate chains. An empty configuration serializes as v1. Each canonical
record is bounded to 4 KiB and binds the exact rig, graph and joint-limit
digests. It declares a contiguous two-bone chain, distinct FK joint controls,
an IK target control, an optional pole control, a mode control and axis with
FK/IK values, a deterministic solver budget, and a maximum positional jump.
Validation resolves declared joints and controls, checks FK control coverage,
and checks the mode axis and values against the control definition. These are
explicit authored roles; they are not inferred from shape or control labels.
Different chains cannot share controls until a control arbitration contract is
defined. Chains may still share joints, so the set is not evidence of
conflict-free concurrent switching. The common rig, graph and limits are validated and hashed once per
collection. A conservative 64 MiB bound on their serialized sizes multiplied
by chain count is a byte proxy, not a strict CPU-operation bound; configurations
outside it are refused rather than partially admitted.

The human application boundary stages accepted autorig and IK/FK
configuration through revision-checked commands and project history. The
project transaction API also supports compare-and-swap configuration of a
current bundle. A proposal computes matching values for one declared chain;
it does not apply control values, switch modes, blend or key animation, or
arbitrate simultaneous chains. It currently requires the default coordinate
policy and refuses posed ancestors outside the chain. It bounds positional
jump but reports rather than enforces effector orientation continuity.

Cold decoding validates historical internal consistency. The application
read path checks current mesh readiness; other consumers must call
`validate_accepted_autorig_current` before using a decoded bundle. External
source content digest and input binding provenance remain caller assertions,
and reviewer identity is not authenticated. The stored layer is not
automatically marked stale when the source changes. Sparse correspondence
leaves unlisted target vertices rigid; the bundle and whole-project journal
size limits still apply.

This describes the in-progress source change, not a native editor, independent
asset, external-runtime or full character-production acceptance claim. In
particular, the mapping from world-space target/pole values to actual control
channels, multi-chain conflict arbitration, and mode blending, keying and
state ownership remain architectural decisions.
