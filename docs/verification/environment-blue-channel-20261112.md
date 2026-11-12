# Environment blue-channel LUT correction (2026-11-12)

Isolated candidate based on material lane f6e3055. Current remote main inspected:
4b9944651d3b151ed9a30aa919770345dd7506ba. Main retains the same environment.cpp
blob 1bfd8a8cd8fb9c4614aca1104d1d614b8154c910 as the faulty candidate.

## Defect and correction

The LDR panorama decoder indexed its 256-entry linear-channel lookup with the
blue byte's *pixel offset*, rather than the decoded blue byte. This produces
incorrect blue radiance and reads beyond the lookup for ordinary panoramas.
Correct the lookup to `linear_channels[pixels[pixel_offset + 2U]]`.
No admission bound or test tolerance is relaxed.

The existing constant-panorama all-mip test already catches this fault. Added
an explicit blue-to-red SH ratio check and an environment-only diagnostic entry
that executes both LDR and HDR admission/prefilter tests. The ratio avoids
confusing the rounded SH basis constant with a channel-decoding error.

## Executed evidence

Installed LLVM-MinGW Clang 22.1.8, Debug, isolated Ninja build
`build-material-validation-20261112`, target
`cartographer_d3d12_scene_renderer_tests`, serial build: passed.

`cartographer_d3d12_scene_renderer_tests.exe --environment-only`: passed, exit 0.

Full executable after the lookup fix passes the LDR and HDR environment cases,
saved-project scene preparation, opacity, neutral physical outputs, sheen and
anisotropy CPU cases. It then fails at the separate fixture assertion
`fixture binds a saved perspective camera to its scene object`.
This is not a full-suite pass or independent verification of main's 228/228 claim.
No main, shared staging, Opus checkout or denied session was changed.

## Integration

This correction is additional to f6e3055 (receiver-plane PCF and native material
validation). Port those two commits only; do not port dc0a56a, whose default-program
test adaptation is specific to the older local baseline. Main intentionally
rejects non-executable default material programs. Rebuild and run current main
before claiming integrated acceptance.
