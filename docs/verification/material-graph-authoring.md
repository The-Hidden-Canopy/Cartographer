# Native material graph authoring

The Material instrument can create a four-output starter graph from an
existing project's PBR material factors, add constant, parameter, surface-input,
arithmetic/mix, and texture-sample nodes, edit constant, parameter, texture
source, and UV-set values, change typed input links, and route compatible nodes
to material outputs. The source graph remains
in the Cartographer-owned material catalog.
Graph edits pass through a human-only application action and the existing
project material-catalog command, so journal, undo, redo, and `.carto`
save/reopen use the same durable boundary as other material edits.

An update must match the current project revision, source graph revision, and
canonical digest. The replacement graph must validate and advance its source
revision exactly once. A new graph starts at revision one. Stale requests,
missing graph identities, revision jumps, invalid graphs, and malformed
revision/digest pairs fail without publishing a project mutation.

Graph nodes receive monotonically increasing IDs within the current graph.
Removing a supported node is allowed only after it is disconnected from all
consumers and outputs; the removed ID is stored in a bounded, sorted tombstone
set. Updates must preserve every prior tombstone and record each removal
exactly once, so a deleted ID cannot be reused. Unsupported nodes are retained
until their payload format is understood.
Input links and output assignments are type-filtered, and each proposed
topology change is structurally validated before replacing the local draft.
Cycles, incompatible types, and broken required outputs are rejected without
mutating the draft. Texture-sample nodes and unsupported nodes already in a
graph are preserved. Texture-sample nodes can now select only texture assets
admitted in the current project catalog; their U/V links remain typed scalar
links and their UV-set ID must be nonzero. Unsupported payload edits remain
unavailable. Structural support diagnostics are not a shader compiler result:
no DXIL, GPU program, rendered material, or live viewport shading is produced
by this workflow.

Materials can be associated with a graph source through a separate human-only
command. The association pins graph identity, source revision, and canonical
digest in the material catalog. If the graph is edited later, the old
association remains intact and is projected as stale; only an explicit rebind
can adopt the new revision. Binding and unbinding advance the material source
revision and use journal, undo/redo, and `.carto` persistence. This records
authoring intent. The saved-scene D3D12 preview also evaluates a bounded subset
of current graph bindings, as described below; the CPU value action remains a
separate read-only operation.

## CPU reference evaluation

`evaluate_material_graph` evaluates the output-reachable subset of constants,
typed parameters, explicit surface inputs, add, multiply, and mix nodes. It binds
the result to the canonical graph digest and a deterministic digest of all
provided inputs. Missing, mistyped, or output-unreachable values fail; no
parameter or surface fallback is invented and extraneous values are not silently
ignored. Reachable texture-sample and unknown nodes fail with an explicit
unsupported diagnostic. Unreachable unknown nodes remain retained in the source
graph and are not part of the output closure. Receipt validation can replay
against the supplied input map and rejects changed inputs or altered output
values instead of trusting the receipt's node-value payload.

The CPU evaluator itself is a value reference only. It does not decode/sample
texture assets, lower shader code, create a renderer material, mutate the
material catalog, or render a viewport preview. The desktop panel has an explicit
`Evaluate CPU values` action for the current draft. It displays assigned PBR
outputs when available. Reachable parameter and surface inputs have typed
controls; a current binding's saved values load into them, and absent values
remain absent until the user explicitly supplies them. The user can save the
current values through a separate human-only action. Any input change, draft
edit, or source reload clears the displayed receipt. Graphs with unsupplied
inputs show a refusal message rather than inventing values.
The preview is labeled as not rendered. `tests/material_graph_tests.cpp` now
authors checks for arithmetic, deterministic input/evaluation receipts, missing
and mistyped inputs, extraneous parameter/surface values, stale graph revisions,
output closure, reachable texture refusal, non-finite arithmetic, changed
same-revision graph content, changed input sets, and tampered output values.
The tests were not run under the active no-test instruction. The desktop wiring
was not built or exercised in the native app during this sprint.

Verification:

- `cartographer_material_graph_tests` checks typed graph validation,
  deterministic serialization, v1-to-v2 migration, tombstone validation and
  persistence, stale-source checks, and retained unsupported nodes.
- `cartographer_application_tests` checks graph creation/edit, exact revision
  advancement, stale project/digest rejection, invalid revision rejection,
  typed topology/output changes, cycle rejection without project mutation,
  refusal of untracked removals and identity reuse, pinned graph bindings,
  stale projection after graph edits, explicit rebind/unbind, unsupported-node
  preservation, journal receipts, undo/redo, and save/reopen.
- `cartographer_ui_tests` checks native snapshot projection, graph-source read,
  human graph factor/topology edits, revision-pinned binding, operation
  receipts, rejection of raw AI mutation, and project-catalog admission and
  save/reopen of texture-sample source/UV metadata. The new persistent-input
  human/AI-boundary cases are authored but not run under the active no-test
  instruction.
- `cartographer_d3d12_scene_renderer_tests` has source cases for transient
  material-graph inputs, exact graph revision/digest binding, request identity,
  missing-input refusal, and foreign/stale input sources. These additions were
  not run under the active no-test instruction.
- `cartographer_scene_render_cli_tests` has source cases for explicit typed
  parameter and surface inputs, receipt source/input digests, changed-input
  request identity, missing/unreachable or mistyped values, duplicate options,
  and foreign graph refusal. It was not run under the active no-test instruction.

