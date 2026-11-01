# Character production capability family

Character Production is a Cartographer engineering series for headless,
deterministic CPU-reference libraries over shared character records. Promotion
gates for native workspace integration, external-runtime admission, corpus
coverage, and end-to-end acceptance remain open. The operations below provide
reference evidence only.

## Libraries

| Library | Owns |
| --- | --- |
| `carto_character` | `CharacterDefinition`/`RigDefinition`, controls, drivers, morph and weights records, rational time, coordinate policy, manifests, invalidation, rig symmetry edits and declared control copies |
| `carto_weights` | geodesic/heat auto weights, brush painting, flood/prune/mirror weight commands, inverse fitting, influence lowering, cross-topology weight transfer through a canonical correspondence record |
| `carto_pose` | FK, two-bone and Jacobian IK, constraints, contacts, IK/FK matching, pose library, pose mirroring, motion paths |
| `carto_deform` | linear-blend and dual-quaternion skinning, morph stages, corrective drivers, delta-smooth correction |
| `carto_face` | expressions, regions, phonemes/visemes, lip-sync alignment |
| `carto_motion` | keyed curves, curve key/tangent editing, layers, retargeting, mocap intake, performance catalog |
| `carto_sim` | groom, collision filters, and generic cache receipts; cloth execution kernels are excluded from the public target |
| `carto_gpudef` | GPU deformation layout, CPU emulation, parity and performance evidence |
| `carto_workspace` | character workspace session, command store, record codecs, and the journaled workspace controller (apply/undo over a hash-chained command journal) |

Each library has a matching `cartographer_<name>_tests` CTest suite
(`character_foundation`, `character_production`, `weights`, `pose`, `deform`,
`face`, `motion`, `sim`, `gpudef`, `workspace`).

## Editing operations

All of these are pure: they validate their inputs, return a new record plus
evidence, and refuse without partial edits. The caller keeps the previous
record for undo.

- **Weight commands** (`carto/weights/paint.hpp`). `flood_weights` adds to or
  sets one joint's weight on a listed vertex set. `prune_weights` drops
  unlocked influences below a threshold but always keeps a vertex's strongest
  influence. `mirror_weights` copies remapped weights across a symmetry plane
  through a `JointMirrorMap` and averages weights for vertices on the plane.
  All three use the brush's lock-aware renormalization. Each returns a
  `WeightEditReceipt` binding the input and output binding digests, plus the
  lock and joint-map digests where they apply. Mirroring refuses when a vertex
  has no source within tolerance, when it would overwrite a locked weight, or
  when a pair maps onto a joint that has no weights in the binding. Running a
  mirror twice changes nothing the second time.
- **IK/FK matching** (`carto/pose/ik_fk_match.hpp`). `match_ik_to_fk` derives
  the IK target and pole from the posed FK limb. `match_fk_to_ik` returns FK
  locals that hold a solved IK pose. Both measure the chain's world-space
  position jump and refuse a switch above the requested bound. Two-bone IK
  does not solve effector orientation, so the effector rotation jump is
  reported, not enforced.
- **Explicit IK/FK authoring and proposals**
  (`carto/pose/ik_fk_authoring.hpp`). The in-progress follow-on records a
  caller-authored contiguous two-bone chain, FK controls, IK target and
  optional pole controls, mode axis and values, and deterministic solver
  settings pinned to the exact rig, graph and limits. The proposal API
  returns a suggested mode value and matched pose data; it does not apply
  control values or key a switch. It requires the default coordinate policy
  and refuses posed ancestors outside the chain. Position discontinuity is
  bounded; effector orientation discontinuity is reported, not enforced.
  Separate proposals do not arbitrate simultaneous constraints or chains.
- **Pose library** (`carto/pose/pose_library.hpp`). The library stores named
  local poses with a canonical digest. `apply_library_pose` blends a stored
  pose in by weight, optionally limited by a joint mask. `mirror_pose` swaps
  and reflects paired joint locals, which assumes a symmetric rest pose.
  `compute_motion_path` samples a joint's world path over an exact rational
  time range with a bounded sample count.
- **Curve editing** (`carto/motion/curve_edit.hpp`). Operations insert,
  delete, offset, and scale (retime) keys, and set tangents (flat, linear,
  auto_smooth). Keys are addressed by exact tick. A retime that would land two
  keys on the same tick is refused. Retiming divides tangents by the scale, so
  each key keeps its slope in value per second. Each edit returns a receipt
  with the source and result curve digests.
