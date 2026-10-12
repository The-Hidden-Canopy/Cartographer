# Biological authoring boundary

`carto_biological` is the public source-authoring layer for the production-to-
biological continuation. It binds the same revision/digest discipline used by
Cartographer's existing production, scientific, kinematic, and representation
libraries into one bounded document without making Cartographer a biological
runtime.

The document contains:

- `FieldRecord` for orientation, density, strand, weave, feather, granular,
  tissue, skin, injury-overlay, morphology, and biomechanics fields. A field
  names its domain, coordinate frame, value representation, stable seed,
  sample count, coverage relationship, and content digest.
- `StructureRecord` for organism, anatomical, tissue, integument, appendage,
  garment, and surface identities. A structure may correspond to a public
  `KinematicGraph` joint only when the exact graph digest is present.
- `AttachmentRecord` for origin/insertion, containment, contact, overlay, and
  continuation relationships. It carries semantic anchors and evidence, not
  current pose or force state.
- `LayerRecord` for composable microstructure, strand, weave, feather,
  granular, anatomy, tissue, skin, organ, injury-overlay, growth, species, and
  biomechanics layers.
- `StageRecord` for authored morphology stages. It is not a game history or a
  current injury state.
- `RepresentationFamily` for hero, close, gameplay, crowd, and distant
  products. Each product retains a continuity identity and digest-bound
  correspondence to authored layers.

The canonical text format is deterministic and bounded. `validate()` checks
all local values, references, parent cycles, cross-layer correspondence, and
representation continuity. `canonical_digest()` is available only after that
validation succeeds. `verify_reimport_continuity()` rejects source identity
regressions and removal of previously authored identities. A prepared
publication is admitted only when its source and canonical authoring digest
still match the current document.

## Ownership boundary

Cartographer owns the authored records and prepared-product evidence. A private
consumer may lower a prepared product into its own execution representation,
but the public library does not contain provider handles, worker endpoints,
runtime pose, frame buffers, telemetry, health, damage history, or authority to
mutate game state. A faster or cheaper representation never becomes source
truth merely because it was selected for execution.

The library is intentionally a contract and provenance layer. It does not
claim a biological solver, medical simulation, GPU execution, VANTA admission,
or rendered-pixel equivalence.
