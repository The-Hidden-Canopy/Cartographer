# Cartographer import sprint — local claim

- Lane: human static glTF scene/material append, based on `b9f0e9ccc4a9c06180640d3efe8da7ae596d1cf4`.
- Owner: the assigned Terra import worker, branch `codex/terra-sprint-import-20261028`.
- Exclusive scope: sprint-plan revision 3 import lane, including the assigned application, registry, project, editor, regression and ledger paths. Native UI and shared test registration belong to integration.
- Gate: reuse the bounded glTF decoder, human admission, source registry and journaled history; retain source lineage on undo/redo. No external resources, skin, morph, animation, or fallback appearance.
- Validation: implementation is source-only until the integration owner schedules and runs the combined gate. No build, test, compiler or foreground execution is authorized in this lane.

## Historical source-observed blocker and ownership resolution

The parser returns a temporary one-based mesh list, node hierarchy, material
catalog and encoded images. It has no application caller. A source may contain
multiple meshes and instances of each mesh; materials and image records also
need durable bindings to that same exact source. `SourceImportRecord` currently
contains one `artifact_identity`, while `ProjectAssetRegistry::set_record`,
`validate` and `plan_source_reimport` require distinct relative source paths and
artifact identities. A project-local content-addressed source blob has one
path, so one source record per decoded mesh would violate that registry contract.
The source browser also recognizes one `mesh.asset.<id>` per mesh source record;
an invented scene record does not provide a durable source-to-every-mesh mapping.
The material record has no source identity, while texture provenance alone is
not a complete material/geometry/hierarchy mapping.

`ImportSourceMeshCommand` admits exactly one mesh and creates one root object.
`ReplaceProjectDocumentCommand` restores an older entire snapshot on undo;
its `source_asset_registry_` would revert as well, losing tombstones established
by the import. It therefore cannot safely stand in for the required multi-product
append. The current `ApplySourceReimportPlanCommand` preserves source-generation
lineage but is a registry-only command, not an atomic scene/material append.
These are source observations, not reproduced runtime failures.

The coordinator resolved the ownership request in `cartographer-sprint-plan.json`
scope revision 3, assigning the registry, project and editor seams to this lane.
The implementation below supersedes the earlier no-code blocker.

## Implementation and native caller contract

`ImportGltfSceneAction` is dispatched through `HumanApplicationAccess` with
the current project namespace, generation and revision, source identity, expected retained
source generation (zero for a new identity), source SHA-256, an explicit rights
confirmation and a nonblank single-line justification. The project must be
saved. Before dispatch the native owner must stage the original source bytes and
every embedded image at their exact digests in that project's existing
`.carto-assets/source-blobs` store. The bounded parser can identify those images;
staging is a separate caller operation and this action writes no blobs. Missing,
changed, corrupt or unsupported inputs refuse. Native UI and its staging flow
remain unwired and integration-owned.

The action reuses `io::import_gltf_scene`, maps parser temporary indices to
fresh project IDs, and stages all meshes, nodes, material assignments, images
and samplers before one `ImportGltfSceneCommand` in the normal command bus and
application journal. `SourceImportRecord` v4 retains a typed, ordered mapping
from source/parser product indices to project IDs with a digest over that
mapping and the exact source digest. A parser-provided default sampler uses the
parser's temporary sampler identity; it is not asserted to be a source-array
entry. A mapping is historical provenance, not proof that a product has not
subsequently been edited. A changed or missing bound product is not replaced
by fallback content. Active imported products cannot be removed through the
ordinary object/mesh/catalog removal paths while their source record is active;
an explicit source-detachment workflow is not implemented. Generic source
reimport adoption cannot forge or remove these mappings independently.

Undo removes products and the active source while retaining the registry
tombstone; redo rechecks source and image blobs and allocates the next source
generation. Command failure restores its in-memory pre-state, and the existing
application journal rollback restores document and history on append failure.
Stored blobs are retained, including staged but unused inputs. There is no
whole-project replacement command or parallel history.

Source-record versions 1–3 retain their prior encoding and are readable. New
v4 records are embedded in the existing project source-registry serialization;
older binaries that only understand versions 1–3 must refuse v4 rather than
silently omit it. The enclosing project schema number is unchanged: integration
must explicitly audit this forward-compatibility decision and the divergent
schema-35 branch before delivery. Digest validation detects inconsistent stored
mapping bytes; it is not an authenticated signature against a party able to
rewrite both the mapping and its digest.

## Integration handoff

- Positive: append a source with multiple meshes, repeated mesh instances,
  parented nodes and distinct material/texture bindings to a nonempty project;
  preserve source bytes, all remapped product identities, transforms and
  material regions through undo, redo, save and cold reopen.
- Negative: wrong human admission, stale project generation/revision or source
  generation, duplicate or conflicting identity/path, invalid image/source,
  unsupported skin/morph/animation/external references and failed journal
  append must leave project, registry, history and durable event state unchanged.
  Undo must retain tombstones; redo must advance the same source generation.
- Required wiring: integration owns native shell/UI invocation and pre-staging,
  `tests/CMakeLists.txt`, combined build and the Gate 1 journey.
- Test source added: `tests/application_native_import_sprint_tests.cpp` covers
  old/v4 codec, mapping tamper, two-mesh hierarchy and colors, rights and stale
  refusals, unsupported skin/malformed input, failed journal append, undo/redo,
  tombstones and save/reopen. It is unregistered and unrun. Texture fixtures,
  external references, AI-source admission and failing undo/redo journal paths
  need integration coverage. No compile, build, runtime or native acceptance ran.
- Risk: source and image staging is caller-owned and may leave advisory orphan
  blobs. Code and tests have not been built or run. Import provenance does not
  authenticate a rewritten source or mapping. Source removal/detachment and
  broad renderer/native acceptance remain open; no promotion is claimed.
- Rollback: revert the eventual scoped commit or remove the uncommitted patch;
  preserve existing user blobs. Existing v4-imported projects require explicit
  compatibility handling before rolling back to an older reader.
- Rejected: whole-project snapshot replacement that rewinds registry lineage,
  fake per-mesh paths, unbound meshes/materials, silently dropping unsupported
  glTF features, a parallel history and running unleased validation.
- Cross-product: no RegOS, IDA_Training, hidden_canopy_bot, Ask_IDA, VANTA or
  Condition change. The application C++ API and source-record codec are changed;
  no dataset/training change is made. Source capability is unvalidated and
  promotion is not advanced; Cartographer native acceptance remains open.
