# Connection graph admission parity

## Findings and minimal repair

The graph's readback/replacement validation checked the structure of an edge's
compatibility receipt without recomputing its endpoint contract. A validly
shaped receipt could claim requirements that were never satisfied, omit shared
tags, or survive changed endpoint requirements. An edge kind could also lack
its required namespace. This admitted fabricated evidence through a governed
project replacement despite the stronger live `connect` gate.

The new baseline regression reproduced those five admissions, a two-edge cycle
using reversed endpoints and different edge kinds, and numeric edge kinds 256
and 260 narrowing to supported values. A second boundary checks acceptance-tier
numbers before narrowing; tier labels must not be normalized into valid claims.
Its baseline reproduced eight further invalid receipts: 256–259 and 512–515
wrapped to structural, reference, native, and full-game tier values.

`ConnectionPortGraph::validate` now recomputes endpoint compatibility, checks the
same kind namespace as `connect`, and requires exact shared tags and satisfied
requirements in endpoint order. A sorted endpoint-pair set rejects parallel
attachments before adjacency deduplication can hide the two-edge cycle. This
replaces the former quadratic duplicate scan with indexed duplicate detection.
Existing longer-cycle detection remains in place. The graph decoder checks
kind bounds before its enum conversion.
The acceptance-receipt decoder likewise checks its maximum supported tier
before conversion. Receipt schemas and valid serialized bytes are unchanged.

## Files, capability, and invariants

- `libs/connection/src/graph.cpp`: graph admission parity, endpoint-pair check,
  and strict decoded enum bounds. Live and readback namespace checks share the
  same helper.
- `tests/connection_port_graph_tests.cpp`: malformed/stale evidence, unsatisfied
  requirements, wrong namespace, two-edge cycles, enum narrowing, asymmetric
  valid requirements, exact-byte readback, and acceptance-tier boundaries.
- `tests/condition_prefab_placement_tests.cpp`: a fabricated graph replacement
  must preserve exact project bytes, undo/redo counts, and journal length; a
  serialized project carrying that forged graph must refuse readback.
- `libs/core/include/carto/core/math.hpp`: the existing viewport, UV alignment,
  and desktop picking/stroke paths called missing `Vec2d` arithmetic. The shared
  type now supplies addition, subtraction, scalar multiplication, dot product,
  and squared length using the same component semantics as `Vec3d`. The first
  local arithmetic repair was replaced by this shared API after the desktop
  build exposed additional consumers. No object layout or mutation authority
  changes. `tests/test_main.cpp` checks constexpr projection arithmetic,
  nonfinite evidence, overflow, and continued 3D overload behavior;
  `tests/viewport_interaction_tests.cpp` covers perpendicular motion, stale
  revision, and degenerate/nonfinite projected axes.
- `libs/ui/src/ui.cpp`: the merge review formatter now passes an object's
  `local_transform` to the transform formatter. The former callback failed to
  compile. Existing Agent Workbench tests assert exact before/after transform
  review strings and no mutation during comparison.
- `apps/desktop/main.cpp`: lifts the existing camera-pinned reference-image
  resolver into class scope so both viewport drawing and the presentation
  panel can use it. The former panel call could not compile because the
  resolver was local to the viewport function. Its existing camera identity,
  current scene transform, finite-state, and legacy placement checks are kept.
- `tests/application_tests.cpp`: adds actual alignment admission, stale
  endpoint/generation/selection/boundary refusal, AI refusal, journal failure
  rollback, human success, undo/redo event counts, and reopen coverage to the
  existing two-face UV fixture. See [UV seam verification](uv-seam-authoring.md).
  Compile-only fixture repairs unwrap asserted optional mesh revisions, use
  the real mesh snapshot digest for reopen equality, and qualify the existing
  packing test's application types/human-source test seam. Production
  authority remains unchanged.
- `tests/agent_workbench_tests.cpp`: saved/undone/redone projects now use
  separate load results, because `ProjectDocument` intentionally has no public
  assignment operator. All coordinate/history assertions remain. A misplaced
  weld-provenance block in `tests/application_tests.cpp` was moved into its
  existing UV attribute fixture, preserving its assertions and using the
  bounded corner API with exact fixture counts.
- `tests/ui_tests.cpp`: the sculpt surface-stroke fixture now captures its
  actual selected project generation in the scope where the generation-bound
  actions use it. The misplaced unused capture was removed from another
  fixture; stale-generation refusal assertions are retained.
  Its visibility test asserts the documented source-only viewport rather than
  claiming unimplemented public brush evaluation.

