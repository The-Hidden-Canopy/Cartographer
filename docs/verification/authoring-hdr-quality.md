# Authoring HDR environment quality

Radiance RGBE import remains bounded to 2048×1024 source images and 32 MiB of
encoded input. Its solid-angle-filtered authored grid now retains up to
256×128 samples; larger inputs are still reduced. Existing grids of at most
128×64 keep presentation catalog version 5 and their prior serialized bytes.
Larger grids use version 6; this reader accepts older versions and refuses a
larger grid labeled as an older version. Older readers reject version 6.
The existing presentation catalog and project byte limits still apply.

The standard prefiltered linear RGBA32F atlas is 128×207 (423,936 logical
bytes); the high tier is 256×335 (1,372,160 logical bytes). Both contain seven
directional specular levels, a 32×16 diffuse region and the same 64×64 BRDF
lookup. Products are immutable, tied to the saved source digest and never bake
camera exposure, environment orientation or intensity. The default selects the
high tier for sources wider than 128 pixels. Callers can explicitly compile a
standard or high product and pass it to the viewport; there is not yet a
desktop quality selector or a dynamic GPU-memory budget. GPU cache identity
includes the atlas size as well as the source digest.

On one local Quadro P5200 run, a 256×128 synthetic source containing a single
bright texel produced a roughness-zero sample of 17.9634 in the standard tier
and 28.6488 in the high tier. The corresponding single CPU prefilter timings
were 556 ms and 2,025 ms. A separate native 64×64 viewport fixture reported
1,372,160 upload bytes, 0.417 ms GPU upload and 0.153 ms raster time for a high
atlas; CPU/GPU parity, tier switching, and warm reuse passed. These are
synthetic samples, not sustained prop-scene performance or visual acceptance.

The HDR integration test exercises high-grid project save/reload and refuses
an oversized grid mislabeled with the old catalog version. The presentation
catalog suite passes, except its symlink-escape case was skipped because the
Windows account lacks symlink privilege. Existing CPU environment and native
viewport tests cover source rotation, roughness, clearcoat, mapped normals,
failed replacement, and resident display. See the local integration ledger for
the exact commands and device observations.

Remaining work includes larger source-backed HDR environments without inline
project-text growth, configurable desktop and device-memory budgets, multiple
and area lights in the authoring path, cross-engine material round trips, and
full-resolution prop turntables with human visual review.
