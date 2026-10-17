# Project source-asset registry and reimport plans

Cartographer project schema 21 stores a bounded source-asset registry in the
`.carto` document. A record binds a stable source identity to a project-relative
path, source generation and digest, importer identity/version, typed import
options, a stable artifact identity/digest, and optional per-domain digests for
geometry, hierarchy, and material output. The registry retains generation
tombstones after removal, so deleted identities cannot silently restart at an
old generation.

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

The current built-ins import geometry only. They do not yet preserve OBJ/PLY
materials, UVs, custom normals, source hierarchy, or third-party importer
state. There is no automatic watch/relink flow for a moved original file,
missing-source repair UI, orphan-blob garbage collection, or side-by-side
candidate review panel. Stored source blobs can be large and are not encrypted;
projects and their `.carto-assets/` directories may contain private user data.
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
- `cartographer_project_source_asset_registry_tests` covers journaled
  transaction adoption, stale-plan refusal, `.carto` save/reopen, schema-20
  migration, removal tombstones, and exact source generation continuity.
- `cartographer_application_tests` covers application-command undo/redo and
  generation advancement across replay.
- `cartographer_ui_tests` covers the native controller preview/adoption route
  and rejection of raw AI mutation attempts, saved-project OBJ/PLY/STL source
  linkage, absolute-path exclusion, unchanged-import no-op behavior,
  content-addressed source cleanup after parse failure, in-place source
  reimport, undo/redo generation monotonicity, and explicit unsaved-import
  warnings.

## Remaining work

Moved-source relinking, missing-source reports, orphan-blob reclamation, and
an explicit side-by-side reimport review surface remain open. The existing
source registry plan API still provides read-only classification and stale
plan rejection; the native file import/reimport command is an explicit human
action and journals its mesh and registry mutations together. Importer
execution remains inside Cartographer's existing built-in providers, not a
private donor runtime.
