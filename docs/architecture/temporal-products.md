# Temporal products

`carto_temporal` owns durable authoring truth for time-varying content:
channels of keyframes over an explicit target binding, named clips and
sequences that reference channels by digest, authoring-time event markers with
explicit game bindings, root-motion proposals, and a deterministic CPU sample
reference. Execution, blending, transition policy, and playback state remain
VANTA and game runtime authority.

## Records

- `TemporalChannel` — neutral, versioned, digest-bound record for one
  time-varying channel: scalar or transform keyframes and event markers in
  canonical order, a duration, and a `TargetBinding` (rigid object,
  mechanical subassembly, skinned mesh, camera, light) that carries a stable
  identity string and no runtime handle. Joint and skin definitions belong to
  the kinematic layer and are referenced only by kind.
- `EventMarker` / `EventBindingTable` — authoring-time markers with an
  explicit marker-id to game-binding-name table. Dispatch is never implicit:
  `verify_channel_events` fails closed on the first unbound marker, and
  binding names are game-owned domain strings that Cartographer stores and
  validates but never interprets.
- `MotionClip` — a named, revisioned grouping of digest-bound channel
  references. The `ClipId` is stable across revisions: revision changes
  re-version the clip, they never reissue its identity, so downstream
  references survive reimport.
- `MotionSequence` — ordered clip and channel references with a loop policy
  and a gameplay/presentation purpose designation. Reference order is
  authoring playback order and is content; duplicate references are rejected.
  Which clip plays next and all crossfade behavior remain runtime authority.
- `RootMotionProposal` — a proposal record only. Deriving it from a transform
  channel (`propose_root_motion`) mutates nothing: extraction, stripping, and
  consumption are VANTA/game runtime decisions, the curve carries translation
  and normalized rotation only, and scale is never root motion.
- `RigidConsumerDescriptor` — descriptor-level rigid consumer that temporal
  channels may drive; skin-targeted channels are refused at this layer so
  mechanical animation never synthesizes a fake skinning path.
- `SampleSet` — deterministic CPU reference sampling over a fixed grid. The
  receipt digest binds channel identity, channel content, grid resolution, and
  every sample value; identical inputs produce identical digests on every
  platform. Native backend sampling evidence must reconcile against this
  reference, never replace it silently.

## What is not here

Playback, blending, transition graphs, runtime event dispatch, animation
evaluation on GPU, and VANTA clip admission.

Verification: `cartographer_temporal_channel_tests`,
`cartographer_motion_sequence_tests`, and
`cartographer_temporal_sampling_tests` cover canonical ordering, digest
determinism, event-binding fail-closed behavior, clip identity stability,
root-motion derivation, rigid-consumer gating, sample-set determinism, and
hostile-input rejection. Native-evidence and full-game tiers are deferred to
VANTA/game integration.
