# Neutral prepared-scene envelope

`carto_production::PreparedSceneEnvelope` is the public, destination-neutral
handoff between Cartographer authoring truth and destination adapters. It does
not contain Unreal, Unity, VANTA, native object handles, executable policy, or
mutation authority.

Version 1 binds:

- the exact project namespace, revision, and canonical digest;
- an opaque destination-profile identity and digest;
- units, handedness, up axis, and forward axis;
- reusable asset definitions and stable scene-node placements;
- content-addressed render-mesh, lookdev, world-model, scoped source-material
  catalog, and source-texture products;
- stable material-region assignments;
- included products, source inputs, and declared external dependencies;
- source-to-prepared correspondence; and
- capability evidence that distinguishes preserved source meaning, derived
  products, native realization, runtime validation, and unsupported features.

`compose_prepared_scene()` validates supplied render and lookdev products
against the exact `ProjectDocument` revision and digest. It derives channel
declarations from decoded product contents, rejects incomplete scene scope,
checks lookdev material IDs against durable source assignments, and emits no
native or runtime claim. A destination adapter may add stronger evidence only
after that boundary is actually exercised.

The serializer is deterministic and bounded. Its reader rejects unknown
versions, duplicate or unsorted identities, non-finite or singular transforms,
hierarchy cycles, missing assets, missing material bindings, required unresolved
dependencies, path traversal, case-insensitive output collisions, stale product
references, and trailing data.

The envelope remains distinct from publication. Cartographer now supplies an
atomic [prepared publication owner](prepared-publication.md) and one shared
[preparation operation](preparation-operation.md). Destination import state and
native reimport transactions remain separate owners.
