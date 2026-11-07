# Viewport / workspace native acceptance ledger — 2026-11-07

Scope: native build/runtime acceptance for the VANTA-inspired viewport/workspace
work in the isolated worktree
`<worktree>`
(branch `<candidate-branch>`, base `c10d668`).
No production code or tests were edited by this pass.

## Update 2026-11-07 (second pass — fixed-tree verification)

The integration owner fixed the review issues (including the spatial-combo
swap below). Source state verified before rebuild: the Spatial/Color combos
now bind through `kViewportSpatialModeMenuItems` /
`viewport_spatial_mode_from_menu_index` (`apps/desktop/main.cpp:10844-10860`),
and `tests/desktop_viewport_view_tests.cpp` gained
`menu_item_strings_match_enum_contract` and `orbit_contract_for_view_kinds`
(**20 checks** total). Incremental rebuild of the same isolated root
(`.../final`, BUILD OK) and targeted visible-window re-acceptance followed.
All `corrected-*.png` screenshots are FINAL corrected evidence; the
`final-07-*-defect-evidence.png` shot remains as the earlier DEFECT record.

Headless (NOT visible-window acceptance), from
`<build-root>/final`:

```
ctest -R "cartographer_desktop_(viewshed|viewport_view|sweep_source|style|chrome|browser|workbench_input)_tests|cartographer_desktop_admission_boundary" --output-on-failure
```

- **8/8 suites pass** (adds viewshed, sweep_source, admission_boundary to the
  previous five). `cartographer_desktop_viewport_view_tests` = 20/20 checks.

Visible-window / GPU re-acceptance (same harness as the first pass; isolated
`LOCALAPPDATA=.../appdata-fix`; fixture copy `acceptance-room08-save.carto`):

