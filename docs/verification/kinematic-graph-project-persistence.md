# Kinematic graph project persistence

## Scope

Adds a revisioned project record for canonical `KinematicGraph` bytes and a
`ProjectTransaction` setter. This is a persistence boundary for an explicitly
authored final graph. It does not convert an autorig preview into a rig.

## Admission and persistence contract

- Insert only with expected record revision zero; replace only at the current
  record revision.
- Validate the stable identity, bounded payload, canonical graph codec, and
  SHA-256 of the exact serialized bytes before staging.
- A byte-identical record at its current revision is an idempotent no-op.
- Persist the complete table as mandatory schema-v35 data; read schemas 1–34
  with an empty table and refuse v35 data labeled as an older schema.
- Validate records on project reopen and include them in whole-document
  transaction snapshots so journal replay, undo/redo, and save/reopen retain
  exact graph bytes, revisions, and digests.

## Source-only regression coverage authored

`tests/character_records_project_tests.cpp` now covers journaled insertion and
replacement, stale revision and digest refusal, noncanonical payload refusal,
save/reopen fidelity, tampered-digest refusal, mandatory-table refusal, and
schema-v34 migration. `tests/agent_workbench_tests.cpp` covers bounded typed
project-diff records for graph insert, replacement, and truncation. The
Agent Workbench merge detail rows and compact summaries carry the graph counts,
record revisions, byte lengths, and digests without exposing graph payloads.
The user requested a coding sprint without testing; no build, test, or runtime
verification was run for this change. These checks are authored source
coverage, not passing evidence.

## Remaining AUT-004 work

`AutorigCandidateReview::accept_review()` still returns only a review receipt.
Candidate IDs remain preview-local, and candidates do not supply every final
rig field. A later human-facing conversion must require explicit stable-ID
mapping and authored node-class/rest-frame choices; it must preserve unselected
alternatives, omissions, source lineage, and reviewer provenance. No durable
autorig commit, native acceptance, or promotion is claimed here.
