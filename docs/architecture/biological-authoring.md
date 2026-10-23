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

Schema 2 optionally carries an explicit `EllipsoidProxyProfile` per supported
structure. The clean-room `compile_ellipsoid_proxy()` operation emits an
editable closed mesh within a fixed tessellation budget, with a digest bound to
the exact canonical document and source identity/revision. Product validation
recompiles against the source and rejects stale or modified meshes. These are
coarse geometric proxies only; no anatomy is inferred from semantic labels or
opaque geometry digests, and compilation does not place geometry into a scene.
Schema 1 remains readable and migrates without generated profile data.

`ApplicationSession` admits source updates through a human-only, project-revision-
checked command with journal history and monotonic undo/redo revisions. Source
reads and proxy previews require the captured project revision; preview validates
the payload digest and returns a source-bound product without mutating project,
scene, or history state. The CONDITION workbench exposes a bounded morphology
panel to create or revise source records and preview their coarse proxy
wireframes. Native-authored source identity digests are computed from the
canonical serialized payload with the digest field cleared. Preview remains
non-committing. A separate human-only scene-materialization action requires the
same captured project revision, source revision, exact canonical payload digest,
structure identity, and proxy product digest returned by preview. It recompiles
the proxy before creating an editable scene mesh through the normal project
command/history path. The persisted instance receipt binds the exact source
snapshot, compiler product digest, scene object, and mesh asset. Duplicating an
instance preserves the same receipt lineage; undo/redo and `.carto` save/reopen
keep object and receipt changes atomic. Schema 26 requires its proxy receipt
table and refuses omission or tampering rather than reconstructing provenance.
The receipt describes the materialized base proxy. Subsequent SculptStack edits
remain separate project-authored data and do not rewrite the biological receipt.
The public viewport continues to show the base mesh; private evaluation is not
part of this public authoring contract.

`BiologicalProviderRequest` and `BiologicalProviderResult` bind an external
artifact to the exact authoring source identity, request digest, and a caller
output budget. `validate_biological_provider_result()` is read-only: a stale
source or mismatched request is refused, and an accepted artifact is not
attached to authored fields or published into the project. This establishes a
request/result contract only; it does not execute providers or render spatial
fields.
The artifact retrieval, digest check, and human-only apply boundary are defined
in `docs/architecture/biological-provider-handoff.md`.

`render_scalar_field_preview()` accepts only a content-addressed `CBF32LE1`
blob for a scalar statistical-shading field. The blob contains an exact sample
count and finite float32 values within the authored bounds. It produces a
bounded one-row grayscale raster in sample order, labelled
`DERIVED FIELD PREVIEW - indexed scalar samples; not spatial truth`. The result
binds the source revision, canonical authoring digest, field identity, blob
digest, pixels, and preview digest; validation recompiles against the current
document and exact blob. The operation does not write to the document, infer
sample positions, render geometry, or claim biological correctness. Missing,
stale, malformed, unsupported, non-finite, out-of-range, and over-budget data
are refused rather than replaced with fallback values.

## Ownership boundary

Cartographer owns the authored records and prepared-product evidence. A private
consumer may lower a prepared product into its own execution representation,
but the public library does not contain provider handles, worker endpoints,
runtime pose, frame buffers, telemetry, health, damage history, or authority to
mutate game state. A faster or cheaper representation never becomes source
truth merely because it was selected for execution.

The library is intentionally a contract and provenance layer. It does not
claim a biological solver, medical simulation, GPU execution, downstream consumer admission,
or rendered-pixel equivalence. The ellipsoid product remains a coarse proxy.
The public project format preserves source-bound authoring records but does not
claim anatomical fidelity, medical correctness, or acceptance by a downstream
runtime.
