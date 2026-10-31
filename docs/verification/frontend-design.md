# Native frontend design ledger

Scope: Cartographer native Win32/Vulkan frontend, 2026-10-23. This continues the
[frontend/backend wiring](frontend-backend-wiring.md). The user clarified that
the newer model means the frontend design, then directed inspection of the
attachments. Existing backend behavior and all prior dirty work are retained.

## Reference and decision

The design source is the supplied `Cartographer_VANTA_Viewport_Docking_Code.zip`
(two identical local copies), SHA-256
`15f9408308c9937dc96ac7f2541f1eee49ff243bdfb540b51ecf8b9446a7e2f2`.
Its manifest is dated 2026-10-17. The approved reference image,
`reference/cartographer_ui_target.png`, has SHA-256
`31fe6004309b29e1f5227320d39d49db6b9e06a5ab7ae7ede127370f2755c39a`.
Only that image, README, manifest, and design handoff were extracted into
`build/frontend-design-20261023/reference/` for review. No installer, archive
patch script, or external repository code was executed or imported. The current
newer pinned ImGui version is retained.

The reference calls for a dominant viewport, compact gold identity, cyan active
controls on blue-black surfaces, left scene hierarchy, grouped right Inspector,
bottom asset/history tabs, and detachable instruments. The existing dock tree
already implements its structural layout. This change refines the presentation
and repairs the conflicting styling rather than resetting the user's layout.

The landscape, terrain brush, GIS/minimap, weather, renderer, asset thumbnails,
and runtime preview pictured in the mockup are separate feature work. They are
not simulated with decorative controls or fabricated data.

## Capability delta and files

| Files | Change / capability gained |
| --- | --- |
| apps/desktop/studio_style.hpp | One authoritative native adapter for the shared snapshot tokens and density; complete pinned ImGui color-role mapping, reversible geometry and preserved host font/DPI scales |
| apps/desktop/studio_chrome.hpp | Fitted header/work/footer rectangles, responsive workspace navigation, density/font pressure, malformed-input bounds, and focus-mode space recovery |
| apps/desktop/studio_browser.hpp | Read-only stable-ID scene hierarchy and real mesh-instance grouping/search; ancestors retained for matching descendants; malformed identities/hierarchies fail visibly |
| apps/desktop/main.cpp | Responsive studio header, discoverable commands/project navigation/save, compact contextual toolbar, grouped Inspector, truthful filtered scene/assets, bounded Project/Settings navigation, and less intrusive viewport navigation help; existing dispatch routes retained |
| apps/desktop/carto_studio_dock.hpp | Removes the second hardcoded theme override and accepts the actual shared header/footer reservations; retains docking identities and persistence |
| libs/ui/src/ui.cpp; tests/ui_tests.cpp | Dark reference palette uses cyan interaction and retained bronze identity; light/high-contrast themes and existing persistence contract retained |
| apps/desktop/CMakeLists.txt; tests/CMakeLists.txt | Registers native adapter headers and dedicated test targets using existing bundled dependencies |
| tests/desktop_style_tests.cpp; tests/desktop_chrome_tests.cpp; tests/desktop_browser_tests.cpp | Real headless ImGui style/control geometry, layout and read-only browser adversarial evidence |
| docs/architecture/ui.md; docs/capability-matrix.md; docs/roadmap.md; this ledger | Updated bounded design/evidence claims |

The ignored local queue retains the original VANTA task and its open external
acceptance gates. A local pre-design tracked patch is retained at
`build/frontend-design-20261023/pre-design-tracked.patch` for lineage; it does
not supersede another worker's later changes.

## Governance and boundaries

- State transitions: presentation controls continue through UiController and
  existing human application/editor command admission. No document or editable
  mesh is owned by the new design helpers.
- Audit/domain-event path: existing application journal/history and receipt
  rail remain unchanged. Theme, density, filtering, docking, and local window
  geometry are UI state, not fabricated project events.
- Permission/scope: no role, organization, backend API, or permission model is
  added. Existing source/project generation and revision checks remain in place.
- Human approval: Agent proposals/reviews retain their separate workflow;
  visual cyan selection is not mutation approval or a trust score.
- Adversarial evidence: bad browser hierarchy or identifiers cannot disappear
  under a search filter; invalid geometry inputs cannot overflow chrome bounds;
  changing themes cannot retain another palette's native colors; density changes
  affect real controls rather than only preference labels.

## Verification

Recorded build and headless execution:

- CMake configuration passed with existing bundled ImGui, Vulkan and tests;
  executable output is isolated in `build/frontend-design-20261023/bin`.
  No network access, installation, pin changes, or migrations were required.
- Native desktop and all focused test targets built successfully.
- Focused CTest passed **19/19 suites**, 20.82 seconds: the three new desktop
  style/chrome/browser suites and the preceding sixteen frontend wiring,
  application, UI, core, viewport/input, AI/Agent, preview, journal, registry,
  UV, decal, and public-boundary suites. This counts suites, not assertions.
  The source-relink symlink subcase again explicitly skipped because this
  Windows host cannot create the link; that is not new design evidence.