| # | Recheck | Result | Evidence |
|---|---------|--------|----------|
| R1 | Spatial dropdown order | PASS — now Shaded/Depth/Structure, matching the enum | corrected-01 |
| R2 | Select "Depth" | PASS — combo reads Depth and the viewport renders the depth ramp (near=far gradient fills), not topology | corrected-02 |
| R3 | Select "Structure" | PASS — combo reads Structure and the viewport renders wireframe-only topology (fills dropped) | corrected-03 |
| R4 | Persisted honesty | PASS — after close, `workspace-viewport.prefs` records `spatial=structure` when Structure was selected (pre-fix it recorded structure when Depth was selected) | prefs file |
| R5 | Front (X/Y) does not orbit | PASS — clicked "Orbit view", dragged in the viewport: label stays "Front (X/Y)", orthographic projection pixel-identical; drag instead performed an object selection (Object #1924 shown in Inspector) | corrected-04/05 |
| R6 | Arrangement switching (View ▸ Workspace arrangement) | PASS — Scene enlarges viewport; Review docks Inspector left; both switches kept the Front view (focus retained); each arrangement owns its layout file (`workspace-docking-scene.ini`, `workspace-docking-review.ini` beside the legacy `workspace-docking.ini`) | corrected-06/07 |
| R7 | Per-arrangement customization retention | PASS — floated the Inspector as a native window in Scene, switched to Review and back: the floating Inspector restored at its saved position | corrected-08 |
| R8 | Close/reopen restores view + detached window | PASS — reopen restored the Scene arrangement, the detached Inspector platform HWND, and view `front_xy` | corrected-11 |
| R9 | Dirty project save via native UI | PASS — Tools ▸ "New Box" → status "Modified | Revision 917 | 67 objects" (corrected-09); clicked the top-right "Save" button → status "Saved", **file hash changed** (sha256 prefix `ed25acdbe083…` → new hash) | corrected-10 |
| R10 | Reload after save | PASS — close, reopen app, File ▸ Open of the same copy → "Saved | Revision 917 | 67 objects" | corrected-12 |

No changes to any real user file: all runs used the isolated prefs root and a
fixture copy; the source `room08.carto` remains untouched.

## Update 2026-11-07 (third pass — viewport close/restore binding + regression)

New binding verified in the current final build (incremental rebuild of
`.../final`, BUILD OK; regression: focused eight-suite CTest **8/8 pass**
again). Binding under test (`apps/desktop/main.cpp:29697-29711`): the docked
`Viewport` window's `p_open` is the persisted `work_surface` pane flag;
while hidden a "Viewport hidden" restore card is shown; View > Work surface
toggles the same flag; entering F11 focus restores the viewport. Isolated
`LOCALAPPDATA=.../appdata-close`; no project file modified (close/restore is
workspace chrome; the populated-scene evidence from pass 2 stands).

| # | Exercise | Result | Evidence |
|---|----------|--------|----------|
| C1 | Viewport tab close (X on docked tab) | PASS — viewport removed from dock, dock identity of Scene/Inspector/Assets/History unchanged, "Viewport hidden" card centered with "Restore viewport" and the View > Work surface note | closerestore-01 |
| C2 | Restore via card button | PASS — viewport back in its dock slot, Top (Plan X/Z) view retained, card dismissed | closerestore-02 |
| C3 | View > Work surface toggle | PASS — with the viewport hidden the menu item is unchecked (all other panes checked); clicking it restores the viewport | closerestore-03/04 |
| C4 | F11 focus with hidden viewport | PASS — entering focus restores the viewport (solo viewport, chrome hidden); F11 again returns to the normal workspace with the viewport still visible | closerestore-05 |
| C5 | Detached OS-window close | PASS — detached `ImGui Platform` "Viewport" window (no in-window close X; closed via WM_CLOSE on the platform HWND), platform window destroyed and the viewport became hidden with the restore card shown in the main window | closerestore-06/07 |
| C6 | Hidden-state persistence | PASS — graceful close with the viewport hidden wrote `pane_work_surface=0` to `workbench.prefs`; on reopen the viewport was still hidden and the restore card shown; after restoring and closing, prefs recorded `pane_work_surface=1` | closerestore-08, prefs file |

## Build

- Toolchain: llvm-mingw gcc/g++ 22.1.8 (WinGet `MartinStorsjo.LLVM-MinGW.UCRT`),
  CMake 4.4.1, Ninja 1.13.2, Vulkan SDK 1.4.357.0, bundled pinned ImGui
  (`third_party/imgui`, unmodified).
- MSVC 19.44 CANNOT build this tree: `libs/plugin_package/src/ed25519.cpp`
  uses `__int128` (C4235) and `libs/application/src/application.cpp` exceeds
  the COFF section limit (C1128, no `/bigobj` anywhere in the project). The
  `-static` linker flag in `build-cartographer.bat` also implies a mingw
  toolchain. This matches the pre-existing `%LOCALAPPDATA%/HiddenCanopy/
  Cartographer/workbench-release` exe (built 2025-10-07 with the same recipe).
- Exact configure/build (script: `<build-root>/build-mingw.cmd`):

  ```
  cmake -S <worktree> -B <build-root>/final -G Ninja ^
    -DCMAKE_C_COMPILER=<llvm-mingw>/gcc.exe -DCMAKE_CXX_COMPILER=<llvm-mingw>/g++.exe ^
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-static ^
    -DCARTO_VULKAN_SDK_ROOT=C:/VulkanSDK/1.4.357.0 ^
    -DVulkan_INCLUDE_DIR=C:/VulkanSDK/1.4.357.0/Include ^
    -DVulkan_LIBRARY=C:/VulkanSDK/1.4.357.0/Lib/vulkan-1.lib ^
    -DCARTO_IMGUI_ROOT=<worktree>/third_party/imgui -DCARTO_USE_BUNDLED_IMGUI=ON ^
    -DCARTO_ENABLE_VULKAN=ON -DCARTO_BUILD_DESKTOP=ON -DCARTO_BUILD_TESTS=ON ^
    -DCARTO_BUILD_CLI=ON -DCARTO_BUILD_DEVICE_CPU=ON ^
    -DCARTO_ENABLE_D3D12=OFF -DCARTO_BUILD_D3D12_DESKTOP=OFF ^
    -DCARTO_BUILD_BENCHMARKS=OFF -DCARTO_BUILD_SDK_SHARED=OFF
  cmake --build <build-root>/final --parallel 8
  ```

- Baseline build originally described as "main `c10d668`": same flags, root
  `<build-root>/baseline`. **Retracted
  as a main snapshot — see "Provenance correction" above** (compiled while the
  candidate worktree was being edited; captures renamed to
  `historical-*-early-candidate-unpinned.png`).
- Final build of the integrated tree: same root `.../final` (792 targets, BUILD OK).
- Executable: `<build-root>/final/apps/desktop/cartographer_desktop.exe`

### Provenance correction (read first)

The build originally labelled "baseline (main c10d668)" in pass 1 is **NOT a
verified main snapshot**. It was configured while the worktree was checked out
at `c10d668`, but the 792-target compile ran for ~40 minutes **while other
workers were already editing the same worktree**, so some translation units
picked up candidate changes. Evidence: genuine `git show c10d668:apps/desktop/main.cpp`
has only a `Plan view`/`Perspective` SmallButton toggle and **no** "Display &
shading" section, yet the pass-1 "baseline" screenshots show the four-way view
combo and Display & shading controls — i.e. they show candidate UI. Those
captures (renamed `historical-0X-*-early-candidate-unpinned.png`) and every
pass-1 action attributed to a "baseline" or "c10d668" build are **historical
candidate-worktree evidence at an unpinned source state**, not main behavior.
No archived c10d668 build exists; no claim in this ledger is based on verified
main runtime behavior. The pass-2 `corrected-*` evidence and the pass-3
`closerestore-*` evidence are from the current final build and remain valid.

## Environment isolation (no real user prefs touched)

- The app reads prefs exclusively via the `LOCALAPPDATA` env var
  (`user_preference_path`, `apps/desktop/main.cpp:2620`): prefs root is
  `%LOCALAPPDATA%/Cartographer/`. All runs launched with
  `LOCALAPPDATA=<build-root>/appdata-{baseline,final}`.
  The real `%LOCALAPPDATA%/Cartographer` does not exist on this machine and was
  never created. Project fixture is a copy:
  `projects/acceptance-room08.carto` (sha256 `ed25acdbe…2492`, byte-identical
  to the source `<fixture-source>/room08.carto` after all
  acceptance runs — no loss of work).
- Fixture: populated 66-object motel blockout (`motel_08_safe_place_world_blockout`,
  revision 914, 19 mesh assets / 49 instances), opened through the real
  `GetOpenFileNameW` dialog in the running app.

## Headless/offscreen test results (NOT visible-window acceptance)

Run from `<build-root>/final`:

```
ctest -R "desktop_(workbench_input|viewport_view|style|chrome|browser)" --output-on-failure
```

- 5/5 passed: `cartographer_desktop_workbench_input_tests`,
  `cartographer_desktop_viewport_view_tests` (19 checks: view axes, pose
  round-trips, depth/structure math, codec rejection/recovery/size limits),
  `cartographer_desktop_style_tests`, `cartographer_desktop_chrome_tests`,
  `cartographer_desktop_browser_tests`. These are CPU/headless ImGui-context
  tests; they do not exercise the Win32/Vulkan swapchain.

## Visible-window / GPU acceptance (real process, real input, screenshots)

All driven by SendInput-level mouse/keyboard against the real HWND
(`CartographerDesktopWindow`; detached viewports are `ImGui Platform` HWNDs),
captured with `PrintWindow(PW_RENDERFULLCONTENT)`. **Pass-1 "baseline" rows
below used the contaminated early build (see Provenance correction) — treat
them as historical candidate captures, not main behavior.** Pass-1 "final"
rows used the pre-review-fix candidate tree (the spatial defect recorded
below was found in that tree). Single monitor 1920x1080 (Intel UHD
630 drives the display; a Quadro P5200 is present but not attached to a
display), Windows 11.

| # | Exercise | Result | Evidence |
|---|----------|--------|----------|
| 1 | Launch, populated project open via native file dialog | PASS — 66 objects, hierarchy, assets, CPU-projected plan render | final-01 |
| 2 | View kind switch Top ↔ 3D Perspective | PASS (early-candidate build, unpinned) | historical-03 |
| 3 | New Front (X/Y) orthographic view | PASS | final-02 |
| 4 | New Right (Z/Y) orthographic view | PASS | final-03 |
| 5 | Honest capability labels under Display & shading | PASS — "Available (CPU projected geometry)"; "Orthographic named views render on the CPU painter; the GPU scene preview binds authored or perspective cameras only."; spatial tooltip states no GPU depth/texture readback | final-04, final-05 |
| 6 | Color mode Black & white | PASS — combo + render update | final-06 |
| 7 | Detach Viewport tab → native OS window | PASS — new `ImGui Platform` HWND, fully functional toolbar/view controls; host keeps Scene/Inspector/Assets (early-candidate build shows same) | final-07, historical-02 |
| 8 | Snap back by dragging onto dock center target | PASS (early-candidate build) — platform HWND destroyed, window re-docked | (scratch shots 17/18) |
| 9 | F11 focus mode on/off | PASS (early-candidate build) — all chrome hidden, solo viewport, view retained | historical-01 |
| 10 | Resize small (1100x700) / large (1600x1000) | PASS — adaptive density (compact mode bar at small size) | (scratch shots 21–23) |
| 11 | Minimize → restore; hide → show | PASS — scene intact after restore | (scratch) |
| 12 | Ctrl+Shift+1/2 workbench arrangement flows | PASS (early-candidate build) — panel set changes, Operation ledger appears | (scratch shot 43) |
| 13 | Viewport object selection + Inspector display | PASS (early-candidate build) — Object #615 selected, gizmo drawn | (scratch shot 36) |
| 14 | Graceful close with detached viewport | PASS — clean exit, platform windows retired | — |
| 15 | Persistence: close writes `workspace.prefs`, `workbench.prefs`, `workspace-viewport.prefs`, `workspace-docking.ini` (atomic MoveFileEx) | PASS — all four files present in isolated appdata | (file contents in scratch) |
| 16 | Reopen: layout + view restored | PASS — detached Viewport restored as platform HWND at saved pos/size; Right (Z/Y) restored | final-08 |
| 17 | Per-view camera pose persistence | PASS — `editor_camera` ortho pose in `workspace-viewport.prefs` matches live view distance readout | — |

## Defect found in pass 1 — FIXED and re-verified in pass 2

**Spatial-mode combo row order did not match the enum; Depth ↔ Structure
were swapped in the UI. (Fixed by the integration owner; re-verified — see
R1–R4 in the update section above.)**

- Code: `apps/desktop/main.cpp:10788-10792` — combo items
  `"Shaded\0Structure\0Depth\0"` are cast by index to
  `carto::studio::ViewportSpatialMode`, whose declaration order is
  `shaded, depth, structure` (`apps/desktop/studio_view_settings.hpp:52-55`).
  So combo index 1 ("Structure") sets `depth`, index 2 ("Depth") sets
  `structure`. The display read-back uses the same index, so the combo looks
  self-consistent while being wrong.
- Runtime reproduction (final build): with the populated scene, Display &
  shading → Spatial → clicked the **"Depth"** row. The viewport switched to the
  dense edge-mass rendering (structure fill-drop path,
  `main.cpp:12375`), while the combo kept reading "Depth". On graceful close,
  `workspace-viewport.prefs` honestly recorded `spatial=structure`
  (`final-07-detached-viewport-defect-evidence.png` shows both at once:
  Spatial combo = "Depth", rendering = structure topology).
- Impact: users asking for Depth get Structure and vice versa; the wrong mode
  also persists and reloads. Codec/label layer (`studio_view_settings.hpp`)
  is correct — one-line fix in main.cpp: item order should be
  `"Shaded\0Depth\0Structure\0"` (or map indices explicitly).
- The new headless suite cannot catch this: it tests the codec and math, not
  the ImGui combo binding.

## Limitations / unverified

- **Cross-monitor / offscreen recovery**: only one monitor available. The
  detached viewport opens/restores partially offscreen
  (e.g. `(1186,805)-(2262,1490)` on 1920x1080); `recover_offscreen_platform_windows`
  only clamps windows fully outside all monitors, so a partially-offscreen
  restore is left as saved. Multi-monitor recovery is UNVERIFIED.
- **Document-modifying save**: RESOLVED in pass 2 (R9/R10) — dirty via the
  native "New Box" instrument, saved via the top-right "Save" button, hash
  changed, close/reopen/reload retained the edit (67 objects, revision 917).
  Note for future automation: the `GetOpenFileNameW` filename edit does not
  reflect text via `WM_SETTEXT`/`WM_GETTEXT` readback and the dialog is
  partially occluded by other top-level windows on this desktop, so dialog
  automation needs the dialog raised with `SetWindowPos(HWND_TOPMOST)` and
  keyboard commit; this is a tooling limitation, not an app defect.
- **GPU scene preview / authored cameras**: the populated CPU-projected views
  were exercised; the GPU preview path (authored/perspective cameras with
  material/lighting) was not separately driven — the capability label states
  it binds authored or perspective cameras only.
- **D3D12 desktop**: out of scope (flags OFF, per instructions).
- Screenshots contain only the Cartographer window; no desktop/personal
  content. Captures live in `<build-root>/shots/`
  (full set); only final evidence was copied here.

## Artifacts

- Build roots (outside checkout): `<build-root>/{baseline,final}`
- Automation helpers: `<build-root>/{ui_auto.py,cartoctl.py,build-mingw.cmd}`
- Isolated prefs: `<build-root>/appdata-{baseline,final}/Cartographer/`
- Fixture copy: `<build-root>/projects/acceptance-room08.carto`


## Coordinator regression reconciliation (2026-11-07)

The final native build and the focused eight-suite desktop/frontend CTest run
passed (`8/8`; `desktop_viewport_view_tests` has 20 checks). The broader run is
explicitly not green:

```powershell
cmake --build <build-root>/viewport-build --parallel 2
ctest --test-dir <build-root>/viewport-build --output-on-failure --parallel 2 --timeout 60
ctest --test-dir <build-root>/viewport-build --rerun-failed --output-on-failure
```

- Full parallel run: **185/194 passed, 9 failed**. The serial package-consumer
  test and the 600-second workspace pressure test were allowed to finish; the
  pressure test passed in 340.61 seconds.
- Serial rerun: `cartographer_package_consumer_tests` **passed in 87.47
  seconds**. The earlier 60-second limit was an artificial timeout, not a
  product failure.
- These eight unrelated suites still failed serially and are not claimed as
  passing or repaired by this viewport change:
  `cartographer_condition_authoring_ui_fixture_tests` (SEGFAULT),
  `cartographer_prepared_scene_tests`,
  `cartographer_prepared_publication_tests`,
  `cartographer_preparation_operation_tests`,
  `cartographer_preparation_job_tests`,
  `cartographer_verification_failure_injection_tests`,
  `cartographer_verification_corrupt_artifact_tests`, and
  `cartographer_character_production_tests` (`std::bad_variant_access`, exit
  `0xc0000409`). Their implementation files are outside this change; a clean
  baseline run was not used to attribute them to this candidate or to assert
  that they are pre-existing defects.
