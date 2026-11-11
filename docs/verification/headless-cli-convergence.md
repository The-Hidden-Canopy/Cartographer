# Headless CLI convergence

## Invariants and changes

Tangent-normal bake and inspection commands now quote `platform_profile` in
the same way as AO and multi-tile inspection. The existing normal-bake CLI
acceptance exposed the inconsistent output. Artifact bytes, receipt schemas,
source binding, and verification claims are unchanged. Inspecting a stored
receipt still reports readback verification, not a new worker execution.

CPU AO and normal-bake CLI tests always run their CPU paths. D3D12 builds also
register separate `cartographer_ao_d3d12_cli_tests` and
`cartographer_bake_normal_d3d12_cli_tests`, labeled `GPU`, retaining the native
worker/parity checks without silently executing them in a CPU-only selection.

The new `cartographer_preparation_cli_tests` drives the executable end to end
using a synthetic two-asset, four-node fixture with hierarchy and instancing.
It covers profile persistence and digest binding, all four scopes, canonical
selection order, exact publication replay, malformed profiles/IDs, duplicate
and missing selections, source namespace takeover, source revision rollback,
source/selector immutability on failure, relocation with the source absent, and
tampered payload rejection. Successful source advancement and post-failure
inspection are checked as well. Paths contain spaces.

No production parser, publication policy, or migration schema was changed.
No external assets or dependencies were introduced.

## Acceptance boundary

The existing configured Release build includes the headless CLI and gated
D3D12 desktop compilation. The CPU CLI suites cover glTF/GLB export, scene
import, material export, AO, single/multi-tile tangent normals, and preparation.
Native UI, rendered output, GPU bake parity, and destination-runtime admission
are separate gates and are not established by these tests.

On 2026-11-11, the configured Release build passed and the expanded headless
selection passed **216/216 tests** in 156.76 seconds. The selection excluded
`d3d12`, `authoring_gpu`, native/GPU temporal, desktop chrome/input,
builtin-shader-cache, shader-compile, and scene-render CLI tests. The pressure
test retained its complete 64 MiB aggregate fixture. This result supersedes
the earlier 210-test run that did not build or exercise the CLI.