- The style suite exercises all themes, palette authority across all 63 pinned
  ImGui color roles, actual default control heights and click behavior, theme/
  density reversibility, narrow toolbar item rectangles, and invalid width
  handling. Chrome tests cover density/font pressure, malformed input,
  fitted rectangles, focus mode, and actual ImGui window/control geometry.
  Browser tests cover wrong/duplicate IDs, missing/cyclic parents even under an
  unmatched filter, deep iterative hierarchy, ancestor context, bounded search,
  real grouped counts, and unchanged inputs.
- `git diff --check` passed; existing unrelated evaluation-file line-ending
  warnings remain outside this task.

The focused test command is:

```powershell
ctest --test-dir build/desktop-vulkan-release `
  -R '^cartographer_(desktop_style|desktop_chrome|desktop_browser|frontend_selection_wiring|frontend_asset_wiring|application|ui|viewport_interaction|desktop_workbench_input|ai|preview_transaction|storage_journal|project_source_asset_registry|source_asset_registry|lightmap_uv|agent_workbench|decal|public_api)_tests$|^cartographer_tests$' `
  --output-on-failure --parallel 4
```

Native inspection used the bundled Computer Use skill and the actual updated
executable. The workspace, Scene filter, Inspector empty state, asset drawer,
cyan tabs, and status footer were observed. Header Commands opened the actual
command palette and Escape closed it; F11 entered viewport focus and Escape
restored the dock tree. The initial launch capture was blank before a later
input/refresh showed the rendered application; no immediate-startup latency
claim is made. A native resize attempt did not establish a changed window size;
narrow-layout evidence remains headless.

Native inspection also exposed two real presentation defects, retained as
failure evidence: repeated docked-panel move instructions hid asset search below
the short drawer, and the legacy navigation child used automatic full width,
pushing Project/Settings content offscreen. The first now keeps tab dragging and
the existing Move/Float path in a context menu; the asset search/import row is
compact. The second uses an explicitly bounded navigation width and a regression
that checks the real adjacent child rectangles. Final repair verification is
recorded below:

- Rebuilt the native desktop and affected desktop test targets after the rail
  fix. All **4/4 desktop suites passed**, 1.94 seconds, including the new
  adjacent-child navigation regression. Final native link succeeded.
- Reopened the actual final executable. Settings contents now appeared beside
  the rail. Clicking High Contrast changed real native controls/backgrounds to
  the contrast palette; clicking Dark restored the studio palette. Returned to
  Workspace through the visible navigation. The final workspace remains open.
- The final empty Scene/asset states, compact asset search/import row, Inspector,
  status footer and viewport were inspected. No test object was presented as
  production data and no existing project was edited during native inspection.

Tests not run include populated-scene/card native click-through, every authoring
dialog, the full repository CTest matrix, disconnected-monitor
recovery, multiple GPUs, arbitrary DPI/resize/device-loss combinations, and
clean-machine packaging/deployment. The shared RegOS Python/ida_core/npm commands
do not target this native C++ repository. No other product repository is entered.

The [2026-10-24 Inspector continuation](frontend-inspector-drafts.md) adds a
populated two-box native smoke check and source-bound draft repair. It does not
close the full populated-scene/device acceptance gates listed above.

## Risks, rollback, rejected options

The new dark accent is an intentional visible design change. Appearance
preferences are unchanged in format; switching Light or High Contrast remains
explicit. Shared palette changes affect both native widgets and custom drawing.
Search is bounded and case folding supports ASCII; no locale-aware search claim
is made. Large scenes still require native responsiveness evidence. Old saved
dock layouts are preserved, so screenshots can differ from a first-run layout.
Legacy explicitly sized instrument controls still need a comprehensive Touch
review; the default-control and chrome tests are not universal accessibility
certification.
No project or journal schema migration is introduced.

Rollback selectively removes these design hunks/helpers/test targets and restores
the prior palette; retain the preceding frontend/backend wiring and unrelated
dirty work. Do not reset whole shared files. User layouts should be changed only
through their existing local layout controls; code rollback is not a project-data
rollback.

Rejected: applying the archive's stale transform/installer scripts; downgrading
the ImGui pin; replacing the native frontend with a web mockup; inventing world
rendering, coordinates, asset thumbnails, collaboration locks, or trust metrics;
resetting the user's dock tree; bypassing application commands; and refactoring
unrelated backend work.

## Cross-product trace and promotion

Direct impact: Cartographer native/shared UI presentation and tests. Indirect
impact: native consumers see the updated palette and layouts; any external
screen-coordinate automation must be revalidated. Shared contract: current C++
UI snapshot and existing local preferences, with no backend/network API or saved
project schema change. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA were not
changed or inspected; no cross-repository compatibility or deployment claim is
made. Any external adapter requires separate tests against its real contract.

Dataset progress: none. Training progress: none. Capability progress: bounded
native design/accessibility behavior and truthful browsing. Promotion progress:
only the explicitly recorded build/headless/native evidence; broader native
acceptance remains open.
