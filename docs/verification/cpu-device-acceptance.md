# CPU device acceptance

The CPU reference device is the first executable acceptance rung of the
native graphics docuseries. It exists to make command semantics testable on a
machine without a graphics adapter or shader compiler.

## Run

From the repository root:

```powershell
cmake --preset default
cmake --build --preset default --parallel
ctest --preset default --output-on-failure
```

The focused test can also be run after the build:

```powershell
ctest --preset default -R cartographer_device_tests --output-on-failure
```

## Evidence covered

- device identity and queue capability are explicit;
- vertex and index data can be written through the device boundary;
- a validated command stream clears targets, binds a pipeline, draws an
  indexed triangle, and produces deterministic color readback;
- a tightly packed RGBA32F texture can be uploaded from a transfer buffer,
  sampled through an explicit set-0/binding-0 binding, and copied back to a
  readback buffer;
- position/UV vertex streams, nearest/linear sampler behavior, edge clamping,
  and shader-readable resource-state enforcement are covered;
- depth testing and depth writes reject a farther overlapping triangle;
- copy-queue buffer submission preserves bytes;
- destroyed generational handles are rejected as stale;
- unsupported shader formats and presentation are rejected explicitly;
- invalid rendering scopes, non-triangle-aligned indexed draws, and non-finite
  clears are rejected before execution.

## Evidence not covered

These tests do not claim D3D12 or Vulkan execution, DXIL/SPIR-V compilation,
presentation, shader reflection, PBR/HDR shader fidelity, 8K performance,
memory residency, or GPU utilization. CPU shadow-PCF, PBR/HDR, and temporal
reference contracts are separate numerical tests and do not substitute for
backend or hardware receipts.

When the Windows D3D12 option is enabled, the native smoke test is separate:

```powershell
cmake --preset d3d12-headless-release
cmake --build --preset d3d12-headless-release --parallel
ctest --preset d3d12-headless-release -R cartographer_d3d12_tests --output-on-failure
```

That test proves adapter identity, queue creation, D3D12 resource creation,
upload/readback mapping, native copy-queue bytes, offscreen color clear, fence
completion, and destruction. It is not a graphics-pipeline, shader, texture
readback, presentation, or Vulkan-off production acceptance.
