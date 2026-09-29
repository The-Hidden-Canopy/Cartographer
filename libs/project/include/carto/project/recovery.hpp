#pragma once

#include <carto/core/result.hpp>
#include <carto/journal/checkpoint.hpp>
#include <carto/project/project.hpp>

#include <cstddef>

namespace carto::project {

struct RecoveryReceipt {
    ProjectDocument document;
    core::Revision checkpoint_revision;
    core::Revision recovered_revision;
    std::size_t replayed_entries = 0U;
};

// Recovers a validated project from a verified checkpoint and the bounded
// snapshot-bearing journal entries after it. Unknown payload envelopes fail
// closed; no entry is silently dropped or treated as a patch it cannot prove.
[[nodiscard]] core::Result<RecoveryReceipt> recover_from_journal(
    const journal::CheckpointStore& checkpoints,
    const journal::Journal& journal);

} // namespace carto::project
