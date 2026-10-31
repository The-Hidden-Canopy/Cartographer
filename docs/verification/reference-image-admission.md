# Reference-image admission continuation

The photorealistic-quality completion goal begins with trustworthy source
pixels and rendered evidence. This continuation repairs one bounded PNG
ordering gap; it does not establish photorealistic rendering or complete the
reference-image workflow.

## Finding and repair

At goal start, the shared workspace already contained a PNG zlib/first-block
prefix guard addressing the earlier reserved-DEFLATE-block regression. Its
chunk loop stopped once three prefix bytes had been collected. Later IDAT
chunks could therefore bypass its ordering check.

The new regression failed before repair with `image-data ordering refuses
before codec recovery even after the prefix is complete`. The guard now scans
the complete admitted container. Once a non-IDAT chunk follows image data,
another IDAT is refused before Windows codec recovery. Prefix storage remains
three bytes. Contiguous split IDAT chunks and legal trailing text metadata
retain the exact expected RGBA8 pixels.

## Repair ledger and capability delta

- Files changed: `apps/desktop/reference_image_decoder.cpp`,
  `tests/reference_image_decoder_tests.cpp`, this ledger, and the ignored local
  work queue's goal/evidence notes. Preserve the existing shared-workspace
  decoder/prefix implementation when reviewing or rolling back these hunks.
  A separate integration fix in `libs/ui/src/ui.cpp` supplies the explicit
  `size_t` template argument for two four-item connection-detail sample bounds;
  the current native build exposed mixed-width `std::min` compile errors.
  The four-item bound and read-only review behavior are unchanged.
- Capability gained: image-data ordering is checked after a complete compressed
  prefix, with explicit refusal instead of relying on permissive codec behavior.
- Boundary improved: CRC-correct image-data chunks separated by metadata;
  split points before, within, and after the zlib/first-block prefix; valid
  trailing metadata; exact source pixels and stale source/dimension refusal.
- Governance preserved: decoding remains derived/read-only. Expected source
  digest, dimensions, allocation bounds, source lineage, and human publication
  remain unchanged. No project transition, journal/event, permission, or org
  authority is added. Org scopes, role tiers, justification, and datetimes are
  not concepts in this read-only decoder boundary.
- Tests added: four split positions, each with exact valid-pixel and named
  ordering-refusal checks. Existing reserved-first-block and stale-binding
  regressions remain.
- Tests run: rebuilt `cartographer_reference_image_decoder_tests`; pre-repair
  execution failed as above (0/1, 0.40 s total); repaired execution passed
  (1/1, 0.34 s total). The native desktop and public UI targets built; the
  public UI suite passed (1/1, 2.46 s total). Scoped diff checks passed. The
  current-tree/index and selected-`HEAD` publication audit with ignore-policy
  checking returned `PUBLIC_BOUNDARY_PASS`.
- Tests not run: full configured suites, native pointer/upload/display,
  broad corrupt-stream fuzzing, color-profile fidelity, GPU/PBR quality,
  downstream consumers, and external product suites are outside this bounded
  change. No native visual quality is inferred from a pixel fixture.
- Risks: this is prefix and container-order admission, not a complete DEFLATE
  verifier. Later compressed-stream corruption and JPEG recovery need further
  evidence. The scan adds bounded work over the admitted source chunks and no
  additional source/pixel copies. Native integration is built, while actual
  native interaction/rendered-quality acceptance remains unrun. Existing
  desktop shadowing/conversion and unused-variable warnings remain nonfatal.
- Rollback: revert only the complete-scan/ordering-state hunks and this new
  regression; keep the earlier prefix guard and other shared-workspace changes.
  No saved project or source file is rewritten.
  The two `std::min` type fixes can be reverted independently together with
  compatible callers; they add no schema or runtime authority.
- Rejected: stopping after the prefix, rejecting all trailing metadata,
  inventing replacement pixels, adding an unreviewed codec dependency, or
  treating this test as a final photorealistic frame.

