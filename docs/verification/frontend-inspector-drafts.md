# Inspector draft repair ledger

Cartographer sprint continuation, 2026-10-24. This repairs the populated
Inspector/Transform workflow without changing the attached studio design or
the application's existing mutation authority. Pre-existing dirty work,
including other workers' newer asset/search and docking tests, is retained.

## Finding and repair

Native transform fields were cached only by object ID; vertex position fields
only by vertex ID. Apply then used the current snapshot revision, or invoked a
tool using the current selection. An old edited value could therefore overwrite
a later viewport edit/undo, or follow vertex 1 into another mesh. Active ImGui
text buffers also needed source identities, not just repeated field labels.

The minimal repair adds presentation-only drafts bound to project generation,
project revision, owner, and optional vertex. Clean fields follow current state.
Edited fields retain their value and become stale after a revision change;
Apply is disabled until the operator reloads. Changing owner or project binds
fresh values. Widget ID scopes include the full source identity/revision so a
focused text buffer does not intentionally follow the next source.

New UiController entry points independently recheck the live session immediately
before dispatch. This catches changes after the frame snapshot was captured,
including same-revision project replacement. Vertex Apply requires its exact
single selected owner and vertex. Transform Apply requires a captured revision
and the displayed selected owner. These checks are not authority-bearing tokens;
they protect native field context within the existing human UI session.

## Files and capability delta

| Files | Change |
| --- | --- |
| apps/desktop/studio_draft.hpp | Scoped draft lifecycle and source-bound widget IDs; no document ownership |
| apps/desktop/main.cpp | Transform/vertex input binding, stale draft disclosure, explicit reload, guarded UI commit, and removal of a duplicate Scene presentation header/ID found in populated inspection |
| libs/ui/include/carto/ui/ui.hpp; libs/ui/src/ui.cpp | Add apply_inspector_transform and apply_inspector_vertex_position; scope and selection preflight before existing dispatch |
| tests/frontend_inspector_wiring_tests.cpp | Populated two-mesh fixtures, draft lifecycle, stale/wrong-owner/mode/nonfinite/AI refusals, exact publication checks and happy authoring history |
| tests/desktop_style_tests.cpp | Actual ImGui field-ID isolation across generation, revision, vertex and full-width owner IDs |
| apps/desktop/CMakeLists.txt; tests/CMakeLists.txt | Register helper and focused public-UI integration suite |
| docs/architecture/ui.md; docs/capability-matrix.md; docs/roadmap.md; this ledger | Bounded behavior, evidence and remaining acceptance |

Capability gained: edits survive as reviewable stale drafts instead of being
silently rebased onto a newer source. Same-numbered vertices in different meshes
cannot inherit the prior numeric draft through the ordinary native cache path.

## Governance preserved

- State transition: native input → scoped UiController preflight → existing
  human application dispatch → existing editor/project command. No direct
  document/mesh mutation is introduced.
- Audit/domain-event path: accepted changes retain normal application journal,
  UI receipt rail, one undo step, undo/redo and persistence. Refused preflight
  does not publish a document change, journal event or UI operation receipt.
- Permissions/scope: no role or organization model is invented. Real boundaries
  are human source, current project generation/revision and exact selection.
  Raw AI actions remain refused. This does not turn a draft into AI approval.
- Human approval: edited stale values remain visible until explicit Reload;
  Apply remains an explicit human action. Unrelated project edits also require
  reloading an edited draft; no automatic rebase is inferred.
- Source truth: no live metrics, geometry, trust scores or fallback values are
  fabricated. No background mutations or datetime semantics are added.

## Verification

The new populated frontend suite initially built and passed 1/1 (1.14 seconds
test time, 2.00 seconds total). It exercises real public UI actions and a real
journal, not a mocked backend. Further source-bound widget and adversarial
checks were then added; final focused results follow.

Final evidence:

- Rebuilt the native desktop and all 20 focused test targets successfully
  (33/33 incremental build steps). The existing unrelated Agent Workbench test
  emitted a local-shadow warning; it was not refactored.
- Focused CTest passed **20/20 suites**, 25.73 seconds: Inspector, selection and
  asset frontend integration; desktop style/chrome/browser/input; application,
  UI, core, viewport interaction, AI, Agent Workbench, preview, journal, source
  registries, lightmap UV, decal and public API suites. Suite count is not an
  assertion count. An existing source-relink symlink subcase explicitly skipped
  because Windows cannot create its link; no new Inspector subcase skipped.
