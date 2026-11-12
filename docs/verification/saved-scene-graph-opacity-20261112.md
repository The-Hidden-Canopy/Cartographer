# Saved-scene graph opacity candidate

Date: 2026-11-12. Isolated base: `14ef6ba`. This is source-only work awaiting
integration-owner review and validation. No shared integration checkout,
graph-bake exporter, project schema, or shader was changed.

## Capability and invariant

`prepare_project_scene_render` can lower an optional scalar graph opacity
output through the existing `StandardMaterial` coverage path. Effective alpha
is `graph alpha * graph opacity`; absence of the optional output retains the
legacy graph alpha. Both operands independently must be finite and in [0, 1].
An invalid operand fails even if multiplication would yield a valid value,
including two negative factors or a zero factor hiding invalid input.

Authored alpha mode and cutoff are preserved. Masked materials use the
existing cutoff, blended materials use the existing sorted straight-alpha
pipeline, and opaque materials continue to ignore coverage. No alpha mode is
inferred or persisted. This bounded coverage contract is not physically based
transmission, refraction, subsurface scattering, or a new BRDF.

Preparation remains read-only. Existing graph revision/digest pins, durable
typed input sets, per-capture overrides, evaluated identity, material validation,
texture bindings, and aggregate evaluation limits remain authoritative.
No durable format migration or journal mutation is introduced.

## Files changed

- `libs/d3d12/src/project_scene_renderer.cpp`: coverage lowering and explicit
  operand validation; retains rejection for normal, transmission, subsurface,
  sheen, anisotropy, and displacement outputs.
- `libs/d3d12/include/carto/d3d12/scene_renderer.hpp`: public adapter contract.
- `tests/d3d12_scene_renderer_tests.cpp`: preparation and GPU regression source,
  registered in the existing full test executable. The new
  `--graph-opacity-only` option selects only the preparation regression and
  returns before creating a GPU device.
- `docs/capability-matrix.md`, `docs/roadmap.md`, and this handoff: source-only
  evidence, bounded capability and pending acceptance.

## Authored regression coverage

- Opacity 0, 0.5, and 1 under all three authored alpha modes, with unchanged
  cutoff and consistent lowering of one material shared by three objects.
- Optional-output absence preserves existing alpha behavior.
- Out-of-range operands cannot cancel into an admitted product.
- Source bytes remain unchanged; saved-document reopen reproduces exact
  render request identity.
- Missing opacity parameter fails; durable parameters survive serialization;
  a current transient override changes coverage and request identity; invalid
  typed scalar values, stale source revision and wrong source digest fail.
- Unsupported physical outputs remain unsupported even when their value is
  zero and opacity is also present.
- Native regression compares the opacity graph against explicit equivalent
  material factors in HDR and depth, checks masked cutoff, blended color
  response without depth writes, and unchanged opaque coverage policy.

## Validation and remaining risk

`git diff --check` passed. Independent source review found no blocking issue,
including a second pass over the final adversarial test additions. These are
static checks only, not compilation or behavioral evidence.

No build, compiler, test, benchmark, GPU execution, or native visual review was
run in this lane. The integration owner must first build the final combined
tree. Run `cartographer_d3d12_scene_renderer_tests --graph-opacity-only` for
focused preparation coverage, then the full
`cartographer_d3d12_scene_renderer_tests` and required repository gates on the
approved Windows/D3D12 build. The native assertions are authored expectations,
not observed pixel evidence.

The contract adds multiplicative scalar coverage only. Texture-sample graph
evaluation, graph-plus-variant combinations, live authoring preview parity,
advanced material lobes, displacement and production photorealism remain open.
The existing transparent sorting and shadow limitations are unchanged. Users
must deliberately author masked or blended mode for fractional coverage to
affect the image.

Rejected: treating opacity as transmission, silently selecting blended mode,
clamping invalid graph values, ignoring nonzero unsupported outputs, expanding
the material/GPU ABI without end-to-end shading, or claiming unrun tests pass.

Rollback: revert only this isolated patch (or its eventual scoped commit).
Existing graph sources already store opacity, so rollback restores an explicit
unsupported-output refusal without rewriting source data. Dataset and training
progress are unchanged; capability is an unvalidated source candidate and
promotion remains pending.