The broader executed run also exposed older fixture assumptions: the weld
refusal case now supplies genuinely conflicting edge values (equal values are
supported), and the presentation diff fixture continues its existing journal
tail when copying the base document instead of pairing a revisioned document
with an empty journal. These changes preserve refusal and lineage coverage.
The source-diff material fixture explicitly registers and retains the sampler
referenced by its texture binding. A sample-only grid merge uses a supported
goal; the compiler's refusal of unsupported restriction language remains
unchanged, and the test still proves that the live source revision is unchanged.
Graph-diff fixtures edit the requested stable node identity and supply every
required typed output, rather than relying on vector ordering or invalid
partial material graphs. The catalog/source validation rules are unchanged.
This ledger and the ignored local queue record evidence and scope.

Capability gained: persisted/replaced graphs cannot claim connections that the
authored endpoint rules would refuse. Acceptance tiers remain separate claims.

Publication still enters through the human application action, revision and
graph-digest checks, candidate project, `ReplaceProjectDocumentCommand`, and
approved application-action journal. No new mutation authority, fallback data,
background publication, migration, or schema version is introduced. Invalid
inputs fail instead of rewriting their evidence. Existing graph deletion,
allocation history, undo/redo, and save/reopen paths remain supported.

## Validation

Baseline: `cartographer_connection_port_graph_tests` failed twice, exposing
eight graph admissions and eight acceptance-tier aliases before their repairs.
The rebuilt graph suite passed, as did prefab placement, port compatibility,
socket resolution, core math/commands, viewport interaction, camera math, UV
material/packing, and public UI suites. The full application suite passed
(1/1, 9.41 s total) after fixture repairs. Placement coverage exercises AI
refusal, failed journal append atomicity, human success, retired edge history,
and undo/redo event counts. The desktop linked; scoped diff checks and the
current-tree/index plus selected-`HEAD` audit with ignore-policy checking
passed (`PUBLIC_BOUNDARY_PASS`). The final complete Agent Workbench suite also
passed (1/1, 5.86 s total), including exact transform summaries, bounded mesh
and material graph samples, and governed merge review.

Across the 13 distinct selected suites, 12 passed and the reference-image
decoder suite failed its malformed PNG assertion. The passing suites are
`cartographer_tests`, `cartographer_application_tests`, `cartographer_ui_tests`,
`cartographer_agent_workbench_tests`, `cartographer_condition_prefab_placement_tests`,
`cartographer_connection_port_graph_tests`, `cartographer_connection_port_tests`,
`cartographer_prefab_socket_tests`, `cartographer_viewport_interaction_tests`,
`cartographer_editor_camera_tests`, `cartographer_uv_material_contract_tests`,
and `cartographer_uv_island_pack_tests`. Runs were focused and incremental;
this is not a full configured CTest pass. Native pointer/display acceptance,
runtime attachments, external product suites, deployment, and broader public
refs were not run because they are outside this bounded local acceptance.

Org/role tiers and datetimes are not graph-kernel concepts. Foreign/missing
owners are covered at the enclosing project/socket boundary. Prefab rationale
remains an enclosing rebase requirement; this repair adds no new justification
policy. Native/runtime attachment acceptance is separate.

## Risks, rollback, and rejected options

Previously accepted files with stale/fabricated receipts or parallel attachment
edges will now refuse. Preserve the original bytes for explicit review; the
loader does not silently repair evidence. Compatibility replay adds bounded
per-edge work; no runtime performance measurement or native display acceptance
is inferred from unit tests. Valid graph bytes and the nested codec remain
unchanged.

Rollback: selectively revert this continuation's validation/helper/enum hunks
and new regressions. Preserve the earlier edge-history and removal work in the
same files. The shared vector API and merge callback are separate compile
integration repairs; revert them only with callers requiring those APIs or
restore the callers' compatible implementation. No existing project is
rewritten by this repair. Consumers must rebuild for the new inline vector API.

Rejected: trusting structurally shaped evidence, silently regenerating receipts,
allowing different edge kinds to hide a two-edge cycle, narrowing unchecked
numbers, broad graph refactoring, and treating local validation as runtime or
release acceptance.

Separate follow-up (2026-10-26, code-only): the Windows reference-image
decoder now checks the PNG zlib header and refuses a reserved type in the first
DEFLATE block before WIC decode. This is source-level hardening only; the
existing adversarial decoder fixture was not run, later-block validation still
depends on WIC, and strict full-stream admission is not claimed. See
`presentation-authoring.md`.

Subsequent executed goal continuation: the expanded decoder suite passes
(1/1, 0.34 s total), including the reserved-first-block case and a new full
container ordering guard after the prefix is complete. Complete compressed
stream/JPEG and native visual acceptance remain open. See
[reference-image admission](reference-image-admission.md).

RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA are not edited. Direct impact
is Cartographer graph/project readback and human graph replacement; indirect
consumers may encounter explicit refusal of invalid historical records. No
shared API/schema to those repositories was identified here. Their tests,
datasets/evals, and deployment remain outside this checkout and are not run.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: adversarial graph admission parity. Promotion progress: local
regression evidence only; native, runtime, consumer, and release gates remain.
