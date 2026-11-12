# Explicit UV-domain bake candidate

Date: 2026-11-12. Isolated material lane, based on single-evaluation bake `d2f5832`.

## Added capability

A new three-argument `bake_authoring_material` overload samples an explicit UV origin and positive span into a bounded square raster. Pixel centers and UV derivatives reflect the requested domain. The two-argument unit-square API remains available and delegates to the identical default domain. Every returned bake carries its exact domain alongside the source program digest and map dimensions.

This supports preparing individual authoring subregions or explicit tiles. It does not implement UDIM naming, multi-tile aggregate scheduling, per-tile material assignment, texture-array rendering, export, or automatic stitching. Existing texture address modes still apply. A graph whose UV-dependent material values become invalid outside [0,1] is refused by the existing evaluator rather than clamped silently.

## Numerical and resource boundaries

Both axes require finite, strictly increasing endpoints within +/-1e6, positive finite span and derivative, and distinct increasing pixel centers strictly inside the domain. Huge origins, collapsed extents, zero/negative spans and underflowed steps fail before material evaluation or raster allocation. Coordinate preflight costs two bounded one-dimensional vectors, not another per-pixel material evaluation.

The domain changes neither resolution admission nor the one-evaluation-per-texel algorithm. Retain the separately staged 1024-resolution/export-budget admission during integration. The source program digest intentionally does not identify a particular raster; future tile caches must additionally key domain and dimensions.

## Verification

Regression source covers default-overload parity, explicit subregion roughness against direct surface evaluation, constant-material tiles outside the unit square, domain metadata, source immutability and invalid/collapsed-coordinate cases. The previous full six-map two-pass oracle remains in the same test target.

`git diff --check` passed; source reviewed. No compile, CMake, test binary, GPU, image acceptance or benchmark was run under the build hold. No shared checkout or Opus integration was changed.
