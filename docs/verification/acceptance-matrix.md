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
| primitive creation uses the command boundary | mesh-object command test | object/mesh creation is reversible and stable IDs survive redo |
| single-face extrusion | face extrusion command test | side/cap topology validates; failed distance is atomic; undo/redo works |
| selection mode isolation | selection state test | changing object/vertex/face modes clears incompatible active selections |
| stale selection context | selection state test | missing IDs and mismatched scene/mesh contexts fail closed |
| selection-to-tool boundary | selected-face adapter test | only one validated face delegates; wrong mode, multiple faces, and stale context do not mutate |
| tool registry boundary | tool registry test | only registered factories dispatch, null commands fail, and history receives successful actions |
| boundary half-edges / symmetric links | topology snapshot test | validated |
| compiler mutates source | compile revision/count assertions | source unchanged |
| stale derived render result | render source-revision test | publication rejected |
| failed command in history | command bus test | failed command absent |
| divergent edit after undo | command bus test | redo invalidated |
| unknown project schema | future-version test | load fails closed |
| invalid project flags | deserialization test | malformed record rejected |
| atomic save failure | missing-parent save test | existing file unchanged |
| external asset traversal | asset reference test | rejected |
| interchange loss | OBJ export/import test | warning retained and surfaced |
| clean standalone build | Debug/Release CMake + CTest | passed locally |

Not yet covered: power-loss fault injection, chunk checksums, crash recovery
journals, GPU/Vulkan execution, desktop input/DPI, fuzz corpora, cross-platform
package installation in a clean VM, and downstream Unreal/Unity acceptance.
