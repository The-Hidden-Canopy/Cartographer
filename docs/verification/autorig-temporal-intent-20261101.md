# Autorig temporal intent - source handoff

This continuation is on `codex/autorig-durable-acceptance-20261031`, based on
`33ba165`. Its earlier implementation commits are `f077bfa` (reviewed autorig
persistence groundwork) and `776f29cc` (explicit IK/FK authoring workflow).
Subsequent commits `6ccee98`, `60b9548`, and `4241bb3` respectively add exact
tick step interpolation, refuse unverified constraint arbitration outcomes,
and preserve autorig control history across reacceptance. See the separate
constraint and history handoffs for those changes and their limitations.

The mode-key intent and proposed schema v38 human draft path are a continuation
of those commits, not a claim that the full rig, animation, or runtime workflow
is complete. The final commit identity, reconciliation and validation results
must be recorded by the integration owner against the actual candidate.

## Requirement trace

The referenced source is
`retained character-production requirements archive`:
`chapters/06_RIG_CONSTRUCTION_AND_AUTHORING_CONTROLS.md`,
`chapters/10_IK_FK_CONSTRAINTS_AND_CONTACT.md`,
`chapters/13_ANIMATION_TIMELINE_AND_EDITORS.md`,
`chapters/28_AUDIT_AND_PROMOTION_CHECKLIST.md`, and
`appendices/EXECUTION_MATRIX.md`. These local source documents remain the
requirements reference; this handoff does not replace their full detail.

The character production docuseries specifies the deeper mechanism, not just
a matching output. Chapter 06, sections “Rig graph features” and “Control Rig
as distinct canonical layer,” requires authored rest axes and bend planes, a
typed many-to-many acyclic control/port/driver/space graph with declared order,
unit and coordinate-space compatibility, local/world follow spaces, and native
interaction. Execution matrix `RIG-001`, `RIG-002`, and `RIG-003` require
revision-safe rest migration, typed driver evaluation, and keyable native
control/space manipulation.

Chapter 10 requires 2..N chains, position and orientation objectives, explicit
stages and dependency/priority ordering, joint limits, full-body multi-contact
resolution, and final residual and precision receipts. Hard contacts that
cannot meet their tolerance cannot silently receive arbitrary priority; an
unmet result is a proposal. A contact target derives from a verified source
feature and pinned scene transform. It also requires bidirectional no-jump
IK/FK matching, exact-time mode keying, and continuity measured on both sides
of keyed constraint-weight and space switches. Matrix `IK-001` through
`IK-005` covers typed graphs, arbitrary-chain IK, full-body contact, native
matching/space controls, and sustained contact with VANTA admission.

Chapter 13 requires exact rational clip and source time, explicit control,
component and layer targets, revision-bound undoable edits, discrete step
channels, layer evaluation and native timeline/curve interaction. Matrix
`ANI-001`, `ANI-002`, `ANI-003`, `ANI-005`, and `UI-003` retain those source,
interaction and visual gates. `AUT-004` additionally requires a reviewed native
proposal to remain non-mutating until acceptance. `AUD-005` rejects a
two-bone-only production claim or broken contact continuity.

## What this slice represents

The pure mode-key proposal accepts caller-supplied motion document, clip,
channel and layer, explicit lineage/frame and exact intervals, a declared
channel/control/axis crosswalk, source pose and mode, and a pinned authoring
configuration. It checks the available internal identities and digests,
matches one supported IK/FK chain, and proposes one discrete key at an exact
clip tick while checking the preceding tick. Its portable canonical record
captures the proposed curve and insertion receipt, target, source context,
classification and lineage. It deliberately does not serialize the matched
pose or apply the proposed curve, control, mode, or runtime motion.

The current duration guard converts rational interval endpoints to seconds and
uses an outward error bound; exact integral endpoints receive a special case.
Consequently a valid fractional endpoint at the duration boundary can be
rejected, even when its value (for example 0.5 seconds) is exactly
representable. This is a conservative implementation limit, not proof of
physical or timing authority. A future exact comparison must preserve overflow
and conversion-error safety.

