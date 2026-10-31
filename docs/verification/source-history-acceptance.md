# Source browser and ordered source history acceptance

This verification run follows the earlier code-only browser, prefab, and
whole-snapshot review continuations. Their earlier unverified notes describe
those earlier runs. This note records the subsequent executed evidence and
the additional history failure found in native acceptance.

## Findings and minimal repairs

| Finding | Files/function | Repair and proof |
| --- | --- | --- |
| Thumbnail reuse by generation/revision could display an older compiled snapshot. | `apps/desktop/studio_browser.hpp`, `MeshThumbnailCacheEntry::projection_for`; `apps/desktop/main.cpp` lookup. | Bind reuse to the current source, clear absent/invalid inputs, and exercise replacement and expired-source regressions. Concurrent hardening also binds bounded geometry bytes and checks same-object mutation and revision mismatch; final verification must cover that current implementation. |
| The existing dependency-plan test inspected the first sorted entry instead of the edited identity. | `tests/source_asset_registry_tests.cpp`, `dependencies_bind_to_the_complete_reviewed_registry`. | Locate the metadata change by exact source identity. Add botanical receipt/version compatibility, annotation-preserving relink, forged metadata refusal, and referenced-target removal tests. |
| Ordered metadata undo advanced the project revision and prevented undo of the preceding import or metadata command. | `libs/editor/include/carto/editor/command_bus.hpp`, `libs/editor/src/command_bus.cpp`, `ImportSourceMeshCommand` and `ApplySourceReimportPlanCommand`. | Initial execution retains the exact captured revision. Replay checks a digest of the complete authored project state, comparing source records independently of their retained generation counters. Actual undo/redo still uses existing source reimport plans, so generations/tombstones advance rather than being restored. |
| Concurrent material-graph work referenced `MaterialGraph` before declaring it. | `libs/project/include/carto/project/material_graph.hpp`. | Add only the forward declaration. The first attempted history-regression build exposed this compile error. The other evaluator changes belong to the concurrent work. |

The source history comparison normalizes only a temporary project copy's
revision and registry, then appends every active source record with a fixed
comparison generation. All other serialized project fields and all source
content participate. It never installs that normalized copy. A different
authored object or source payload still refuses replay without consuming the
history entry. Initial deferred commands remain revision-bound.

## Executed evidence

Before the ordered-history repair, the desktop and fifteen focused suites
built and passed across two CTest runs: thirteen authoring suites in 88.40 s,
and browser plus registry suites in 0.84 s. These covered isolated prefab
placement/model codecs, application/UI, Agent whole-snapshot review, frontend
selection/assets/Inspector, production closure, core commands, desktop input,
and project source registry persistence. The cache absent-source case and
the incorrect sorted-entry assumption first failed, then passed after repair.

Native acceptance used the exact rebuilt executable in
`build/frontend-inspector-20261024/bin`. The initial frame was white; F11
exposed the viewport and a second F11 restored the workspace. This startup
limitation remains evidence against unconditional native readiness.

In a separate QA window:

- Created a box, inspected its geometry thumbnail, and dragged its card into
  the live Model viewport. One mesh then had two scene instances.
- Undo restored one instance; redo restored two.
- Saved a new project under `build/outstanding-local-acceptance/native-browser.carto`.
- Imported an explicitly synthetic tetrahedral OBJ through the native dialog.
  The UI reported four vertices/four triangles and a stored exact-byte source.
- Verified the digest-addressed source blob. External-original currentness
  remained explicitly unknown.
- Drafted `qa, synthetic` tags. Review displayed source generation 1 to 2,
  an exact plan digest, unchanged source/artifact digests, and a separate apply
  action. Review itself left the project revision unchanged. Apply advanced
  the source generation and journaled project history.
- Metadata undo and redo restored the annotation values while generations
  advanced to 3 and 4. Continuing undo into the prior import exposed the
  revision-guard failure described above.
- Saved the resulting QA project, restored the original pane split and closed
  only the QA process. The pre-existing desktop from another checkout stayed
  open. No user project was opened or overwritten.

The new `frontend_replays_import_and_metadata_history_with_retained_source_lineage`
regression reproduced the native-discovered failure before production repair:
CTest failed at `ui.dispatch(application::UndoAction{})`. It exercises two
metadata edits after a real public UI import, three ordered undos and redos,
strictly advancing generations/revisions, stable object/mesh IDs, one journal
entry per mutation, stale-review refusal, and saved-project readback.

`source_history_refuses_contested_content_and_stale_initial_admission` adds
lower-level adversarial checks for import and registry undo/redo: out-of-band
authored changes refuse with byte-identical document/history preservation;
new commands still refuse after their captured initial revision changes.

