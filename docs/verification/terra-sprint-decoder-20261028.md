# Decoder lane handoff — 2026-10-28

## Claim and lineage

Exclusive claim: `libs/assets/src/texture_source.cpp`, `libs/assets/src/texture_image_decoder.cpp`, their two public headers, `tests/texture_source_tests.cpp`, `tests/texture_image_decoder_sprint_tests.cpp`, and this handoff. Base `b9f0e9ccc4a9c06180640d3efe8da7ae596d1cf4`; integration owner session `01a14cfd-2c2e-7151-9624-5bec7c0ed57e`. No additional ownership requested. Only the source, existing test, and this ledger changed.

## Source finding and bounded repair

`inspect_jpeg` previously stopped its post-SOS walk on invalid/truncated segment lengths or a premature EOI and returned success. An ICC marker also stopped traversal, hiding later malformed markers. The existing `inspect_texture_source` admission feeds `decode_texture_image_rgba8` (including the WIC path), glTF material inspection/decode and other source consumers. The repair walks all intervening segments and scans to terminal EOI, rejects malformed lengths, empty scans, misplaced markers/data, duplicate frame IDs and unknown/duplicate SOS selectors, and continues metadata inspection after ICC. Existing digest, dimension, source limits and unsupported color-metadata checks remain in their current owners; no caller, journal, project history or authority is added.

`container_inspected` remains structure evidence, not proof of correct entropy or decoded pixels. This repair does **not** certify all JPEG payload corruption: WIC may recover malformed entropy while returning success. A strict, bounded JPEG entropy/codec validator with a documented no-recovery contract and representative baseline/progressive fixtures is a concrete remaining prerequisite before claiming corrupt JPEGs cannot publish. No such decoder was found in the existing assets path. Do not infer this gate or native appearance acceptance from the marker fix.

## Authored tests and integration contract

Existing `cartographer_texture_source_tests` already registers and links this test; no shared CMake edit or new caller wiring required. The existing fake frame-only JPEG fixture was replaced by a complete one-pixel grayscale baseline JPEG fixture. The source suite now checks its exact SHA-256 binding and Windows WIC pixel decode; a real one-pixel PNG checks unchanged source-bound PNG decoding. Negative cases cover malformed and truncated post-scan lengths, early EOI, nested SOI, duplicate frame, bad subsequent SOS, empty scan, data outside a scan, malformed segment after ICC, missing scan, wrong digest/dimensions, byte budget and ICC color refusal. Structural positives cover stuffed bytes and a bounded later scan; these synthetic multi-scan bytes are not asserted to be a decodable progressive image. On non-Windows, PNG/JPEG decode remains unsupported. Integration must execute the source suite on Windows, verify WIC's handling of the exact test JPEG and PNG, and additionally run real baseline and progressive JPEG fixtures, corrupt entropy/recovery cases, and the upstream import refusal/no-publication, journal/undo and save/reopen gates. This lane has no mutation API to test cross-org, roles or journal effects directly.

## Capability delta and governance

Source-observed repair blocks the listed malformed marker paths before source admission or decoded pixels are returned; runtime behavior is unverified. Existing source identity, admission/decode APIs, human command boundary, journal and undo mechanisms are unchanged. No org/permission boundary, schema or authority expansion is introduced. Dataset progress: unchanged. Training/prompt progress: unchanged. Capability progress: code authored, not runtime verified. Promotion progress: none; Gate 1 and native visual acceptance remain open. Condition production remains held.

## Evidence, risks and rollback

Tests/build/compiler/native execution: **not run**, per sprint instruction and shared lease. Scoped diff/source review only. Risks: untested C++/codec fixture compatibility; valid unusual marker sequences may be refused; structural admission alone cannot detect all entropy corruption or WIC recovery; source consumers can inspect without decoding. No cross-repository implementation changes; RegOS, IDA_Training, hidden_canopy_bot and Ask_IDA have no source changes or established direct contract here. The exact JPEG admission behavior is the relevant Cartographer API risk; any downstream adapter must keep existing source binding and mutation history. No dataset/eval change. Deployment is unverified until the consolidated build and native acceptance.

Rejected: silently accepting malformed segments, stopping at ICC, inventing fallback pixels, claiming complete entropy validation, replacing the existing source/history path, expanding owned files, and self-running tests. Rollback: ordinary revert of the lane commit, or reverse the three owned file changes as one unit if delivered as a patch. Preserve unrelated integration and user changes.
