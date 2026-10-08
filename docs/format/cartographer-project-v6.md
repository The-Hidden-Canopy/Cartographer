# Cartographer project format v6

Cartographer v6 is a readable legacy `.carto` authoring format. It
retains the v5 authoring envelope, topology receipt ledger, optional
evaluation-graph digest, and scientific model record, and adds validated mesh
attribute payloads with explicit UV-set identity.

New saves use [project format v7](cartographer-project-v7.md). The parser still
reads v6. Legacy v5 meshes have no
attribute records and load with an empty payload; saving them upgrades the
document to the current format. Unknown versions, duplicate identities, invalid enum values,
non-finite numbers, oversized counts, and mismatched domain cardinalities fail
closed before a document is published.

The v6 mesh section is:

```text
MESH <id> <mesh-revision>
VERTICES <count>
VERTEX <id> <x> <y> <z>
FACES <count>
FACE <id> <vertex-count> <vertex-id>...
ATTRIBUTES
DOMAIN_COUNTS <count>
DOMAIN_COUNT <attribute-domain> <element-count>
LAYERS <count>
LAYER "<stable-layer-id>" <attribute-domain> <attribute-type> <value-count>
ATTRIBUTE_VALUE <typed payload>
UV_SETS <count>
UV_SET <id> "<name>" "<corner-layer-id>" <active-edit> <active-render> <lightmap> <tile-policy-version>
```

Attribute values are serialized according to their declared layer type. UV
coordinates are not duplicated in the UV table: each UV set names one existing
corner-domain `vec2` layer. The table allows at most one active editing set and
one active render set, and every referenced layer must exist and validate.

For vertex, edge, face, and corner domains, the persisted count must match the
materialized mesh topology. Other generic domains remain available for future
authoring payloads but are not silently inferred from mesh topology.

The payload is authoritative editable state, not a glTF-only export detail.
Position-only edits remain valid. Topology edits reject while a payload is
attached until an explicit attribute-transfer result is attached; this avoids
silently misaligning corner UVs or inventing values. VANTA/runtime projection
is outside this format boundary.

`TOPOLOGY_RECEIPTS` remains historical edit lineage and never replaces the
mesh or attribute payload as source of truth. `SCIENTIFIC_MODEL` remains
required for v6 even when empty.
