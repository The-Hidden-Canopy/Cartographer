# VANTA interchange profile

Cartographer keeps the flat, versioned `.carto` file as authoring truth. The
`export-vanta` command emits a derived single-file glTF 2.0 artifact; it does
not convert a project into a second `.carto` dialect and it never mutates the
source project.

```text
project.carto
    -> validated ProjectDocument
    -> compiled mesh snapshots
    -> float32 POSITION/NORMAL + triangle indices
    -> scene hierarchy and revision in glTF extras
    -> scene.gltf
```

The profile is intentionally aligned to the bounded public glTF subset exposed
by VANTA's `MeshAsset` importer:

- positions and normals are float32 and finite;
- triangle-list indices use unsigned 16-bit values when every mesh fits, or
  unsigned 32-bit values otherwise;
- every mesh has one contiguous primitive and a material slot of zero by
  omission;
- buffer sections are four-byte aligned and embedded as a base64 data URI;
- scene nodes preserve local translation, rotation, scale, parent/child
  relationships, visibility, lock state, and source object IDs;
- root extras record the Cartographer profile version, project schema, and
  source revision.

The exporter refuses non-finite float32 narrowing, empty triangle assets, and
invalid source documents. It reports the current loss boundary rather than
inventing channels: Cartographer 0.1 has no authored UVs, tangents, or
materials, so the downstream consumer must use its default material path and
may generate a fallback tangent basis.

The optimization is deliberately evidence-bounded: deterministic ordering,
four-byte packing, and smallest-safe index width are implemented. Vertex-cache
reordering, quantization, mesh compression, LODs, and material baking are not
claimed because they would need independent VANTA performance and visual
acceptance evidence plus a source-map policy for stable authoring identity.

There is no VANTA header, library, build target, runtime service, or private
namespace in Cartographer. VANTA remains an independent consumer. A future
direct import command belongs in a separately governed VANTA adapter and must
add version negotiation, fixture provenance, round-trip/loss tests, and an
independent acceptance run.

The related AI-kernel review is recorded in
[`ai-graphics-kernels.md`](ai-graphics-kernels.md). Its reusable ideas are
explicit dispatch contracts, persistent residency, replay-plan reuse, and
separate telemetry—not a direct CUDA/BF16 kernel port.
