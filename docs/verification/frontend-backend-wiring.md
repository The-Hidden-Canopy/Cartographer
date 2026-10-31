# Frontend/application wiring repair ledger

Scope: Cartographer, 2026-10-23. The native frontend is a C++ adapter over the
in-process application backend. This change connects existing capabilities;
it does not introduce a server or network API. The working tree already
contained other engineering work. Only the wiring changes described here are
attributed to this task; unrelated dirty hunks must be retained.

## Capability delta

| Surface | Capability gained | Boundary improved |
| --- | --- | --- |
| Modify / command palette | Vertex/edge/face conversion; linked, grow, shrink, invert, edge boundary/loop/ring, and one-start shortest-path selection | Captured project generation/revision, explicit path owner/goal, deterministic backend tie ordering; no document/history/journal mutation for selection |
| Modify | 1–4,096 root-mesh instance copies with explicit finite translation steps | Same eligibility for availability and execution; hidden, locked, parented, nonmesh, stale, nonfinite, and excessive-offset requests refuse atomically |
| UV / Material | Human generation of a unique lightmap UV candidate for one selected mesh, alongside the existing Agent draft path | Current scope/selection and backend recipe validation; shared locked owners remain protected; human recipes use human naming |
| Assets | Choose, verify, review, and confirm an exact-byte moved source | Saved-project scope, registered identity, 128 MiB bound, exact SHA-256, managed path, and commit-time revalidation; changed bytes require reimport |
| Model viewport decals | Admit a placement from the captured live displayed source | Agent before/after scenes, stale caches, same-revision project replacement, wrong identities, and altered displayed geometry cannot become authoritative placements |

Native controls compile. The public UI routes are exercised headlessly; native
pointer, dialog, pixel, DPI, and device acceptance are separate gates.

## Findings and minimal repairs

| Finding / location | Product-law implication | Repair and proof |
| --- | --- | --- |
| Six existing action types lacked shared/native routes: ConvertSelectionAction, ExpandSelectionAction, SelectShortestPathAction, DuplicateMeshInstanceBatchAction, RelinkMovedSourceAction, GenerateUniqueLightmapUvAction | Product capability was unreachable from these frontend controls; this alone was not an unaudited backend mutation | Add bounded UI adapters and native controls; retain existing geometry algorithms, application actions, editor commands, and journal path. New frontend suites exercise real public entry points |
| Native decal hit observations lacked displayed-scene source and project-generation binding | Preview or same-revision replacement evidence could be mistaken for the current live source | Capture the actual RenderInstance and scene source; invalidate cache on preview switches; prepare through exact-source admission; independently guard generation in ApplicationSession::set_decal_placement. UI and application regressions cover refusal and normal human commit |
| ApplicationSession::relink_moved_source rejected restoration to an already registered managed locator | The ordinary native import restoration route failed after preparation had recovered the exact bytes | After verified staged-byte admission, return an application no-change receipt for identical registered digest/path. The regression failed before this repair and now checks exact document/journal/history preservation |
| locate_staged_source accepted a canonical alias inside the project | Equal bytes could publish an unreviewed locator if a reviewed managed blob was replaced by an internal symlink | Require the canonical path to equal the expected content-addressed managed path as well as remain inside the project; keep digest verification. The symlink regression is present but skipped on this Windows host because link creation lacks privilege |

## Files changed

| Files | Purpose |
| --- | --- |
| apps/desktop/main.cpp | Native selection/batch/UV/relink controls; source-bound decal hit cache and preview cancellation |
| libs/ui/include/carto/ui/ui.hpp; libs/ui/src/ui.cpp | Shared scoped routes, selection command IDs, exact-byte preparation, captured decal observation admission, and accepted application receipt projection into the UI rail |
| libs/application/include/carto/application/application.hpp; libs/application/src/application.cpp | Required decal generation guard; verified source restoration no-change receipt and exact managed-path check; truthful human UV recipe naming |
| tests/frontend_selection_wiring_tests.cpp; tests/frontend_asset_wiring_tests.cpp; tests/CMakeLists.txt | Public-entrypoint integration and adversarial regression suites |
| tests/application_tests.cpp; tests/ui_tests.cpp | Same-revision New/Open replay rejection and live/exact displayed-source decal admission |
| docs/architecture/ui.md; docs/capability-matrix.md; docs/roadmap.md; this ledger | Bounded behavior, evidence, and remaining acceptance gates |

The ignored local work queue records task status separately. A pre-task tracked
diff is retained locally at
`build/frontend-backend-wiring-20261023/pre-wiring-tracked.patch`; it is not a
license to revert other workers' later changes.

## Governance preserved

- State transition path: UI input → human application admission → existing
  editor command/project validation. Selection changes remain ephemeral.
- Audit/domain event path: actual document changes retain the application
  journal and command history. Batch generation is one command/event/undo step.
  The UI operation rail projects receipts and is not an audit substitute.
  Verified restoration of already registered bytes does not fabricate a project
  mutation or event. Preparation writes only exact, content-addressed bytes.
- Permission/scope path: application human-source checks remain authoritative;
  wrappers validate current project generation/revision and selection. No new
  role, permission, organization, or authority model is introduced.
- Human approval boundary: source relink separates preparation from explicit
  confirmation. Direct UV generation is an explicit human action. Existing
  Agent draft/review behavior remains separate. Raw nonhuman decal mutation is
  still refused, including when its generation is stale.
- No live data is fabricated. The altered-display decal fixture is explicitly
  synthetic. Public SculptStack authoring remains source geometry; private
  brush evaluation is not restored or claimed.

## Tests

