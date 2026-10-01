# Public render hardware boundary

`carto_gpu` is the first public render-resource boundary described by the
cross-repository architecture specification. It is intentionally backend
neutral and does not include Vulkan headers or expose native API handles.

## Implemented foundation

- `BufferHandle`, `TextureHandle`, `SamplerHandle`, `ShaderHandle`,
  `PipelineHandle`, `MeshGpuHandle`, `MaterialGpuHandle`, and
  `RenderTargetHandle` are typed 64-bit generational values.
- `gpu::Registry<Tag, Value>` invalidates removed handles and advances the
  generation before a slot can be reused.
- `BufferDesc`, `TextureDesc`, `SamplerDesc`, `ShaderDesc`, `PipelineDesc`,
  and `RenderTargetDesc` describe the public intent without naming a backend.
- RHI descriptors now carry explicit format, texture dimension, buffer/texture
  usage, sampler, shader-stage, pipeline-state, and device-capability contracts.
  Descriptor validators reject ambiguous format/usage combinations before a
  backend can create a native resource.
- `carto_assets` retains source texture identity, mip metadata, semantic, and
  color-space requirements. `carto_render` provides a validated
  `StandardMaterial`, deterministic CPU PBR/HDR reference math, and immutable
  render-request quality profiles.
- `carto_render_graph` validates resource descriptors, read-before-write
  hazards, write transitions, stage/format compatibility, and alias lifetime
  overlap. Its output is a deterministic compiled description, not a GPU
  submission.

GPU handles are session-local derived state. They are never serialized into a
`.carto` project and do not replace authoring asset IDs or mesh revisions.

## Deferred runtime claims

`carto_gpu` does not yet provide a `Device`, queue submission, fences, surface,
swapchain, shader compiler, or native Vulkan execution. Deferred destruction
planning exists, but native resource retirement is still a backend concern.
The optional `carto_vulkan` target remains a separate runtime seam. The current
headless acceptance validates a caller-provided `geometry::CompiledMesh`,
creates an RGBA16F color target and D32 depth target, compiles and loads the
acceptance shaders, creates a render pass and graphics pipeline, uploads the
compiled vertex/index streams, submits an indexed mesh draw, and performs a
bounded host-visible readback with known clear and center-pixel expectations.
The acceptance API also validates target extent against adapter limits and
requires Vulkan 1.1, selects the reacquired adapter by physical-device UUID,
and rejects a compiled mesh whose source revision does not match the expected
render revision. It supports an opt-in 8192x8192 execution check. It is not evidence of
the production PBR path, render-graph execution, frame pacing, presentation, or
desktop launch.

The optional Vulkan configure path requires a discoverable SDK and fails closed
when one is absent. With the locally installed SDK explicitly selected, the
headless runtime acceptance creates a logical device, obtains a graphics queue,
validates the selected adapter capability record, and accepts the offscreen
compiled-mesh/indexed-draw/readback rung. This still does not prove PBR, render-graph
execution, submission-lifetime, presentation, or multi-adapter acceptance.

## Boundary rule

The authoring path remains:

```text
EditableMesh -> CompiledMesh(revision) -> render snapshot -> GPU resource
```

No renderer target may obtain a mutable `EditableMesh*`. A future resource
registry may associate an authoring asset ID with an ephemeral GPU handle, but
replacement must remain revision-keyed and old resources must survive until
their submission lifetime is complete.
