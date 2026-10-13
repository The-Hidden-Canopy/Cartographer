# DV012 engineering authoring boundary

The public Cartographer slice stores source-linked engineering authoring and
inspection evidence in the project. The Engineering workspace presents a
read-only summary, loads the deterministic DV012 fixture, and exports an
admitted `.carto-inspection` package.

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
save/reopen, undo/redo, UI publication, and explicit refusal to fake a CAD
import.
