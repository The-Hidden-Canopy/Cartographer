# Spatial field authoring slice

Cartographer now has a native, bounded 2D scalar-field source for shared
authoring inputs such as moisture, shade, disturbance, wind components,
drainage tendency, acoustic dampening, fog tendency, and vegetation
suitability. A field persists stable identity, semantic, explicit U/V grid
frame, finite range, row-major samples, and monotonic source revision.

The source supports bilinear sampling and deterministic radial replace, add,
erase, and smoothing brush operations. A brush requires the exact source
revision and reports its dirty-cell rectangle, parent revision/digest, and
resulting revision/digest. This parent link lets incremental derived products
reject a stale or skipped edit rather than trusting only a matching identity.
Out-of-grid sampling and stale edits fail instead of returning fallback values.
Edits are committed through the application command and journal boundary.
The native CONDITION workbench can create semantic grids, inspect a bounded
source-revision-tagged heatmap preview, and click-to-apply revision-bound
replace/add/erase/smooth brushes. Field creation, painting,
undo, and redo all produce application mutation receipts. Undo restores authored
values while advancing the field revision; deleted identities retain lineage
tombstones so a recreated field cannot reuse an old source revision. Raw AI
actions cannot invoke this mutation path.

`cartographer_spatial_field_tests`, `cartographer_application_tests`, and
`cartographer_ui_tests` cover canonical round trip and digest, bounded
sampling, brush falloff/clamping, stale-brush immutability, transaction
journaling, command undo/redo across create/delete, retained identity
tombstones, save/reopen, mandatory schema-v19 lineage enforcement, v18
migration, and v17 migration without invented field data.

This remains a source-authoring slice, not a simulation. Vector fields, field
composition/procedural derivation, hydro coupling, a world viewport overlay,
portable field interchange, and visual acceptance fixtures remain open. The
terrain module derives a source-bound LOD mesh, normals, per-cell slope,
navigation passability, and optional wet/dry mask; it does not yet place terrain
into the scene or create collision/nav runtime objects. The native CONDITION
workbench has an elevation relief map and a read-only analysis action; it
reports product counts without mutating project state. An explicit-frame
adapter converts cell-centered elevations into Cartographer's bathymetry
contract, with hydro wet/dry output checked against the terrain mask and stale
source rejection. It does not claim runoff simulation or a solver. No sibling
runtime or private field implementation is linked.
