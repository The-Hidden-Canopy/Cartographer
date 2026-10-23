#pragma once

#include <carto/core/result.hpp>
#include <carto/production/bake_graph.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace carto::production {

enum class BakeJobState : std::uint8_t {
    queued,
    running,
    cancellation_requested,
    completed,
    cancelled,
    failed,
};

[[nodiscard]] const char* bake_job_state_name(BakeJobState state) noexcept;
[[nodiscard]] bool bake_job_state_is_terminal(BakeJobState state) noexcept;

struct BakeJobSnapshot {
    BakeJobState state = BakeJobState::queued;
    std::string recipe_identity;
    core::Revision source_revision;
    core::Revision target_revision;
    std::size_t diagnostic_count = 0U;
};

// Owns immutable source and target snapshots and runs the tangent- or
// object-space normal-map CPU reference compiler in the background. It never
// publishes artifacts. The caller must publish a completed artifact against
// current meshes through publish_normal_map().
class BakeJob final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<BakeJob>> start(
        BakeRecipe recipe,
        BakeNormalSpace normal_space,
        const geometry::EditableMesh& source,
        const geometry::EditableMesh& target,
        std::stop_token external_stop_token = {});

    ~BakeJob();
    BakeJob(const BakeJob&) = delete;
    BakeJob& operator=(const BakeJob&) = delete;
    BakeJob(BakeJob&&) = delete;
    BakeJob& operator=(BakeJob&&) = delete;

    [[nodiscard]] bool request_cancel() noexcept;
    void wait() noexcept;

    [[nodiscard]] BakeJobSnapshot snapshot() const;
    [[nodiscard]] std::optional<BakeExecutionReport> report() const;

private:
    struct State;
    explicit BakeJob(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

} // namespace carto::production