Direct impact is Cartographer reference-image preflight; indirect impact is
earlier refusal of invalid source bytes before native preview. No project/API
schema or shared contract with RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA
changed. No dataset/eval, installation, network, or deployment action occurred.
Those products' suites remain outside this checkout.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: one adversarial source-admission gap repaired. Promotion progress:
local decoder evidence only; photorealistic, native, consumer, and release
acceptance remain open. The overarching goal stays active until every
outstanding task and actual visual acceptance gate is fulfilled.

## Code-only PNG structure continuation (2026-10-27)

Portable source admission now validates ASCII chunk names and the reserved
chunk-type bit, refuses unknown critical chunks, enforces contiguous IDAT
chunks, and checks PLTE uniqueness, placement, applicability, and indexed-color
requirements. Existing CRC, source digest, pixel, and allocation bounds remain
in force. Ancillary chunks such as valid trailing tEXt remain accepted.
JPEG admission also requires one supported SOF followed by a bounded SOS header
whose component count matches its segment length and is followed by scan bytes.
This is structural admission only; the platform decoder remains responsible for
full entropy-stream decoding.

- Files changed: `libs/assets/src/texture_source.cpp`,
  `tests/texture_source_tests.cpp`, this ledger, and the ignored local queue.
- Capability gained: malformed or unsupported PNG structure now fails before
  platform codec handling, including sources that previously looked framed but
  violated critical-chunk or palette rules.
- Boundary improved: unknown critical chunks, invalid reserved chunk bits,
  noncontiguous IDAT, absent indexed palette, forbidden grayscale palette, and
  duplicate palette cases have source regressions; legal palette and ancillary
  metadata paths remain represented. JPEG regressions refuse a missing scan or
  inconsistent SOS component count.
- Governance preserved: admission remains read-only and digest-bound; no
  fallback pixels, state mutation, publication authority, or cross-product
  contract was added.
- Tests added: focused PNG semantic boundary cases in
  `tests/texture_source_tests.cpp`.
- Tests run: none, per the user's instruction not to test during this sprint.
- Tests not run: all tests, builds, codec execution, and rendered/native pixel
  review. Existing workspace evidence above belongs to earlier changes and does
  not validate this continuation.
- Risks: strict structural checks can expose malformed or nonconforming files
  that a platform codec previously attempted to recover. Full DEFLATE
  verification, JPEG entropy-stream/recovery behavior, color-profile fidelity,
  native upload, and rendered quality remain open.
- Rollback: revert the PNG chunk semantic checks and their focused regressions
  together; no project data or serialized schema changes.
- Rejected: treating platform-codec recovery as structural validation,
  rejecting valid ancillary metadata, or claiming complete compressed-stream
  verification from this change.

Direct impact remains Cartographer source-image admission. RegOS,
IDA_Training, hidden_canopy_bot, and Ask_IDA are unchanged; dataset/training and
deployment status are unchanged. Capability progress is source-only, and
promotion/native visual acceptance remain open.

## Code-only animated-PNG boundary continuation (2026-10-27)

Portable PNG source admission now refuses APNG `acTL`, `fcTL`, and `fdAT`
chunks. The managed texture source and decode receipts represent one static
image; accepting an animation container would leave frame selection dependent
on which platform decoder handles it. Static PNG and legal ancillary metadata
remain admitted under their existing structure and color-metadata rules.

- Files changed: `libs/assets/src/texture_source.cpp`,
  `tests/texture_source_tests.cpp`, this ledger, and the ignored local work
  queue.
- Capability gained: animated PNG data cannot enter the static texture path
  with an implicit first-frame or decoder-specific selection.
- Boundary improved: each APNG control/data chunk family has a refusal
  regression while existing static PNG and ancillary metadata coverage stays
  intact.
- Governance preserved: source inspection remains read-only and digest-bound;
  no fallback pixels, project mutation, publication authority, or schema
  change was added.
