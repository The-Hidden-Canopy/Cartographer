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
- `carto_render_graph` validates resource descriptors, read-before-write
  hazards, write transitions, stage/format compatibility, and alias lifetime
  overlap. Its output is a deterministic compiled description, not a GPU
  submission.

GPU handles are session-local derived state. They are never serialized into a
`.carto` project and do not replace authoring asset IDs or mesh revisions.

## Deferred runtime claims

`carto_gpu` does not yet provide a `Device`, queue submission, fences, surface,
swapchain, shader compiler, deferred destruction queue, or native Vulkan
execution. The optional `carto_vulkan` target remains a separate runtime seam.
A passing headless test proves handle and graph invariants only; it is not
evidence of a working GPU, viewport, frame rate, or desktop launch.

## Boundary rule

The authoring path remains:

```text
EditableMesh -> CompiledMesh(revision) -> render snapshot -> GPU resource
```

No renderer target may obtain a mutable `EditableMesh*`. A future resource
registry may associate an authoring asset ID with an ephemeral GPU handle, but
replacement must remain revision-keyed and old resources must survive until
their submission lifetime is complete.
