# Single-evaluation material bake candidate

Date: 2026-11-12. Isolated material lane; not merged or pushed.

## Implemented

The old bake evaluated the whole material program for every texel twice, then once again at the center. The candidate evaluates it exactly once per texel. It immediately quantizes non-emissive maps and retains three linear emission doubles per texel until the global emission scale is known. Emission quantization then uses those stored values, without another graph/texture evaluation. Center material metadata comes from that same traversal.

For N texels, source-level evaluation count changes from `2*N + 1` to `N`. This is an operation-count reduction, not a measured speedup. Pixel-center coordinates, derivatives, quantization arithmetic, scan order, source digest, and global emission maximum are unchanged for valid inputs.

## Explicit memory tradeoff

Three doubles add 24 bytes per texel of temporary scratch. At this branch's existing 256 cap that is 1.5 MiB; when combined with the separate staged 1024-cap change it is 24 MiB. Six retained RGBA8 maps add another 24 bytes per texel. Thus 1024 bake output plus this scratch is 48 MiB, excluding source texture/library/program storage. No full MaterialSample or graph-register cache is retained. Scratch is released before the result returns.

This branch deliberately does not copy or supersede the uncommitted 1024-cap/export-budget work in the boundary staging checkout. During integration retain that newer resolution/budget guard; the optimization starts after admission and GPU packet validation. Update its memory-budget documentation to distinguish 24 MiB output from 24 MiB scratch.

## Regression source

The prior two-pass algorithm is frozen in a test-only header. New cases compare all six map byte arrays, dimensions, source identity, normal/UV binding, emission scale, center metadata and texture references at 4, 16 and 64 resolution for a plain material and a UV-varying HDR-emission graph. Invalid dimensions and missing executable programs remain refused.

`git diff --check` passed. Source reviewed against the material schema and existing test target linkage. No test executable, compiler, CMake, GPU, image comparison or benchmark was run: build hold remains active. The regression cases are implemented, not passed. This does not close end-to-end visual acceptance.