- Tests added: refusal coverage for `acTL`, `fcTL`, and `fdAT`.
- Tests run: none, per the user's code-only instruction.
- Tests not run: all tests, builds, codec execution, native interaction, and
  pixel/render review.
- Risks: APNG is rejected even if a future caller could use animation-aware
  decoding; adding that capability will require an explicit frame/time-bound
  source contract.
- Rollback: revert the three chunk-name checks and their focused tests; no
  project data or serialized schema changes.
- Rejected: silently selecting a frame based on platform codec behavior or
  treating a single-frame decode receipt as proof that animated source content
  was preserved.

Direct impact is static source-texture admission. RegOS, IDA_Training,
hidden_canopy_bot, and Ask_IDA are unchanged; there is no shared API/schema,
dataset/eval, deployment, or network impact. Dataset, training, and promotion
progress are unchanged; capability progress is source-only, and native visual
acceptance remains open.

## Code-only reference image color-encoding refusal (2026-10-27)

Texture source inspection now surfaces PNG color-encoding chunks, JPEG ICC
APP2 profile markers in the bounded pre-SOS parse and post-SOS marker walk,
JPEG EXIF color-space/gamma tags, and TGA 2.0 gamma/color-correction fields.
The reference-image decoder checks the exact source digest and dimensions first,
then refuses flagged sources because this preview path does not perform a
profile-aware transform. The D3D12 equirectangular
environment loader also refuses them before pixel decode: an explicit
linear/sRGB choice does not establish how embedded profile metadata should be
transformed. The shared RGBA8 decoder used by saved material textures also
refuses flagged sources, after checking the exact source binding. Descriptive
PNG text, ordinary EXIF attributes, and non-color TGA 2.0 extension fields
remain accepted.

- **Files changed:** `libs/assets/include/carto/assets/texture_source.hpp`,
  `libs/assets/src/texture_source.cpp`,
  `apps/desktop/reference_image_decoder.cpp`,
  `libs/d3d12/src/environment.cpp`,
  `libs/assets/src/texture_image_decoder.cpp`,
  `tests/texture_source_tests.cpp`,
  `tests/reference_image_decoder_tests.cpp`,
  `tests/d3d12_scene_renderer_tests.cpp`, this ledger, and the ignored local
  queue.
- **Capability gained:** reference preview, shared RGBA8 texture decode, and
  D3D12 panorama ingestion refuse profile-bearing image inputs before codec
  conversion or material/environment use can imply color-correct display.
- **Boundary improved:** PNG `cHRM`, `gAMA`, `iCCP`, `sBIT`, `sRGB`, `cICP`,
  `mDCv`, and `cLLi` chunks; JPEG ICC APP2 segments, EXIF `ColorSpace` and
  `Gamma` tags, plus TIFF color-characterization tags in EXIF IFD0; and TGA 2.0
  gamma and color-correction fields are surfaced to the caller. Reference-image,
  shared saved-material texture, and D3D12 panorama consumers refuse flagged
  sources before pixel decode. Exact digest/dimension mismatch retains
  stale-data precedence.
- **Governance preserved:** this remains read-only source inspection and
  decoding. No project mutation, serialized schema, or publication authority
  changed.
- **Tests authored:** PNG and TGA color metadata classification versus
  descriptive metadata, JPEG ICC detection before and after SOS, little- and
  big-endian EXIF color-space/gamma parsing and malformed-pointer refusal,
  profile-bearing reference
  and shared texture decode refusal, stale binding precedence, D3D12
  saved-material texture-loader refusal, and D3D12 environment-loader refusal.
  **Run:** none, per the user's code-only instruction.
- **Tests not run:** builds, test suites, WIC pixel decoding for profile-bearing
  sources, D3D12 saved-material or environment decode/execution, native
  upload/display, and visual review.
