# DV012 engineering authoring boundary

The public Cartographer slice stores source-linked engineering authoring and
inspection evidence in the project. The Engineering workspace presents source
and topology lineage, loads the deterministic DV012 fixture, and lets an
operator add a manual dimensional requirement against a current topology
binding and review proposed inspection targets. A target decision is a
revision-bound human command: only a proposed target whose active
characteristic, requirement, and source topology are current can be accepted
or rejected. Rejected candidates remain in the authoring record. Requirement
records keep their source text, acceptance rule, source identity, and topology
identity; they do not replace imported or extracted claims. The workspace also
imports a bounded BOM CSV profile and displays source-to-assembly reconciliation
rows, and lets an operator add a sensor and access contract to an eligible
requirement. It exports an admitted `.carto-inspection` package when publication
gates pass.

Characteristic creation is revision-bound and human-only. The application
copies the requirement's current topology bindings, checks the requirement to
characteristic kind mapping, requires explicit sensor, coverage, and allowed
approach values, and marks dependent path products stale. It does not generate
targets or claim that a sensor, robot, or motion plan is runtime-validated.
Material-verification requirements have no supported inspection-kind mapping
in this profile and remain blocked from characterization.

An operator can add a proposed inspection target to a current characteristic
and one of its current topology bindings. The command records the explicit
workcell frame, workcell revision, pose, sensor standoff, and a one-line human
justification. It rejects stale project or authoring revisions, stale source
bindings, unsupported frames, unnormalizable rotations, scaled poses, duplicate
targets, and standoffs outside the characteristic's sensor bounds. A later
workcell revision change blocks target acceptance until a new target is
authored. New targets remain proposed until human review; target authoring
marks existing paths and dependent products stale. Pose entry does not test
reachability, collision, sensor visibility, or execution feasibility.

The workcell panel edits transforms for existing entities and reference frames,
and transforms and dimensions for existing keep-out regions. Every edit is a
human command with a new operator-supplied workcell revision and a one-line
reason retained with the affected item identity and workcell-state digest in
revision history. Existing workcell content cannot be replaced through a
general authoring import. It marks non-rejected targets and path products
stale. Existing source locators remain visible as recorded metadata;
the editor does not import or verify robot models, fixture assets, or
calibration data. These edits describe static workcell context and do not
establish robot reachability or collision results.

## BOM CSV profile

The Engineering workspace accepts a UTF-8 CSV with this exact header order:
`part_number,revision,description,material,quantity,unit`. It supports quoted
commas and escaped quotes, caps input at 4 MiB and rows at the authoring
collection limit, and rejects malformed quoting, extra or missing columns,
blank rows, non-finite quantities, and non-positive quantities. The source
revision is the SHA-256 of the exact file bytes; no supplier revision is
inferred. Re-importing changed bytes creates a new source snapshot and marks
the prior snapshot's reconciliation unresolved while preserving its source
digest and BOM rows.

Reconciliation matches only an exact engineering part external ID. Duplicate
IDs remain ambiguous; missing part IDs remain source-only; omitted placed
parts remain assembly-only. Revision, material, or quantity conflicts are
preserved in the imported source values and the workspace shows those values
alongside the mapped assembly values; the status reports the primary
discrepancy. Quantity comparison is enabled only for `each` and a bounded,
unambiguous occurrence hierarchy; unknown units, missing material, or
cyclic/deep occurrence hierarchies remain unresolved. The importer does not
perform fuzzy matching or unit conversion.

## Import and runtime boundary

The public STEP AP242 provider may recognize a source header, but it does not
claim B-Rep support. `read_assembly` remains unsupported until a CAD kernel is
connected. The fixture is synthetic acceptance data, not a live CAD import.
Inspection-package admission validates the public handoff contract; it does
not prove robot reachability, collision safety, execution feasibility, or
hardware behavior. Those checks remain with the consuming runtime.

## Persistence and failure behavior

Project schema 16 stores a digest-bound `ENGINEERING_AUTHORING` record when
present. Engineering authoring schema 2 retains workcell revision history and
continues to read schema 1 records. Mutations pass through editor commands and
can be undone or redone.
Source invalidation preserves authored identity while marking dependent
products stale; stale authoring cannot be published as admitted. Project
formats 1 through 14 remain readable.

The acceptance tests cover deterministic fixture round-trip, provenance,
tamper rejection, stale-source invalidation, schema-version fencing, project
save/reopen, undo/redo, UI publication, revision-bound manual requirement
authoring, revision-bound accept/reject decisions, AI-proposal rejection,
invalid source/topology/unit and stale-revision rejection, strict BOM CSV
parsing, source-preserving BOM re-import, ambiguous/missing reconciliation,
invalidated BOM evidence, requirement-kind compatibility, missing/contradictory
access constraints, stale source/topology refusal, proposed-target authoring,
workcell-frame revision checks, revisioned entity and keep-out edits,
workcell-history migration and round-trip, journal undo/redo, and explicit
refusal to fake a CAD import. A newly authored requirement is not
publication-ready by itself:
package publication still requires its linked inspection characteristics and
all other package gates.
