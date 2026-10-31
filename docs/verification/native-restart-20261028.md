# Native rendering restart: isolated source and pending acceptance

## Source provenance

The restart starts at `1f84ed65c7020ca4184c34d2bf06503a2993b9d3`.
Commit `59f297b8f7839c689dbecd95ae7d1635ef9b5e78` preserves 99 pre-existing
source records selected from the transitive library dependencies of `carto_ui`
and `carto_d3d12_scene`, their native/CLI adapters, and the existing source-format
notes. The original dirty checkout was not reset, stashed, or edited. Unrelated
desktop work, tests, and the private local queue were excluded from this import.

`native-restart-source-manifest.json` records original byte hashes and indexed
Git blob identities. The source import is not new implementation authorship or
validation evidence. A frozen acceptance checkout must record its own actual
source, binary, shader, fixture and environment hashes.

The prior realism integration at
`e05fb221050c11338639b2fbac8a852983dec8a3` was inspected. It includes the native
authoring material renderer and the bounded glTF/GLB scene and JPEG material
import route introduced by `8221a89`. Its historical receipts are tied to their
own source revisions and binaries. They are not acceptance of this restart.
Its source and current dirty main both declare project schema 35 with different
uncommitted feature surfaces; a shared version number is not proof that their
source changes can be merged blindly. The existing route must be reconciled and
reused for the next import/display slice.

## Minimal repair

Changed production paths:

- `libs/d3d12/shaders/cartographer_scene_pbr.hlsl`: place `view_projection` at
  `c0` and `model` at `c4` explicitly.
- `libs/d3d12/shaders/cartographer_scene_shadow.hlsl`: place
  `light_view_projection` at `c0` and `model` at `c4` explicitly.
- `libs/ui/src/ui.cpp`: capture the local `quoted_text` helper in
  `biological_summary_text`.

Both shaders mixed implicit matrix placement with explicit later `packoffset`
fields. The host writes the matrices at bytes 0 and 64. The current PBR and
shadow uploads are 192 and 144 bytes respectively; implicit matrices could be
placed beyond those uploads. Earlier native captures and the scene regression
exposed empty geometry despite a successful metadata-only CLI check. Historical
failed images and checks remain failures, not appearance evidence.

The UI summary lambda used a local helper without capturing it, preventing the
current UI library from compiling. Its string formatting and review behavior are
otherwise unchanged.

## Capability delta and invariants

The repair restores the intended host/shader matrix contract and removes the
isolated UI compile defect in source. Correct native output is still unproven.
No project schema, authoring command, journal/domain-event path, permission,
org/source scope, human approval boundary, or publication capability is changed
by these five lines. The imported source baseline has substantial pre-existing
changes and requires independent validation.

No tests were added or edited in this repair. Test authoring, compilation,
execution, benchmarks and native foreground acceptance await a serialized
validation lease. No such job was run during this implementation pass.

## Required frozen acceptance

1. Compile the UI library, scene capture CLI and D3D12 desktop from the exact
   candidate revision. Reflect both shader matrix offsets and verify that the
   complete constant buffers fit their host uploads.
2. Compare the preserved broken source baseline with the repaired candidate
   using a pinned diagnostic scene, identical camera/light/material inputs,
   actual GPU HDR/depth/display readbacks and native captures. Check visible
   geometry, depth ordering under reversed draws, nonuniform/mirrored normals,
   and source-bound texture/UV/sampler behavior. A valid receipt or file size
   alone does not prove visible scene content.
3. Exercise shadow receivers, opaque casters and masked cutouts. Verify source
   and request identity, actual shader/binary hashes, material identity,
   resource ownership, and stale/corrupt/missing-input refusal. Preserve failed
   evidence and do not substitute fallback colors for missing authored inputs.
4. Reconcile the existing realism scene/material importer and native authoring
   route before claiming import/display completion. Bind imported source,
   material catalog, textures, UVs and normals through save/reopen and actual
   native display. CPU or GLB checks alone do not close that gate.
5. For each actual reference model, pin the authored project/catalog and
   prepared products before fixed-view appearance review. Missing Pine Ridge
   authored materials/catalog products need a source-owner handoff. The
   authoring `project::MaterialCatalog` and
   `production::compose_prepared_material_catalog` own that source/product
   boundary; a native consumer must not invent the absent authored appearance.

