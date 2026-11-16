# Thin strand preview sampling, 2026-11-16

The wolf-ear study exposed missing subpixel strand coverage. The authoring
viewport rasterizes once per pixel; its temporal reprojection does not jitter
the projection or add spatial samples. Increasing strand count alone left
speckles and did not repair the ear's flat rim or stretched back texture.

The desktop now starts rendered review at 200% of canvas resolution, bounded
by the existing native 4K/attachment limit or CPU one-megapixel limit. The
existing 100%, 50% and 25% choices remain available. The opaque viewport image
is displayed at canvas size. This supplies four raster samples per displayed
pixel when the target is uncapped and aligned. It is supersampling, not an
analytic hair shader or a guarantee that every thinner strand is resolved.

## Repair ledger

- Changed files: `apps/desktop/main.cpp`,
  `apps/desktop/studio_rendered_preview.hpp`, `tests/CMakeLists.txt`,
  `tests/desktop_preview_sampling_tests.cpp`, and this ledger.
- Capability gained: an uncapped default review resolves a 0.3-pixel strand
  that lies between the previous pixel centers. The old quality choices are
  retained. This does not establish photorealism.
- Boundary improved: invalid scale, nonfinite/empty canvas, aspect underflow,
  extreme dimensions and missing work budgets are rejected or capped before
  target allocation. Existing native and CPU memory limits remain in force.
- Governance preserved: these are derived preview pixels. No document,
  material, saved environment, source identity, transition API, journal event,
  permission, org scope or approval boundary changes. The rendered-preview
  picking guard and presented-live-canvas return gate remain in force.
- Tests: the sampling fixture reproduces total disappearance at 100%, checks
  actual strand identity and shaded radiance at the new default, verifies that
  rendering preserves the source, and exercises native/CPU extent limits and
  malformed inputs. Existing preview return/layout tests are also required.
- Risks: higher uncapped GPU work and memory use; the renderer's existing cap
  can reduce the requested factor. Source geometry and photographic lighting
  defects remain visible. The change does not introduce hair-specific shading.
- Rollback: revert this sampling commit and rebuild the desktop. No project
  migration or asset rollback is required. A user can also select 100%.
- Rejected: widening every strand, smoothing the reference photograph,
  raising renderer budgets, weakening source/history identity checks, or
  promoting the failed ear study as a photorealistic game asset.

## Impact trace

Direct impact is limited to Cartographer desktop review quality. Condition
can consume the new Cartographer pin; its runtime mesh/schema contract is
unchanged. No RegOS, IDA_Training, hidden_canopy_bot or Ask_IDA code, API,
dataset or evaluation artifact is touched. No database migration or deployment
script is involved. Publishing the local dependency revision is separate work.

Dataset progress: no change. Training progress: no change. Capability progress:
bounded spatial preview sampling, subject to recorded test/build results.
Promotion progress: the art remains an unaccepted study. GPU/UI appearance and
game animation/performance require their own evidence; these tests do not
substitute for that evidence.

## Verification on this machine

- Release desktop build succeeded at
  `build/render-work/preview-sampling-review/cartographer_desktop.exe`.
- `cartographer_desktop_rendered_preview_tests` passed.
- `cartographer_desktop_preview_sampling_tests` passed with native D3D12
  enabled, checking both CPU and GPU strand disappearance/recovery (two CTest
  cases passed, 6.91 seconds total).
- Six fixed-camera offscreen D3D12 captures rendered the actual rejected ear
  study at 512 and 1024 pixels. Comparing both at a 512-pixel canvas showed
  reduced speckling at the larger target. No magenta numeric-error pixels were
  present in those captures. This is an offscreen comparison, not GUI parity.
- Full repository tests, interactive desktop presentation and graphical
  Condition launch were not rerun: verification was scoped to the changed
  sampling/extent contract and the existing editing-return guard. Game
  performance and photorealistic art acceptance remain separate follow-ups.
- The previously recorded broader GPU parity tolerance failure is unresolved;
  this repair does not claim a successful full renderer suite.
