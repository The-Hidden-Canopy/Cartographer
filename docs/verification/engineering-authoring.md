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
exports an admitted `.carto-inspection` package when its publication gates pass.

## Import and runtime boundary

The public STEP AP242 provider may recognize a source header, but it does not
claim B-Rep support. `read_assembly` remains unsupported until a CAD kernel is
connected. The fixture is synthetic acceptance data, not a live CAD import.
Inspection-package admission validates the public handoff contract; it does
not prove robot reachability, collision safety, execution feasibility, or
hardware behavior. Those checks remain with the consuming runtime.

## Persistence and failure behavior

Project schema 16 stores a digest-bound `ENGINEERING_AUTHORING` record when
present. Mutations pass through editor commands and can be undone or redone.
Source invalidation preserves authored identity while marking dependent
products stale; stale authoring cannot be published as admitted. Project
formats 1 through 14 remain readable.

The acceptance tests cover deterministic fixture round-trip, provenance,
tamper rejection, stale-source invalidation, schema-version fencing, project
save/reopen, undo/redo, UI publication, revision-bound manual requirement
authoring, revision-bound accept/reject decisions, AI-proposal rejection,
invalid source/topology/unit and stale-revision rejection, and explicit
refusal to fake a CAD import. A newly authored requirement is not
publication-ready by itself: package publication still requires its linked
inspection characteristics and all other package gates.
