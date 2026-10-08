#include <carto/project/recovery.hpp>

#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace carto::project {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_metadata_value(std::string_view value) {
    return !value.empty() && value.find_first_of("\r\n") == std::string_view::npos;
}

core::Result<std::string> snapshot_from_entry(const journal::JournalEntry& entry) {
    const std::string payload(entry.payload.begin(), entry.payload.end());
    std::size_t cursor = 0U;
    const std::size_t header_end = payload.find('\n', cursor);
    if (header_end == std::string::npos) {
        return core::Result<std::string>::failure(
            validation("journal snapshot envelope header is incomplete"));
    }
    const std::string_view header(payload.data(), header_end);
    const bool project_snapshot = header == "CARTOGRAPHER_PROJECT_SNAPSHOT_V1";
    const bool application_mutation = header == "CARTOGRAPHER_APPLICATION_MUTATION_V1";
    const bool project_transaction = header == "CARTOGRAPHER_PROJECT_TRANSACTION_V1";
    if (!project_snapshot && !application_mutation && !project_transaction) {
        return core::Result<std::string>::failure(
            core::Diagnostic(core::ErrorCode::unsupported,
                             "journal entry does not contain a supported project snapshot envelope"));
    }

    cursor = header_end + 1U;
    bool actor_seen = false;
    bool operation_seen = false;
    bool document_bytes_seen = false;
    std::uint64_t declared_bytes = 0U;
    std::size_t document_start = 0U;
    while (!document_bytes_seen) {
        const std::size_t line_end = payload.find('\n', cursor);
        if (line_end == std::string::npos) {
            return core::Result<std::string>::failure(
                validation("journal snapshot envelope metadata is incomplete"));
        }
        const std::string_view line(payload.data() + cursor, line_end - cursor);
        const std::size_t separator = line.find('=');
        if (separator == std::string_view::npos || separator == 0U) {
            return core::Result<std::string>::failure(
                validation("journal snapshot envelope metadata is malformed"));
        }
        const std::string_view key = line.substr(0U, separator);
        const std::string_view value = line.substr(separator + 1U);
        if (key == "actor") {
            if (actor_seen || !project_transaction || !safe_metadata_value(value)) {
                return core::Result<std::string>::failure(
                    validation("journal snapshot envelope actor metadata is invalid"));
            }
            actor_seen = true;
        } else if (key == "operation") {
            if (operation_seen || project_snapshot || !safe_metadata_value(value)) {
                return core::Result<std::string>::failure(
                    validation("journal snapshot envelope operation metadata is invalid"));
            }
            operation_seen = true;
        } else if (key == "document_bytes") {
            if (document_bytes_seen || value.empty()) {
                return core::Result<std::string>::failure(
                    validation("journal snapshot envelope byte count is invalid"));
            }
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), declared_bytes);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                declared_bytes > std::numeric_limits<std::size_t>::max()) {
                return core::Result<std::string>::failure(
                    invalid("journal snapshot envelope byte count is invalid"));
            }
            document_bytes_seen = true;
            document_start = line_end + 1U;
        } else {
            return core::Result<std::string>::failure(
                validation("journal snapshot envelope contains an unknown metadata field"));
        }
        cursor = line_end + 1U;
    }

    if ((project_transaction && (!actor_seen || !operation_seen)) ||
        (application_mutation && !operation_seen) ||
        (project_snapshot && (actor_seen || operation_seen))) {
        return core::Result<std::string>::failure(
            validation("journal snapshot envelope metadata does not match its header"));
    }
    if (declared_bytes != payload.size() - document_start) {
        return core::Result<std::string>::failure(
            validation("journal snapshot envelope byte count does not match its payload"));
    }
    return core::Result<std::string>::success(payload.substr(document_start));
}

} // namespace

core::Result<RecoveryReceipt> recover_from_journal(
    const journal::CheckpointStore& checkpoints,
    const journal::Journal& journal) {
    const auto candidate = checkpoints.recover(journal);
    if (!candidate) {
        return core::Result<RecoveryReceipt>::failure(candidate.error().with_context(
            "project checkpoint recovery"));
    }

    const std::string checkpoint_text(candidate.value().snapshot.begin(), candidate.value().snapshot.end());
    auto checkpoint_document = ProjectDocument::deserialize(checkpoint_text);
    if (!checkpoint_document) {
        return core::Result<RecoveryReceipt>::failure(checkpoint_document.error().with_context(
            "checkpoint project snapshot"));
    }
    std::optional<ProjectDocument> document;
    document.emplace(std::move(checkpoint_document.value()));
    if (document->revision() != candidate.value().checkpoint.project_revision) {
        return core::Result<RecoveryReceipt>::failure(
            stale("checkpoint project revision does not match its metadata"));
    }

    core::Revision current_revision = document->revision();
    for (const auto& entry : candidate.value().pending_entries) {
        if (entry.revision_before != current_revision) {
            return core::Result<RecoveryReceipt>::failure(
                stale("pending journal entry does not start at the recovered revision"));
        }
        const auto snapshot = snapshot_from_entry(entry);
        if (!snapshot) {
            return core::Result<RecoveryReceipt>::failure(snapshot.error().with_context(
                "pending journal entry"));
        }
        auto next = ProjectDocument::deserialize(snapshot.value());
        if (!next) {
            return core::Result<RecoveryReceipt>::failure(next.error().with_context(
                "pending project snapshot"));
        }
        if (next.value().revision() != entry.revision_after) {
            return core::Result<RecoveryReceipt>::failure(
                stale("pending project snapshot revision does not match its journal entry"));
        }
        document.emplace(std::move(next.value()));
        current_revision = document->revision();
    }

    if (auto result = document->validate(); !result) {
        return core::Result<RecoveryReceipt>::failure(result.error().with_context(
            "recovered project"));
    }
    return core::Result<RecoveryReceipt>::success(RecoveryReceipt{
        std::move(document.value()),
        candidate.value().checkpoint.project_revision,
        current_revision,
        candidate.value().pending_entries.size(),
    });
}

core::Result<SnapshotRecoveryReceipt> recover_latest_snapshot(
    const journal::Journal& journal) {
    const auto entries = journal.read_all();
    if (!entries) {
        return core::Result<SnapshotRecoveryReceipt>::failure(entries.error().with_context(
            "latest snapshot recovery journal verification"));
    }
    if (entries.value().empty()) {
        return core::Result<SnapshotRecoveryReceipt>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "journal contains no snapshot-bearing recovery entry"));
    }

    const journal::JournalEntry& entry = entries.value().back();
    const auto snapshot = snapshot_from_entry(entry);
    if (!snapshot) {
        return core::Result<SnapshotRecoveryReceipt>::failure(snapshot.error().with_context(
            "latest snapshot recovery entry"));
    }
    auto document = ProjectDocument::deserialize(snapshot.value());
    if (!document) {
        return core::Result<SnapshotRecoveryReceipt>::failure(document.error().with_context(
            "latest snapshot recovery document"));
    }
    if (document.value().revision() != entry.revision_after) {
        return core::Result<SnapshotRecoveryReceipt>::failure(stale(
            "latest snapshot recovery revision does not match its journal entry"));
    }
    if (auto result = document.value().validate(); !result) {
        return core::Result<SnapshotRecoveryReceipt>::failure(result.error().with_context(
            "latest snapshot recovery validation"));
    }
    return core::Result<SnapshotRecoveryReceipt>::success(SnapshotRecoveryReceipt{
        std::move(document.value()), entry.revision_after, entries.value().size()});
}

} // namespace carto::project
