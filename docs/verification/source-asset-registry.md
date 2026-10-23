# Project source-asset registry and reimport plans

Introduced in Cartographer project schema 21 and retained in the current
schema 31, the `.carto` document stores a bounded source-asset registry. A
record binds a stable source identity to a project-relative path, source
generation and digest, importer identity/version, typed import options, a
stable artifact identity/digest, and optional per-domain digests for geometry,
hierarchy, and material output. The registry retains generation tombstones
after removal, so deleted identities cannot silently restart at an old
generation.

The native OBJ/PLY/STL import path now reads each selected file once under a
128 MiB bound, stages those exact bytes in the project's
`.carto-assets/source-blobs` content-addressed store, and parses that stored
snapshot. A saved-project import binds the source digest, built-in importer
identity/version, declared options, geometry digest, and project-relative blob
path to the created mesh asset. The original workstation path is never written
to the project. `.carto-assets/` is ignored by Git because it contains local
project payloads, not source code. An unsaved workspace can still import, but
the receipt warns that the source is not linked and recommends saving and
importing again.

Reimporting from the same source path reuses its opaque source identity.
Unchanged input is a no-op; a changed geometry replaces the existing mesh
asset in place, while a metadata-only source change preserves the current mesh
and updates only provenance. The mesh change and registry update are one
undoable, journaled command. Undo/redo keep source-generation lineage
monotonic. Parsing happens against a transient project-local snapshot; parser
failure removes that snapshot and never promotes a persistent blob. If a later
command or journal admission fails after the content-addressed write, an
unreferenced blob may remain; it cannot appear in project truth and is retained
until a future safe garbage-collection workflow exists.

The OBJ parser also preserves complete per-corner UV data and finite nonzero
`vn` records in `cartographer.uv0` and `cartographer.normal0` corner layers,
including distinct values at a shared-vertex seam. Its exporter writes active
render UVs, the named custom-normal layer, and Cartographer face material-region
IDs through triangulation; it omits material assets/assignments, additional UV
sets, and unsupported attributes. PLY/STL remain geometry-only, and no built-in
preserves source hierarchy or third-party importer state. The production
render-mesh compiler consumes `cartographer.normal0` by default, splits render
vertices at normal discontinuities, and carries those normals through
deterministic artifact serialization and PBR upload packing. The native
Modify panel now provides a human-only selected-face custom-normal control.
It writes the normalized vector to that face's stable corner records in the
`cartographer.normal0` layer, seeds other corners from the current derived
normals when the layer is first created, and uses the project mesh command so
the edit is revision-bound, journaled, saveable, undoable, and redoable. The
Modify panel also provides human-only selected-edge sharp mark and clear
controls. They write explicit true/false values to the complete typed boolean
`cartographer.edge_sharp` edge layer through a revision-bound project mesh
command; stale selections, shared/locked mesh owners, missing values, and AI
requests fail closed. These are durable source annotations and do not change
render-normal selection. The native `EditableMesh` viewport still draws
geometry-derived normals; normal visualization and multi-face authoring remain
open. There is no automatic
watch/relink flow for a moved original file, missing-source repair UI,
orphan-blob garbage collection, or side-by-side candidate review panel. Stored
source blobs can be large and are not encrypted; projects and their
`.carto-assets/` directories may contain private user data.
The `.carto` project stores the registry's bounded serialization as hex so
embedded line breaks cannot disrupt the surrounding project record stream;
this is encoding, not encryption.

`plan_source_reimport` is read-only and classifies added, unchanged,
metadata-only, geometry, hierarchy, material, multiple-domain, unclassified,
removed, and identity-conflict cases. Applying a plan recomputes it against the
exact base registry digest, refuses stale/forged classifications and unresolved
identity conflicts, then produces a new registry without mutating its input.
Application adoption is a human-dispatched action. It is undoable and redoable;
replay advances source generations monotonically instead of restoring stale
generation numbers. Application mutation journaling records the project
snapshot and operation like other authored changes.

## Verification

- `cartographer_source_asset_registry_tests` covers source-record validation,
  canonical round trips, hostile paths/options, deterministic classification,
  non-mutating preview, stale and forged plan rejection, stable artifact
  identity conflicts, adoption, and retained tombstones.
- `cartographer_obj_uv_tests` covers OBJ UV and custom corner-normal seams,
  exact import/export round trips, malformed/partial payload rejection, and
  the documented lossy material boundary.
- `cartographer_project_source_asset_registry_tests` covers journaled
  transaction adoption, stale-plan refusal, `.carto` save/reopen, schema-20
  migration, removal tombstones, and exact source generation continuity.
- `cartographer_production_closure_tests` covers authored corner-normal
  admission, hard-normal vertex splits, custom-normal tangent generation,
  artifact serialization/reopen, PBR upload propagation, and fail-closed
  missing, zero-length, and wrong-domain normal requests.
- `cartographer_application_tests` covers application-command undo/redo and
  generation advancement across replay, plus human-only selected-edge mark and
  clear receipts, stale-selection rejection, save/reopen, undo/redo, and exact
  edge-layer contents. `cartographer_ai_tests` keeps the edge operation
  unclassified, non-previewable, non-requestable, and not auto-approvable.
- `cartographer_ui_tests` covers the native controller preview/adoption route
  and rejection of raw AI mutation attempts, saved-project OBJ/PLY/STL source
  linkage, absolute-path exclusion, unchanged-import no-op behavior,
  content-addressed source cleanup after parse failure, in-place source
  reimport, undo/redo generation monotonicity, and explicit unsaved-import
  warnings.

## Remaining work

Missing-source reports, orphan-blob reclamation, and an explicit
side-by-side reimport review surface remain open. Moved-source relinking is
implemented as an advisory plan/apply pair over the registry (digest-identical
moves only; changed content fails closed into the reimport flow) journaled
through the application action boundary. The existing
source registry plan API still provides read-only classification and stale
plan rejection; the native file import/reimport command is an explicit human
action and journals its mesh and registry mutations together. Importer
execution remains inside Cartographer's existing built-in providers, not a
private donor runtime.
