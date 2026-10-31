# Cartographer alpha G1 integration

Paths marked `<local-...>` identify retained machine-local evidence outside the public repository.

The integration branch starts at exact
`b9f0e9ccc4a9c06180640d3efe8da7ae596d1cf4`, including the preserved native
shader/UI repair and `0f26d9f` bake compile repair. The original dirty main,
source checkpoint, prior realism checkout and private canonical queue remain
separate. No release or complete-alpha acceptance is implied by this branch.

## Current measured blocker and repair

The global executor's bounded r2 job froze `0f26d9f`. Configure passed; the
build failed with two diagnostics for undeclared `light_view` at the shadow
constant upload in `libs/d3d12/src/scene_renderer.cpp`. No requested binaries,
tests or native evidence were produced. The r2 retry receipt SHA-256 is
`a7dd24e4fed8b20d3357aeabc743a09b8ff50188c0fd2df01456dc5a38b2caa9`;
the global coordinator records its exact evidence path. Failed evidence remains.

The integration repair moves the existing request-derived shadow view matrix
to render-call scope. The mesh-upload preparation and later shadow draw upload
then use that same value, including the cached-mesh path. It does not change
matrix computation, shaders, constant-buffer layouts, source identity,
materials, permissions, journal or human authoring authority. The two-line
scope repair is unbuilt and requires the next scoped integration lease.

## Terra ownership and entry to G1

The subsequent global r3 baseline job froze `ceaf48c`: configure passed and
the `light_view` diagnostic was absent. Build failed on four metadata uses of
`quoted_text` before its local declaration in `libs/ui/src/ui.cpp`, plus one
three-argument `MessageBoxA` call in `apps/d3d12_desktop/main.cpp`. No requested
binaries or tests/native evidence were produced. Receipt SHA-256:
`28e31e6820aad459b03f34d47bd9cdb35eeedd087ecd7a75c8683de6441fdd1b`.

The next bounded integration repair moves the unchanged quote helper before
the metadata rows in the same comparison lambda and restores the missing
window argument to the native error dialog. Metadata/source text and contested
rows are retained with their existing formatting; diagnostics remain visible.
No authoring mutation, source mapping, ABI, saved schema or journal changes.
No additional test/build/native execution accompanies this source repair;
the updated exact G1-A request replaces the earlier pin after commit. Rollback
is this formatter/dialog repair commit only, retaining all prior evidence.

Three Terra lanes work from the same pinned base: human scene/material append,
JPEG stream admission, and deformation method/space admission. Their current
exclusive paths are controlled by the coordinator's sprint plan and explicit
scope updates. The integration owner does not edit active lane paths. Shared
native callers, root/test registration and this combined candidate belong to
integration. Each handoff must supply exact local commits, changed paths,
authored tests and limits. Workers author tests but execute no jobs.

After review, integrate only the accepted owned diffs and record their exact
commits plus one combined frozen revision. Resolve the native action invocation
against the actual supplied interface. Source-generation/tombstone preservation
and journal failure atomicity are mandatory; no whole-project history shortcut
or copied schema 35 is admitted. Shared test registration must name tests that
actually exist in the frozen handoff. No absent test becomes an implicit pass.

## Gate1 acceptance

Reviewed decoder and deformation source handoffs are locally frozen and landed:

| Lane | Frozen worker commit | Integration commit |
| --- | --- | --- |
| JPEG marker admission | `d070dcb0f5f90954b2402b1bd58465ec5f8e7447` | `13e1b9d401b564153e0f51011aa9204756414ceb` |
| Deformation admission/finite output | `141731d81436adf5304d203857500bcb1e1eb369` | `e36acbeb2fdaa42f5b881e6d16fcaa7787b6cd57` |

Every handed-off raw source hash matched before staging, and landed Git blobs
match the frozen commits. Integration did not modify worker source while
freezing it. Their existing `cartographer_texture_source_tests` and
`cartographer_deform_tests` registrations include the authored cases; no new
test target is implied. Both patches and their tests remain uncompiled/unrun.
Their own ledgers describe rollback, risks and exact boundaries.

JPEG marker structure is not full entropy validation or a no-recovery codec
guarantee. The deformation patch has no native product caller and does not
establish pose-to-graph provenance or morph-target identity validation. Those
limitations remain open. The import mapping/action lane is still active, so
the combined G1 source has not been frozen and no native caller is wired to a
guessed interface. Its explicit additional registry/project/editor ownership
and compatible one-source-to-many-products design are in the integration
owner's coordination handoff.

