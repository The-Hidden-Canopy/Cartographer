#pragma once

#include <carto/journal/journal.hpp>
#include <carto/workspace/commands.hpp>
#include <carto/workspace/session.hpp>
#include <carto/workspace/store.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <array>
#include <filesystem>
#include <vector>

namespace carto::workspace {

// Receipt for one committed workspace command. Wall time is deliberately
// absent: identity comes from the applied revision and the journaled entry
// digest, never from a clock (ch04 determinism).
struct WorkspaceApplyReceipt {
    core::Revision applied_revision;
    assets::Sha256Digest journal_entry_digest;
};

// The workspace controller (ch19 application mechanics): the single
// mutation authority over a CharacterSourceStore. It owns one session per
// WorkspaceDomain, the store every session projects, and the journal every
// committed command is appended to. apply() runs the full admission chain
// fail-closed — payload validation, session domain/target check, the expected
// source revision gate, referential validation against the target bundle,
// and inverse computation against the pre-mutation state — before anything
// touches source; the bundle mutation, digest refresh, revision advance and
// journal append commit only after every gate has passed. undo() is a
// forward journal entry: the inverse recorded at apply time, with its
// expected revision rewritten to the revision current at undo time.
class WorkspaceController final {
public:
    explicit WorkspaceController(std::filesystem::path journal_path);

    [[nodiscard]] CharacterSourceStore& store() noexcept { return store_; }
    [[nodiscard]] const CharacterSourceStore& store() const noexcept { return store_; }
    [[nodiscard]] journal::Journal& journal() noexcept { return journal_; }
    [[nodiscard]] const journal::Journal& journal() const noexcept { return journal_; }

    // Exactly one session exists per domain; sessions are presentation-only
    // and are never journaled as source mutations.
    [[nodiscard]] WorkspaceSession& session(WorkspaceDomain domain) noexcept;
    [[nodiscard]] const WorkspaceSession& session(WorkspaceDomain domain) const noexcept;

    // Commits one validated, journaled command and returns its receipt.
    [[nodiscard]] core::Result<WorkspaceApplyReceipt> apply(
        const WorkspaceCommand& command);

    // Undoes the most recent applied command as a forward command. Fails
    // closed when nothing has been applied through this controller.
    [[nodiscard]] core::Result<WorkspaceApplyReceipt> undo();

private:
    [[nodiscard]] core::Result<WorkspaceApplyReceipt> apply_impl(
        const WorkspaceCommand& command, bool record_undo);

    CharacterSourceStore store_;
    std::array<WorkspaceSession, 6U> sessions_;
    journal::Journal journal_;
    // Inverses recorded at apply time, in commit order. Computing the exact
    // inverse requires the pre-mutation state, so it can only happen here.
    std::vector<WorkspaceCommand> undo_stack_;
};

} // namespace carto::workspace
