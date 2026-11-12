# Autorig accepted-graph source handoff, 2026-10-29

Owner: Terra editor/character worker, in an isolated local checkout.
Base: `bdd5746298a6e4a9cd048031719a9d8e63400997` (the recorded governed
candidate). This lease covers only `libs/character/CMakeLists.txt`,
`libs/character/include/carto/character/autorig_graph.hpp`,
`libs/character/src/autorig_graph.cpp`, this document and
`tests/autorig_graph_handoff_tests.cpp`. It does not claim shell, UI,
application, project, test registration, other workers' files or integration.

The adapter checks the current mesh and analysis against the candidate set,
checks the human review receipt against that exact set and candidate, and
requires an explicit one-to-one mapping to durable identities and artist-supplied
local rest transforms and node properties. Candidate parentage maps to those
identities. It returns a validated graph, graph digest and acceptance digest.
Omitted parts and mechanical-port evidence are refused because the current
graph cannot represent them. Reviewer identity is a caller assertion, not
authentication. The resulting graph is prepared in memory only.

The test source covers a real candidate review, mapped parentage, canonical
graph round trip, stale source and analysis, inconsistent receipts, invalid or
incomplete mappings, non-finite transforms, and unrepresented parts. It is
not registered: `tests/CMakeLists.txt` belongs to the consolidation owner.
No compile, tests, native application, GPU, or foreground interaction were
run under the shared execution governance. This source is not a passing or
visual acceptance claim.

Next gate: coordinator review and integration against the then-current exact
candidate, register and run the focused test under the shared build lease,
then wire an atomic journaled project operation that persists the graph and
durable review/source linkage together. The existing generic graph setter
alone does not preserve acceptance provenance. The native workspace must supply
the final properties and exercise save, close, reopen, undo/redo, stale-source
refusal and real visual authoring. Port axes/limits and omitted-part handling
need an explicit durable model before those cases can be accepted. No schema
or existing `.carto` data was changed in this handoff.