The first bounded validation request is G1-A: freeze the reviewed renderer,
decoder and deformation commits, build the three native/UI targets plus the
existing texture-source and deformation test targets, then run only those two
focused suites. Its exact combined revision is recorded in the coordination
request after this ledger commit. G1-A is a partial compile/admission receipt;
it cannot close the full import/history/native/rig G1 journey. Existing active
baseline execution finishes first, and no job runs without the global lease.

One global lease at a time governs affected builds, focused positive/negative
checks and native acceptance. The gate includes real launch/open/import/render/
edit/undo/redo/save/close/cold-reopen, exact geometry/UV/normals/material/texture
identity, malformed source refusal, stale/wrong-authority admission, source
generation history and journal failure recovery. Review actual nonblank,
depth-correct authored-scene GPU/native pixels tied to source/binary/shader/blob
hashes; metadata or CPU interchange alone is insufficient.

Supported rig/pose/deformation fidelity remains part of the gate. If its actual
native product path is absent, record an implementation blocker; the bounded
deformation admission repair does not supply or prove that path. Preserve
unsupported skins/morphs/animation and all broader native/DPI/performance/
package/consumer failures and unrun gates. No gate passes until its applicable
conditions actually pass. All 75 previously open canonical items remain open.

## Repair ledger

- Changed files: `libs/d3d12/src/scene_renderer.cpp` and this ledger.
- Capability delta: removes a measured C++ scope blocker in source; runtime
  cache/shadow correctness and the combined alpha remain unverified.
- Tests added/updated/run: none in this root slice. No build, compiler check,
  test, benchmark or native launch was started by integration in this slice.
- Required next: affected build, cached/uncached shadow execution and the G1
  journeys above under a new exact-source lease.
- Risks: later compile/runtime failures remain possible; active worker patches
  and native caller contracts have not been combined or validated yet.
- Rollback: revert this scope-repair commit only, retaining pinned base and
  prior failed receipts. No project/schema/journal rollback is required.
- Rejected: disabling shadow uploads, removing a failed render path, editing
  active worker files, patching validation source in place, running an unleased
  job, treating authored tests as product acceptance or publishing an alpha.
- Cross-product trace: no RegOS, IDA_Training, hidden_canopy_bot, Ask_IDA, VANTA
  or Condition production changed; no shared API/schema/dataset/deployment
  change from this scope repair.

Dataset/training progress: none. Capability progress: source repair of a
measured compile blocker, unverified at runtime. Promotion progress: none.
Cartographer readiness precedes Condition asset/marketplace production. After
each accepted handoff, continue the next actionable canonical alpha prerequisite.

## Acceptance reconciliation and source audit — 2026-10-28

This dated entry supersedes the earlier pending G1-A statements, while retaining
them as the history of the request. The permitted change in this slice is to
coordination and acceptance records. Production code, tests, Git history, the
global resource lease and the six existing native UI integration edits are not
modified by this reconciliation.

### Earned G1-A evidence

The global executor completed the bounded job against exact source
`25b2c5798e34ac165e7d7555ba5bfdd8c086edcd`. The three targets `carto_ui`,
`cartographer_scene_render` and `cartographer_d3d12_desktop` built. Both focused
test targets built, and `cartographer_texture_source_tests` plus
`cartographer_deform_tests` passed, 2/2. The receipt was read and its SHA-256
independently matched during this reconciliation:

- Receipt: `<local-evidence>/cartographer-g1a-20261028/receipt.json`
- Receipt SHA-256: `867a9b68a26f56e14f9d923565212eb566fc55724633d89503b0f3bb4813d6bb`
- Scope: partial build and CPU/Windows codec/deformation admission only.
- Native application launched: no. Reviewed images: none.

Those results do not validate the current combined source. Its committed base
is `fae4f7d95259ba38e79edc94ae12275df4161cd4`, which freezes the import handoff
on top of the protected G1-A source. Six existing modified files add the native
confirmation/staging caller, a retained-generation query, warning propagation
and import test registration. Their raw source hashes are recorded in the
integration owner's `cartographer-status.json`. They remain unbuilt and unrun.
The source-record v4/project35 compatibility decision is source-reviewed only;
old/new round trips and recovery still require execution.

### Source-traced governance findings

These are code-path findings, not newly executed failure receipts.

