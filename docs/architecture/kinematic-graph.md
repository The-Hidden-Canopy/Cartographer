# Kinematic graph and lowering

`carto_kinematic` owns the durable authoring truth of articulated assemblies:
which frames exist, how they are parented, how each may move, and which mesh
vertices follow which joints. Current pose, velocity, and every other motion
state are runtime/game-owned and are deliberately absent. VANTA owns runtime
embodiment of the lowered products; games own domain semantics such as door or
shoulder meaning carried in optional semantic-type strings.

## Records

- `KinematicGraph` — a provider-neutral authored hierarchy that must be a
  single rooted tree: exactly one root, every parent resolves, no cycles,
  finite rest transforms, bounded node count. Each `KinematicNode` carries
  stable `JointId`, parentage, rest transform, joint kind (`fixed`, `hinge`,
  `slider`, `ball`), and deformation class (`rigid`, `skinned`).
- `JointLimit` / `JointLimitSet` — authored travel bounds per joint, never
  current pose. Disabled axes must be exactly zero so the canonical form is
  unambiguous; generous hard bounds (64 rad, 1e6 m) reject probable units
  mistakes. Limits are referenced from lowering descriptors by digest.
- `SkinBinding` — which mesh is deformed, by which joints, with which
  normalized weights (sum within `1e-6` of one, at most eight influences per
  vertex), relative to which bind pose. The binding is content-addressed
  against both the mesh and the graph digests so a stale binding fails closed
  instead of silently deforming a changed source; per-joint bind transforms
  are the graph rest transforms, not duplicated.
- Lowering descriptors — neutral, serialization-ready records for the private
  VANTA lowering stage. `SkeletalAssetDescriptor` lowers the deforming
  subgraph (every skinned joint plus its ancestors, so the chain to the root
  is whole) with graph, skin, and limit digests; `ArchetypeDescriptor` lowers
  the rigid complement so identical mechanisms share one archetype identity
  derived from the structural subgraph digest. Instance identity for repeated
  assemblies is preserved by scene/instance-field products, not expanded
  here.
- `require_capabilities` — fail-closed target admission: any required
  capability marked unsupported fails with `unsupported`; optional
  unsupported capabilities are reported, not fatal.

## What is not here

Pose evaluation, constraint solving, runtime articulation, animation playback,
VANTA skeletal-asset execution, and engine rig types.

Verification: `cartographer_kinematic_graph_tests`,
`cartographer_kinematic_identity_tests`, and
`cartographer_kinematic_limits_tests` cover tree invariants, rest-transform
finiteness, skin weight normalization and stale-binding rejection, limit
containment, lowering descriptor digests, capability admission, and
hostile-input rejection. Native-evidence and full-game tiers are deferred to
VANTA/game integration.
