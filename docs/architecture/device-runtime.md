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
than truncating authoring data. `prepare_gpu_render_plan` declares the bounded
shadow, opaque PBR, temporal, tone-map, and optional present passes against the
validated render graph and attaches a `KernelContract` to every prepared pass.
`PipelineDesc` carries an explicit vertex input layout; the D3D12 backend maps
it to native input elements.

The D3D12 target packages forward PBR, depth-shadow, temporal-resolve, and HDR
tone-map HLSL sources. This is native GPU preparation, not shader compilation,
pipeline creation, execution, or performance evidence; those operations remain
paused while the host is used for an ML ablation.

## N6: precision and evidence boundary

`carto_device/kernel_evidence.hpp` makes the software-defined precision
boundary explicit. A kernel contract names its precision role, direction,
requested representation, requested arithmetic, optional backend, and fallback
policy. A local execution receipt records the actual representation, actual
arithmetic, actual path, device identity, source/binary/compiler identities,
submission serial, and any explicit fallback reason.

The validator rejects backend/path mismatches, silent representation changes,
fallbacks not permitted by the contract, missing fallback reasons, and native
receipts without SHA-256 source and binary identity. The process-local ledger
is append-only and has no network/export surface. It deliberately carries no
timing or throughput fields, so experiment claims cannot be manufactured by
the runtime contract. This is evidence plumbing, not a telemetry system.

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
loader, graphics command encoding, sampled-texture descriptor tables, debug
receipts, device-loss state, and submission-aware deferred destruction. The
optional Win32 presentation seam owns DXGI swapchain lifecycle, resize, and
present operations; a compile-only desktop target exercises that boundary
without pretending that swapchain back buffers are already imported into the
device resource registry. The first end-to-end native graphics attempt
currently reaches PSO creation but fails with `E_INVALIDARG`; no GPU
draw/readback/presentation claim is made until that receipt is repaired and
re-run. Native execution and 8K allocation are intentionally paused while a
separate ML ablation uses the machine. Vulkan remains an optional compatibility
path until a separately evidenced removal decision.

## Verification

The default headless preset builds the CPU device and runs
`cartographer_device_tests`. The tests cover a complete indexed triangle
submission and readback, RGBA32F texture upload/sampling/readback, buffer copy
on the copy queue, generational stale handle rejection, command-stream dump
determinism, unbalanced scopes, invalid draw counts, and non-finite clear
rejection. Render architecture tests separately cover the shadow and temporal
reference contracts.
