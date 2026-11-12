# Saved-scene exact-neutral physical output compatibility

Date: 2026-11-12. Isolated material candidate based on `470cf52`; not integrated into main.

## Change

Saved-scene preparation admits explicitly evaluated zero transmission, subsurface, and displacement outputs. Previously the mere presence of any of these outputs rejected the graph, even when the author disabled that effect. Exactly positive or negative zero is neutral; no tolerance, float32 narrowing, or clamping is used. Nonzero values, including values too small for float32, still refuse. Graph normal output remains unsupported.

This is a graph compatibility improvement, **not** transmission, subsurface, or displacement rendering. The existing graph/input digests retain explicit neutral outputs in provenance. Missing inputs still fail; a transient nonzero override cannot reuse a neutral result.

## Regression coverage added (not executed)

- All three scalar outputs: zero and negative zero accepted, positive/negative active values and 1e-100 refused.
- Saved-project replay, source immutability, distinct graph identity, and unchanged alpha coverage.
- Required parameter absence refused; durable zero parameter accepted; transient nonzero parameter refused.
- Existing unsupported-output regression changed to use its nonzero node instead of the formerly blanket-rejected zero metallic node.

## Verification and handoff

`git diff --check` passed. Source and test wiring reviewed. No compiler, CMake build, CPU suite, shader compilation, GPU run, native window, screenshot, or visual acceptance was performed because the parent build hold remains active.

Next: integrate the isolated material sequence after ownership review, then run the existing D3D12 scene renderer preparation tests and GPU suite serially. Verify shader/capture parity for the preceding sheen and anisotropy commits before treating this sequence as accepted. The UV agent owns separate geometry/tangent patches; this change touches no UV, exporter, or bake files.
