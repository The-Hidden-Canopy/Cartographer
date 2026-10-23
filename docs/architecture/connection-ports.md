# Connection ports and graphs

`carto_connection` owns durable, neutral authoring records for physical and
logical connection interfaces — hitches, mounts, hinges, moorings, utility
couplings — so a destination can answer "may these two endpoints join, and
under which contract?" from digest-bound evidence instead of convention.
Ports are authoring truth; what is attached right now is not.

## Authority split

- **Cartographer** owns port identity, owner binding, local reference frames,
  compatibility vocabulary, and the typed connection graph.
- **Games/downstream consumer** own live attachment state. `PortAttachmentState` records
  which opaque runtime tokens are bound to which authored port identities at
  the moment of reporting; it is never an input to compatibility, never
  enters authoring truth, and is preserved only selectively by reimport
  reconciliation (`reconcile_attachments_after_reimport`).
- Lowering to native constraint or attachment objects is a downstream runtime
  responsibility; no engine shape, constraint, or handle appears here.

## Records

- `ConnectionPort` — stable `PortId`, durable scene-object owner, optional
  provider-neutral world binding, `ReferenceFrame` (named frame identity,
  optional parent frame, finite transform to parent, stable coordinate
  convention reference — never an inline convention payload), sorted
  compatibility tags, and a `PortCapabilityContract` of required/offered
  capability strings. Contracts are plain data and carry no executable
  semantics.
- `PortCompatibilityTag` — namespaced vocabulary terms; well-known term sets
  exist for `mechanical-hitch`, `equipment`, `door-hinge`, `maritime`, and
  `utility`. Unknown terms remain representable without being promoted to
  vocabulary constants. Two ports join only when they share at least one
  identical tag and each required capability on one side is offered by the
  other.
- `check_port_compatibility` — pure, mutates nothing, and refuses
  incompatible endpoints before any state change with deterministic
  `PortCompatibilityEvidence`.
- `ConnectionPortGraph` — ports plus typed `PortConnectionEdge` records whose
  edge kind constrains the tag namespace the join may use. `connect()` is
  fail-closed: existence, compatibility, namespace, duplicate-edge, cycle,
  and bound checks all run before any state change. The graph is an acyclic
  attachment forest.
- `PortGraphAcceptanceReceipt` — strict, never-promoted acceptance tiers
  (`structural`, `reference_behavior`, `native_evidence`, `full_game`).
  `advance_port_graph_acceptance` moves exactly one tier at a time with fresh
  non-zero evidence; skipping, demoting, re-claiming, and advancing past
  `full_game` all fail closed.

## What is not here

Live attachment tokens as authoring truth, native constraint solving, downstream runtime admission, and any claim that structural graph validation implies
runtime or full-game acceptance.

Verification: `cartographer_connection_port_tests` and
`cartographer_connection_port_graph_tests` cover compatibility gates,
capability contracts, reference frames, fail-closed joins, cycle/duplicate
rejection, tier advancement, and hostile-input rejection. Native-evidence and
full-game tiers are deferred to downstream consumer/game integration.
