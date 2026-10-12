#pragma once

#include <carto/core/result.hpp>
#include <carto/production/preparation_operation.hpp>

#include <cstddef>
#include <memory>
#include <optional>

namespace carto::project {
class ProjectDocument;
}

namespace carto::production {

enum class PreparationJobState : std::uint8_t {
    queued,
    running,
    cancellation_requested,
    published,
    cancelled,
    stale,
    failed,
};

[[nodiscard]] const char* preparation_job_state_name(
    PreparationJobState state) noexcept;
[[nodiscard]] bool preparation_job_state_is_terminal(
    PreparationJobState state) noexcept;

struct PreparationJobSnapshot {
    PreparationJobState state = PreparationJobState::queued;
    core::Revision source_revision;
    std::optional<assets::Sha256Digest> operation_id;
    std::optional<PreparationStage> terminal_stage;
    std::size_t diagnostic_count = 0U;
};

// Owns one immutable ProjectDocument snapshot and executes the same
// prepare_and_publish operation used by synchronous clients on a background
// thread. Destruction requests cancellation and joins; it never detaches a
// worker that could outlive its captured source or output policy.
class PreparationJob final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<PreparationJob>> start(
        const project::ProjectDocument& document,
        PreparationOperationRequest request);

    ~PreparationJob();
    PreparationJob(const PreparationJob&) = delete;
    PreparationJob& operator=(const PreparationJob&) = delete;
    PreparationJob(PreparationJob&&) = delete;
    PreparationJob& operator=(PreparationJob&&) = delete;

    // Returns true when cancellation was accepted before a terminal outcome.
    [[nodiscard]] bool request_cancel() noexcept;
    void wait() noexcept;

    [[nodiscard]] PreparationJobSnapshot snapshot() const;
    // A report becomes visible only after a terminal state is published.
    [[nodiscard]] std::optional<PreparationOperationReport> report() const;

private:
    struct State;
    explicit PreparationJob(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

} // namespace carto::production
