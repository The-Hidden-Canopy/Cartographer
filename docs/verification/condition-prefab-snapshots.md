# CONDITION prefab geometry snapshots

New human-authored prefab placements copy every member's complete editable mesh
into a distinct mesh asset. Editing a prototype's geometry or a placed member
therefore leaves the other mesh unchanged. The existing root/child layout and
single journaled, undoable project command remain the placement boundary.

## Persisted bindings

CONDITION authoring schema 10 first separated instance meshes from source
meshes. Schema 11 adds independent accepted source/instance receipts to each
instance member, and schema 12 adds durable rebase decision history:

- `instance_mesh_asset_id`: the independent placed mesh. Existing
  `mesh_asset_id` and `mesh_revision` retain their source-binding meaning.
- `snapshot_mesh_digest`: the exact saved mesh state, including mesh revision,
  vertex/face identities, geometry, attribute domains and values, and UV-set
  descriptors in canonical project save order.
- `instance_local_transform`: the captured child placement beneath the root.
- `source_snapshot_mesh_digest`: the last explicitly accepted source mesh
  snapshot, independent of the instance's accepted geometry.
- `instance_mesh_revision`: the revision of the isolated instance mesh; source
  revision remains in `mesh_revision`.

Schema-10 members migrate with equal source and instance snapshots and retain
their mesh revision for both. This preserves their exact previous currentness
contract. Schema-9 instances remain explicitly unisolated; migration does not
invent either new receipt.

The digest uses the project's existing full mesh-state encoding, with the
classic numeric locale. It is distinct from the geometry-only digest used by
some interchange contracts. Copies retain authored UVs and attributes.

Source revision/evidence, source object/mesh identity, source world transform,
source and placed mesh revisions/digests, child hierarchy, and local placement
must still match before an instance is reported current or published through
the application. Its snapshot mesh must still have exactly one scene owner;
duplicating it as a shared instance invalidates the isolation receipt without
changing the mesh bytes. Ownership counts are built once per scene snapshot.
A changed source or local override is reported stale; neither
mesh is automatically replaced. Moving the instance root remains a normal
placement edit and does not change child-local bindings.

## Preservation and bounds

- New snapshot mesh IDs are distinct from their source IDs and from all other
  recorded instance snapshots. An explicitly bound prior instance child can
  serve as a new prototype; its new placement gets another independent copy.
- Mesh-keyed material-region assignments are copied to each new mesh. Existing
  material, graph, and texture definitions remain shared catalog references;
  this is a geometry snapshot, not a frozen appearance package.
- A restricted material variant cannot silently acquire a wider mesh scope.
  Placement is refused if an assignment cannot legally reference its clone.
- Sources with sculpt/garment authoring or cloth-rest receipts require an
  explicit transfer implementation and are refused rather than dropping that
  source-bound state.
- Placement bounds aggregate vertices, edges, faces, corners, attribute values
  and descriptors to 100,000 elements before copying or hashing. Currentness
  inspection applies the same bound independently to sources and snapshots.

All member resolution, copies, assignments and authoring changes are staged in
a candidate project. Failure does not publish partial members or orphan meshes.
Journal failure restores the original document and history, including redo.
The placement's root/child transform undo chain also exercises
[ordered transform history](ordered-transform-history.md): exact target state
is checked while project revisions continue to advance.

## Legacy compatibility

Schemas 1 through 11 remain readable. A schema-9 instance has no independent
snapshot receipt: its new fields remain zero/identity. Saving it as schema 12
preserves that explicit unisolated state. Schema 10 is upgraded only from its
already persisted identical source/instance receipt. The editor labels it legacy/unisolated
and refuses publication; the model package path also refuses unisolated members.
Missing snapshot fields are never filled from today's possibly edited source.
Historical geometry cannot be reconstructed from the old receipt alone.

The outer project format is unchanged. Older readers that support only authoring
schema 11 cannot interpret schema-12 rebase receipt records.

## Remaining acceptance

This repairs the geometry-isolation prerequisite for source-linked prefab
rebase. The application now has a revision/digest-bound human rebase command,
explicit geometry, placement, and membership decisions, a required rationale,
and schema-12 receipts that persist the reviewed digest and accepted decisions.
The command stages the project and authoring payload together through the
existing undoable project journal. Typed socket resolution remains read-only;
port-graph connection acceptance, source-bound sculpt/garment transfer, native
interaction acceptance, and frozen appearance packaging remain open.
Snapshot and decision records are evidence, not authority outside the
application's human-action and currentness checks.

## Repair ledger

- Files: CONDITION authoring model/codec; project mesh-state digest adapter;
  application placement/currentness/status; model and placement tests;
  existing CONDITION acceptance tests; CMake and capability documentation.
- Capability: prototypes and placements retain independent editable geometry,
  including authored payloads, until an explicit future rebase decision.
- Invariants: human admission, source/revision binding, journal/undo/redo,
  exact evidence, no silent transfer loss, no variant-scope expansion.
- Risks: independent snapshots increase project size; currentness hashes bounded
  mesh state synchronously; legacy placements remain unpublishable until the
  explicit rebase flow is used. Older schema-11 readers cannot interpret the
  new schema-12 receipt block.
- Rollback: restore a retained pre-change project backup before using an older
  reader. Reverting source code alone does not downgrade saved authoring bytes.
- Rejected: shared mutable prototype meshes, reconstructing historical geometry
  from current source, forced replacement of local overrides, widened material
  variant scopes, and claiming the full prefab/rebase/socket task is complete.

## Subsequent executed snapshot verification

The later desktop/authoring continuation rebuilt and passed both
cartographer_condition_prefab_snapshot_tests and
cartographer_condition_prefab_placement_tests within 15/15 focused suites
(61.39 seconds). Model/placement, isolation, source drift, payload preservation,
journal failure, AI refusal, bounds, undo/redo, and reopen cases executed.
These results close the earlier snapshot prerequisite's local verification
gap. Native rebase/socket and the remaining acceptance listed above are
separate gates. See [source history acceptance](source-history-acceptance.md).

## Schema-12 rebase code continuation (2026-10-25)

The current unverified code continuation adds the explicit rebase action,
operator rationale, durable per-member choice receipts, and shared aggregate
snapshot budgeting. Receipt serialization preserves the reviewed authoring
revision/digest, accepted source-versus-instance geometry and placement
choices, and detached/deleted/added membership decisions. The command remains
human-only, checks project generation/revision and authoring revision/digest,
stages changes in one candidate project, and commits through
`ReplaceProjectDocumentCommand`. Tests were authored for schema migration,
receipt validation and round trip, missing-rationale refusal, source geometry
acceptance, removal/detach, world-transform preservation, and undo/redo receipt
history. They have not been built or run, and the native panel has not been
interacted with. Do not treat this continuation as verified acceptance.

The subsequent executed integration pass rebuilt the schema-12
`cartographer_condition_prefab_snapshot_tests` and passed (1/1, 0.21 s).
Rebase/socket/placement acceptance also passed in the focused run, with final
application/UI verification after linking. See
[rebase integration evidence](prefab-rebase-review.md#subsequent-executed-integration-verification-2026-10-25).
Native interaction and runtime acceptance remain open.
