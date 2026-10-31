# Connection edge allocation history

## Finding and repair

`ConnectionPortGraph::erase_edge` retains the next edge identity in memory,
but the version-1 decoder reconstructed that counter from surviving edges.
Deleting the greatest edge, saving, and reopening could therefore allocate a
previously admitted identity again. The new delete/reopen regression failed
with `reopening after deletion must not reuse an admitted edge identity`
before the codec repair. This broke durable identity lineage.

The smallest repair persists `NEXT_EDGE_ID` in nested graph version 2 whenever
the retained counter differs from the value reconstructable from live edges.
Graphs with reconstructable counters keep their exact version-1 layout.
Readers accept both versions; version 2 requires a strict whole-token unsigned
counter. Nonzero counters must exceed all live identities. Zero means the
identity space is exhausted, including after the maximum edge is removed.
Classic numeric locale fixes framing independently of process locale.

This changes neither the outer project schema nor the application admission
path. The surrounding in-progress prefab work already adds project schema 34.
The graph remains nested in that project's existing graph record.

## Files and boundaries

- `libs/connection/include/carto/connection/graph.hpp`: maximum codec version.
- `libs/connection/src/graph.cpp`: retained counter validation, versioned
  encoding/decoding, and locale-stable framing. Explicit removal methods were
  already being implemented in the shared workspace and were preserved.
- `tests/connection_port_graph_tests.cpp`: delete/reopen, lower-edge deletion,
  maximum-ID deletion/exhaustion, overlapping and malformed counters, missing
  state, downgrade refusal, and locale coverage alongside existing happy paths.
- `tests/condition_prefab_placement_tests.cpp`: project reopen preserves the
  retired edge; removal through `UiController` refuses AI proposals and failed
  journal writes, emits exactly one accepted application-action record with
  retained counter data, and preserves undo/redo journal behavior.
- `docs/format/cartographer-project-v34.md`: nested codec compatibility.

Accepted mutations still enter through the human application action, candidate
project replacement, command history, and approved application journal.
Preview/replay tests do not publish their reconstructed graph. A failed append
must preserve exact project bytes, undo count, and journal length. Existing
project generation/revision/digest binding and owner existence validation are
unchanged. No background or AI publication path was added.

## Validation and limits

The pre-repair regression reproduced the bug. The rebuilt graph and prefab
placement suites passed within the 12/12 selected CTest run (17.75 s). Final
application/UI executable verification passed 2/2 (22.55 s). The native desktop
and public API target also built; the public API test passed. The current
tree/index and selected-`HEAD` publication audit, including ignore policy,
returned `PUBLIC_BOUNDARY_PASS`. Scoped diff checks passed.
No native pointer/display test,
runtime attachment behavior, GPU result, or full-game acceptance is inferred.
Cross-org roles, justifications, and datetimes are not graph-kernel concepts;
the enclosing application authority and prefab rationale tests remain in scope.

Risks: older graph-version-1 readers must refuse newly written retained-history
records. Old files that already lost retired IDs cannot reconstruct them;
this repair does not invent historical evidence. Undo intentionally restores
the recorded project snapshot, including its allocation state. The low-level
graph replacement API remains an explicit replacement boundary, rather than
an append-only global identity service. Large-graph validation performance and
port identity retirement are separate work.

Rollback: revert only the codec/counter/locale hunks and their new regression
cases, preserving other shared-workspace removal and prefab implementation.
Keep backups of projects containing nested version-2 graphs. Do not strip
the counter or silently downgrade it to make an older reader accept a file.

Rejected: deriving history from live edges, wrapping the identity counter,
accepting signed or partial numeric tokens, silently dropping history for old
readers, changing the outer project schema again, and unrelated graph rewrites.

RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA were not edited. No shared
API/schema contract to those repositories was identified locally. Direct
impact is Cartographer save/reopen and graph lineage; indirect consumers must
update their nested-codec reader. Cross-product suites and deployment are not
run because they are outside this checkout and no deployment is requested.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: retained connection identity lineage under deletion and reopen.
Promotion progress: local regression acceptance only; native/runtime/consumer
and broader release gates remain separate.
