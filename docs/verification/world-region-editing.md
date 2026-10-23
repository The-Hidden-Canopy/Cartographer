# Large-world region editing verification

## Public authoring contract

`WorldRegion` is an authored partition record with a stable ID, exact revision,
signed integer cell key, and bounded references to semantic areas, paths,
neighbor regions, and cross-region paths. Cell keys do not imply geographic
bounds, inferred geometry, residency, or runtime streaming state.

Region memberships are sorted, unique, and bounded. Each semantic area can be
assigned to at most one region; region cells are unique. Neighbor references
must resolve reciprocally. A path connecting areas assigned to different
regions must be recorded by both neighboring regions as a cross-region path.
Invalid references and incomplete cross-region edits reject the complete edit
atomically.

`WorldModel::apply_region_edits` accepts at most 1,024 region edits per batch
and at most 16,384 memberships in each membership list. New regions use an
expected revision of zero; updates require the exact current region revision.
Revision overflow and stale updates fail closed. A caller can rebase a patch
to an unchanged region onto a newer model while preserving a disjoint edit;
the project transaction still enforces its own document revision and journal
boundary.

The world-model serializer is schema v3. Schema v1 and v2 inputs remain
readable, acquire an empty region table, and serialize in the current format.
Region data enters a project through `ProjectTransaction::set_world_model`.

## Acceptance evidence

Built and ran on 2026-10-20:

```text
cmake --build build/headless-release --target cartographer_world_model_tests --parallel 4
  passed (91/91 build steps)

ctest --test-dir build/headless-release --output-on-failure -R ^cartographer_world_model_tests$
  passed (1/1)
```

The test executable covers stable identities and large signed cell keys,
schema-v3 round trip, schema-v2 migration, one-sided neighbor and cross-path
rejection, duplicate cell rejection, per-region and batch bounds, stale edit
rejection, disjoint region rebase, transaction journal behavior, and final
project save/reopen. A stale project transaction emits no journal event; the
remote and rebased accepted edits remain traceable.

This evidence is for the provider-neutral authoring model and project
persistence. It does not claim geographic partition inference, runtime
streaming/materialization, or interactive desktop region-editing acceptance.