The pending schema v38 path stores this as an explicitly **unadmitted** draft.
The human application boundary requires a saved journal, explicit acknowledgment
of asserted/unadmitted data, bounded rationale, project generation, source
namespace and revision, current accepted-rig row revision and digest, and a
uniquely selected current authoring configuration. It reruns the pure proposal
from caller inputs, checks its canonical record, and stages an insert-only
draft through the snapshot command and journal. The typed historical read is
not a currentness, pose, or execution admission. The project and application
changes are pending integration review and must be evaluated together with the
pose codec and schema migration on the exact final candidate. The proposed
outer draft envelope records the project namespace and accepted-rig row
identity, revision and digest checked at insertion, with the checked
pre-insertion project revision. These are historical pins, not proof that that
rig remains current after later source changes. The envelope checksum binds the
stored fields and inner digest for integrity, but is not an attestation: cold
load does not authenticate a human, the historical checks, or the caller's
source assertions.

## Provenance and unfinished semantics

The context's source, motion-document and pose digests, source-pose
classification, scene/reference frame, source/clip/channel clock
correspondence, and crosswalk are caller assertions, not externally
authenticated evidence. Graph FK and matching use the graph's world frame;
calling it world does not bind it to an independently verified scene transform
or prove a physical-world coordinate. Likewise, the current proposal receives
a layer but the record does not preserve the complete layer snapshot, other
layers or evaluated layer stack. Reconstructing a source curve from the record
does not establish that it is the authoritative current layer or that a
time-sampled transition is continuous. The measured same-input position jump
is not proof of temporal pose continuity; orientation jump is not a fully
admitted limit.

The source-pose digest is asserted, and the portable expected record omits the
computed match. Changed source locals can therefore yield a different
successful match while producing the same expected record. The stored draft
does not prove a human reviewed that same match; owner-verified canonical pose
input and a bound proposal/evidence receipt are needed before admission.

Existing `TemporalChannel` and `MotionClip` serialization may depend on the
ambient locale, which can change their purported canonical digests. The owner
must verify and correct this with a locale regression before relying on those
digests across environments. Undo can remove an inserted draft and a later
insertion can reuse its identity and row revision. External references must
therefore include project generation/revision and digest and use independently
verified branch/restore lineage; the draft ID alone is not globally stable.

Authored rest, evaluated preview, current admitted assertion, authored intended
future, predicted alternative, observed assertion and executed assertion must
remain distinct. A stored assertion or numerical result is not evidence that
an observation, execution, physical contact or material state occurred.
Authoring undo/redo changes source history; it does not reverse physical time
or VANTA-owned World effects. Skinning weights do not establish mass or
gravity-dependent weight. Physical occupancy, construction, material, mass,
field and simulation evidence must come from their typed owner and admission
path rather than geometry labels or a curve sample.

The current control evaluator maps six local numeric channels to joint offsets
and applies numeric follow inheritance; it does not provide the specified
typed inverse from graph/world target position and pole direction to control
channels. Existing IK authoring defines a world position and a world direction,
but does not define a complete port/space transform chain, parent/follow-space
inversion, underdetermined many-to-many control solution, or pole-handle
parameterization. These must be explicitly represented and validated; affected
joint lists alone cannot determine them.

The CPU constraint graph has priority, sequential and blend declarations, but
current safety behavior refuses blend and unsupported dependency interactions.
Different target chains may still affect common ancestors. The full product
needs authored conflict ownership, priority/hardness, shared-DOF and contact
semantics, final-pose constraint and limit revalidation, and deterministic
replay. The source does not specify a universal blend weight or quaternion
mean; do not invent one or treat fail-closed behavior as completion. Mode
ownership, evaluated layered blending, switch/space continuity, source and
restore lineage, and owner-approved key application likewise remain to be
resolved in the real editor/runtime contract.

## Verification and acceptance

