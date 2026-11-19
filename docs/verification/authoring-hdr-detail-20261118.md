# Higher-detail authoring HDR — 2026-11-18

The desktop HDR import offers an explicit high-detail choice. It retains up to
512×256 linear Rec.709 source samples instead of the default 256×128 grid.
The automatic lighting product for a source wider than 256 samples uses a
512×591 RGBA32F atlas: 4,841,472 logical bytes. A caller may explicitly build
a 256-wide or 128-wide atlas for the same source. This is a bounded quality
choice, not a claim that the source or output is 4K.

The existing one-argument HDR decoder remains available with its prior default.
The quality-specific decoder has a separate name. Presentation catalogs use
v7 only for environment heights above 128; smaller catalogs retain their v5 or
v6 byte format and 8 MiB limit. A v7 catalog may use up to 16 MiB. Old readers
reject the new version rather than interpreting a larger grid as an older one.
Import, edit history and project save/reload preserve the embedded source.

The local HDR candidate `d2b5bc16956d7433839c43c8f37d7df72e90576c` was
reviewed. Its hard-coded 512-wide atlas alone would have enlarged a source
already filtered to 256×128. This change instead uses the current width-derived
atlas layout and retains a higher-resolution source when explicitly requested.

## Verification

- The final Release build passed the authoring environment, HDR integration,
  presentation catalog and project-number-format CPU tests (4/4), and the
  native authoring viewport GPU suite (1/1). The tests cover
  a narrow bright source, solid-angle filtering, v5/v6/v7 bounds, rejected
  oversized and invalid quality inputs, undo/redo, save/reload, CPU/GPU
  agreement, warm atlas reuse and an explicit lower-memory fallback.
  The catalog suite skipped its symlink-escape case because the Windows account
  lacked the required symlink privilege. A high-precision v7 catalog exceeding
  the old 8 MiB limit also passed project parsing, atomic save and reload.
- The old one-argument decoder symbol and the new quality-specific symbol were
  both present in the built library. A pre-change CLI rejected a saved v7
  project with an explicit version-mismatch diagnostic. The desktop D3D12
  compile-only target built; it was not launched or visually accepted.
- On a synthetic 512-wide source, the CPU reflection peak was 129.758 with the
  256-wide atlas and 553.364 with the 512-wide atlas. One P5200 GPU fixture
  reported 4,841,472 upload bytes, 2302.87 ms CPU prefilter and 1.4567 ms GPU
  upload under shared system load. These are fixture measurements, not 4K
  frame timings, physical residency, or peak memory.
- A proposed sample-hoisting optimization preserved the fixture atlas hash,
  but shared-load timings were too variable to establish an improvement. It
  was removed and the final tests were rerun on the retained implementation.

## Remaining limits

At this checkpoint the HDR decoder rejected sources above 2048×1024 and inputs
above 32 MiB. The later [4K streaming follow-up](authoring-hdr-4k-stream-20261119.md)
raises those input limits while retaining bounded lighting grids. The high-detail
desktop choice has not been exercised interactively. Existing compiler warnings remain in unrelated
application and bundled UI sources. The bottle's scene-refraction band and
gray appearance, offscreen and layered refraction, transparent shadows,
cross-engine matched captures, motion and isolated performance acceptance are
outside these test results and remain open.

Rollback is a reviewed revert of this isolated change. Projects saved with v7
lighting must first be reimported or converted to a supported smaller source
before opening them with an older reader; do not silently discard their detail.
