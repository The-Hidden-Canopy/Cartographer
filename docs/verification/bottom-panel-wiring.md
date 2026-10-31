# Bottom panel wiring (2026-10-26)

## Capability delta

The Graph and Assets bottom-panel tabs now lead to their existing authoring
surfaces. The dock title and context label follow the active tab. Graph reports
saved material-graph and binding counts, then opens the Material workspace.
Assets reports counts from the existing validated mesh browser projection and
opens the searchable Assets instrument. Timeline and Console identify their
unavailable providers explicitly; the operation strip is labeled as project
command history rather than animation time.

## Governance preserved

- State transition path: no project or domain state is changed by the new panel
  content; workspace navigation uses `UiController::set_workspace`.
- Audit/domain event path: no domain mutation was added.
- Permission/org-scope path: no authority or organization scope was added.
- Human approval boundary: authoring remains in the existing human-operated
  instruments and workflows.

## Tests

- Added: none.
- Updated: none.
- Run: none, at the user's request to complete code without testing.
- Not run: build, automated tests, and native UI interaction/review.

## Rejected options

Did not present the operation receipt strip as an animation timeline or imply
that the Graph tab is a general procedural-geometry graph editor. Did not add
new project state or duplicate the existing Assets browser.

## Risks and rollback

The panel routing and native layout are source-reviewed only. Roll back by
restoring the `draw_bottom` panel title and removing its Graph/Assets/Timeline/
Console branches in `apps/desktop/main.cpp`.

## Cross-product trace

- Direct impact: Cartographer desktop bottom-panel discoverability and truthful
  status.
- Indirect impact: none established for RegOS, IDA_Training,
  hidden_canopy_bot, or Ask_IDA.
- Shared contract touched: Cartographer `UiController` workspace navigation
  and read-only application snapshot projections.
- API/schema risk: no API or serialized schema changes.
- Dataset/eval risk: none introduced; no evaluation data changed.
- Deployment risk: desktop source was not rebuilt, so native compilation and
  layout remain unverified.
- Required follow-up tests: build the desktop target; exercise Graph and Assets
  routing with populated and empty snapshots; confirm workspace-navigation
  failure feedback; inspect narrow and normal native layouts.

## Promotion status

- Dataset progress: unchanged.
- Training progress: unchanged.
- Capability progress: source wiring added; runtime behavior unverified.
- Promotion progress: unchanged; no promotion claimed.