- **Delta-smooth correction** (`carto/deform/delta_smooth.hpp`). At bind
  time the mesh is Laplacian-smoothed and each vertex's offset from the
  smoothed surface is stored in a tangent frame. At apply time those offsets
  are reapplied over a smoothed deformed mesh. The result reproduces the bind
  mesh exactly at rest, is unchanged in shape under rigid motion (it moves
  with the mesh), and reports the size of its correction. A collapsed
  deformed frame is refused.
- **Rig symmetry** (`carto/character/rig_symmetry.hpp`).
  `analyze_rig_symmetry` reports rest-transform divergence and parent
  mismatches across `RigDefinition` symmetry pairs without correcting them,
  so deliberate asymmetry is preserved. `mirror_rig_rest` copies reflected
  rest transforms from one side to the other, refuses pairs whose parents do
  not mirror each other, and is the identity when applied in both directions.
  `mirror_rig_controls` is the control-graph half of the same contract:
  declared `ControlSymmetryPair`s copy one side's control definition onto
  the other, remapping `affected_joints` through the joint mirror map and
  `follow_space` through the control mirror map (refusing remaps that would
  cycle), carrying `control_set` verbatim, and leaving the joint graph
  untouched. Both operations are pure: new graphs plus receipts with
  source/result digests and changed counts.
- **Rig-control symmetry** (`carto/character/rig_symmetry.hpp`).
  `mirror_rig_controls` copies a declared source control to its paired
  destination, remapping affected joints through the declared rig pairs
  and follow-space references through declared control pairs. It returns
  new graphs with source/result digests and leaves the inputs unchanged.
  Missing or reused pairs, unpaired affected joints, unsupported axes or
  directions, and invalid resulting dependency graphs fail closed. Pairing
  is authored explicitly; geometry analysis does not infer control pairs.
- **Cross-topology weight transfer** (`carto/weights/transfer.hpp`).
  `transfer_weights` carries a finished weight layer across a topology change
  through a canonical, digest-bound `WeightCorrespondence` (per target vertex,
  1-8 source-vertex samples with barycentric coordinates) and an injective
  joint map (an empty map is the identity map, the same-skeleton/new-mesh mode).
  Influences are gathered through the correspondence, capped strongest-first,
  renormalized lock-aware, and re-admitted through `validate_influences`;
  target vertices with no correspondence source refuse the whole transfer
  without partial output, mirroring the auto-weight unreachable-region rule.
  The receipt binds correspondence, joint map, locks, and input/output binding
  digests, and the layer carries `WeightProvenance::transferred`.

## Non-claims

- **AutoRig feature-analysis outcomes** (`carto/character/autorig_analysis.hpp`)
  are source/revision/digest-bound evidence, not a riggability verdict. A
  unique reflected position can still have `topology_compatible == false`;
  it remains in `symmetry_pairs` with that mismatch visible. Vertices with
  multiple or contested reflected matches are listed in `ambiguous_vertices`
  and are never resolved by nearest-match tie-breaking. Vertices without a
  unique counterpart, including isolated or asymmetric geometry, remain in
  `unmatched_vertices`; centerline vertices are reported separately. Connected
  components remain distinct, and geodesic entries outside a seed component
  are marked unreachable. Invalid, stale, or over-budget input fails without
  a partial receipt. These outcomes describe observed geometry only: this
  analysis does not decide that a mesh is riggable, produce a skeleton, or
  choose anatomy.

- **Bounded candidate proposals** (carto/character/autorig_candidates.hpp)
  require the exact current mesh, admitted partonomy, and provenance-bearing
  landmarks. They enumerate every one-anchor-per-represented-part alternative
  within caller budgets, preserve omitted parts and anchor provenance, and
  rank by declared part coverage, nonadjacent clearance, mesh-relative length,
  and joint budget weights. Candidate joints use preview-local IDs. Stale
  sources, missing root anchors, invalid policies, degenerate bounds, and
  exceeded budgets fail without a partial result. These scores are ordering
  evidence, not calibrated confidence or an acceptance decision. The path does
  not infer anatomy, mechanical ports, allowed regions, bend arcs, controls,
  weights, familiarity, or riggability. An explicitly weighted optional
  source-surface measure reports the fraction of bone centerlines without a
  transverse intersection against the triangulated source surface; it is not
  a watertight-containment test, volumetric bone collision, or anatomy verdict.
  Its default weight is zero, and its bounded triangle-query budget fails the
  whole candidate set rather than returning partial rankings. Candidate and
  morphology-class acceptance gates remain open.