Remaining work includes texture-capable native shader lowering, live rendered
Vulkan viewport evaluation, and CONDITION production-board acceptance. The
saved-project D3D12 adapter has a code-only bridge for current CPU-evaluable
graphs with PBR-supported outputs. The desktop passes a transient input set
only after its CPU evaluation receipt validates against the exact saved graph
revision and digest. The scene-render CLI also accepts repeated explicit typed
parameter/surface arguments and binds them to the loaded graph revision and
canonical digest. Both paths fold the input digest into request/material
identity; the desktop clears old captures when inputs change. Per-capture input
maps remain transient; durable values are described below and have not been
built or run. Aggregate D3D12 input validation is capped at 262,144 values
across graph bindings, matching the saved-scene graph-node evaluation budget.
Aggregate input validation is capped at 262,144 values across all graph
bindings, matching the saved-scene graph-node evaluation budget.
The Material instrument also creates texture-sample nodes from admitted source
records and edits their source and UV-set fields. Those nodes remain explicitly
unsupported by the CPU evaluator and saved-scene renderer.
Missing, mistyped, output-unreachable, foreign/stale, or unused graph inputs,
texture samples, graph-plus-variant combinations, and unrepresentable outputs
fail closed. These regressions have not been built or run in this sprint, so
this is not GPU or live viewport acceptance evidence. The live Vulkan material
viewport does not evaluate the graph.

## Subsequent executed CPU verification (2026-10-25)

The later coding continuation repaired missing test type imports for
`MaterialGraphEvaluationInputs`, `MaterialGraphValue`, and
`MaterialSurfaceInput`, then built the material suite and native desktop.
`cartographer_material_graph_tests` passed within the 12/12 focused run
(17.75 s); application/UI suites passed after their final links (2/2, 22.55 s).
This supersedes the earlier unrun status for the bounded CPU reference task.
Arithmetic, typed explicit inputs, output closure, changed source/input
evidence, tampered receipts, unsupported texture sampling, and nonfinite
arithmetic were exercised. The existing source remains unchanged by evaluation.

Files in this continuation: `tests/material_graph_tests.cpp` (test imports)
and this verification/ignored queue update. The evaluator, public declarations,
build registration, and native controls were already authored in the shared
workspace and were preserved. There is no additional production or schema
change from the import repair. Rollback is the three test imports and this
verification continuation; rolling back the CPU feature itself additionally
requires its existing evaluator/header/build/native-control hunks.

Human publication and journal/history boundaries remain unchanged. No fallback
values, shader success, live rendered pixels, or broad material-workflow
completion are inferred. Native pointer/display acceptance, texture/GPU work,
external product suites, and deployment were not run. No shared contract with
RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA was identified or changed.
Dataset and training progress are unchanged; capability progress is tested CPU
reference evaluation; promotion remains local to this bounded value contract.

## Durable material graph inputs (2026-10-26; code-only)

Material bindings now persist typed parameter and surface values in material
catalog schema v4; v1-v3 remain readable and migrate bindings with empty input
sets. The human-only `SetMaterialGraphInputsAction` pins project revision,
material revision, graph identity, graph revision, and canonical graph digest.
It validates every supplied key against the graph's reachable input schema,
advances the material revision, and commits through the existing catalog
journal path. Partial value sets are allowed as authored data, while CPU/D3D12
evaluation still refuses any required value that is absent. Stale binding
values remain preserved during source edits. Rebinding the same graph identity
adopts its new source pin while retaining those values; the renderer checks
their keys and types against the current graph and refuses any mismatch until
the inputs are corrected or cleared. Binding a different graph resets the
prior graph's values rather than carrying them across by matching key names.

The saved-scene D3D12 adapter uses durable binding values by default. An
explicit per-capture input map replaces that complete set for the request; it
does not merge individual keys. The request contributes its exact input digest
to material/request identity. The desktop
loads the selected material's saved values, lets the user edit them, and commits
the typed values back to that binding. The live Vulkan material viewport and
texture-sample shader lowering remain unsupported.

- **Files changed:** material graph and catalog sources/headers, application
  action/snapshot wiring, agent project-diff tracking, desktop controls,
  D3D12 saved-scene adapter, material/application/UI/agent/D3D12 regression
  sources, and verification ledgers.
- **Capability gained:** typed graph inputs can be saved with a material and
  reused by the offline saved-scene D3D12 adapter.
- **Boundary improved:** stale revisions/digests, unreachable keys, type
  mismatches, missing evaluation values, and cross-material UI reuse fail
  closed; request overrides do not mutate saved inputs.
- **Governance preserved:** updates require the human action boundary, exact
  source evidence, material revision advancement, and the existing journaled
  material-catalog commit. No AI, texture-sampling, or Vulkan authority was
  added.
- **Tests added:** catalog v4 round trip and v3 migration, invalid subset
  refusal, application no-mutation refusal and journaled happy path, UI AI
  rejection/human acceptance, project-diff visibility, and durable renderer
  default/explicit override cases. **Run:** none, at the user's request.
- **Risks:** code and serialization have not been compiled or exercised; old
  binary readers will not understand catalog v4 and must be upgraded before
  opening newly saved projects. GPU pixels, pointer flow, and production
  acceptance remain unverified.
- **Rollback:** revert the schema-v4 input fields/actions, snapshot/diff and
  renderer integration, associated UI/tests/docs; existing schema-v1-v3
  readers remain unchanged.
- **Rejected:** fabricating missing inputs, merging request overrides into
  durable values, auto-rebinding edited graphs, and claiming Vulkan or texture
  sampling support.
- **Cross-product trace:** direct impact is Cartographer's material catalog,
  desktop, agent read-only diff, and D3D12 offline adapter. No RegOS,
  IDA_Training, hidden_canopy_bot, or Ask_IDA contract or data changed.
  Dataset/training progress is unchanged; capability progress is code-only;
  deployment and promotion remain open. Follow-up: build and run catalog,
  application, UI, agent-diff, and D3D12 suites, then review a saved-value
  D3D12 capture and desktop input interaction.
