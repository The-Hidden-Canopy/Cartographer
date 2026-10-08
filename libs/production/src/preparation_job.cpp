#include <carto/production/preparation_job.hpp>

#include <carto/project/project.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace carto::production {
namespace {

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

bool has_diagnostic_suffix(
    const PreparationOperationReport& report,
    std::string_view suffix) {
    return std::any_of(
        report.diagnostics.begin(), report.diagnostics.end(),
        [suffix](const PreparationDiagnostic& diagnostic) {
            return diagnostic.code.ends_with(suffix);
        });
}

PreparationJobState terminal_state(const PreparationOperationReport& report) {
    if (report.succeeded()) return PreparationJobState::published;
    if (has_diagnostic_suffix(report, ".cancelled")) {
        return PreparationJobState::cancelled;
    }
    if (has_diagnostic_suffix(report, ".stale_data")) {
        return PreparationJobState::stale;
    }
    return PreparationJobState::failed;
}

PreparationOperationReport exception_report(
    const project::ProjectDocument& document,
    std::string message) {
    PreparationOperationReport report;
    report.source_revision = document.revision();
    report.terminal_stage = PreparationStage::resolve;
    report.diagnostics.push_back(PreparationDiagnostic{
        "carto.prepare.worker.internal_failure",
        PreparationDiagnosticSeverity::error,
        PreparationStage::resolve,
        document.source_namespace(),
        std::nullopt,
        std::nullopt,
        "preparation.worker",
        std::move(message),
        {},
    });
    return report;
}

} // namespace

const char* preparation_job_state_name(PreparationJobState state) noexcept {
    switch (state) {
    case PreparationJobState::queued: return "queued";
    case PreparationJobState::running: return "running";
    case PreparationJobState::cancellation_requested: return "cancellation-requested";
    case PreparationJobState::published: return "published";
    case PreparationJobState::cancelled: return "cancelled";
    case PreparationJobState::stale: return "stale";
    case PreparationJobState::failed: return "failed";
    }
    return "unknown";
}

bool preparation_job_state_is_terminal(PreparationJobState state) noexcept {
    return state == PreparationJobState::published ||
        state == PreparationJobState::cancelled ||
        state == PreparationJobState::stale ||
        state == PreparationJobState::failed;
}

struct PreparationJob::State final {
    using ExternalStopCallback = std::stop_callback<std::function<void()>>;

    State(
        const project::ProjectDocument& source_document,
        PreparationOperationRequest source_request)
        : document(source_document), request(std::move(source_request)),
          source_revision(document.revision()),
          initially_cancelled(request.stop_token.stop_requested()) {}

    void launch() {
        const std::stop_token external_token = request.stop_token;
        worker = std::jthread([this](std::stop_token worker_token) {
            PreparationJobState expected = PreparationJobState::queued;
            static_cast<void>(lifecycle.compare_exchange_strong(
                expected, PreparationJobState::running,
                std::memory_order_acq_rel));

            PreparationOperationReport completed;
            try {
                auto worker_request = request;
                std::stop_source initial_stop;
                if (initially_cancelled) initial_stop.request_stop();
                worker_request.stop_token = initially_cancelled
                    ? initial_stop.get_token()
                    : worker_token;
                completed = prepare_and_publish(document, worker_request);
            } catch (const std::exception& error) {
                completed = exception_report(document, error.what());
            } catch (...) {
                completed = exception_report(
                    document, "background preparation failed with an unknown exception");
            }
            const PreparationJobState outcome = terminal_state(completed);
            {
                std::lock_guard lock(report_mutex);
                completed_report = std::move(completed);
            }
            lifecycle.store(outcome, std::memory_order_release);
        });
        worker_stop = worker.get_stop_source();

        if (external_token.stop_possible()) {
            external_stop = std::make_unique<ExternalStopCallback>(
                external_token, std::function<void()>{[this] {
                    static_cast<void>(cancel());
                }});
        }
    }

    bool cancel() noexcept {
        PreparationJobState observed = lifecycle.load(std::memory_order_acquire);
        for (;;) {
            if (preparation_job_state_is_terminal(observed)) return false;
            if (observed == PreparationJobState::cancellation_requested) {
                static_cast<void>(worker_stop.request_stop());
                return true;
            }
            if (lifecycle.compare_exchange_weak(
                    observed, PreparationJobState::cancellation_requested,
                    std::memory_order_acq_rel)) {
                static_cast<void>(worker_stop.request_stop());
                return true;
            }
        }
    }

    project::ProjectDocument document;
    PreparationOperationRequest request;
    core::Revision source_revision;
    bool initially_cancelled = false;
    std::atomic<PreparationJobState> lifecycle{PreparationJobState::queued};
    mutable std::mutex report_mutex;
    mutable std::mutex worker_mutex;
    std::optional<PreparationOperationReport> completed_report;
    std::jthread worker;
    std::stop_source worker_stop;
    std::unique_ptr<ExternalStopCallback> external_stop;
};

PreparationJob::PreparationJob(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

core::Result<std::unique_ptr<PreparationJob>> PreparationJob::start(
    const project::ProjectDocument& document,
    PreparationOperationRequest request) {
    try {
        auto state = std::make_unique<State>(document, std::move(request));
        auto job = std::unique_ptr<PreparationJob>(
            new PreparationJob(std::move(state)));
        job->state_->launch();
        return core::Result<std::unique_ptr<PreparationJob>>::success(
            std::move(job));
    } catch (const std::exception& error) {
        return core::Result<std::unique_ptr<PreparationJob>>::failure(io_error(
            "unable to start background preparation: " + std::string(error.what())));
    } catch (...) {
        return core::Result<std::unique_ptr<PreparationJob>>::failure(io_error(
            "unable to start background preparation"));
    }
}

PreparationJob::~PreparationJob() {
    static_cast<void>(request_cancel());
    wait();
}

bool PreparationJob::request_cancel() noexcept {
    return state_ != nullptr && state_->cancel();
}

void PreparationJob::wait() noexcept {
    if (state_ == nullptr) return;
    std::lock_guard lock(state_->worker_mutex);
    if (state_->worker.joinable()) state_->worker.join();
}

PreparationJobSnapshot PreparationJob::snapshot() const {
    PreparationJobSnapshot result;
    if (state_ == nullptr) {
        result.state = PreparationJobState::failed;
        return result;
    }
    result.state = state_->lifecycle.load(std::memory_order_acquire);
    result.source_revision = state_->source_revision;
    if (!preparation_job_state_is_terminal(result.state)) return result;
    std::lock_guard lock(state_->report_mutex);
    if (!state_->completed_report.has_value()) return result;
    result.operation_id = state_->completed_report->operation_id;
    result.terminal_stage = state_->completed_report->terminal_stage;
    result.diagnostic_count = state_->completed_report->diagnostics.size();
    return result;
}

std::optional<PreparationOperationReport> PreparationJob::report() const {
    if (state_ == nullptr || !preparation_job_state_is_terminal(
            state_->lifecycle.load(std::memory_order_acquire))) {
        return std::nullopt;
    }
    std::lock_guard lock(state_->report_mutex);
    return state_->completed_report;
}

} // namespace carto::production
