# Static scene admission prerequisite

This slice reuses Cartographer's existing bounded glTF/GLB scene and material
parser from `e05fb221050c11338639b2fbac8a852983dec8a3` (introduced in
`8221a89`). It is layered on the isolated native repair at `0f26d9f`.
`native-import-source-manifest.json` binds the three reused source records,
their original Git objects and the adapted file bytes. Original authorship
and license remain those of The Hidden Canopy LLC's Cartographer repository.

## Files and capability

- `libs/io/include/carto/io/gltf_material_import.hpp`: copied material result
  and source-image bytes, with exact artifact digest.
- `libs/io/include/carto/io/gltf_scene_import.hpp`: copied editable meshes,
  hierarchy, material catalog and unsupported-feature warnings.
- `libs/io/src/gltf_material_import.cpp`: existing static triangle parser,
  adapted to the current digest-bound texture decoder.
- `libs/io/CMakeLists.txt`: compile the parser and declare its assets dependency.
- This ledger and the source manifest retain provenance and acceptance limits.

The parser returns copied values. It has no application session, project
mutator, file reader, network client, background job or publication path.
Geometry, UVs, source normals/tangents, transforms and explicit material
assignments remain one source-bound result. Skins, morphs, animation, cameras,
unsupported extensions, unrepresentable culling/samplers and external resources
refuse visibly. A static importer does not satisfy the broader rigged-asset gate.

The adaptation keeps the prior 32 MiB per-image source limit, 2048 dimension,
4,194,304 pixels and 256 MiB aggregate decoded-image budget. It calls the
current PNG/JPEG decoder with the independently inspected source digest and
dimensions; current corruption and unsupported color-metadata checks remain
in force. PNG/JPEG pixel decoding requires Windows Imaging Component on the
current implementation. Other platforms return the existing explicit refusal.

## Governed integration decision

The old `material_library.cpp` adapter was inspected but is not included.
It writes a candidate registry and accepts it through whole-project replacement.
The current `ImportSourceMeshCommand` instead retains source-generation
tombstones and allocates fresh generations during undo/redo. The native append
adapter must reconcile that contract before source imports can become live
project edits. No application action, CLI command, desktop button, schema,
journal envelope, permission or human-approval boundary changes in this slice.

## Repair ledger and acceptance

- Tests added/updated/run: none. This parser is uncompiled and unverified in the
  current integration. Test ownership and execution remain with the coordinator.
- Required validation: bounded source/material admission and normal/UV/sampler
  fidelity, corrupt and oversized inputs, external-reference/unsupported-feature
  refusal, plus current decoder behavior. The prior glTF/JPEG/scene fixtures at
  `8221a89` are historical source references, not a current pass.
- Required later: human-only generation/revision-bound append, source-generation
  history, missing confirmation/stale context refusal, journal failure atomicity,
  undo/redo, save/cold-reopen and actual native authored appearance.
- Risks: the adapted decoder can intentionally refuse inputs accepted by the old
  implementation; compilation and runtime compatibility remain unproven. Supported
  static input remains deliberately narrow. There is no native import completion.
- Rollback: revert this admission commit; keep the native repair and private source
  checkpoint. No authored project or source blob is mutated by this parser.
- Rejected: copying schema 35 from the realism branch, reimplementing its parser,
  substituting missing appearance, silently dropping rigs/animation, porting the
  old live adapter before source-history reconciliation, or unleased validation.

Cross-product impact: no RegOS, IDA_Training, hidden_canopy_bot, Ask_IDA, VANTA
or Condition source changed. Public IO APIs are additive; existing saved schemas
and prepared-scene contracts are unchanged. No dataset/eval or deployment change.
Dataset/training progress: none. Capability progress: source admission reuse,
pending current execution evidence. Promotion progress: none. All 75 previously
open gates remain open; dependent Condition asset/marketplace production is held.
