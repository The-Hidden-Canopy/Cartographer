# Web geometry derived layer

Cartographer owns authoring topology and derives a vendor-neutral geometry
package for downstream consumers. The boundary is:

```text
EditableMesh + typed face attributes
  -> CompiledMesh + source identity
  -> carto::web_geometry compiler
  -> ClusterHierarchy + provenance + RuntimeGeometryPage payloads
  -> bounded portable package
```

The web-geometry targets are split so the package/core contract does not link
the authoring geometry library. `carto_web_geometry_compile` is the only target
that consumes `CompiledMesh`; `carto_web_geometry_core` and
`carto_web_geometry_package` consume stable IDs, bounds, revisions, digests,
page tables, and payload bytes. This keeps a downstream consumer-side package reader from
depending on Cartographer authoring internals.

## Implemented C1–C5 slice

- options are canonical and digestable, with bounded leaf and page limits;
- leaf clustering is deterministic and uses compiled triangle adjacency,
  same-authored-face fan adjacency, material/semantic/break boundaries, hard
  normal boundaries, lock groups, and hard triangle caps;
- `source_faces` is an index-aligned table for raw compiled-triangle indices;
  it is not leaf traversal order;
- hierarchy validation proves one root, exact child references, acyclicity,
  child bounds containment, non-empty clusters, exact triangle coverage, and
  canonical digest integrity;
- every cluster has revision-bound provenance, compiler-options digest, source
  FaceIds, and a content digest; parent provenance is recursively checked to
  cover child provenance;
- deterministic runtime pages pack renderable cluster records up to
  configurable target and hard payload bounds; the root can carry a coarse LOD
  payload as well as child links;
- page payloads use a little-endian `WGP1` record stream: version, cluster
  count, cluster IDs, raw compiled-triangle IDs, FaceIds, and three position /
  normal pairs per triangle;
- package serialization is bounded and round-trips hierarchy, provenance,
  pages, payloads, and digests; the public page decoder rejects malformed,
  non-finite, duplicate, truncated, and trailing records.
- consumer import admission rechecks canonical encoding, expected source
  revision, optional compiler-options digest, and caller budgets for clusters,
  triangles, pages, payload, and serialized bytes. It returns a package-bound
  receipt and never mutates project truth.
- runtime admission runs after consumer import, applies device-specific
  cluster/triangle/page/resident-payload limits, and returns a digest-bound
  receipt naming the caller-provided device, driver, viewport, measured package
  counts, and limits. It does not allocate or reserve residency.

The package/header schema is version 2. It intentionally separates the new
page/options contract from any earlier experimental schema-1 shape.

The CPU compiler allocates no GPU resources and does not perform residency,
render scheduling, or downstream consumer mutation. Page payloads are derived geometry;
`.carto` authoring truth remains authoritative. The backend-neutral
`project_cluster_overlays` contract can project validated cluster bounds into
viewport rectangles with explicit row-major matrix and Vulkan/D3D depth
conventions. `resolve_cluster_overlay_pointer` checks current object identity,
package digest, and source revision before returning source FaceIds for
overlapping cluster candidates in front-to-back order. It leaves ambiguous
candidates visible to the caller instead of choosing one. Projection and
pointer resolution remain read-only geometry contracts: they do not mutate
selection, allocate resources, or perform runtime admission.

`build_cluster_overlay_draw_plan` converts visible projected bounds into
bounded triangle ranges. The D3D12 acceptance harness rasterizes those ranges
on the Quadro P5200 and verifies the captured pixels use the planned cluster
colors. The harness does not integrate drawing or pointer events into the
desktop viewport.

## Explicitly deferred

The compiler now emits a deterministic representative source-triangle payload
for the root parent when at least two non-degenerate source triangles are
present. Its error receipt uses the enclosing sphere diameter as a conservative
bound: both the original surface and representative triangles lie within that
sphere. Smaller or degenerate inputs may remain grouping-only. This is a coarse
CPU LOD, not a quality-optimized decimator. Full semantic/material registries,
desktop overlay integration, pointer-event routing, and runtime residency
management remain separate acceptance gates. Projection math retains
clip-crossing clusters conservatively and rejects non-finite inputs; native
D3D12 offscreen rasterization is tested, while desktop visual acceptance remains open.

The benchmark reports local CPU compile and package-serialization timing for a
fixed fixture. It is not GPU throughput, runtime FPS, residency evidence, or
8K acceptance.

## Evaluation publication boundary

`carto_web_geometry_evaluation` publishes the validated package into a typed,
deterministic evaluation-graph chain. The publication receipt binds the six
derived stages to source/options/cluster/hierarchy/page/package digests and
retains page and cluster identities. Comparing two receipts produces a
downstream-closed invalidation set, so a consumer can rebuild only the
affected derived stages and address changed cluster/page records.

This is graph intent and invalidation evidence, not evaluated geometry
execution. The adapter does not mutate `.carto` authoring state, write the
evaluation cache, allocate GPU resources, admit a downstream consumer package, or publish
runtime residency.

`inspect_package` provides a second read-only consumer boundary for editor and
problem surfaces. It reports deterministic cluster bounds, parent/page
identity, triangle and source-face counts, and payload summary data. It emits
an explicit warning and a package-bound conservative error receipt for
grouping-only parents. That receipt uses the cluster sphere radius as a
non-renderable bound (`grouping-sphere-radius-v1`); it makes the absence of a
simplified parent representation visible without presenting the bound as a
renderable LOD. `project_cluster_overlays` consumes that validated diagnostic
snapshot and produces package/revision-bound screen-space rectangles using an
explicit projection matrix, conservative clip behavior, and viewport bounds.
`resolve_cluster_overlay_pointer` maps pointer coordinates back to revision-
bound cluster and source-face candidates. `build_cluster_overlay_draw_plan`
returns bounded triangle ranges for native rendering. These helpers remain
read-only with respect to selection and runtime residency.

`plan_runtime_page_interest` consumes the same package-bound overlay snapshot
and returns sorted, deduplicated visible-cluster and runtime-page identities.
It rechecks cluster metadata and error-receipt identity before returning the
plan. The plan is a handoff contract for a future residency owner; it does not
read page bytes, allocate memory, mutate residency, or admit a runtime.
