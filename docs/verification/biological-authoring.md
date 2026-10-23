# Biological authoring verification

The `cartographer_biological_authoring_tests` target is the acceptance slice
for `carto_biological`.

It covers:

1. deterministic source serialization and canonical digest stability;
2. round-trip preservation of structures, fields, attachments, layers, stages,
   kinematic graph correspondence, and representation variants;
3. hostile trailing input and future schema rejection;
4. cyclic structure rejection and reversed/non-finite field-bound rejection;
5. duplicate representation-tier rejection;
6. prepared-publication receipt validation and stale source/digest rejection;
7. reimport continuity with a changed product digest but preserved authored
   identity and continuity token;
8. same-revision source replacement rejection;
9. a negative-space check that the public serialized record contains no runtime
   pose, game health, or telemetry state; and
10. generic canid and human-proxy authoring records, including distinct
    structures, attachments, morphology/covering layers, authored stages,
    representation receipts, canonical round-trip digests, revision-advanced
    reimport continuity, and same-revision replacement rejection; explicit
    ellipsoid profiles also compile to deterministic editable proxy meshes
    whose source/product receipts reject stale morphology.
11. all 20 registered typed biological layer families, including required
    field kinds, matching layer kinds, explicit uncertainty, and canonical
    schema-3 round-trip alongside generic layers. Generic-only documents
    retain schema-2 encoding. This validates the serialization contract;
    it does not establish family-specific biological CPU simulation or
    reviewed real-organism fidelity.

The `cartographer_application_tests` target verifies that the human-only
biological source action uses the project command/history path, preserves
monotonic source revisions through undo/redo, reopens through `.carto`, and
offers revision-fenced read-only mesh preview with no scene mutation. The
separate scene-materialization action rejects stale project/source revisions,
source-payload mismatches, and product mismatches; success creates an editable
mesh with an exact source/product receipt. Duplicate, undo, redo, and save/reopen
preserve receipt-to-object/mesh linkage. A source-receipted proxy also accepts
human-authored SculptStack records; save/reopen preserves both the stack and
biological receipt. The public viewport remains on base geometry because private
evaluation is outside the public target graph.

The `cartographer_biological_project_tests` target verifies that multiple
biological source documents persist in `.carto` schema 26 through
`ProjectTransaction`. It covers exact byte-digest binding, save/reopen,
schema-27 migration, malformed digest and missing-lineage rejection, initial
import at a preserved external source revision, exact-next revision updates,
reimport continuity, same-revision replacement rejection, stale deletion,
tombstone-preserving deletion/recreation, journal event counts, and invalid
identity rollback. The v26 scene-receipt table is mandatory; tampered product
receipts and omitted receipt sections fail deserialization. Schema-27 projects
still migrate with no invented biological source or instance receipt.

The tests are CPU/reference contract tests. They do not claim downstream consumer
execution, GPU timing, native biological simulation, medical correctness, or a
presented frame. Schema 2 optionally carries bounded ellipsoid proxy profiles (local
center, radii, and tessellation). A clean-room compiler produces deterministic
editable meshes with source/revision and product-digest validation. These are
coarse proxies, not anatomically faithful forms. Schema-1 sources migrate
without invented profiles, and proxy output is not implicitly committed to a
scene or represented as a reviewed production asset. The application API is
human-gated and history-backed; its CONDITION workbench panel supports source
selection, creation/update, bounded ellipsoid parameters, explicit read-only
proxy wireframe preview, and receipt-checked editable scene materialization.
The public Sculpt workspace stores authored records without evaluating them in
the viewport. The desktop Vulkan/ImGui executable builds. Evidence-backed
anatomical fidelity and screenshot-backed interactive GUI acceptance remain open;
the proxy remains explicitly coarse and no biological or medical accuracy is
implied.

The `cartographer_biological_provider_contract_tests` target covers exact
source/request binding, stale revisions and digests, mismatched request/result
identities, malformed operations, caller/hard output-byte budgets, and fetched
artifact digest/length mismatch. The contract is read-only: it does not execute
a provider, attach fields, or publish results. The test source was compiled and
linked against the existing libraries and passed through CTest. The current
tree now configures and builds normally.

The `cartographer_biological_field_visualization_tests` target verifies that
exact digest-bound scalar sample blobs produce deterministic grayscale bytes;
stale or malformed data, unsupported field semantics, non-finite or
out-of-range values, and insufficient budgets fail closed; source revision
changes invalidate the output; and preview leaves authored serialization
unchanged. This is a one-row indexed CPU raster with no spatial sample
positions, not a rendered anatomical or biological field.

The `cartographer_condition_authoring_ui_fixture_tests` target also emits a
synthetic CONDITION wildlife/human `.carto` fixture. It admits separate generic
canid, human, and avian source records with strand/feather source layers, creates
three receipt-bound editable proxies through the human-gated application command,
checks each instance command's undo/redo boundary, records one SculptStack,
and verifies source, receipt, and authored-record persistence after reopen. It prints end-to-end fixture elapsed time as a trace
note, not as a performance benchmark. The fixture asserts the source
payloads contain no donor repository/commit fields, runtime handles, worker
endpoints, telemetry, or private-donor markers. This is a source/transaction
acceptance fixture, not external donor geometry, anatomical validation, or a GUI
screenshot.