The desktop and the four directly affected suites then rebuilt. CTest passed
4/4 in 8.31 s: frontend asset wiring, core commands, desktop browser, and
source registry. This includes the concurrent same-object thumbnail mutation
and revision-mismatch tests.

Final focused acceptance passed **15/15 in 61.39 s**, after rebuilding the
related targets and adding a journal-write failure regression. That case
obstructs the journal append, verifies exact document/publication/history
rollback, and then successfully undoes/redoes the prior admitted metadata.
The sixteen selected targets include the native desktop and these fifteen
test executables:

```text
cmake --build build/desktop-vulkan-release --target cartographer_desktop cartographer_frontend_asset_wiring_tests cartographer_frontend_selection_wiring_tests cartographer_frontend_inspector_wiring_tests cartographer_production_closure_tests cartographer_condition_authoring_tests cartographer_condition_prefab_snapshot_tests cartographer_condition_prefab_placement_tests cartographer_agent_workbench_tests cartographer_application_tests cartographer_ui_tests cartographer_desktop_workbench_input_tests cartographer_project_source_asset_registry_tests cartographer_source_asset_registry_tests cartographer_desktop_browser_tests cartographer_tests --parallel 4
ctest --test-dir build/desktop-vulkan-release --output-on-failure -R '^cartographer_(tests|(frontend_asset_wiring|frontend_selection_wiring|frontend_inspector_wiring|production_closure|condition_authoring|condition_prefab_snapshot|condition_prefab_placement|agent_workbench|application|ui|desktop_workbench_input|project_source_asset_registry|source_asset_registry|desktop_browser)_tests)$'
```

Builds were executed in bounded groups as recorded above. Scoped
`git diff --check` passed. The existing Windows source-symlink case explicitly
skips for missing link-creation privilege. Build warnings remain in unrelated
material/rebase initialization and thumbnail float conversion/shadowing code.
SHA-256 fingerprints of the repair inputs are retained in
`build/outstanding-local-acceptance/verified-repair-inputs.json`.
The current-tree/index and selected-`HEAD` public-boundary audit, including
ignore-policy checks, returned `PUBLIC_BOUNDARY_PASS`. Other references,
retained PR heads, forks, and all-history release gates are not cleared.

Not run: a full configured suite, a second native ordered-history session
after repair, native arbitrary-domain merge, native prefab rebase/socket
acceptance, DPI/device/multi-monitor coverage, clean-machine packaging,
consumer/provider deployment, and the other products' suites. These require
their separate fixtures/environments and are not implied by focused CTest.
The original native startup white-frame issue remains open.

## Repair ledger and capability delta

Additional test files are `tests/frontend_asset_wiring_tests.cpp` and
`tests/test_main.cpp`. The earlier browser and registry test files remain part
of this pass. Existing transform, prefab, material-evaluator, and other shared
workspace changes are preserved.

Capability gained: current asset previews and ordered source/metadata history
can be checked together rather than accepting isolated happy paths. The
native-discovered history failure now has a deterministic regression.

State transitions remain in admitted editor commands and existing project
mutators. Journaling remains in `ApplicationSession`; the public regression
checks exactly one new journal entry per accepted mutation. Human review,
exact initial revisions, stale-plan refusal, source identities/digests, and
retained generation lineage remain enforced. No org/role service or AI-to-live
mutation boundary is added.

Risks: history comparison copies and serializes the project synchronously;
large-project replay cost is unmeasured. It tolerates project revision and
retained source-lineage advancement only. Other changed authored content,
including differing mesh revisions, still refuses conservatively. C++ method
signatures and serialized schemas are unchanged by this repair, but private
command object layouts change; binary consumers must rebuild.

Other work is actively changing shared application and authoring files during
this run. These are bounded repair/test results, not a clean, frozen whole-tree
release receipt. Earlier broad suite results are attributed to their actual
run above; later focused results cover the additional repair.

Rollback: revert only this pass's source-history fields/helpers/guard changes,
cache integration, new regression hunks, and compile-only forward declaration.
Preserve the existing transform fix and all concurrent changes in those same
files. No migration or saved-data downgrade is required for these repairs.

Rejected: removing revision checks without a state comparison; restoring old
source generations; ignoring contested records; direct history/document writes;
automatic review acceptance; broad command refactoring; or promoting a whole
workflow from a bounded test result.

RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA were not edited. No shared
contract with those products was identified in these repair hunks; indirect
consumer integration is unverified. Their APIs, schemas, datasets/evals,
deployment, and tests are outside this run. Required follow-up remains broader
native/DPI/device, consumer/provider, clean-machine, performance, and
all-public-reference acceptance. No deployment or remote mutation occurred.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: the named regression boundaries and native workflow evidence.
Promotion progress: local evidence only; broader gates remain open.
