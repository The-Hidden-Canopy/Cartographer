# UV seam authoring

## Capability and boundary

The native UV inspector can mark or clear an internal shared edge as a seam for
one UV set, including when the two faces currently have matching endpoint UVs.
Each UV set stores the marks in its own boolean edge-domain layer named by
`attributes::uv_seam_attribute_id`. The inspection report treats an edge as a
seam when it has either an authored mark or a coordinate discontinuity. Marked
edges split edge-connected UV islands; vertex-only contact remains separate.
The existing align action remains available only for coordinate-discontinuous
edges. Aligning coordinates preserves an explicit seam mark. The deterministic
island packer also treats marked edges as chart boundaries, so a same-coordinate
mark produces separate packed charts. Its recipe and artifact identities are
version 3 because chart connectivity now includes the explicit seam layer.

The mutation is human-only and enters project history through
`SetProjectMeshCommand`. It binds to the project generation and revision, mesh
revision, object, UV set, selected face, and expected prior marker value. It
rejects missing UV sets, stale markers, boundary edges, wrong selection,
hidden or locked objects, locked shared instances, no-op requests, and AI
proposals. The layer persists as ordinary mesh attribute data, so save/reopen,
undo, and redo use the existing project persistence path.

## Evidence authored

- `tests/uv_material_contract_tests.cpp` covers same-coordinate explicit marks
  in seam counts and island splitting.
- `tests/uv_island_pack_tests.cpp` covers a marked same-coordinate edge becoming
  separate packed charts and updates the expected algorithm identity.
- `tests/application_tests.cpp` covers human history, AI refusal, wrong-face
  selection, boundary-edge refusal, stale marker refusal, and save/reopen plus
  undo/redo persistence.
- `apps/desktop/main.cpp` wires the mark/clear control and preserves the
  coordinate alignment control for discontinuous edges.
- `libs/production/src/uv_island_pack.cpp` consumes the same per-set marker
  layer as the inspector and validates its edge-domain type and cardinality.

These regression cases are authored but unrun. No build, CTest, save/reopen
execution, or native click-through acceptance is claimed.

## Remaining limits and risk

This is explicit seam marking, not an unwrap, relax, or extraction tool. The
generic edge-attribute transfer path fails closed when a topology edit cannot
map an edge value. A mesh carrying seam markers may therefore refuse an edit
that creates an edge without source provenance until that operation defines an
explicit marker-transfer rule. This avoids silently inventing seam decisions.
Island extraction/repacking and multi-tile bake acceptance remain separate
work.

Only Cartographer contracts changed. RegOS, IDA_Training, hidden_canopy_bot,
and Ask_IDA were not edited; no shared API or dataset contract changed.

## Current-source verification continuation (2026-10-26)

The follow-on build exposed calls to nonexistent `Vec2d` arithmetic in UV
alignment, projected viewport drag, and desktop picking/stroke paths. The
bounded shared repair in `libs/core/include/carto/core/math.hpp` supplies the
missing component arithmetic. It retains the alignment action's existing
`1e-20` squared tolerance and revision, selection, lock, candidate-mesh,
command, and journal checks. No source-mesh authoring rule or object layout is
changed. Initial local arithmetic replacements were removed after the desktop
build exposed the additional shared consumers.

The existing two-face application fixture now exercises actual discontinuous
endpoint alignment, stale endpoint/generation/selection and boundary refusal,
AI refusal, failed journal append atomicity, human success, exact journal counts,
undo/redo, and file readback. Current-source build/tests are being executed;
native pointer/display acceptance remains separate.

Compile-only fixture corrections explicitly unwrap asserted optional mesh
revisions, qualify packing-test application types, and use the existing
human-source test seam for AI refusal. The newly added reopen assertion binds
the canonical full mesh snapshot digest, including UV descriptors and values.
The fixture explicitly recaptures the selected face after each accepted mesh
edit, because the previous component selection retains its captured revision.
The observed stale-revision refusal was kept; the handler was not relaxed.

Executed acceptance: `cartographer_uv_material_contract_tests` and
`cartographer_uv_island_pack_tests` passed; the rebuilt complete
`cartographer_application_tests` passed (1/1, 9.41 s total), including the new
alignment/journal assertions and existing seam marking, packing, history, and
reopen cases. The public UI suite also passed after its fixture corrections,
and the native desktop linked. This supersedes the earlier unrun status for
these local contracts. Native pointer/display, extraction/relax, consumer,
performance, and multi-tile bake gates remain outside this acceptance.

Rollback for this compile repair is the new shared vector methods/overloads and
their direct tests, together with compatible callers; the existing consumers
require those operations to compile. Do not roll back the surrounding
shared-workspace UV implementation as part of that repair. Seam marking,
packing, and alignment still require
explicit human authoring. No automatic seam decision, fabricated texture size,
silent metadata loss, or new schema is introduced by the compile repair.