| Finding | Owning code | Minimal repair requirement | Proof required |
| --- | --- | --- | --- |
| P1: failed undo journaling can leave history inconsistent | `ApplicationSession::undo`, `CommandBus::undo/redo`, `ImportGltfSceneCommand::execute` | Restore command state and history independently of fallible external source admission when a durable append fails. | After an admitted import, remove a source/image blob and inject an Undo journal append failure. Refusal must preserve exact document, registry generation/tombstones, undo/redo state and journal bytes; restoring inputs must leave ordinary history usable. Also retain positive undo/redo and Redo append-failure coverage. |
| P1: component edits omit lock admission, and the single-owner selection skips the shared-mesh scan | Project component commands and owning `ProjectDocument` mutation paths | Refuse mutations of the selected locked owner or a locked instance sharing the mutated mesh, including replay where applicable. Unrelated locked objects must not block independent imports or edits. | Locked selected owner and locked shared sibling refuse atomically through human and typed AI paths. An unlocked unique mesh still edits, undoes and redoes; event behavior remains intact. |
| P2: durable mutation payload omits AI source and proposal provenance | `ApplicationSession::append_mutation_event` and mutation-envelope reader | Persist bounded, validated source/provenance in a versioned envelope and retain supported legacy recovery. | Human and AI events retain attribution after journal reopen. Legacy entries recover without invented attribution; malformed provenance and failed appends refuse without state changes. |

The undo failure follows from the existing control flow: command undo moves the
import to redo; a journal failure triggers `history_.redo()`; import execution
then verifies source blobs. If that verification fails, the document snapshot
is restored but the command remains undone in history. The existing import test
checks initial import append failure and missing-source redo independently; it
does not combine missing source with Undo append failure.

The inspected runtime API client already rejects mismatched organization scope
before job polling and artifact download. Raw AI application actions are
explicitly rejected. No new missing-org or raw-AI bypass finding is asserted.
Role tiers and naive datetimes are not contracts of these selected local C++
command paths; applicable human admission and project scope must be tested.

### Next bounded gate and ownership

The orchestrator announced new exclusive assignments to the same three Terra
sessions: import owns application/CommandBus history and durable provenance;
deformation owns project-level lock admission; decoder owns strict source-pixel
admission. This entry records announced reservations, not verified running
state or completed repairs. The six native UI integration files remain reserved
to consolidation. Shared headers, callers and test registration must be agreed
before integrating a handoff; no worker patch is accepted merely by assignment.

`CARTO-ALPHA-G1-B-COMBINED` is recorded as **not ready**. Before execution it
needs one audited frozen combined revision, exact new lane test registrations,
source/binary/toolchain pins and a new explicit lease. Its first bounded
validation should build the affected targets and run the existing native scene
import, application, UI, source registry, project source registry, texture source
and deformation suites plus the exact new lane suites. Preserve every failed
attempt and do not execute stale binaries after a failed build.

The actual import dialog is in `apps/desktop/main.cpp`, built by
`cartographer_desktop` with the pinned Vulkan/ImGui dependencies.
`cartographer_d3d12_desktop` is a different target; its passing build does not
compile or exercise the authoring dialog. Native editor compilation and its
foreground import/history/save/cold-reopen journey therefore need their own
explicit resource scope and actual dependency, fixture and blob pins.

Required missing cases include UI refusal before staging for absent rights,
blank/control justification and stale/foreign project context; raw AI import
refusal; retained-generation reimport after undo; old source-record versions;
textured UV/material/source preservation; unsupported external, skin, morph
and animation input; corrupt/missing source/image input; and journal failure
through execute, undo and redo. The existing two-colored-triangle fixture is
an untextured structural test, not evidence of texture fidelity or photorealism.
Native rig/pose integration and reviewed authored scene pixels remain open.

### Reconciliation ledger

- Changed files: this acceptance ledger and the integration owner's existing
  `coordination/cartographer-status.json` only.
- Capability gained: acceptance bookkeeping distinguishes the earned G1-A
  result from the untested combined candidate and names three adversarial
  failure cases. No new executable capability is claimed.
- Invariants preserved: existing command/journal and human admission paths,
  project/source identities, source snapshots, worker reservations, all prior
  failed evidence and the coordinator's sole resource authority.
- Tests added/updated/run in this slice: none. Existing receipt and source were
  inspected; no compiler, build, test, native or benchmark process was started.
- Tests not run: combined and adversarial suites and native journeys remain
  pending frozen source and an explicit scoped lease.
- Risks introduced: no production/API/schema risk from this record-only
  change. The three source findings, import compatibility and missing native
  evidence remain real unresolved risks.
- Rollback: remove only this dated ledger addition and the corresponding
  owner-status reconciliation if incorrect, retaining receipts, source hashes,
  existing edits and the historical request. Do not erase earned evidence.
- Intentionally rejected: modifying production under record-only approval,
  altering active worker paths, weakening tests, guessing source or appearance,
  treating G1-A as full G1, self-granting a lease or promoting the alpha.
- Cross-product trace: no direct production change to RegOS, IDA_Training,
  hidden_canopy_bot or Ask_IDA, and no shared API/schema change in this slice.
  Those repositories were not inspected here, so indirect compatibility and
  their deployment behavior are unverified. No dataset ingestion or training
  occurred. Their own integration tests remain required if a future repair
  actually changes a shared contract.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: source audit and corrected acceptance accounting only. Promotion
