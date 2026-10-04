# Cartographer project format v5

Cartographer v5 keeps the v4 authoring envelope, topology lineage ledger, and
optional evaluation-graph reference, then adds a bounded generic scientific
model record. New saves write v5. The parser continues to read v1 through v4
documents; legacy documents receive an empty scientific model and are upgraded
to v5 on their next save.

```text
CARTOGRAPHER_PROJECT 5
AUTHORING "cartographer.authoring" "meters" "right_handed_y_up"
NAME "..."
REVISION <unsigned integer>
OBJECTS <count>
OBJECT <id> <parent-id-or-zero> "name" <TRS> <visible> <locked> <mesh-id-or-zero>
MESHES <count>
MESH <id> <mesh-revision>
VERTICES <count>
VERTEX <id> <x> <y> <z>
FACES <count>
FACE <id> <vertex-count> <vertex-id>...
TOPOLOGY_RECEIPTS <count>
TOPOLOGY_RECEIPT <mesh-asset-id> "<serialized topology receipt>"
EVALUATION_GRAPH_DIGEST <64 lowercase hexadecimal characters>
SCIENTIFIC_MODEL "<serialized CARTOGRAPHER_SCIENTIFIC_MODEL 12 record>"
END
```

`TOPOLOGY_RECEIPTS` is required by v5 and remains historical lineage, not an
alternate mesh source of truth. `EVALUATION_GRAPH_DIGEST` remains optional.
`SCIENTIFIC_MODEL` is required by v5 even when its record contains no entities;
this makes the absence of scientific state explicit and prevents a partially
recognized future record from looking like a complete project.

The scientific record is a provider-neutral authoring substrate. It carries
typed stable IDs for regions, boundaries, interfaces, sources, loads,
parameters, fields, probes, studies, result sets, morphology structures,
attachments, landmarks, field-linked morphology growth domains, functional genes,
developmental programs and traces with optional tissue assignments and
parameter-value evidence,
lineage/mutation/recombination receipts (the nested lineage codec is v3 and
preserves v1/v2 readability plus optional external source identifiers), and bounded morphometric observations, signatures,
comparisons, and non-persisted bilateral landmark and signature-comparison
previews, plus sequence-genome contigs and variants, anatomy/tissue ontology-part
mappings with ontology version/source-release metadata, typed tissue-region records
with optional structure/region bindings,
stage/developmental/physical metadata, and geometry bindings,
field-visualization descriptors with derived CPU
scalar sample-to-color and vector glyph previews, and provider-import
lineage receipts with explicit preservation dispositions, plus derived
forward/reverse phenotype-capability trace queries. It validates parent hierarchies, cross-entity references,
duplicate IDs, finite numeric values, bounded collections, explicit
UTC provenance timestamps, and non-zero content digests.
A field or result set can reference a study and a region without claiming that
a solver ran.

Functional-genome diffs remain derived inspection data: they report added,
removed, and changed genes, group-derived modules, and changed neural values.
They also report whether the developmental-program reference, lineage
reference, or source digest changed.
They do not interpret sequence variants or silently promote sequence records
into functional genes.

Functional-genome records also retain an optional source digest and lineage
reference beside the developmental-program reference. These values identify
the authored record's input and ancestry; they are not execution receipts and
do not authorize a provider or runtime. The nested functional-genome codec is
v2 and remains readable from v1 records, which decode with empty lineage and
no source digest.

The standalone `CARTOGRAPHER_FUNCTIONAL_GENOME_EXCHANGE 1` envelope is the
provider-neutral adapter boundary for an imported functional genome. It binds
one source import receipt (including its source digest and preservation report)
to the canonical functional-genome payload and a semantic SHA-256 digest.
Round-trip validation therefore detects payload tampering or accidental
sequence-genome substitution before an external provider adapter is invoked.
The functional genome exposes stable-ID replacement and removal operations for
bounded authoring edits; callers must publish the enclosing project through the
normal journaled transaction path. Re-exporting an edited genome produces a
new semantic digest while retaining the original source receipt.
The envelope is an exchange artifact, not a MENAGERIE validator result or a
runtime admission record; MENAGERIE remains responsible for interpreting and
admitting its own candidate format.

Phenotype capability traces only index provider-produced capability receipts:
they can return a capability's contributing structure/gene/instruction/trace IDs
or the capabilities affected by one such source. They do not compile phenotype,
execute developmental programs, or reimplement MENAGERIE semantics.

Scientific result sets and fields are derived or provider-bound state. They do
not silently mutate authored geometry, convert a sequence into a functional
genome, turn a result into source truth, or grant MENAGERIE/VANTA runtime
authority. Provider execution, genome interpretation, morphology compilation,
and runtime admission remain separate contracts.

