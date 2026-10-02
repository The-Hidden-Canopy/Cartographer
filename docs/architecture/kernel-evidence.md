# Kernel precision and evidence boundary

Cartographer treats a low-bit representation as a storage or operand
contract, not as a claim about the arithmetic executed by a device. The
public device layer therefore records the distinction explicitly:

```text
requested role + representation + arithmetic
                -> actual path + representation + arithmetic
                -> source/binary/compiler identity
```

`carto_device` exposes `KernelContract`, `KernelExecutionReceipt`, and
`KernelEvidenceLedger` for this boundary. A contract identifies the kernel,
operation, precision role, direction, requested representation, requested
arithmetic, optional backend, and whether fallback is allowed. A receipt must
identify the actual representation and arithmetic, the execution path, the
device identity, and the relevant artifact identities.

The prepared render plan carries one contract per shadow, opaque-PBR, temporal,
tone-map, and present pass. These are requested intents only; they become
execution receipts only after a backend actually submits and identifies the
work.

The precision role is intentionally not the arithmetic type. For example,
`int2_delta` may be a compressed operand representation decoded into an FP32
path. A `software_fp32` receipt must say `fp32` arithmetic, and a changed
representation is rejected unless the contract explicitly allows fallback and
the receipt gives a reason. Native D3D12 and Vulkan receipts require SHA-256
source and binary identities plus compiler-recipe identity.

The ledger is process-local, append-only for its lifetime, and has no exporter.
It is evidence available to the local operator, not analytics. It does not
collect timings, throughput, power, prompts, model handles, account identity,
or usage data. Performance and training claims remain separate experiment
artifacts and cannot be fabricated by this contract.

`ShaderBinary::source_digest` and `ShaderBinary::binary_digest` preserve the
source and compiled-bytecode identity for native artifacts. The D3D12 compiler
path derives a compiler-recipe digest from the target profile, entry point,
optimization, and debug options. That field is not presented as a hash of the
DXC executable; a stronger compiler binary identity requires backend support.

The native GPU boundary now has a bounded local D3D12 acceptance tranche. The
headless receipt records Quadro P5200 identity, runtime DXC source/binary
identity, a depth-only shadow draw/readback, a temporal resolve over three
sampled inputs and one storage output, an offscreen sampled-material PBR draw
into an HDR RGBA32F target with depth, an HDR-to-display tone-map draw and
readback, indexed draw/RGBA32F readback, storage-texture compute dispatch and
readback, descriptor/resource retirement, empty debug-layer receipts, typed
swapchain resize/present, desktop launch smoke, and an 8192x8192 target clear.
This does not make GPU geometry
authoring authoritative: the CPU path remains the authoring and kernel-benchmark
authority. Full IBL and production render-graph scheduling/parity, final output
encoding, input/DPI behavior, GPU timing, throughput, power, 8K
frame/readback performance, training, promotion, and outbound telemetry remain
unclaimed.