- **Risks:** this also refuses explicit sRGB metadata even when a future
  color-managed renderer could handle it; the current preview path has no
  verified profile transform. JPEG structural parsing stops at the first SOS;
  post-scan ICC/EXIF detection uses a bounded marker walk, not complete entropy
  or marker validation. TGA detection covers standard TGA 2.0 extension gamma and
  color-correction fields; custom developer data and other undocumented color
  signals are not interpreted. Existing profile-bearing sources may require a
  supported conversion flow.
- **Rollback:** revert the source-admission flag and PNG/JPEG/TGA metadata
  detection, the reference, shared texture, and D3D12 decoder refusals, the
  focused regression cases, and this section. No saved project bytes need
  migration.
- **Rejected:** silently stripping profile metadata, treating WIC conversion as
  proof of a color transform, or displaying profile-bearing pixels as verified.
- **Cross-product trace:** direct impact is Cartographer reference-image
  preview, saved material texture decode, and D3D12 panorama ingestion. Indirect
  impact is a diagnostic field on the shared transient `TextureSourceAdmission`
  C++ contract. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts and datasets are untouched. No
  persisted API/schema changed.
- **Required follow-up:** build and run the source, reference decoder, and D3D12
  scene-renderer suites; then verify a profile-aware conversion policy and
  native display output before admitting profile-bearing images.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress is code-only; promotion, deployment, and visual acceptance remain
open.

## Code-only full PNG DEFLATE verification (2026-10-27)

The Windows texture decoder now validates the complete zlib/DEFLATE stream
across the contiguous PNG IDAT chunks before passing source bytes to WIC. It
checks the zlib header and advertised history window, stored/fixed/dynamic
blocks, Huffman codes, exact decompressed scanline byte count, legal PNG row
filters, the Adler-32 trailer, and the absence of bytes after that trailer.
WIC still supplies the decoded RGBA8 pixels; this guard prevents codec recovery
from turning malformed compressed source data into accepted pixels.

- **Files changed:** `libs/assets/src/texture_image_decoder.cpp`,
  `tests/reference_image_decoder_tests.cpp`, this ledger, and the ignored local
  work queue.
- **Capability gained:** malformed, truncated, checksum-invalid, wrong-sized,
  and overlong PNG image data is refused before platform decoding. Valid stored,
  fixed-Huffman, and dynamic-Huffman fixtures, including length-distance copies,
  retain the exact expected pixels.
- **Boundary improved:** bad Adler-32, truncated DEFLATE and zlib trailers,
  post-trailer bytes, short/long scanline output, and reserved row filters are
  represented alongside all three supported DEFLATE block forms.
- **Governance preserved:** decoding remains read-only and source-digest bound.
  No fallback pixels, project mutation, persisted schema, or publication
  authority is added.
- **Tests authored:** stored/fixed/dynamic valid fixtures and the malformed
  stream cases above in `tests/reference_image_decoder_tests.cpp`.
  **Run:** none, per the user's instruction not to test during this sprint.
- **Tests not run:** every build, test, codec execution, shader compile, and
  native/visual acceptance check. The current source is uncompiled and
  unverified.
- **Risks:** this is a new custom DEFLATE validator and remains unverified; an
  implementation or compatibility defect could reject valid PNG data or admit
  a stream incorrectly. WIC remains the pixel decoder. JPEG entropy/recovery,
  PNG color transforms, native upload/display, and rendered visual quality are
  outside this slice.
- **Rollback:** revert the full-stream validator and these focused fixtures,
  then restore the prior reserved-first-block guard. Keep source/container
  admission and independent material/publication changes intact.
- **Rejected:** trusting WIC's recovery behavior, inventing replacement pixels,
  adding an unreviewed external codec dependency, or claiming visual acceptance
  from source-level changes.
- **Cross-product trace:** direct impact is Cartographer PNG source decoding.
  No shared API/schema with RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA
  changed; no dataset, training, or deployment action occurred.
- **Required follow-up:** review and run the focused decoder suite, then relevant
  source-admission and D3D12 consumer suites; separately verify native displayed
  pixels and visual quality.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress is code-only and unverified; promotion, deployment, and visual
acceptance remain open.
