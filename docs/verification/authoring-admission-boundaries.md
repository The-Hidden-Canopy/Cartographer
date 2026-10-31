# Authoring admission boundary repairs

This ledger covers four bounded Cartographer repair slices. Other existing
workspace edits, including CONDITION schema, asset-lease, and frontend work,
are outside this ledger and were preserved.

## Findings, files, and proof

| Finding and product law | Minimal repair and files | Regression evidence |
| --- | --- | --- |
| Unsigned stream extraction allowed signed decal identity/revision tokens to wrap into valid unsigned source bindings. Malformed bytes must not alias authored identity. | `libs/production/src/decal.cpp`, `read_unsigned`: require a complete unsigned decimal token with `from_chars`. Tests: `tests/decal_tests.cpp`. | Twenty-five malformed-field cases cover signs, overflow, and mixed tokens across five integer fields. Maximum unsigned identities and live revision boundaries round-trip exactly. Existing transaction/journal tests remain in the suite. |
| Generated glTF/GLB accepted mesh-level weights/extensions while discarding unsupported semantics. Admitted geometry must not silently lose those semantics. | `libs/io/src/generated_mesh.cpp`, `import_generated_botanical_gltf_impl`: refuse those fields before primitive ingestion. Tests: `tests/generated_gltf_import_tests.cpp`. | Four current-digest fixtures cover weights and extensions in both glTF and GLB. Existing valid imports retain source receipts, material regions, and UVs. |
| Evaluation graph parameter revisions used permissive unsigned extraction; canonical numbers also depended on global locale. Source identity and digest must survive a locale change without reinterpretation. | `libs/eval/src/graph.cpp`, `EvaluationGraph::deserialize`, `serialize`, and `canonical_graph`: use the existing strict integer reader and classic stream locale. Tests: `tests/evaluation_attributes_tests.cpp`. | Signed, overflowing, and mixed revision tokens refuse. Large valid revisions round-trip. A custom digit-grouping locale preserves identical canonical bytes, digest, and reopened graph. |
| Evaluation graph input ownership validation compared every connection against every other connection despite allowing large serialized graphs. Hostile input must not force an avoidable quadratic validation scan. | `libs/eval/src/graph.cpp`, `EvaluationGraph::validate`: index occupied `(NodeId, PortId)` inputs while retaining duplicate-connection checks. Tests: `tests/evaluation_attributes_tests.cpp`. | A graph with 4,096 legal fan-out connections admits; a final competing writer to an occupied input refuses. This is correctness coverage, not a measured performance claim. |
| Agent geometry/topology steps could mutate a locked sibling through an unlocked object sharing its mesh. Existing lock and work-envelope restrictions must cover every affected alias. | `libs/agent/src/workbench.cpp`, `AgentTask::execute_step`: remove the material/UV-only exemption from the existing alias lock check. Tests: `tests/agent_workbench_tests.cpp`. | Face and vertex operations refuse before speculative mutation when a sibling is locked. Unlocked siblings retain human acceptance, one journal append, and undo/redo. |

## Verification run

The four targets built successfully in `build/character-family-debug`:

```text
cmake --build build/character-family-debug --parallel 4 --target cartographer_evaluation_attributes_tests cartographer_decal_tests cartographer_generated_gltf_import_tests cartographer_agent_workbench_tests
```

| CTest suite | Result | Test time |
| --- | --- | --- |
| `cartographer_evaluation_attributes_tests` | Passed | 0.79 s |
| `cartographer_generated_gltf_import_tests` | Passed | 0.97 s |
| `cartographer_decal_tests` | Passed | 0.24 s |
| `cartographer_agent_workbench_tests` | Passed | 16.06 s |

Scoped `git diff --check` passed. Git warned about future LF-to-CRLF conversion
in the graph source/test. The build emitted an existing local-variable shadow
warning in a separate material-graph test fixture.

The current-tree/index and selected-`HEAD` publication-boundary audit returned
`PUBLIC_BOUNDARY_PASS`, including ignore-policy checks. This does not clear
other branches, retained PR references, forks, or all-history release gates.
The installed toolchain required approved execution outside the sandbox;
audit scratch files were confined to the workspace build directory.

## Governance and capability delta

- State transition path: parsing and validation refuse before authoring;
  admitted agent edits retain the existing speculative command and human
  acceptance path.
- Audit/domain event path: the agent regression proves preview/refusal adds
  no live journal entry and accepted work adds one entry. Existing decal
  transaction/journal coverage remains active.
- Permission and scope path: work-envelope, protected-object, alias, and
  touch-budget checks remain in place; every affected mesh alias is checked
  for its persisted lock state.
- Human approval boundary: these changes add no automatic or AI acceptance.
- Capability gained: malformed source identities, unsupported mesh semantics,
  locale drift, and locked-alias edits now fail at their admission boundary.

## Risks, rollback, and rejected options

Previously accepted malformed signed records and lossy generated meshes now
return validation errors. Geometry operations involving a locked shared alias
now refuse intentionally. A graph containing multiple defects may report an
input conflict before a later malformed connection; both paths refuse.

Rollback consists of reverting only the listed production hunks and their new
regression functions/invocations. In particular, preserve other edits in the
shared agent source/test files. No migration, dependency, or serialized schema
change is required for these repair hunks.

Silent semantic loss, permissive numeric coercion, mutation through a locked
alias, and unrelated refactoring were intentionally rejected.

Direct impact is confined to Cartographer. RegOS, IDA_Training,
hidden_canopy_bot, and Ask_IDA files/contracts were not changed; their suites
were not run. Public C++ signatures and serialized schemas are unchanged by
these repairs. No full-suite, native viewport, deployment, or consumer-runtime
acceptance is claimed. Broader intake, decal, procedural-graph, and agent
workspace tasks remain open for their remaining workflow gates.

Dataset progress: unchanged. Training progress: unchanged. Capability progress:
the named admission boundaries have local regression evidence. Promotion
progress: no broader workflow or deployment promotion is claimed.
