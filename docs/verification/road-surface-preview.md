# Road surface authoring preview

This note records the public Cartographer road-ribbon preview. It is a design
visualization derived from authored road intent and a revisioned curve source;
it does not establish construction, safety, or regulatory compliance.

## Product and source boundary

`ApplicationSession::road_surface_preview` resolves a road source and its exact
centerline from the current `.carto` project. The returned product carries the
project generation, road identity/revision/digest, and centerline
identity/revision/digest. The native CONDITION workbench displays the ephemeral
ribbon in the central plan-view work surface. It does not write a project
transaction, journal event, scene object, exported mesh, collision product, or
runtime record. The UI suppresses the preview when the project generation or
either source receipt no longer matches.

Each station contains a center point plus left/right road and shoulder edges.
Uniform curve-parameter samples are used (they are not equal-arc-length
samples); duplicate shared segment endpoints are merged. The builder is capped
at 16,384 stations. It rejects zero horizontal runs, sampled grade above the
authored maximum, sampled circumcircle radius below the authored minimum,
road/shoulder envelopes that exceed the minimum radius, reversing or unstable
joins, and miter scales above four. Rejected input produces no partial preview.

The grade and radius tests are sample-based checks, not a continuous-curve
proof, survey, geotechnical analysis, or engineering certificate. No terrain
draping or earthwork is inferred. The preview does not generate frontage,
driveways, signs, drainage/culvert geometry, navigation, or collision data.

## Verification

`cartographer_road_source_tests` covers deterministic straight-road dimensions,
station/triangle bounds, invalid numeric output, grade and curvature rejection,
reversing joins, envelope/radius disagreement, and hostile sampling sizes.
`cartographer_application_tests` proves the preview is read-only, binds both
source digests and the project generation, rejects missing sources, and yields
new receipts after the road and centerline are revised. The desktop target
compile verifies native viewport/workbench integration; no screenshot-backed
visual acceptance is claimed.
