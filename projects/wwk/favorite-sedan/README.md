# WWK Favorite Sedan

Cartographer authoring source for the Wolves Without Kings (WWK) **Favorite
Sedan**, the anchor vehicle of WWK's multi-year damage/repair arc
(`vehicle:favorite-sedan`). The sedan exists in three persistent states —
`clean`, `hood-scratched`, `hood-panel` — which the WWK runtime flips between
by toggling object visibility as damage and repair receipts arrive.

This directory is the source of truth for that vehicle. WWK's runtime build
script previously hand-spliced the same geometry into its generated master
project; new exports must be generated from this source (see `EXPORT.md`).

## Files

- `favorite-sedan.carto`: self-contained project in project schema 34, the
  same schema as Cartographer's committed `examples/first-block` authoring
  source. Three variant root objects, named per-panel child objects, and
  per-face `condition.material_region` material regions. No textures, UV
  sets, or external assets.
- `validate_favorite_sedan.py`: standard-library-only validation script.
  Proves the source parses and the WWK variant bindings are complete. Run
  `python validate_favorite_sedan.py` from this directory; it exits non-zero
  on any contract violation.
- `EXPORT.md`: the recipe WWK follows to regenerate `wwk_favorite_sedan.gltf`
  from this source.

## Source identity

- Project schema: `CARTOGRAPHER_PROJECT 34`
- Source namespace (stable source id): `cartographer.project.wwk.favorite-sedan`
- Name: `WWK - Favorite Sedan`, revision 1
- Units: meters. Coordinate contract: right-handed, +Y up. Vehicle
  convention: **front at -Z**, wheels bottom at local y = -0.6, body
  footprint about 2.0 x 4.84 m. All geometry is authored at the vehicle
  origin; every object transform is identity, so panel meshes concatenate
  directly.

The namespace is the durable identity WWK must key on. Object and mesh
**numeric ids in this file are project-local**; WWK binds by namespace plus
the variant root names below, never by bare numeric id.

## Variant states (authored visibility)

Each variant is a root object whose `visible` flag is the authored state.
WWK's runtime keeps exactly one variant visible at a time.

| Variant root object            | Root visible | WWK master source object | WWK mesh id | WWK mesh asset id | WWK material slug              |
|--------------------------------|--------------|--------------------------|-------------|-------------------|--------------------------------|
| `Favorite Sedan (clean)`       | 1            | 51                       | 19          | 1025              | `sedan-favorite-clean`         |
| `Favorite Sedan (hood scratched)` | 0         | 52                       | 20          | 1026              | `sedan-favorite-hood-scratched` |
| `Favorite Sedan (hood panel)`  | 0            | 53                       | 21          | 1027              | `sedan-favorite-hood-panel`    |

WWK master object names match the variant root names exactly, so the mapping
is name-stable across both projects.

## Named panels

Each variant root has the same five child panels, in this order:

| Panel        | Contents                                                                   | Regions present |
|--------------|----------------------------------------------------------------------------|-----------------|
| `body`       | shell front/rear structure, cabin, roof, pillars, windshield, rear glass, trunk deck | 1, 3, 4 |
| `hood`       | hood surface boxes plus the crash/repair evidence strips on the left fender line | 2               |
| `doors`      | door band (middle slice of the body shell) and both mirrors                | 1, 4            |
| `wheels`     | four tires and four hubs; the scratched variant's left-front tire is shoved off line | 5           |
| `front-clip` | front/rear bumpers, grille, headlamps, tail lamps; the crash takes the left headlamp, the repair fits a larger unit | 1, 4 |

The damage arc never touches `body` or `doors`, and touches `wheels` and
`front-clip` only in the `hood-scratched` and `hood-panel` states. The
validator asserts `body` and `doors` are byte-identical across all three
variants, and `wheels` is byte-identical between `clean` and `hood-panel`.

### Body shell split

WWK's original hand-spliced blockout modeled the lower body shell as one box
(0, -0.075, 0 +- 0.95, 0.175, 2.35). This source splits that box into three
z-contiguous boxes — front structure, door band (`doors` panel), rear
structure — so the door line is an authored, nameable panel. The union is
identical to the original solid; the validator folds the three boxes back
and compares against the original WWK box inventory at 5-decimal precision.
The only export consequence is two extra internal seam quads in the composed
mesh (see `EXPORT.md`).

## Material regions

Every face of every mesh carries a `condition.material_region` face-domain
uint32 region id, so WWK binds material sets per region instead of per mesh:

| Region | Meaning                                                              |
|--------|----------------------------------------------------------------------|
| 1      | body paint (shell, cabin, roof, trunk, bumpers, door band)           |
| 2      | hood surface and crash/repair evidence strips                        |
| 3      | glass (windshield, rear glass)                                       |
| 4      | trim (pillars, grille, lamps, mirrors)                               |
| 5      | wheels (tires, hubs)                                                 |

The project's `MATERIAL_CATALOG` is intentionally empty (as in
`examples/first-block`); material binding is WWK-side. The faithful default
binding maps **every region of a variant to that variant's material slug**,
reproducing the runtime blockout, which applies one material per variant
mesh. Region 2 exists so WWK can later bind only the hood to the mismatched
replacement material without re-authoring geometry.

## Validation

```powershell
python validate_favorite_sedan.py
```

Checks, fail closed (exit 1 on the first class of violation):

- schema-34 header, authoring envelope, and stable source namespace;
- full structural parse: object records (13 fields, resolvable parents,
  non-degenerate rotations, mesh bindings cover each mesh exactly once),
  per-mesh vertices/faces/attributes/UV sets with matching counts, and the
  exact empty schema-34 trailing table skeleton terminated by `END`;
- variant tree: three roots with authored visibility 1/0/0, identical five
  panel sets, identity transforms, mesh revision 1, region coverage per panel;
- panel sharing: `body`/`doors` identical across variants, `wheels` identical
  between clean and hood-panel;
- geometry contract: box-winding template, axis-aligned decomposition,
  wheel bottom at y = -0.6, sedan envelope extents, and the folded box
  inventory equal to WWK's original blockout inventory per variant;
- binding completeness: every region 1-5 documented, every variant bound to
  its WWK material slug, and the slug set is exactly WWK's three authored
  sedan material sets.

## License

Authored numeric content for the WWK project; distributed under the
Cartographer repository's `LICENSE`, matching the WWK cross-repo authoring
agreement.