The nested scientific serializer has its own version and a 16 MiB bound. Its
version 12 optional morphology (nested morphology schema v2; v1 morphology
records remain readable), functional-genome, developmental-program,
developmental-trace (nested developmental-trace schema v2; v1 trace records
remain readable), lineage (nested lineage schema v3; v1/v2 lineage records
remain readable), morphometric, sequence-genome, anatomy (nested
anatomy schema v3; v1/v2 anatomy records remain readable), and field-
visualization, import-ledger, phenotype-capability, gene-regulatory-network,
anatomy-provider-manifest, and scientific-replay-capsule sections
remain authoring records: they do not execute development, transcription,
translation, variant interpretation, or tissue behavior. The outer project
retains its 128 MiB bound. Unknown project or nested scientific
versions, malformed quoted records, dangling references, cycles, invalid
digests, invalid timestamps, trailing data, and oversized counts fail closed.
The sequence-genome nested codec is v3: v1/v2 records remain readable, while
v3 additionally persists bounded source annotations as native zero-based,
half-open intervals. GFF3 exchange converts explicitly to and from its
one-based, inclusive coordinate convention and requires a reference-genome
digest; imported annotations remain source annotations and never become
functional-genome, developmental, phenotype, or runtime state. The import
BED6 exchange uses the same native interval layer, encodes stable identity in
the BED name, and makes its integer score conversion explicit; BED6 fields
that have no native representation are not silently promoted to richer
semantics. GTF 2.2 uses the same reference-bound layer with strict quoted
attribute parsing and explicit one-based-inclusive conversion; unsupported
directives and ambiguous escapes fail closed. The import
ledger has a nested v2 codec that preserves v1 readability and
records bounded source-key dispositions as `preserved`, `converted`, `ignored`,
`unsupported`, `ambiguous`, or `repaired`; an adapter cannot silently turn an
unmapped source record into phenotype or anatomy runtime meaning.
Anatomy-provider manifests use a nested v2 codec that optionally records the
source project revision while preserving v1 readability. When present, the
revision is identity metadata for stale-candidate checks; it is not proof of
provider execution or runtime admission. The explicit authoring-side candidate
gate additionally requires a matching expected source revision, a source
manifest digest, at least one binding, and a digest for every bound geometry;
it still does not perform provider execution or runtime admission.
The model also exposes derived, non-persisted development projections ordered
by stage ordinal or instruction identity. The timeline joins authored
instructions with optional trace entries, while the instruction-impact view
joins one authored instruction to its planned and observed structure changes;
both keep planned changes separate from observed changes and are inspection
data, not execution evidence.
Paired landmark preview requires explicit named correspondence and a shared
coordinate frame; it computes only deterministic displacement and RMS distance
and does not infer an alignment transform or provider-generated morphospace.
Signature comparison requires matching descriptor, coordinate frame,
normalization, component labels, and component count; its CPU reference
metric is Euclidean distance with similarity `1 / (1 + distance)`. These
previews do not overwrite persisted provider comparisons.
Field visualization previews consume caller-provided finite scalar/vector
samples, apply explicit or observed bounds, and produce bounded CPU reference
projections. Scalar samples, slices, contours, and probes map values to
deterministic palette colors; vector previews derive normalized directions,
magnitudes, bounded glyph lengths, and 2D streamline points; and the
isosurface preview uses deterministic marching tetrahedra over a bounded
regular volume with normalized grid coordinates, while the volume preview uses
bounded nearest-sample front-to-back compositing and emits premultiplied RGBA
pixels plus per-ray source ranges. These previews reject forged or non-finite
derived metadata. None is persisted field data, a rendered pixel buffer, a
solver result, or native GPU execution.

Scientific mutations use `ProjectTransaction::set_scientific_model`; the staged
document is validated and serialized into the journal payload before it is
published. Direct mutable access to the project scientific model is not
provided. Save/load reopens the same deterministic bytes and preserves the
model revision boundary.

This format does not claim SQLite/WAL storage, multi-file power-loss atomicity,
solver execution, result visualization, provider import execution, sequence-
genome semantics, anatomy/tissue execution, or private integration behavior.
Replay capsules prove only the named source/configuration identity; they do not
prove that a provider or solver ran. Those remain separate acceptance gates.

The standalone lineage Newick adapter is an exchange surface, not a replacement
for the native lineage codec. It requires a rooted single-parent tree and emits
controlled labels containing stable lineage identity and generation. It rejects
reticulation, branch lengths, comments, and other unsupported constructs rather
than discarding them; generic external labels receive deterministic path-derived
IDs on import. The adapter reports omitted mutation/recombination receipts,
genome/morphology digests, and per-node provenance, and its journaled import
uses caller provenance. Newick data does not imply phenotype, morphogenesis,
solver execution, or VANTA/MENAGERIE runtime admission.

The standalone anatomy ontology-table adapter is likewise semantic-only. Its
bounded envelope preserves namespace/accession, parent term, label, category,
definition, ontology version, and source release with hex-encoded fields and
caller provenance on import. An empty anatomy shell may adopt the declared
schema; a populated or mismatched term set is rejected. Parts, tissue regions,
relations, geometry, ontology reasoning, phenotype behavior, and runtime
admission remain outside this interchange surface.
