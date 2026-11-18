# Material-worktree semantic integration, 2026-11-18

Base: d61a226 (lossless embedded PNG compression retained).
Source: eight uncommitted files in carto-material-v5, preserved unchanged with a binary patch outside this checkout.

## Candidate scope
- glTF COLOR_0 import/export using a validated single vertex/corner color4 layer.
- Shared canonical project source identity and per-request per-mesh compilation reuse.
- Repair export attribute whitelist so the newly supported color layer reaches serialization.
- Exact RGBA application-import/export/reimport regression coverage and canonical identity equivalence coverage.

## Explicitly held, not integrated
- The entire glass shader and scene_renderer changes. Independent review found partial-transmission background leakage, ignored blended alpha, and incorrect within-draw interface composition.
- The unrelated 1024-to-4096 shadow resolution increase.
- Larger import/provenance limits, pending bounded memory and admission tests.

Original worktree and source patch remain available; no original files are reset or deleted.

## Verification
Release Clang/Ninja D3D12-enabled build passed. Five focused suites passed: production closure, glTF geometry parity, glTF scene import, glTF material export, and desktop scene-preview camera/request integration. Exact RGBA roundtrips and source-identity equivalence passed. The first parity run retained an obsolete all-colors-unsupported expectation; it now tests multi-layer refusal and preservation of existing output, and the final rerun passed. Independent review found no remaining blocker in retained changes. Full suite, GPU visual acceptance and performance measurement were not run. No visual quality claim or publication implied.

