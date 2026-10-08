# Cartographer device runtime

The native graphics docuseries is implemented in bounded stages. The first
public stage keeps graphics semantics owned by Cartographer while allowing
backend work to proceed without leaking D3D12, Vulkan, or other native types
into the authoring and render-graph layers.

## N1: device contract

`carto_device` defines the backend-neutral device boundary:

- adapter identity and capability reporting;
- validated shader binaries with an explicit binary format;
- generational buffer, texture, sampler, shader, and pipeline creation;
- bounded read/write helpers for headless verification;
- queue submission, completion serials, and waits.

The contract is intentionally smaller than a production backend. It does not
serialize native handles, select an adapter, or imply presentation support.

## N2: semantic command IR

`carto_device_ir` is the backend-neutral submission language. It carries
resource transitions, barriers, copies, clears, rendering scopes, viewport and
scissor state, pipeline/resource bindings, push constants, draws, dispatch,
timestamps, timeline synchronization, and presentation intent.

Streams are validated before a device may execute them. Validation rejects
unbalanced labels and rendering scopes, stale-looking handles, invalid extents
and ranges, duplicate bindings, non-finite clear values, and malformed draw or
timeline commands. A deterministic textual dump is available for receipts and
backend parity diagnostics.

## N3: CPU reference device

`carto_device_cpu` is the semantic oracle for the first execution rung. It
supports single-level RGBA32F/D32F 2D resources, buffer copies, color and
depth clears, tightly packed RGBA32F texture upload/readback/copy, indexed
triangle rasterization with position/UV streams, depth testing, simple
blending, nearest/linear sampled texture bindings, timestamps, timeline
checks, and readback. Unsupported formats, filtered blits, presentation, and
other operations fail explicitly rather than silently becoming successful
no-ops. Sampled textures require an explicit shader-readable resource state;
the reference binding contract is one RGBA32F texture at set 0/binding 0.

This device is a reference and acceptance backend, not a performance claim. It
does not prove D3D12, Vulkan, shader compilation, presentation, HDR/PBR
fidelity, 8K throughput, or GPU utilization.

## N4: render-reference semantics

`carto_render` now includes deterministic CPU contracts for bounded shadow-map
percentage-closer filtering and temporal history resolve. Shadow input depth
values are validated, kernel radius is bounded, and edge taps are explicit.
Temporal history is rejected on revision mismatch or camera cuts, motion can
reject individual pixels, and history is clamped around current samples.
These APIs establish numerical and stale-data behavior; they are not shader,
render-graph, presentation, or native GPU evidence.

## N5: GPU preparation

`carto_render` exposes aligned `GpuFrameConstants`, `GpuObjectConstants`,
`GpuMaterialConstants`, `GpuLightConstants`, and `GpuTemporalConstants`.
Material and light packers reject non-finite or non-representable values rather
than truncating authoring data. Public render-graph construction uses ordinary
resource and pass descriptors without publishing private execution identities
or optimization policy. `PipelineDesc` carries an explicit vertex input layout;
the D3D12 backend maps it to native input elements.

The D3D12 target packages forward PBR, depth-shadow, temporal-resolve, and HDR
tone-map HLSL sources. A bounded headless acceptance now executes the forward
PBR source with a sampled material into an HDR RGBA32F color target, runs the
tone-map source, writes and reads a depth-only shadow target, and resolves
current color, history, and motion inputs through the temporal compute source.

The bounded headless D3D12 acceptance is real on the local Quadro P5200:
runtime DXC produces the native shader binaries, graphics and compute work are
submitted, indexed/sampled/depth draws and temporal output are read back,
native adapter/resource ownership and descriptor retirement are exercised, and
the debug-layer receipt is empty. The acceptance also covers typed hidden-window
swapchain resize/present, a bounded desktop launch smoke, and an 8192x8192
temporal dispatch followed by bounded RGBA32F probe readback. It is
graphics-device evidence, not a GPU topology-kernel benchmark, full render-graph
parity, or a general production-renderer claim.

## Backend boundary

The intended production path is:

```text
authoring snapshot -> render graph -> device IR -> selected device backend
```

No backend-specific header is required by `carto_device_ir`, `carto_device`,
or the render graph. The opt-in `carto_d3d12` target owns Windows DXGI/D3D12
headers, deterministic adapter selection, native queue creation, resource
mapping, DXGI-to-Cartographer format mapping, state transitions, buffer and
texture copies, offscreen clears, shader compilation through the runtime DXC
loader, graphics and compute command encoding, sampled-texture and
storage-texture descriptor tables, bounded multi-input compute bindings, debug
receipts, device-loss state, and submission-aware deferred destruction. The optional Win32 presentation seam
owns DXGI swapchain lifecycle, typed back-buffer import, resize, and present
operations. The bounded native receipt covers runtime DXC,
sampled-material PBR/HDR/tone-map, shadow-depth, and temporal-resolve
execution/readback, storage dispatch/readback, indexed draw,
descriptor/resource retirement, typed hidden-window presentation, and empty
debug-layer receipts on the Quadro P5200. Topology authoring and its optimized
kernels remain CPU-authoritative. Full IBL/render-graph scheduling parity,
final output encoding, resize/DPI/input acceptance, GPU performance evidence,
and 8K frame/readback performance remain unverified. Vulkan remains an optional
compatibility path until a separately evidenced removal decision.

## Verification

The default headless preset builds the CPU device and runs
`cartographer_device_tests`. The tests cover a complete indexed triangle
submission and readback, RGBA32F texture upload/sampling/readback, buffer copy
on the copy queue, generational stale handle rejection, command-stream dump
determinism, unbalanced scopes, invalid draw counts, and non-finite clear
rejection. Render architecture tests separately cover the shadow and temporal
reference contracts.
