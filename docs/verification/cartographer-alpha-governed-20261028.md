# Cartographer governed alpha candidate — 2026-10-28

This is a source integration ledger, not an execution receipt. The base is
`5a4e4ea4f67a9d3e4894c1ce7cdc5120b0687843`, the separately protected G1-B0
native import caller precursor. Worker handoffs were based on
`fae4f7d95259ba38e79edc94ae12275df4161cd4`. No active worker file was edited.
The integration branch is `codex/cartographer-alpha-g1-governed-20261028`.

## Source and capability delta

The source manifest next to this ledger records every producer hash, the
integration result and the deliberately bounded shared-header merge. Four
handoffs were copied from hash-verified snapshots in the integration owner's
protected precursor checkout, with their original ledgers and notices intact:

- Mesh lock admission: guard replacement and direct checkpoint mesh copies at
  `ProjectDocument`; an affected locked instance blocks the write. Unrelated
  locked meshes do not become global edit blockers.
- Source registry: enforce existing persisted record/registry byte limits
  before adoption, reject conflicting typed product claims, and retain v1-v4
  canonical framing and source-generation/tombstone semantics.
- History/provenance: stage undo/redo on a command checkpoint, move history
  only after journal success and discard the copy on failure. A real redo still
  verifies external blobs. Preserve original owner identity on vertex/extrusion
  replay. V2 mutation evidence records source and hex-encoded provenance;
  supported V1 input remains readable with absent source attribution.
- Texture decode: add pre-WIC baseline and eight-bit progressive Huffman
  admission while preserving digest, extent, limits and returned pixel owners.
  Other frame processes and precision remain outside the new entropy check.

These are candidate capabilities. No new compiled, runtime, native visual,
codec conformance, performance or photorealism result is claimed.

## Integration changes and lineage

Only the agreed `RecoveryJournalEntry` evidence fields were merged from the
worker's application header. The precursor's source-generation query and all
other header content remain intact. Source copies otherwise retain producer
bytes, except the following root-owned test adaptations:

- Register all five existing worker test sources with their owning libraries.
- Update the existing current-writer assertion to V2 plus human attribution;
  retain V1 input fixtures in storage/recovery and the new provenance suite.
- Compare the complete serialized document after failed Undo/Redo and missing
  source, compare restored journal bytes, and print caught test failures.
- Extend the existing native import test with the public UI controller path:
  rights/justification, stale generation/revision, foreign project context,
  raw AI, malformed source and unsupported skin/animation refusal before
  staging; source-cache and saved-file preservation; human operation receipt;
  retained-generation lookup, undo/reimport and cold-reopen lineage.

The UI fixture remains untextured structural geometry. It does not prove UV,
texture sampling, native confirmation interaction or material appearance.

A review script initially decoded a worker file with the Windows locale and
incorrectly reported unrelated middle-dot edits. Explicit UTF-8 comparison
disproved that report. The filter stopped before writing source. Raw snapshots,
the mistaken review-r1 and corrected review-r2 remain preserved in the protected
precursor's `build/sprint2-source-audit-20261028` evidence. No label hunk was
excluded. The portable manifest records the corrected review digest.

## Governance preserved and remaining boundaries

- State transition path: existing application actions and command history;
  no new parallel authoring/history owner or raw AI action route.
- Audit/domain event path: existing application journal, now with a versioned
  mutation envelope and compatible supported recovery. Local project code
  does not acquire RegOS backend authority.
- Permission/project scope: exact human import admission and saved project
  namespace/generation/revision, current mesh locks and original owner binding.
  Organization scopes remain with the existing runtime API owner.
- Human approval boundary: no new automatic import confirmation, permission,
  source rights claim, native foreground grant or publication authority.

Journal append can report failure to truncate a partial write. Document/history
rollback does not make an unrepairable storage device produce unchanged journal
bytes. That severe storage-fault gate remains explicitly unresolved; evidence
must remain visible and recovery must refuse inconsistent history. No journal
implementation was changed to hide or waive that outcome.

Older binaries that only support mutation V1 cannot recover V2 records. New
C++ command checkpoint methods and evidence fields require rebuilding consumers.
The project/source record framing is not changed by these sprint-2 repairs;
source record v4/project35 compatibility still needs the actual stored-fixture
and import/reimport/save/reopen gates. Source detach remains unimplemented.

## Tests and gate status

Added registrations: `cartographer_history_atomicity_sprint2_tests`,
`cartographer_journal_provenance_sprint2_tests`,
`cartographer_project_locked_mesh_sprint2_tests`,
`cartographer_source_registry_v4_sprint2_tests` and
`cartographer_texture_image_decoder_sprint2_tests`.

Updated tests: native scene import/controller, full history failure comparisons
and current-writer attribution. All new/updated tests are unrun. No compiler,
build, test, benchmark, native launch or GPU capture was started for this
candidate. The global coordinator alone grants and executes resource jobs.

The next bounded combined gate must freeze this source, register exact tests,
build affected targets and run the focused suites plus existing application,
UI, agent-workbench, project, source registry, texture/deformation and journal
regressions appropriate to the changed paths. Capture actual failures and
source/tool/dependency/binary pins. A passing G1-B0 result applies only to its
own precursor revision and three named suites.

Actual textured/UV/material fidelity, externally produced JPEG corpus and
resource pressure, severe storage faults, native confirmation and camera,
rendered authored scene pixels, supported rig/pose caller, native save/close/
cold-reopen, packaging, performance and consumer acceptance remain open.

## Repair ledger

1. Files changed: the 20 producer paths and the root test registration,
   existing writer/native-import tests, this ledger and its source manifest.
   The manifest distinguishes raw producer source from root adaptations.
2. Capability gained: candidate lock, source readback, history atomicity and
   provenance boundaries, plus additional source-pixel admission and controller
   regression coverage. Execution has not established these behaviors.
3. Invariants: source lineage/tombstones, explicit unsupported inputs, source
   pixel identity, human admission, durable error evidence, one resource owner
   and protected G1-A/G1-B0 source/evidence are retained.
4. Tests run: none for this source. Source/diff/hash review is not execution.
5. Tests not run: all new and affected suites and native gates, pending an exact
   source freeze and explicit scoped resource lease.
6. Risks: unverified command-copy/delegate behavior and C++ ABI, older V1-only
   readers, lock-related undo refusal, registry copy/serialization cost, codec
   algorithm and fixture correctness, valid-input refusal and resource cost.
7. Rollback: revert scoped integration commits while preserving all source
   snapshots, journals, blobs and prior evidence. Before using a V1-only binary,
   preserve V2 journals and use a compatible reader; never delete audit history
   to simulate compatibility. No migration or destructive rollback is executed.
8. Rejected: weakening blob checks on actual redo, inverse replay for failed
   durable history, app-only lock checks, invented legacy attribution, duplicate
   product claims, silent fallback pixels, accepting source as runtime evidence,
   active worker edits, unleased execution and premature alpha promotion.

Cross-product trace: no RegOS, IDA_Training, hidden_canopy_bot or Ask_IDA source,
API or deployment was changed. Their repositories were not inspected here;
indirect compatibility is unverified. No dataset ingestion, training, secret,
dependency download, main merge, remote branch, push or release occurred.
Future shared-contract changes require their own follow-up integration tests.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: source candidate only. Promotion progress: unchanged. Full G1 and all
75 previously open canonical gates remain open. Condition production is held.
