# Export recipe: WWK Favorite Sedan

How WWK regenerates `wwk_favorite_sedan.gltf` (and its cooked mesh assets)
from the Cartographer authoring source in this directory. WWK implements the
exporter on its side; this document is the contract that exporter must
satisfy. Nothing in this directory builds or exports by itself.

## Source

- File: `projects/wwk/favorite-sedan/favorite-sedan.carto` in this repository
  (Cartographer).
- Identity: namespace `cartographer.project.wwk.favorite-sedan`, project
  schema 34, revision 1.
- Before any export run, execute
  `python projects/wwk/favorite-sedan/validate_favorite_sedan.py` and require
  exit 0. The export must refuse to run on a failing source (fail closed).

## Export steps

1. **Parse** the `.carto` with a line-oriented reader for project schema 34.
   Do not key on numeric object or mesh ids; select the three variant roots
   by namespace + exact root names:
   `Favorite Sedan (clean)` / `Favorite Sedan (hood scratched)` /
   `Favorite Sedan (hood panel)`.
2. **Compose one mesh per variant.** For each variant root, concatenate its
   five panel meshes in canonical panel order — `body`, `hood`, `doors`,
   `wheels`, `front-clip` — preserving the authored box order within each
   panel. All transforms are identity, so no transform math is required.
   Renumber vertices per composed mesh and carry each face's
   `condition.material_region` value alongside the face. Quads are authored;
   triangulate only at WWK's adapter boundary and report that loss in the
   export manifest, per the Cartographer format rule that derived formats
   narrow only at a validated boundary.
3. **Bind material sets per region.** For each composed variant mesh, group
   faces by region id and bind each group to the variant's WWK material per
   the table below (the faithful default reproduces the current runtime
   look; region 2 may later be bound to the mismatched material alone):

   | Region | clean                  | hood-scratched                  | hood-panel                  |
   |--------|------------------------|---------------------------------|-----------------------------|
   | 1 body paint | `sedan-favorite-clean` | `sedan-favorite-hood-scratched` | `sedan-favorite-hood-panel` |
   | 2 hood | `sedan-favorite-clean` | `sedan-favorite-hood-scratched` | `sedan-favorite-hood-panel` |
   | 3 glass | `sedan-favorite-clean` | `sedan-favorite-hood-scratched` | `sedan-favorite-hood-panel` |
   | 4 trim | `sedan-favorite-clean` | `sedan-favorite-hood-scratched` | `sedan-favorite-hood-panel` |
   | 5 wheels | `sedan-favorite-clean` | `sedan-favorite-hood-scratched` | `sedan-favorite-hood-panel` |

4. **Emit `wwk_favorite_sedan.gltf`** with three nodes named exactly like
   the variant roots, one composed mesh per node, and node visibility set to
   the root's authored flag (clean visible, the other two hidden). WWK's
   runtime flips visibility between these three nodes as
   `vehicle:favorite-sedan` damage/repair receipts arrive; no other variant
   switching mechanism is needed.
5. **Record provenance.** The export manifest must state the source path,
   source namespace, source revision, and the validation result from step 1.

## WWK binding table

| Variant root                    | WWK master source object | WWK mesh id | WWK mesh asset id | Material slug                   |
|---------------------------------|--------------------------|-------------|-------------------|---------------------------------|
| `Favorite Sedan (clean)`        | 51                       | 19          | 1025              | `sedan-favorite-clean`          |
| `Favorite Sedan (hood scratched)` | 52                     | 20          | 1026              | `sedan-favorite-hood-scratched` |
| `Favorite Sedan (hood panel)`   | 53                       | 21          | 1027              | `sedan-favorite-hood-panel`     |

WWK replaces its hand-spliced `sedanFavoriteMesh` blockout (currently in
`scripts/build-south-sofia-cartographer-assets.mjs`) with the composed output
of this recipe. The master `south-sofia.carto` object records 51/52/53 keep
their current names, transforms, visibility flags, and mesh/asset ids; only
the mesh geometry source changes.

## Expected differences from the hand-spliced blockout

- **Box order** is panel-canonical (body, hood, doors, wheels, front-clip),
  not the legacy interleaved order. Composed mesh bytes therefore differ from
  today's spliced output; mesh digests and downstream hashes change once at
  cutover. Geometry as a solid is equivalent, and the validator proves the
  per-variant box inventory matches the legacy inventory after folding the
  door band split.
- **Door band split**: the lower body shell is three boxes (front structure,
  door band, rear structure) instead of one. Two extra internal seam quads;
  identical silhouette and volume.
- **Region metadata** is new: faces carry region ids WWK's previous pipeline
  did not have. WWK's per-mesh single-material binding is the default region
  binding, so rendering is unchanged until WWK opts into per-region sets.

## Format constraints WWK must respect

- The source is project schema 34. Newer Cartographer saves use schema 40;
  if this source is ever round-tripped through the Cartographer application,
  the file (and its byte-level formatting) will change. Pin exports to the
  committed file and record its digest; treat any re-saved copy as a new
  revision and re-run validation.
- Vertex and face ids are mesh-local (1-based, dense). Object records carry
  position(3), quaternion(4), scale(3), visible, locked, mesh id — 13
  numeric fields.
- Coordinates are decimal text at up to 5 fractional digits; comparisons in
  the validator are at that precision. Keep numbers finite and in meters;
  do not introduce scientific notation or locale-specific formatting when
  editing the source.
- The trailing schema-34 table skeleton (empty `SCIENTIFIC_MODEL`,
  `WORLD_MODEL`, `MATERIAL_CATALOG`, `PRESENTATION_CATALOG`,
  `CONNECTION_PORT_GRAPH`, and the empty count tables between them,
  terminated by `END`) is part of the format. Preserve it verbatim; a
  missing or reordered table is a parse failure.
- `MATERIAL_CATALOG` is intentionally empty. Material slugs are a WWK-side
  concern; do not add materials to the Cartographer source to express them.
