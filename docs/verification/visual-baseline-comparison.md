# Reusable RGB8 visual baseline comparison

The render library exposes the existing `carto-visual-baseline-v1` RGB8
comparison contract in `carto/render/visual_baseline.hpp`. It preserves the
96×96 D3D12 web-geometry fixture's existing metadata field order, byte encoding,
per-channel tolerance, inclusive changed-pixel limit, and exact platform-label
comparison. Numeric formatting uses the classic locale so the same metadata
is emitted under a caller's custom number-grouping locale.
An LF baseline and its CRLF checkout equivalent compare identically; a lone
carriage return, altered field or added byte remains a mismatch. This matters
because the tracked metadata is LF in Git but may be CRLF in a Windows worktree.

Callers supply dimensions, fixture and platform labels, channel tolerance,
allowed changed-pixel count, two complete RGB8 spans, and an explicit pixel
work budget. Zero/oversized/overflowing dimensions, mismatched span sizes,
missing fixture/backend identity, embedded metadata line breaks or NUL bytes,
and foreign metadata fail closed. Each pixel counts once if at least one
channel exceeds the threshold. The result reports both changed-pixel count and
maximum observed channel delta. Inputs and baseline files are never modified.

`cartographer_visual_baseline_tests` supplies source coverage for metadata
compatibility with the tracked metadata file, locale independence,
identity/tolerance mismatch, threshold
boundaries, multiple changed channels per pixel, byte-count errors, work
budget, zero and oversized dimensions, and malformed metadata fields. The
existing `cartographer_web_geometry_overlay_d3d12_tests` now calls the same
comparison path for its recorded baseline.

The comparator does not capture a foreground window, read BMP files, create a
baseline, authenticate caller-supplied adapter labels, or attest GPU execution.
A match is pixel/metadata comparison evidence only. Foreground presentation,
capture provenance, cross-device coverage, performance, and human review
remain separate gates. New source and regression cases require a fresh
coordinator-granted build and have not been run by this source-only change.