Synthetic diagnostic room colors are fixture inputs, not final asset quality.
Cartographer readiness comes before dependent Condition character/asset
authoring and marketplace production. Those dependent activities remain on
hold. All 75 open queue gates remain open until their own evidence passes.

The subsequent read-only repository review also identified missing lock checks
in generic component commands and loss of AI source/request provenance in the
durable application mutation envelope. Neither finding is repaired by this
shader/UI slice. They remain separate governed-authoring prerequisites for the
runnable alpha, with negative, journal-failure and happy-path coverage required
under the coordinator's test ownership.

## Repair ledger

- Tests run: none; validation lease pending.
- Tests not run: shader compilation/reflection, UI/scene builds, negative and
  lifecycle suites, native before/after/reopen, input/DPI, sustained performance,
  full repository and external-consumer acceptance.
- Risks: the preserved native source baseline is unverified; the packing fix
  may expose later shader/pipeline or source-adapter failures. Native import and
  authored appearance are not established by this repair.
- Rollback: revert the five-line repair commit only. Keep the imported source
  provenance and original dirty checkout. No saved-project or journal rollback
  is required.
- Rejected: changing the host matrix layout to follow accidental compiler
  placement, suppressing the failed depth regression, accepting a black capture
  from metadata, recreating the prior material importer, blanket-copying the
  dirty checkout, publishing the private queue, or running unleased validation.
- Cross-product trace: no RegOS, IDA_Training, hidden_canopy_bot, Ask_IDA,
  Condition or VANTA production paths are edited. No cross-product API/schema,
  dataset/eval, deployment or authority expansion is introduced by the repair.

Dataset progress: none. Training/prompt progress: none. Capability progress:
isolated source repair awaiting execution evidence. Promotion progress: none;
native rendered quality and full Cartographer readiness remain unproven.

## First frozen compile job and bounded build repair

Lease `45d4d9c9-3dd8-4267-ba35-09d3714e6fcb` froze `dd23017` in an
independent acceptance checkout. Configure passed with Release, LLVM-MinGW
Clang 22.1.8, Ninja, D3D12 enabled, Vulkan/ImGui shell, tests and benchmarks disabled,
and a maximum of four compiler jobs. The requested targets were `carto_ui`,
`cartographer_scene_render`, and `cartographer_d3d12_desktop`.

All eight DXC 1.6 vertex/pixel compilations and reflections completed. Before
the packing repair, PBR matrices were at bytes 192/256 with a 320-byte buffer
against a 192-byte host upload; shadow matrices were at 144/208 with a 272-byte
buffer against a 144-byte upload. After repair, both stages of both shaders
place matrices at 0/64 and report buffers of 192/144 bytes respectively. The
reflection used the renderer's HLSL 2021, strict, optimized, stripped-debug
settings. This confirms that ABI defect; it does not establish rendered output.

The C++ build failed with five diagnostics in the preserved `bake_graph.cpp`:
three calls lacked a `BentNormalExecutionReport` error overload and two uses
of `ProjectDocument::meshes()` had only a forward declaration. No requested
binary was produced. The source manifest remained unchanged and every launched
child completed. Logs, tool/source/shader hashes and commands remain in the
acceptance checkout's `build/native-acceptance-20261028/receipt.json`.

The implementation follow-up adds the missing project header and a bounded
bent-normal diagnostic overload using the existing diagnostic-limit policy.
Its failure, source-ray identity and artifact-validation checks remain active.
No journal, state, permission, source-generation, or approval path changes.
No tests were added or run. This follow-up requires a new frozen build lease;
the failed acceptance source was not patched or rebuilt. Rollback is this
follow-up commit only, retaining `dd23017` and its failed evidence.

Rejected: disabling bent-normal checks, dropping diagnostics to compile,
changing saved schemas, patching the acceptance checkout, or broadening the
completed lease into native or suite acceptance. Dataset/training progress
remains none; capability progress is measured shader ABI repair plus an
unverified compile repair; promotion and native appearance remain open.
