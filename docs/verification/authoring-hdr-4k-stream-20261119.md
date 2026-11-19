# Bounded 4K HDR input follow-up

The Radiance RGBE importer accepts Y-first 2:1 sources up to 4096×2048 and
64 MiB encoded. Above 2048 pixels wide it streams decoded pixels into a
solid-angle-weighted lighting grid instead of allocating the full decoded
source. The default grid remains at most 256×128; the explicit high-quality
choice retains at most 512×256. This is **not** a native 4K reflection atlas.
The desktop input limit uses the same shared byte bound. The legacy decoder
entry point, smaller-input algorithm and existing default remain available.

## Verification and evidence

- Before this change, the new 4K test failed against the 2048×1024 limit.
  The final HDR integration and native authoring GPU suites passed (2/2).
  They exercise RLE and flat 4K input, exposure and color correction, weighted
  energy, truncation, input bounds, fractional resampling, axis reversals,
  project v7 save/reload, and a decoded 4K source reaching the native GPU
  environment atlas. The authored environment CPU suite also passed in the
  preceding run. An accidentally started duplicate test run was stopped; the
  independent original run completed successfully.
- The GPU suite ran on an NVIDIA Quadro P5200. Its synthetic 64×64 viewport
  fixture reported 4,841,472 atlas upload/resident payload bytes, 2680.22 ms
  CPU prefilter time and 1.45555 ms GPU upload under shared load. Those values
  are neither a 4K frame benchmark nor peak physical GPU memory. The desktop
  D3D12 compile-only target built; interactive use was not tested.
- Three standalone runs using the actual Release assets library and synthetic
  4096×2048 input measured 402.787–403.504 ms for RLE (548,961 encoded bytes)
  and 593.029–593.799 ms for flat RGBE (33,554,529 encoded bytes), on the
  available Intel Core i7-8750H under shared load. With input already resident,
  process peak working-set increases were 4,349,952–4,546,560 bytes for RLE
  and 4,612,096–4,616,192 bytes for flat input. The flat process total peak was
  42,418,176–42,422,272 bytes. These are process measurements, not whole-system
  or GPU peak memory. Probe sources, binaries, hashes and raw results are under
  `D:/HiddenCanopy/Evidence/E/HiddenCanopy/.scratch/carto-hdr-4k-stream-benchmark-20261119a/`.
- Old and new library probes produced byte-identical decoded grids for the
  tested 512×256, 1026×513 (including reversed axes), and 2048×1024 fixtures.
  This establishes those fixtures, not universal byte equivalence. All allowed
  streamed source heights from 1025 through 2048 were also checked for
  nonpositive latitude overlap weights at both grid tiers.
- The earlier checkpoint documents older-reader v7 rejection and the catalog
  symlink-escape test skipped for lack of Windows symlink privilege. Existing
  desktop and third-party warnings remain; the desktop executable was not
  visually accepted. No real HDR asset, moving turntable, cross-engine match,
  refraction quality or isolated 4K frame performance is established here.

## Rollback and boundaries

A reviewed revert restores the previous 2K/32 MiB input limits. Preserve the
source HDR files and reimport affected lighting at supported dimensions before
using an older decoder; v7 high-detail projects require a compatible reader or
an explicit conversion. The separate scene-refraction prototype and its visible
bottle artifacts are unaffected. The atlas is still capped at 512 pixels wide,
so further environment detail and full pipeline acceptance remain open.
