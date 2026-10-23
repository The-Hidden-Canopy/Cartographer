# ADR-0010: Retain the optional Vulkan compatibility path

Status: accepted; compatibility-only.

## Decision

Keep `carto_vulkan` as an optional compatibility path. Do not remove it and do
not promote it as Cartographer's production renderer. Configure it only when
the caller explicitly enables Vulkan and provides the SDK and shader compiler.

## Evidence and pressure

The retained headless rung probes Vulkan 1.1, selects an adapter by physical
device UUID, validates the expected mesh revision and target limits, uploads a
compiled mesh, submits an indexed draw to RGBA16F/D32 targets, and performs
bounded readback. The native shell also uses the optional Vulkan surface. Those
capabilities are useful compatibility evidence, while the current acceptance
does not cover production PBR, render-graph scheduling, frame pacing, broad
device coverage, or complete presentation parity. See
`docs/architecture/rhi.md`, `docs/verification/native-shell-proof.md`, and
`docs/verification/acceptance-matrix.md`.

## Rejected options

- Removing the backend now would discard a functioning, separately gated
  compatibility seam without evidence that its maintenance cost outweighs its
  utility.
- Promoting it as the production renderer would exceed the retained evidence
  and conflate the headless acceptance rung with complete rendering parity.

## Guardrails and review

The backend remains opt-in and fails configuration when required prerequisites
are missing. Its presence is not a production-renderer or broad-compatibility
claim. Revisit this decision when retained device evidence and product usage
show whether the path should gain production parity or be removed.
