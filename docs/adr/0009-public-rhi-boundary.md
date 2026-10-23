# ADR-0009: Public backend-neutral RHI boundary

Status: foundation accepted; native backend execution deferred.

## Decision

Expose backend-neutral resource descriptions, typed generational handles, and
a headless frame-graph compiler through `carto_gpu` and `carto_render_graph`.
Do not expose Vulkan handles, private donor headers, or a mutable authoring
mesh through those targets.

## Consequences

- stale GPU references fail deterministically after slot reuse;
- render graph hazards are diagnosed before a future executor submits work;
- project files remain independent of session-local GPU allocation;
- native device, queue, fence, swapchain, and deferred-destruction work can be
  added behind this seam without changing authoring ownership.

## Non-claim

Headless graph compilation is not Vulkan execution, desktop acceptance, or a
performance result. Those require a real SDK, device, runtime, and retained
evidence.
