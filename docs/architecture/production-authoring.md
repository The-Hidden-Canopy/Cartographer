# Sculpt and garment authoring boundary

`carto_production_authoring` closes the public source layer before biological
fields are compiled. It deliberately records authoring truth and evidence, not
an interactive renderer or a private runtime solver.

## SculptStack

`SculptStack` binds a mesh asset and base-mesh digest to ordered form,
secondary-form, damage, weathering, and microdetail layers. Each layer owns
bounded brush strokes, surface-constrained samples, symmetry, falloff, mask
identity, settings identity, and optional displacement identity. Parent cycles,
zero identities, non-finite samples, unsorted stroke identity, unsafe surface
domains, and stale source publication all fail closed.

The stack digest is the authoring source key. `SculptPublication` separately
binds a produced product and topology-lineage digest to that exact stack. The
public contract does not claim that the brush kernel, local remesher, or
topology correspondence algorithm is present in this checkout.

## GarmentAsset

`GarmentAsset` records fabric intent, panels, seams, pins, stretch/shear/bend
and collision constraints, and baked/skinned/live product references. Panel
outline/rest-shape digests and weave-field digests remain content-addressed;
pins bind an exact biological body source and structure identity. A garment
cannot silently join panels to a missing fabric, seam to a missing panel, or
pins from multiple body source identities.

`DrapeRequest` and `DrapeReceipt` are a deterministic evidence boundary. A
receipt is admissible only when its request, garment digest, source identity,
and iteration count match. The public library does not create cloth runtime state or claim native/GPU execution.

## Ownership and non-claims

Cartographer owns the authored stack/garment records and their prepared-product
receipts. A downstream consumer or a game may lower them into private execution forms. Runtime
pose, cloth velocity, collision handles, clothing history, game damage, and
telemetry are not part of the public serialization. A product digest never
overwrites source truth.
