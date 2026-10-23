#include <carto/production/bake_job.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace carto::production {
namespace {

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

bool has_cancelled_diagnostic(const BakeExecutionReport& report) {
    return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
        [](const BakeDiagnostic& diagnostic) {
            return diagnostic.code == "bake_cancelled";
        });
}

void append_cancelled_diagnostic(BakeExecutionReport& report) {
    if (has_cancelled_diagnostic(report)) return;
    report.diagnostics.push_back(BakeDiagnostic{
        "bake_cancelled",
        "normal-map bake was cancelled",
        true,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    });
}

BakeExecutionReport worker_failure_report(std::string message) {
    BakeExecutionReport report;
    report.diagnostics.push_back(BakeDiagnostic{
        "bake_worker_internal_failure",
        std::move(message),
        true,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    });
    return report;
}

BakeJobState terminal_state(const BakeExecutionReport& report) {
    if (has_cancelled_diagnostic(report)) return BakeJobState::cancelled;
    return report.succeeded() ? BakeJobState::completed : BakeJobState::failed;
}

} // namespace

const char* bake_job_state_name(BakeJobState state) noexcept {
    switch (state) {
    case BakeJobState::queued: return "queued";
    case BakeJobState::running: return "running";
    case BakeJobState::cancellation_requested: return "cancellation-requested";
    case BakeJobState::completed: return "completed";
    case BakeJobState::cancelled: return "cancelled";
    case BakeJobState::failed: return "failed";
    }
    return "unknown";
}

bool bake_job_state_is_terminal(BakeJobState state) noexcept {
    return state == BakeJobState::completed ||
        state == BakeJobState::cancelled ||
        state == BakeJobState::failed;
}

struct BakeJob::State final {
    using ExternalStopCallback = std::stop_callback<std::function<void()>>;

    State(
        BakeRecipe source_recipe,
        BakeNormalSpace source_normal_space,
        const geometry::EditableMesh& source_mesh,
        const geometry::EditableMesh& target_mesh,
        std::stop_token source_external_stop_token)
        : recipe(std::move(source_recipe)),
          normal_space(source_normal_space),
          source(source_mesh),
          target(target_mesh),
          external_stop_token(source_external_stop_token) {}

    void launch() {
        if (external_stop_token.stop_possible()) {
            external_stop = std::make_unique<ExternalStopCallback>(
                external_stop_token,
                std::function<void()>{[this] { static_cast<void>(cancel()); }});
        }

        worker = std::jthread([this](std::stop_token) { run(); });
    }

    bool cancel() noexcept {
        BakeJobState observed = lifecycle.load(std::memory_order_acquire);
        for (;;) {
            if (bake_job_state_is_terminal(observed)) return false;
            if (observed == BakeJobState::cancellation_requested) {
                static_cast<void>(stop_source.request_stop());
                return true;
            }
            if (lifecycle.compare_exchange_weak(
                    observed,
                    BakeJobState::cancellation_requested,
                    std::memory_order_acq_rel)) {
                static_cast<void>(stop_source.request_stop());
                return true;
            }
        }
    }

    void run() noexcept {
        BakeJobState expected = BakeJobState::queued;
        static_cast<void>(lifecycle.compare_exchange_strong(
            expected, BakeJobState::running, std::memory_order_acq_rel));

        BakeExecutionReport completed;
        try {
            completed = normal_space == BakeNormalSpace::tangent
                ? compile_tangent_normal_map_cpu(
                    recipe, source, target, stop_source.get_token())
                : compile_object_space_normal_map_cpu(
                    recipe, source, target, stop_source.get_token());
        } catch (const std::exception& error) {
            completed = worker_failure_report(error.what());
        } catch (...) {
            completed = worker_failure_report(
                "background normal-map bake failed with an unknown exception");
        }

        BakeJobState outcome = terminal_state(completed);
        {
            std::lock_guard lock(report_mutex);
            completed_report = std::move(completed);
        }

        BakeJobState observed = lifecycle.load(std::memory_order_acquire);
        for (;;) {
            if (observed == BakeJobState::cancellation_requested ||
                stop_source.stop_requested()) {
                {
                    std::lock_guard lock(report_mutex);
                    if (completed_report.has_value()) {
                        completed_report->artifact.reset();
                        append_cancelled_diagnostic(*completed_report);
                    }
                }
                lifecycle.store(BakeJobState::cancelled, std::memory_order_release);
                return;
            }
            if (bake_job_state_is_terminal(observed)) return;
            if (lifecycle.compare_exchange_weak(
                    observed, outcome, std::memory_order_acq_rel)) {
                return;
            }
        }
    }

    BakeRecipe recipe;
    BakeNormalSpace normal_space = BakeNormalSpace::tangent;
    geometry::EditableMesh source;
    geometry::EditableMesh target;
    std::stop_token external_stop_token;
    std::atomic<BakeJobState> lifecycle{BakeJobState::queued};
    std::stop_source stop_source;
    mutable std::mutex report_mutex;
    mutable std::mutex worker_mutex;
    std::optional<BakeExecutionReport> completed_report;
    std::unique_ptr<ExternalStopCallback> external_stop;
    std::jthread worker;
};

BakeJob::BakeJob(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

core::Result<std::unique_ptr<BakeJob>> BakeJob::start(
    BakeRecipe recipe,
    BakeNormalSpace normal_space,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    std::stop_token external_stop_token) {
    try {
        auto state = std::make_unique<State>(
            std::move(recipe), normal_space, source, target, external_stop_token);
        auto job = std::unique_ptr<BakeJob>(new BakeJob(std::move(state)));
        job->state_->launch();
        return core::Result<std::unique_ptr<BakeJob>>::success(std::move(job));
    } catch (const std::exception& error) {
        return core::Result<std::unique_ptr<BakeJob>>::failure(io_error(
            "unable to start background bake: " + std::string(error.what())));
    } catch (...) {
        return core::Result<std::unique_ptr<BakeJob>>::failure(io_error(
            "unable to start background bake"));
    }
}

BakeJob::~BakeJob() {
    static_cast<void>(request_cancel());
    wait();
}

bool BakeJob::request_cancel() noexcept {
    return state_ != nullptr && state_->cancel();
}

void BakeJob::wait() noexcept {
    if (state_ == nullptr) return;
    std::lock_guard lock(state_->worker_mutex);
    if (state_->worker.joinable()) state_->worker.join();
}

BakeJobSnapshot BakeJob::snapshot() const {
    BakeJobSnapshot result;
    if (state_ == nullptr) {
        result.state = BakeJobState::failed;
        return result;
    }
    result.state = state_->lifecycle.load(std::memory_order_acquire);
    result.recipe_identity = state_->recipe.identity;
    result.source_revision = state_->recipe.source.revision;
    result.target_revision = state_->recipe.target.revision;
    if (!bake_job_state_is_terminal(result.state)) return result;
    std::lock_guard lock(state_->report_mutex);
    if (state_->completed_report.has_value()) {
        result.diagnostic_count = state_->completed_report->diagnostics.size();
    }
    return result;
}

std::optional<BakeExecutionReport> BakeJob::report() const {
    if (state_ == nullptr || !bake_job_state_is_terminal(
            state_->lifecycle.load(std::memory_order_acquire))) {
        return std::nullopt;
    }
    std::lock_guard lock(state_->report_mutex);
    return state_->completed_report;
}

} // namespace carto::production