- In a separate native QA process, created two real boxes through Commands,
  selected the first from Scene and inspected both real mesh cards. Entered X=5
  without applying it, then created the second box. The draft retained X=5,
  displayed the source-change explanation, and disabled Apply. Explicit Reload
  restored X=0 and re-enabled Apply. No stale transform was committed.
- Saved that QA project through the ordinary Save dialog into
  `build/frontend-inspector-20261024/native-inspector-qa.carto` with its normal
  journal, verified Saved status, and closed only that QA process. The user's
  pre-existing saved project window remained untouched.
- The populated Inspector exposed two nested Scene presentation headers with
  the same ImGui ID. Removed the redundant outer wrapper, preserving the existing
  presentation editor and its header. Final native rebuild passed 2/2 steps;
  the already-green helper/backend suites were not repeated for this layout-only
  removal. Native click-through of that final cosmetic removal was not repeated.
- `git diff --check` passed. Local logs are retained under
  `build/frontend-inspector-20261024/final-focused-ctest.log`.

Covered boundaries include changed selection with matching vertex IDs, wrong
mode/cardinality, stale revision, replacement projects with identical revision
and IDs, missing captured transform revision, nonfinite positions/transforms,
raw AI refusal, retained stale drafts, explicit reload, and full-width owner
identities. Refusal comparisons include serialized project bytes, journal bytes,
revision, dirty state, undo/redo counts, selection and UI receipt count. Happy
paths prove one journal event, human receipt, UI rail, undo/redo and save/reopen.

Build configuration initially could not find CMake on PATH; executing the
cached installed tool path was denied in restricted execution. Automatic
approval review allowed use of the existing toolchain with outputs confined to
`build/frontend-inspector-20261024/`. No tools were installed or downloaded.

Native observation initially captured an unrelated foreground surface. A fresh
Cartographer window selection and activation recovered the intended application.
That existing window contains a saved project and was not edited or closed.
No existing native project is used as a destructive test fixture.

Tests not run: full current repository matrix, all native authoring dialogs,
all populated native workflows, native vertex-draft click-through,
multi-monitor/DPI/device/packaging acceptance,
or deployment. The earlier full-matrix queue entry is historical evidence,
not a claim that the full matrix was rerun after this repair. RegOS Python,
ida_core and npm commands do not target this C++ repository.

## Risks, rollback, rejected options

The stricter draft policy can require Reload after an unrelated project edit.
Changing selection/project intentionally loads that source's values; local
drafts are not durable or serialized. Future native fields must also use their
source context and live commit preflight. No project/journal schema, network API,
dependency or migration is changed. The public C++ UI API gains additive helpers.

Rollback selectively removes these Inspector hunks/helpers/tests and API entries;
retain earlier frontend wiring, design and unrelated dirty work. A pre-task
tracked diff is retained locally at
`build/frontend-inspector-20261024/pre-inspector-tracked.patch` for lineage.
Code rollback does not change authored documents; accepted test/application
edits use the existing undo/history and saved backup paths.

Rejected: stamping old values with today's revision, caching vertex ID without
its owner, silently rebasing dirty drafts, bypassing the UI/application command
path, inventing extra permissions/organizations, altering backend geometry or
history algorithms, modifying the user's open saved project, and unrelated UI
or dependency refactors.

## Cross-product trace and promotion

Direct impact: Cartographer native fields and shared UI admission. Indirect
impact: these UI callers receive stale-context diagnostics where an old draft
could previously commit; external native consumers may adopt the additive
helpers. Shared contract: immutable application snapshot and existing human
dispatch/receipt model. No saved schema or service API changes.

RegOS, IDA_Training, hidden_canopy_bot and Ask_IDA were not inspected or changed.
No cross-repository adapter compatibility, dataset/evaluation promotion or
deployment result is inferred. Required follow-up: verify any external adapter
against its own contract, expand populated native interaction coverage, and
exercise device/DPI cases before broader native promotion.

Dataset progress: none. Training progress: none. Capability progress: bounded
stale-draft/selection refusal and preserved happy authoring behavior. Promotion
progress: only the recorded engineering evidence; native acceptance is partial.
