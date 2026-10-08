# Heterogeneous production closure acceptance

The bounded public slice is admitted when:

- a valid cook key and receipt validate deterministically;
- representation, spatial-scope, truth, observer, precision, and materialization
  identifiers change cook identity, remain deterministic across graph insertion
  order, and survive version-two ledger round trips;
- version-one ledgers remain readable and can be rewritten without changing
  legacy unscoped receipt digests;
- a receipt with a stale source revision or digest is rejected with
  `stale_data`;
- the canonical admission-policy digest is stable for semantically equal
  signed-zero values, and a policy change invalidates an earlier decision;
- a bounded decision receipt preserves every candidate, exactly one admitted
  selected index, rejection reasons, context/hardware binding, and a digest
  over nested artifact receipts; duplicate, tampered, stale, or mismatched
  candidates fail closed;
- non-zero GPU timing in a decision requires device-timestamp evidence; host
  timing cannot be promoted into the GPU field;
- error, confidence, resident-memory, cook-time, CPU-time, GPU-time, disk-space,
  and artifact-output-size budgets fail closed, including exact-limit
  acceptance and invalid negative time limits; rejected over-budget output does
  not append a ledger receipt;
- failed, stale, and cancelled receipts cannot carry output;
- exact receipt retries are idempotent, conflicting admitted output for one
  deterministic cook key fails closed before blob mutation, and serialized
  duplicate evidence is rejected rather than silently collapsed;
- a newer non-admitted receipt masks an older admitted receipt in the ledger;
- a bounded cook graph rejects cycles and duplicate/self-dependencies;
- cook order is deterministic and invalidation is downstream-only, including
  for an external source key;
- typed spatial footprints are canonical cook identity; dirty-region traversal
  invalidates overlap plus global dependents, skips unrelated tiles, and fails
  closed for unbound footprints, invalid extents, and incompatible grids;
- schema-v7 projects persist all five provider-neutral world primitives through
  one journaled transaction and reject cycles, dangling cross-references,
  hostile input, future variant revisions, and dangling scene/mesh bindings;
- derived bytes publish only when their digest and size match an admitted
  receipt, and stale reads fail closed;
- a real editable mesh compiles into deterministic version-two bytes with
  stable source provenance, sorted material submeshes, explicit UV seam
  splitting, and optional tangent generation;
- the render-mesh decoder rejects trailing bytes, unsupported flags, hostile
  counts, invalid geometry, incomplete submesh ranges, and non-finite values;
- a journaled and atomically saved project reopens, compiles by mesh asset ID,
  persists and reopens its production decision, publishes through the
  receipt-bound blob store, and makes both the artifact and decision stale
  after an edited/re-saved source changes its canonical digest and revision;
- bounded texture containers are inspected without decode claims; true-color
  TGA decode, semantic RGBA8 mip generation, mip persistence, native-material
  compilation, and mesh/material draw-package persistence reject malformed,
  stale, missing, duplicate, unsupported, tampered, and trailing inputs;
  caller-provided limits cannot raise process allocation ceilings, and the
  native material ABI has exact compile-time size, alignment, and offsets;
- the native D3D12 acceptance path uploads every aligned RGBA8 subresource and
  reads a selected non-base mip back without debug-layer receipts;
- the complete Release suite and this contract's focused test pass.

Run from the repository root:

```powershell
cmake --build build --config Release --target cartographer_production_closure_tests cartographer_production_decision_tests cartographer_world_model_tests cartographer_texture_source_tests cartographer_material_artifact_tests cartographer_lookdev_draw_package_tests
ctest --test-dir build -C Release --output-on-failure -R "cartographer_(production_closure|production_decision|world_model|texture_source|material_artifact|lookdev_draw_package)_tests"
```

This evidence proves the public contract, bounded Cartographer render-mesh,
texture, material, draw-package, and semantic-world authoring, CPU/reference
admission, spatially sparse cook-graph planning, decision replay, and content-addressed derived-byte
replay. The separate D3D12 suite proves bounded mip upload/readback and device
timestamp mechanics. It does not claim VANTA runtime consumption/admission,
remote workers, private selection policy, saved-project-to-runtime acceptance,
or final photoreal acceptance.