Added frontend suites prove selection nonmutation; deterministic shortest-path
ties; wrong mode/owner/goal; stale revision/generation; unavailable batch sources;
count/offset bounds; one-command batch lineage; UV and source-relink human
receipts, journal behavior, undo/redo, and save/reopen; changed source bytes and
unknown identities; preparation nonpublication; and ordinary native-import
source restoration without registry mutation. Existing application/UI tests
were updated for generation-bound decal admission and same-revision replacement.

The organization/role-tier/justification requirements in the shared RegOS
contract are not fields of these Cartographer actions. This change tests the
real project/session and human-source boundaries instead of adding fictitious
organizations or approval fields. No background mutation or datetime behavior
is added. Existing journal failure/rollback and AI admission suites remain part
of focused verification.

Recorded execution:

- Configured the existing Release desktop build with tests and Vulkan enabled;
  executable output was isolated under
  `build/frontend-backend-wiring-20261023/bin`. No dependencies were fetched.
- Built `cartographer_desktop` and all 16 focused test targets successfully.
  Final desktop/asset-test rebuild passed after the UV availability corrections.
- Final focused CTest: **16/16 passed**, 11.54 seconds, including both new
  frontend suites and application, UI, core, viewport interaction, desktop
  workbench input, AI, preview transaction, storage journal, project source
  registry, source registry, lightmap UV, Agent Workbench, decal, and public API
  suites. These are suite counts, not individual assertion counts.
- The source-symlink subcase explicitly skipped with Windows' required-privilege
  diagnostic. Ordinary relinking and same-locator restoration executed.
- `git diff --check` passed. Git reported existing line-ending warnings in
  unrelated evaluation files; no whitespace errors were found.

The final focused test command was:

```powershell
ctest --test-dir build/desktop-vulkan-release `
  -R '^cartographer_(frontend_selection_wiring|frontend_asset_wiring|application|ui|viewport_interaction|desktop_workbench_input|ai|preview_transaction|storage_journal|project_source_asset_registry|source_asset_registry|lightmap_uv|agent_workbench|decal|public_api)_tests$|^cartographer_tests$' `
  --output-on-failure --parallel 4
```

Failure evidence retained in this decision: the ordinary source-restoration
test failed before the application no-change repair. The first decal fixture
incorrectly assumed private sculpt evaluation, then attempted visibility editing
without the required component selection. Both fixture errors were corrected
to match the public product boundary; production behavior was not weakened to
make them pass. The final UI suite passed. An initial nested CMake/Ninja log
recompaction failed with permission denied; direct CMake configuration succeeded
and the subsequent builds passed without deleting generated logs or escalating.
The existing Agent Workbench compilation emitted an unrelated local-shadow
warning; it was not refactored in this task.

Not run: full CTest matrix; native click-through/screenshots, file-dialog
interaction, DPI/resize/device/GPU acceptance; clean-machine packaging; and
deployment. The RegOS Python, ida_core, and npm commands supplied in the shared
contract do not target this C++ repository. No other repository was entered.
The symlink substitution subcase reports an explicit skip when Windows cannot
create its link; a green suite does not prove that subcase executed.

## Risks and rollback

- SetDecalPlacementAction now requires the caller's captured project generation.
  Callers that omit it fail closed after project replacement; external source
  callers must be rebuilt and updated. No saved project or journal schema
  changes or migrations are required.
- Human lightmap attributes now use `uv.lightmap.*` and `Lightmap` recipe names;
  the Agent path retains `uv.agent.lightmap.*` and `Agent Lightmap`. Consumers
  that incorrectly assumed every generated candidate was Agent-authored may
  need adjustment. Existing UV layers are retained.
- Shared unlocked mesh instances receive the same UV asset change. Locked
  shared owners remain protected. Candidates do not imply bake readiness.
- Decal click admission performs bounded source compilation/projection. Its
  existing geometry/outline caps still apply; native responsiveness is unmeasured.
- Cancelled relink reviews can leave unreferenced immutable blobs. Garbage
  collection and hostile concurrent filesystem replacement beyond admission
  checks are not solved here.
- Rollback code by selectively reverting this ledger's symbols/hunks, test
  targets, and documentation. Do not reset entire shared dirty files. Existing
  document edits are reversible through ordinary undo; preserve original source
  files and project backups. Blob cleanup is a separate reviewed operation.

Rejected: direct document/mesh writes from native callbacks; fake UI success
receipts; changed-byte fallback relinking; treating Agent previews as live;
using project revision alone for decal admission; reimplementing traversal or
UV algorithms in the frontend; private sculpt runtime restoration; schema or
dependency changes; broad UI redesign; cross-repository edits; and deployment.

## Cross-product trace and promotion

| Product | Direct impact | Indirect impact / required follow-up |
| --- | --- | --- |
| Cartographer | Native/shared frontend routes and bounded application admission described above | Rebuild native consumers; exercise actual controls/dialogs; run symlink case on a link-capable host; test packaging/device behavior before native promotion |
| Experimental-Internal-RegOS | None; repository not inspected or changed | No execute_transition, permissions, audit API, or organization schema contract changed here. Any external adapter remains unverified |
| IDA_Training | No dataset, training, or evaluation artifact changed | These regression fixtures are engineering evidence, not a promoted dataset/eval release; any future reuse requires separate review |
| hidden_canopy_bot | None; no bot/API/schema modification | No command/moderation contract change established; any Cartographer adapter requires its own compatibility test |
| Ask_IDA | None; no query/interface modification | No answer provenance or mutation authority expansion; any external integration remains unverified |

Shared contract touched: Cartographer's public C++ application/UI source API and
its receipt projection. No network API, project schema, third-party dependency,
or cross-repository data contract was changed. Deployment is manual and was not
performed.

Dataset progress: none. Training/prompt progress: none. Capability progress:
bounded frontend access and tested refusal behavior. Promotion progress:
headless/native-build evidence only; full native acceptance remains open.
