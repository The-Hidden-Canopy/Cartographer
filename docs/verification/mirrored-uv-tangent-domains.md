# Mirrored UV tangent domains

## Code-only continuation (2026-11-12)

The public render-mesh compiler separates generated tangent smoothing domains
by primary UV triangle orientation, in addition to source vertex, authored
normal, and primary UV coordinate. Frames are generated before render vertices
are deduplicated. A mirrored chart can therefore split a shared endpoint even
when its position, normal, and UV coordinates equal the neighboring chart.
Previously, opposite orientations could cancel or mix their tangent frames.

The final vertex key already includes tangent direction and handedness. UV1,
additional UV streams, vertex colors, material submesh order, and source face
and vertex identities remain preserved. Secondary UV and material splits do
not alter primary UV tangent smoothing. Valid authored tangent layers retain
their existing admission and preservation path.

Degenerate/non-finite UV determinants still fail closed at the existing
1e-12 threshold. Existing vertex/index bounds remain in force. No fallback
frame, mesh mutation, private kernel, or external dependency is introduced.

## Regression coverage authored

`cartographer_production_closure_tests` contains cases for:

- two mirrored triangles sharing both UV edge endpoints;
- exact per-chart tangent direction and handedness, with and without reversed
  material-group traversal order;
- stable source IDs, secondary/additional UVs, colors, and PBR upload signs;
- deterministic repeated compilation and serialized artifact reopen;
- unchanged source geometry, attributes, UVs, and revision;
- a secondary UV seam with unequal per-triangle directions that must still
  share the primary UV smoothing result; and
- preservation of explicit authored tangent frames instead of regeneration.

## Persistence and acceptance boundary

The derived render-mesh format stays at v4. No authoring schema or project
migration is needed. Existing artifacts remain readable; regenerate affected
mirrored-chart derived products to obtain the corrected frames. This patch
does not rewrite saved artifacts or change direct interchange exporters.

Only source review, an independent static review, and `git diff --check` were
performed in this continuation.
The test cases above were added but not executed. Build, CTest, GPU rendering,
normal-map visual acceptance, and full integration remain pending. Run the
focused production-closure target and the normal headless acceptance suite
once build execution is cleared. This is not MikkTSpace conformance or a
photoreal production-scene acceptance claim.