Test sources for the continuation are authored; no compile, test, migration,
native editor, or runtime acceptance is asserted here. The active build lease
belongs to the integration owner. Focused mode-intent and draft tests are
registered in the pending source. On the reconciled candidate, that owner must
run them, together with schema migration and
save/reopen/journal/undo failure regressions, and review the source/API changes.
Acceptance still requires independent assets and the chapter 10 multi-contact
and chapter 13 native timeline, retime, publication, CPU/GPU and VANTA runtime
gates. No integration claim, merge or deployment is granted by this handoff.

## Source change counts

Counts are cumulative against `776f29cc`, including the three earlier commits and this uncommitted milestone. New files below were untracked before staging; counts are added/deleted lines.

| Path | Added | Deleted | Before staging |
| --- | ---: | ---: | --- |
| `docs/format/cartographer-project-v38.md` | 37 | 0 | new |
| `docs/verification/autorig-history-preservation-20261101.md` | 41 | 0 | tracked |
| `docs/verification/autorig-temporal-intent-20261101.md` | 209 | 0 | new |
| `docs/verification/constraint-arbitration-safety-20261101.md` | 36 | 0 | tracked |
| `docs/verification/keyed-curve-step-interpolation.md` | 40 | 0 | tracked |
| `libs/application/include/carto/application/application.hpp` | 82 | 0 | tracked |
| `libs/application/src/application.cpp` | 188 | 0 | tracked |
| `libs/motion/include/carto/motion/curve.hpp` | 9 | 3 | tracked |
| `libs/motion/include/carto/motion/curve_edit.hpp` | 2 | 1 | tracked |
| `libs/motion/src/curve.cpp` | 27 | 2 | tracked |
| `libs/motion/src/curve_edit.cpp` | 4 | 0 | tracked |
| `libs/pose/CMakeLists.txt` | 3 | 0 | tracked |
| `libs/pose/include/carto/pose/constraints.hpp` | 10 | 3 | tracked |
| `libs/pose/include/carto/pose/ik_fk_mode_key_intent.hpp` | 134 | 0 | new |
| `libs/pose/include/carto/pose/ik_fk_mode_key_record.hpp` | 48 | 0 | new |
| `libs/pose/src/constraints.cpp` | 139 | 74 | tracked |
| `libs/pose/src/ik.cpp` | 5 | 1 | tracked |
| `libs/pose/src/ik_fk_mode_key_intent.cpp` | 178 | 0 | new |
| `libs/pose/src/ik_fk_mode_key_record.cpp` | 189 | 0 | new |
| `libs/project/include/carto/project/accepted_autorig.hpp` | 8 | 1 | tracked |
| `libs/project/include/carto/project/project.hpp` | 30 | 1 | tracked |
| `libs/project/include/carto/project/transaction.hpp` | 14 | 5 | tracked |
| `libs/project/src/project.cpp` | 175 | 0 | tracked |
| `libs/project/src/transaction.cpp` | 45 | 5 | tracked |
| `tests/CMakeLists.txt` | 25 | 0 | tracked |
| `tests/accepted_autorig_project_tests.cpp` | 18 | 12 | tracked |
| `tests/application_autorig_history_tests.cpp` | 186 | 0 | tracked |
| `tests/application_mode_key_draft_tests.cpp` | 452 | 0 | new |
| `tests/autorig_ik_fk_project_tests.cpp` | 74 | 17 | tracked |
| `tests/character_records_project_tests.cpp` | 59 | 22 | tracked |
| `tests/condition_prefab_placement_tests.cpp` | 4 | 1 | tracked |
| `tests/constraint_arbitration_safety_tests.cpp` | 363 | 0 | tracked |
| `tests/ik_fk_mode_key_intent_tests.cpp` | 270 | 0 | new |
| `tests/motion_step_curve_tests.cpp` | 123 | 0 | tracked |
| `tests/pose_tests.cpp` | 3 | 5 | tracked |
| `tests/project_authoring_standards_tests.cpp` | 7 | 4 | tracked |
| `tests/project_source_asset_registry_tests.cpp` | 6 | 4 | tracked |
| `tests/rig_module_project_tests.cpp` | 8 | 7 | tracked |
| `tests/sweep_source_project_tests.cpp` | 2 | 2 | tracked |
| `tests/unadmitted_mode_key_project_format_tests.cpp` | 41 | 0 | new |
