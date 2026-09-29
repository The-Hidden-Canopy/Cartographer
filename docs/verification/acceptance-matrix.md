# 0.1 acceptance matrix

This matrix records evidence for the implemented foundation. A passing test is
evidence only for the named behavior; it does not promote deferred features.

| Specification pressure | Test evidence | Result |
| --- | --- | --- |
| dangling parent / hierarchy cycle | scene parent and world-transform tests | blocked with diagnostic |
| parent deletion policy | scene removal test | implicit deletion rejected |
| zero-area face / duplicate directed edge | mesh topology test | mutation rejected and source unchanged |
| non-manifold edge | mesh non-manifold test | rejected in 0.1; no silent flattening |
| deterministic primitive construction | box/plane primitive test | dimensions, winding, bounds, and invalid-size rejection covered |
| component edit preserves topology on failure | vertex command test | invalid move rolls back and does not enter history |
| project-bound vertex tool | project vertex tool test | selected second asset changes only that asset; wrong mode, invalid geometry, stale owner, and undo/redo fail safely |
| primitive creation uses the command boundary | mesh-object command test | object/mesh creation is reversible and stable IDs survive redo |
| single-face extrusion | face extrusion command test | side/cap topology validates; failed distance is atomic; undo/redo works |
| selection mode isolation | selection state test | changing object/vertex/face modes clears incompatible active selections |
| stale selection context | selection state and project vertex tool tests | missing IDs, different mesh instances, changed mesh revisions, rebinding, and stale owners fail closed |
| selection-to-tool boundary | selected-face adapter test | only one validated face delegates; wrong mode, multiple faces, and stale context do not mutate |
| tool registry boundary | tool registry test | only registered factories dispatch, null commands fail, and history receives successful actions |
| multi-mesh tool context | tool registry test | object-bound selection resolves its owning mesh asset; an unselected mesh remains unchanged; undo/redo restores both |
| boundary half-edges / symmetric links | topology snapshot test | validated |
| compiler mutates source | compile revision/count assertions | source unchanged |
| stale derived render result | render source-revision test | publication rejected |
| failed command in history | command bus test | failed command absent |
| divergent edit after undo | command bus test | redo invalidated |
| external project edit during tool history | project vertex tool test | revision-conditional undo refuses to overwrite the intervening edit |
| application action boundary | application tests | new/open/save, selection, primitive creation, transforms, tool dispatch, undo/redo, and failed-action diagnostics stay behind `ApplicationSession` |
| failed front-end action | application tests | invalid selection/tool actions record a problem and do not add history |
| UI authority boundary | UI tests | `UiController` routes authoring through `ApplicationSession`; successful project receipts are projected into a bounded operation rail and failed actions do not create UI operations |
| UI shell invariants | UI tests | theme/density/operator/workspace state is bounded; the viewport cannot be hidden; application workspace state remains validated |
| persistence truth and AI admission boundary | UI tests | fresh/new projects report `Not saved`; successful save reports `Saved`; failed save does not claim `Saved`; an unadmitted AI proposal is rejected without revision/history mutation and a normal human action still succeeds |
| edge selection boundary | editor/application/UI tests | stable EdgeId values survive compiled identity mapping; replace/toggle selection is mesh/revision-bound, cross-object/stale/missing edges fail closed, and selection changes do not change project revision |
| unsupported UI capability | UI tests | edge authoring remains explicit as deferred; AI-First discloses unavailable tools and does not create a mutation path |
| UI preference integrity | UI tests plus native source audit | atomic local preference save/load works; malformed or oversized preferences are rejected, reset through the validated workspace path, surfaced as UI diagnostics, and native read/write failures use an explicit warning surface |
| bounded command palette and shortcut errors | UI tests | query size is bounded; failed undo/redo/save shortcuts return diagnostics without error-path crashes |
| failed project open preserves active state | application tests | malformed/unavailable open leaves the current project and selection context intact |
| workspace layout boundary | application tests | empty, unknown, duplicate, or viewport-less layouts fail closed; valid compact layouts update without changing document revision |
| dirty replacement/close boundary | application tests | new/open/close require explicit discard confirmation and preserve the current project before confirmation |
| bounded diagnostics | application tests | repeated failures retain a bounded latest-problem window |
| atomic viewport snapshot | application implementation and render boundary | any visible-instance failure leaves the published viewport scene empty/unavailable |
| compiled hit identity | application tests | viewport snapshot exposes compiled vertices and triangle faces with stable source IDs |
| unknown project schema | future-version test | load fails closed |
| duplicate/trailing project records | round-trip parser test | duplicate mesh assets and data after END fail closed |
| parent record ordering | round-trip parser test | a child may refer to a later parent record without changing hierarchy |
| non-finite and deep hierarchy evaluation | scene boundary tests | overflowing world transforms and hierarchy depth beyond 4,096 fail closed |
| revision exhaustion | revision/parser tests | maximum revisions do not wrap or enter authoring state |
| invalid project flags | deserialization test | malformed record rejected |
| oversized project records | round-trip parser test | top-level collections and face width fail closed at the v1 resource limits |
| atomic save failure | missing-parent and blocked-temporary save tests | existing file unchanged when validation or temporary output cannot proceed |
| external asset traversal | asset reference test | rejected |
| interchange boundary | OBJ export/import test | feature-loss warning retained; oversized face input fails closed |
| clean standalone build | Debug/Release CMake + CTest | passed locally |
| optional Vulkan/desktop prerequisites | negative CMake probes plus local native build/launch evidence | Vulkan configure stops without a discoverable SDK; desktop configure stops unless Vulkan is explicitly enabled; the current checkout also builds and launches a responsive native window against pinned prerequisites |
| typed GPU handle lifetime | render architecture tests | slot reuse advances generation and stale handles fail; type confusion is rejected at compile time |
| render-graph hazards | render architecture tests | valid write transitions compile; read-before-write, illegal stage/format use, duplicate use, and overlapping aliases fail |
| content-addressed blob integrity | storage/journal tests | known SHA-256, bounded storage, read verification, and tamper detection pass |
| package manifest/layout boundary | storage/journal tests | manifest is human-readable, required package directories are validated, top-level JSON fields cannot be supplied by nested lookalikes, escaped text round-trips, malformed/trailing and future versions fail closed, and no fake database is created |
| durable journal integrity | storage/journal tests | revision-bound and in-process concurrent append serialization, cross-process sidecar-lock contention, staged transaction atomicity, snapshot-envelope recovery, exact application envelope binding, payload/hash chain, checkpoint filename/content binding, malformed checkpoint fail-closed behavior, and truncated-record rejection pass |
| mutation authority boundary | project/application/editor/storage tests plus public-header negative compile probe | `ProjectDocument` mutators and publication swap are private to trusted production paths; project-bound editor commands and `ToolContext` require a non-forgeable admission token; low-level model tests use isolated access harnesses; public persistence methods remain read/validation/save operations |
| provider lifecycle boundary | provider/plugin tests | duplicate descriptors, invalid transitions, unavailable provider diagnostics, capability absence, and built-in OBJ importer/exporter readiness fail visibly when violated |
| plugin protocol boundary | provider/plugin tests | sandbox-default manifest, network/path permission rejection, framed round-trip, malformed nested-object rejection, truncation, and payload-size bounds pass |

Not yet covered: SQLite WAL package storage, schema migrations, power-loss fault
injection, patch-specific delta replay, crash recovery UX, GPU/Vulkan execution
beyond the exercised local launch, desktop input/DPI and resize behavior,
native UI visual acceptance, fuzz corpora, cross-platform package installation
in a clean VM, and downstream Unreal/Unity acceptance.