- **Candidate review session** (`carto/character/autorig_candidates.hpp`)
  retains a speculative copy of the source-bound constraints and current
  candidate set. A reviewer can select a current digest, lock an anchor that
  appears in that candidate, supply a provenance-bearing anchor edit, and
  replan against the same mesh revision. Locks filter alternatives; edits and
  replans invalidate the previous selection. A self-checksummed review
  acceptance receipt binds the source, mesh geometry, constraints, policy, the
  exact candidate-set digest, and the selected candidate. It
  requires caller-supplied human-authored reviewer identity and rationale and
  records a review choice only: the library does not authenticate the reviewer,
  preview-local joint IDs are not installable, and no project or rig is
  mutated.

- No native workspace, viewport gizmo, or input acceptance for any of the
  operations above.
- Pose libraries, weight-edit receipts, and curve-edit receipts persist in
  project schema v33 as bounded, digest-bound tables admitted through
  journaled transactions ([schema v33](../format/cartographer-project-v33.md)).
  Pose libraries are revision-checked; the two receipt tables are historical
  ledgers retained across undo/redo and save/reopen.
- Authored kinematic graphs persist independently in schema v35 as canonical,
  digest-bound records with project-local revisions. `ProjectTransaction`
  admits inserts and compare-and-swap replacements; the records participate in
  project journal snapshots and save/reopen. Agent Workbench diffs and merge
  details expose bounded identity, revision, byte-count, and digest summaries.
  See
  [schema v35](../format/cartographer-project-v35.md).
- Rig module templates now have a bounded canonical value serializer and
  parser plus a project-local library. Insert refuses an existing stable
  identity; replacement requires both the current entry revision and digest,
  and project transactions journal successful changes. Project schema v30
  migrates older files with an empty library. Instantiation grafts a template's joint/control fragment into
target graphs under fresh stable ids with a digest-bound receipt. The library
has no delete API or native editor workflow yet.
- Schema v36 adds a journaled accepted-autorig bundle containing the reviewed
  graph mapping, explicitly authored rig and controls, joint limits, and a
  transferred binding with its full per-vertex weights and receipt. Candidate
  IDs remain preview-local; callers supply durable IDs and final node fields.
  Cold records need a current-mesh readiness check before use. This is not a
  complete persistent IK/FK setup or a native editor workflow. See
  [schema v36](../format/cartographer-project-v36.md) and the
  [handoff](../verification/autorig-durable-handoff-20261031.md).
- The in-progress schema-v37 follow-on preserves v36 bundle-v1 data and adds
  bounded, explicitly authored IK/FK chain records in nested bundle v2.
  Configuration is revision checked, and the human application command uses
  project history; reads check the current source mesh before returning a
  bundle. External source digests and binding provenance remain caller
  assertions. A v36 reader cannot read a v37 project, and a v1-only nested
  reader cannot read v2. See [schema v37](../format/cartographer-project-v37.md)
  and the [follow-on handoff](../verification/autorig-ik-fk-follow-on-handoff.md).
  This source work does not establish native workflow or runtime acceptance.

## Required workflow and remaining gates

The character-production mandate is an artist-controlled path from source
mesh and optional semantic evidence through analysis, alternatives and
review, to a source-bound rig, editable skinning and animation, and finally
validated runtime use. The intended autorig chain starts with topology,
components, geodesics and symmetry (`AUT-001`), generates and ranks
alternative hypotheses (`AUT-002`), admits provenance-bearing optional
landmarks and partonomy (`AUT-003`), and presents preview, locks, edits and
replanning without project mutation until human acceptance (`AUT-004`).
Unfamiliar and unriggable inputs must retain ambiguity or refusal (`AUT-005`).
Mechanical joint kinds, axes and limits must be authored or supported by
evidence; shape alone does not establish a physical constraint. Manual
construction remains a supported fallback.

Representation and interaction remain separate gates. The local ledger groups
`RIG-004/005` around rest symmetry, declared control copies, and reusable
templates that can be instantiated, edited and migrated without overwriting
author work; `RIG-006` requires mixed rigid/organic articulation,
a multi-link fixture and non-skin export. `SKN-003` and `UI-002` require
native paint, locks and heatmap interaction with undo/reopen; `SKN-004`
requires certified transfer with seam ambiguity and failure reports. An
internal sparse transfer record does not by itself meet those gates.

The local `IK-004` gate is no-pose-jump matching; editable poles, aim, spaces
and native switching remain broader product requirements. `ANI-002` and `UI-003` require human key, tangent
and retime editing with undo, visual proof and correct pose while scrubbing.
The current two-bone proposal does not implement the complete control-space,
orientation, simultaneous-constraint, timeline or runtime behavior. The
full independent asset corpus, native interaction, undo/reopen, visual and
external-runtime acceptance remain open; component suites are not equivalent
to those gates.
