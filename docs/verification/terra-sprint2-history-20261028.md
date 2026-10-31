# History and journal provenance sprint — local claim

Paths marked `<local-...>` identify retained machine-local evidence outside the public repository.

Base: `fae4f7d95259ba38e79edc94ae12275df4161cd4`, detached worktree `<local-worktree>/carto-sprint2-history`. This lane owns application mutation and editor history, not component lock policy. Integration owns shared registration, build, execution and acceptance.

## Durable undo and redo

CommandBus gains a durable prepare/commit/abort path. Prepare copies the top command, runs undo or redo on the copy, and retains the original command and its original stack until the application journal append succeeds. On append failure ApplicationSession discards the candidate and restores its prior document; it does not invoke the inverse command or re-read external source bytes. Real redo still executes the command and its existing source-blob verification. Copyable built-in command types support checkpoints and nested delegate commands are deep copied. Commands without checkpoint support are refused before durable undo/redo.

Integration granted a non-overlapping handoff for the private project loop-cut and knife-cut command checkpoint support in `libs/editor/src/tools.cpp`; both opt into the same mechanism. Other custom command types require checkpoint support. This is source-observed, not a reproduced or validated result.

The journal implementation attempts to truncate a partial record on append failure, but can report that truncation itself failed. This in-memory protocol cannot guarantee unchanged durable journal bytes in that case. Journal-owner repair or a separately accepted uncertain-outcome/recovery boundary is required; this lane did not modify the journal implementation.

## Provenance and selected-owner replay

Integration granted the requested handoff for the project recovery reader and the three additive `RecoveryJournalEntry` evidence fields. The application writes a V2 mutation envelope with operation, active source, bounded hex-encoded provenance and document bytes. Both recovery readers retain V1 and validate V2; AI proposal and agent-workbench entries require provenance. Inspection exposes V2 attribution, while V1 source remains absent rather than inventing human attribution. Older binaries that only understand V1 cannot recover V2 records.

The selected-vertex and selected-face extrusion commands retain their original object owner and refuse replay if it is absent, rebound or locked. Shared-mesh sibling lock enforcement remains the separate project-boundary lane's work.

## Validation and handoff

`tests/application_history_atomicity_sprint2_tests.cpp` is authored but not registered or run. It covers a source-bound glTF import, missing source, failed undo journal append, stack/lineage/revision and journal bytes, real redo refusal, failed redo append and retry. It also covers checkpointless commands, private tool checkpoints and selected-owner removal or rebinding. `tests/application_journal_provenance_sprint2_tests.cpp` covers V1 recovery, V2 human and AI attribution, malformed metadata and missing AI identity. Both test files remain unregistered and unrun. Integration owns registration, actual execution, full serialized-document comparisons, partial-write fault injection, shared-mesh sibling locks and normal edit/history regressions. Existing tests that assume a V1 writer need review.

No test, compiler, build, benchmark, native run, visual acceptance, or diff-check ran. Remaining risks include unverified copy semantics, command dependencies, partial durable writes, V2 compatibility with old binaries, and integration with the separately owned lock policy. Rollback: ordinary revert of the eventual scoped commit, or discard only these owned uncommitted files; preserve existing projects and journal files. No RegOS, IDA, bot, Ask_IDA, Condition or dataset changes. No promotion or alpha acceptance is claimed.
