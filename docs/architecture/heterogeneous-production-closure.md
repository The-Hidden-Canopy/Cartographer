# Heterogeneous production closure

Cartographer owns authoritative authoring state; runtime products are derived
artifacts. The production closure contract makes that boundary explicit for
render meshes, material packages, residency pages, publication packages, and
evidence bundles without requiring the public repository to own VANTA's runtime.

`carto_production` provides five fail-closed invariants:

1. A cook key binds a product to the source identity, source revision, source
   digest, artifact kind, execution form, algorithm, settings digest, and
   platform profile, and—when scoped—stable representation and spatial-scope
   identifiers.
2. An admitted receipt carries measured quality, confidence, cost, output size,
   and a digest over its complete canonical evidence.
3. Publication revalidates the receipt against the current source and the
   current error, confidence, resident-memory, cook-time, CPU-time, GPU-time,
   disk-space, and artifact-output-size policy. Cost ceilings are optional;
   when configured, equality is admitted and any value above the ceiling is
   rejected. The per-publication output bound is distinct from the blob store's
   global storage limit.
4. The append-only ledger treats an exact retry as idempotent, rejects two
   different admitted outputs for one deterministic cook key, and never falls
   back to an older admitted product after a newer failure, cancellation, or
   stale result for the same key.
5. A durable decision receipt binds every considered artifact, the exact
   selected index, opaque policy identity, canonical public-threshold digest,
   hardware profile, rejection reason, and timing-evidence basis. Publication
   revalidates the selected artifact and refuses stale source or policy state.

The same key is also usable in `ProductionCookGraph`. The graph is bounded and
deterministic: registered products must be acyclic, external authoritative
inputs may remain unregistered, and invalidation returns only the changed node
and its downstream products in cook order. This keeps a material or source
change from recooking unrelated products without pretending that the graph is
an interactive worker pool.

`SpatialTileRange` adds an integer, grid- and level-qualified footprint for
spatial nodes. Its canonical range must be bound into `spatial_scope` so a
footprint cannot change without changing the cook identity. Region-aware
invalidation skips non-overlapping scoped products, retains global/unknown
downstream products, and rejects incompatible grids or levels rather than
assuming they do not overlap.

`ProductionArtifactStore` is the byte boundary for this contract. It publishes
immutable bytes to the existing SHA-256 blob store only after the receipt,
current source, policy, payload size, and payload digest agree. Reads require a
current admitted receipt and reverify the blob; a stale source or missing
receipt cannot fall back to an older artifact. A conflicting deterministic
output is rejected before blob creation, so failed admission does not leave an
unreferenced payload mutation.

This is deliberately a contract layer, not a pretend GPU backend. GPU queues,
VANTA residency, remote workers, and private production adapters consume the
same receipt shape when those execution paths are enabled.

Schema-v7 projects can now supply a canonical digest over provider-neutral
semantic areas, paths, structure graphs, reconstructible instance fields, and
sparse variants. That digest is suitable as an authoritative production source
basis. The public world model does not contain the adaptive policy or runtime
materializer that chooses or realizes a consumer representation.

The public policy compares the cost values supplied in a receipt with explicit
ceilings. Its canonical digest makes a policy change invalidate an earlier
decision even when the artifact bytes did not change. `CostEvidenceBasis`
requires non-zero GPU time to come from a completed local or worker device
timestamp; a host-clock estimate cannot be relabeled as GPU evidence. The
contract still does not authenticate a producer or choose among candidates.
A runtime adapter must supply trustworthy evidence before making hardware-
performance claims.

The ledger writer emits version two records containing the optional scope
identifiers and opaque truth, observer, precision, and materialization profile
identifiers. The reader remains compatible with version one ledgers; their
unscoped keys retain the original canonical identity and receipt digest. These
identifiers describe a decision context only—the public contract does not
implement the private policy that chooses a representation, precision, region,
or materialization schedule.

## Portable render-mesh product

`carto_production` can compile one bounded render-mesh artifact directly from
an `EditableMesh`. UV selection is explicit by `UvSetId`; material grouping is
optional and accepts only a face-domain `uint32` slot-index layer. The compiler
uses stable source face/corner identity, deterministically groups submeshes by
slot, duplicates render vertices only where authored UV seams require it, and
retains source vertex/face IDs for provenance. Tangents are generated only
when requested and only from valid authored UV triangles; degenerate UV input
is rejected rather than assigned a fabricated frame.
Version two carries compiled per-vertex normals only; it does not represent
custom corner normals or hard-normal seams, so those inputs are not silently
approximated by this artifact.

The saved-document overload validates the `ProjectDocument`, selects a mesh by
project asset ID, hashes the canonical serialized project, and binds that
digest plus project revision into the artifact. The caller supplies a stable,
non-path project identifier for the receipt key; filesystem paths and project
names are not copied into the portable payload.

The version-two payload is little-endian and bounded to 256 MiB, 2,000,000
render vertices, 6,000,000 indices, and 65,536 submeshes. In addition to the
primary UV stream and tangent, it can preserve an independently selected UV1
stream and bounded authored vertex/corner color. It binds the enclosing
source revision/digest and mesh revision but omits local paths, material
definitions, device handles, telemetry, and execution-selection policy. The
existing production receipt and blob store bind the complete payload digest and
publication admission. The producer-to-store saved-project fixture now also
creates, persists, reopens, and revalidates a decision receipt before publishing
the bytes. This is not a VANTA consumer or runtime-admission implementation;
the private adapter and saved-project-to-runtime acceptance fixture remain
separate work.

## Texture, material, and native draw products

The public texture path structurally admits bounded PNG, JPEG, TGA, DDS, and
EXR containers without confusing inspection with pixel decode. The first real
decoder is deliberately narrow: bounded 24/32-bit true-color TGA, including RLE
and origin canonicalization, produces digest-bound top-left RGBA8 pixels.
Deterministic semantic mip generation filters sRGB in linear light,
renormalizes normal maps, handles odd extents, and optionally preserves alpha
coverage. Canonical persistence binds every level and rejects unknown framing,
tamper, truncation, trailing bytes, and source-limit violations. Caller-selected
limits may narrow, but cannot raise, the absolute source-byte, dimension,
decoded-pixel, mip-count, or blob-store allocation ceilings.

`CompiledMaterialArtifact` lowers one supported public material into the exact
native PBR constant ABI, including scalar dielectric F0 and clearcoat. It binds
source identity, revision, canonical source digest, feature mask, constants,
and artifact digest. Compile-time size, alignment, and field-offset assertions
freeze that cross-backend layout. Unsupported authored bindings fail compilation
instead of silently disappearing.

`LookdevDrawPackage` binds one exact render-mesh artifact to exactly one compiled
material artifact for every canonical submesh. It persists draw ranges,
material slots, native constants, source/artifact digests, and resident bytes;
missing, duplicate, unknown, stale, or forged bindings fail closed. It contains
no descriptor handles or private runtime policy. The D3D12 upload planner turns
an admitted RGBA8 mip chain into aligned subresource footprints and explicit
copy commands; native acceptance uploads and reads a non-base mip back. These
products establish a public native-ready boundary without claiming a private
VANTA consumer or final presented photoreal frame.
