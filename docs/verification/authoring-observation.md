# Authoring observation query

`carto::editor::evaluate_point_sightline` is a deterministic, read-only CPU
query over a captured `RenderScene`. It reports point-to-point geometric
visibility, or the nearest blocking object, mesh asset/revision, and source
face. The result carries the project snapshot revision and must be rejected
after that revision changes.

`carto::editor::evaluate_route_visibility` evaluates landmark visibility from
an ordered list of explicit route stations. It reports one point-sightline
receipt and cumulative route distance per station; it does not invent
intermediate samples. Station-to-object exclusions support observations made
from a route mesh without hiding other occluders. Sample count, route length,
per-sightline geometry work, and aggregate triangle work are bounded. The
native UI wrapper binds the whole query to the exact current viewport snapshot
and is read-only.

`carto::editor::evaluate_landmark_viewshed` evaluates one bounded row-major
target grid from a fixed observer. Callers supply every target's world
position, including elevation from their admitted terrain/source sample; the
query does not interpolate terrain or create fallback heights. It returns a
point-sightline receipt for every cell and a visible-cell count that can feed a
heatmap. The result is tied to one project snapshot and aggregate triangle
budget, and is rejected as a whole when any target or receipt is invalid.

The desktop CONDITION viewport draws the current terrain-grid result as
visible/blocked cell rectangles and labels it as a geometry-only preview with
the visible-sample count. It checks the result against current terrain identity,
revision, digest, LOD, and project revision before drawing; stale state produces
a warning instead of a heatmap. This is an immediate-mode sampled overlay, not
authored geometry or continuous terrain visibility.

`carto::editor::evaluate_authoring_observation` projects an explicit target
list through a normalized observer basis and bounded horizontal/vertical view
cone. Each caller-supplied target preserves its stable identity and semantic
kind (object, landmark, sign, interaction, clutter, or biological reference).
An explicit kind mask filters candidates; remaining targets are classified as
out of range, outside the cone, visible, or occluded. Geometric visibility is
evaluated only for in-cone targets and carries the existing point-sightline
blocker receipt. The UI wrapper requires the exact current viewport revision.
No object centers, semantic classes, or target positions are inferred.

The query excludes optional observer/target objects to avoid self-occlusion.
It requires each tested occluder to have a current compiled mesh and a bound
asset identity. Missing or stale evidence, non-finite geometry, excessive
distance, and occluder/triangle budget overflow fail the whole query; it never
returns partial visibility as if it were complete.

Focused acceptance:

```powershell
cmake --build build/headless-release --target cartographer_observation_tests --parallel 4
ctest --test-dir build/headless-release -R cartographer_observation_tests --output-on-failure
```

The fixture exercises synthetic point, route, and grid landmark sightlines,
transformed occlusion, endpoint and per-station self-exclusion, deterministic
nearest-blocker selection, stale and unbound source refusal, invalid input,
tampered receipts, and per-query/aggregate budget boundaries. The Rural
Road/Forest application fixture also evaluates the saved road-and-scatter
viewport scene from explicit route stations, repeats the query for deterministic
parity, verifies no project or journal mutation, and rejects the receipt after
the project changes. The grid test checks mixed visible/blocked cells,
row-major receipts, explicit elevation samples, and aggregate-budget refusal.
The semantic projection test covers kind filtering, horizontal/vertical cone
exclusion, visible and occluded targets, deterministic receipts, stale source
refusal, malformed camera/target input, aggregate triangle overflow, and UI
non-mutation. This is not an interpolated route sweep or a continuous terrain
viewshed. The sampled native overlay has no pixel-capture acceptance here; it
does not establish silhouette metrics, sign-legibility, interaction
reachability, a ray-traced renderer result, or production-scene acceptance.

Current-source evidence (2026-10-18): `cartographer_observation_tests` rebuilt
and passed (1/1); `cartographer_condition_authoring_tests`,
`cartographer_observation_tests`, and `cartographer_ui_tests` passed together
in CTest (3/3). These headless query/UI-contract checks do not replace a
recorded native viewport review.
