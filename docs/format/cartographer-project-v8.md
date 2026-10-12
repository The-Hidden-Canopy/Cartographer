# Cartographer project format v8 (legacy)

Cartographer v8 is a readable legacy `.carto` authoring format. New saves use
[project format v9](cartographer-project-v9.md), which persists a stable source
namespace. Version 8
retains every v7 record and adds a required, independently versioned
`MATERIAL_CATALOG` record. The catalog is durable authoring truth: deleting
derived caches, compiled material products, or native engine realizations does
not delete the creator's texture interpretation, material parameters,
variants, sampler intent, or material-region assignments.

The project terminal records are:

```text
TOPOLOGY_RECEIPTS <count>
TOPOLOGY_RECEIPT <mesh-asset> "<receipt>"
EVALUATION_GRAPH_DIGEST <sha256>       # optional
SCIENTIFIC_MODEL "<versioned model>"
WORLD_MODEL "<versioned world model>"
MATERIAL_CATALOG "<versioned source catalog>"
END
```

The nested material catalog contains bounded, stable-ID maps for:

- source textures with project-relative locators, content digests, source
  format, optional known dimensions, provenance, and source revision;
- engine-independent sampler intent;
- typed standard-PBR material source definitions and texture bindings;
- explicit material variants with typed optional overrides and bounded mesh
  scope; and
- mesh/material-region assignments keyed by stable authored region identity.

Catalog validation rejects zero or duplicate identities, absolute or escaping
source paths, unsafe text, unknown enums, non-finite or invalid material
values, unresolved texture/sampler/material/variant references, mismatched
variant scope, and trailing data. `ProjectDocument` additionally requires each
assignment and scoped variant to reference an existing mesh. An assignment
must resolve to a non-zero face-domain `condition.material_region` or
`cartographer.material_slot` value on that mesh.

Mesh replacement fails before mutation when it would orphan an assignment.
Mesh deletion fails while an assignment or variant scope still references the
asset. Catalog changes enter authoritative state only through
`ProjectTransaction`; the journal append remains the commit point and failed
admission emits no event.

The parser continues to read v1 through v7. Those projects receive an explicit
empty material catalog in memory and upgrade only when saved. A v8 document
must contain the catalog record even when it is empty, preventing accidental
field loss from looking like a valid current save.

This public schema contains no VANTA type, Unreal or Unity object handle,
runtime command, provider credential, or native material instance. Destination
adapters may compile richer native products, but those products do not replace
the source catalog.
