# Preparation profile v2

Version 2 is the current preparation profile schema. It extends
[profile v1](preparation-profile-v1.md) with one bounded, canonically ordered
per-product-class policy table: which engine-independent product classes a
preparation includes and the maximum approximation error each class may
carry. Profiles remain bounded, destination-neutral policy records consumed
by the shared [preparation operation](../architecture/preparation-operation.md);
they store no output path, mutable UI selection, native engine type,
credential, cancellation state, or publication authority.

The canonical UTF-8 text grammar is:

```text
CARTOGRAPHER_PREPARATION_PROFILE 2
IDENTITY "<stable profile identity>"
CHANNELS <active-render-uv> <unique-lightmap-uv> <generate-tangents> "<material-region-layer-or-empty>" "<vertex-color-layer-or-empty>"
LOOKDEV <include-lookdev> <require-lookdev-for-material-regions>
PRODUCTS <policy-count>
PRODUCT_CLASS <class-ordinal> <include> <max-approximation-error>
... one PRODUCT_CLASS record per declared policy, ascending ordinal ...
END
```

Boolean fields are exactly `0` or `1`. Empty quoted layer names mean that the
corresponding optional layer is disabled. Disabling the active render UV
also requires lightmap selection and tangent generation to be disabled, and
required lookdev cannot be enabled while lookdev generation is disabled.

## Product-class policies

The class ordinal mirrors `PreparationProductClass` so profiles and prepared
products agree on class identity:

| Ordinal | Product class |
| --- | --- |
| 0 | `render_mesh` |
| 1 | `lookdev_draw_package` |
| 2 | `world_model` |
| 3 | `material_catalog` |
| 4 | `texture_source` |
| 5 | `semantic_sidecar` |
| 6 | `connection_port_graph` |
| 7 | `physical_twin` |
| 8 | `collision_candidates` |
| 9 | `navigation` |
| 10 | `kinematic_graph` |
| 11 | `temporal_clip` |
| 12 | `archetype` |

Omission of a class from the `PRODUCTS` table selects the portable default:
the class is included and only lossless preparation is admitted. The
approximation error budget is the maximum permitted deviation for that class;
zero requires an exact product. Declared policies must be sorted by ascending
class ordinal, unique per class, at most 64, with a finite non-negative
budget; an excluded class must carry a zero budget.

## Compatibility

The reader is bounded to 64 KiB and accepts version 1 and version 2. A v1
profile predates per-product-class policies: it reopens with every product
class included under the portable exact-only default, and the next supported
save persists it as v2. v2 profiles may carry the `PRODUCTS` table between
the `LOOKDEV` record and `END`, or end directly after `LOOKDEV`. The reader
rejects unknown versions, invalid ordinals, unsorted or duplicate class
policies, policy-count overruns, non-finite or negative budgets, excluded
classes with nonzero budgets, malformed records, and trailing data.

The canonical SHA-256 digest is computed from the exact normalized
serialization and is recorded in each prepared-scene envelope. Loading a
profile does not grant a provider or destination any mutation authority.