progress: unchanged. Full G1 and all 75 previously open queue gates remain open;
Condition asset and marketplace production remains held.

### Sprint 2 ownership contract follow-up

The canonical coordinator record is now `cartographer-sprint2-plan.json`, scope
revision 4, based on exact `fae4f7d`. Its three existing sessions and their
producer status records were inspected. History and lock work are in progress;
decoder reports an untested partial baseline-JPEG handoff. These observations
do not establish successful execution or complete strict JPEG admission.

The history worker requested recovery, private-command, journal and shared-header
support. The integration owner's status records a bounded handoff for V2/legacy
recovery in `libs/project/src/recovery.cpp`, checkpoint support for only the two
private loop-cut/knife commands in `libs/editor/src/tools.cpp`, and a disjoint
`RecoveryJournalEntry` evidence-field hunk in the worker's isolated header.
All other shared-header ownership and the six integration source edits remain
with consolidation. Source absence in legacy journal entries must stay unknown;
the reader must not manufacture human attribution.

No production ownership extension was granted for `libs/journal/src/journal.cpp`.
Its append implementation already exposes failed partial-record truncation.
Document/history rollback must not depend on re-reading external source blobs;
an unrecoverable storage failure must remain visible and must not be described
as unchanged durable journal bytes. That storage-fault gate remains unresolved.
No production code, test source or execution changed in this contract follow-up.

## G1-B0 native import caller precursor

The existing six integration edits are source-reviewed for a separate precursor
freeze on top of `fae4f7d`. This does not include any active sprint-2 worker
patch. The source review observes explicit human rights/justification checks,
exact project namespace/generation/revision admission before staging, parser
refusal before persistence, exact source/image digest staging, a retained source
generation query and the existing atomic import action. Parser warnings reach
the operation receipt. Cache additions may remain after a refused application
action; they are not claimed to be authoring commits or rolled-back bytes.

The actual desktop caller is the existing ImGui confirmation modal and
`UiController::import_static_gltf_scene`; it has not compiled or run. Its six
raw source hashes were checked against the prior owner-status snapshot before
freeze. That preserves the existing implementation rather than adding a
competing caller. The dated acceptance additions are included in the freeze;
the exact resulting commit and dependency hashes belong to the external
precursor manifest and owner-status request, avoiding a self-referential commit.

Offline dependency inspection found `C:/VulkanSDK/1.4.357.0` with the Vulkan
headers, x64 import library and `Bin/glslc.exe`. The bundled ImGui pin declares
upstream commit `d88f238d1d363ef387382b2e29ad12cf96f826ac`, docking branch, with
its MIT notice retained. Presence and local hashes are dependency evidence,
not a compiler run or a new upstream authenticity check. Existing Clang/Ninja/
CMake/CTest and Windows SDK WinSQLite3 inputs must also be pinned in the request.
No download, installation, dependency substitution or network operation occurs.

G1-B0 requests a clean exact-source acceptance refreeze, a separate build
directory, Vulkan and desktop ON, D3D12 and benchmarks OFF, then a build of
`cartographer_desktop` and the existing `cartographer_native_scene_import_tests`,
`cartographer_source_asset_registry_tests` and
`cartographer_project_source_asset_registry_tests`. Only those three suites may
run, serially, after their exact target build succeeds and test discovery is
verified. The coordinator must preserve old evidence, record commands and
source/tool/dependency/shader/binary hashes and refuse changed inputs or stale
executables. No work starts without its explicit scoped grant.

This precursor can establish compilation of the real dialog/controller and
execution of the existing kernel import/registry fixtures. It cannot establish
UI interaction, textured source/UV fidelity, full v1-v4 compatibility, the three
open governance repairs, strict JPEG admission, native rig support, actual
rendered appearance, save/cold-reopen in the native application or full G1.
Its untextured structural fixture is not a photorealistic reference scene.

Freeze ledger: the six existing source/API/registration files and this ledger
are the only intended commit paths. Tests authored in this freeze: none; the
existing import test is registered by the pre-existing integration diff. Tests
run: none. Risks remain an uncompiled caller and unexecuted fixtures, retained
source-record v4/project35 compatibility and all named governance/native gates.
Rollback is the eventual scoped caller freeze, preserving cached user blobs and
accounting for v4-imported projects before using an older reader. Rejected:
editing active worker files, including unfinished patches, stale binary reuse,
claiming precursor success before receipts or broad alpha promotion. No direct
RegOS/IDA_Training/hidden_canopy_bot/Ask_IDA, dataset, training or deployment
change accompanies this freeze; the existing Cartographer API additions remain
subject to the combined gate. No push, main merge or Condition production.
