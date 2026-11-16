# Native material review capture (2026-11-12)

Optional test-harness command:
`cartographer_d3d12_scene_renderer_tests --write-material-review <output-folder>`.

Produces three 513x513 native D3D12 PPM captures (sheen weights 0, 0.5 and 1),
plus source/request/pixel and shader identities, adapter and limitations. Each
case is captured twice and must match byte-for-byte in display pixels and depth.
The default test suite is unchanged. The images are synthetic single-triangle
material fixtures, not a production asset, photorealism proof or native-window
acceptance. Their purpose is reproducible inspectable evidence instead of only
console assertions. No private evaluator is linked or copied.

Built with installed LLVM-MinGW Clang 22.1.8 Debug in the isolated material build.
Command passed on NVIDIA Quadro P5200; six native captures matched in three pairs,
with clean debug/device state. Raw captures and lossless PNG serializations are
in the local review artifact directory; receipt contains all seven shader hashes
for each case. The middle-weight PNG was visually inspected: expected single
triangle, black background, no missing/garbled output. This does not certify
lighting fidelity of a complex asset or the current public main integration.

Remote main still reported 4b994465 at this check. Do not overwrite Opus work;
this is an additional optional test-only handoff after f6e3055, 83d4dbc, 2440520.
