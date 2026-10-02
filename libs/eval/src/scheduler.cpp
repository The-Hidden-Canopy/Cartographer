#include <carto/eval/scheduler.hpp>

#include <algorithm>
#include <exception>
#include <utility>

namespace carto::eval {

namespace {

constexpr std::size_t kMaxWorkers = 64U;
constexpr std::size_t kMaxOutstanding = 1'000'000U;

core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, std::move(message)));
}

core::Diagnostic stopped_diagnostic() {
    return core::Diagnostic(
        core::ErrorCode::invalid_state, "evaluation scheduler is stopped");
}

EvaluationCandidate failed_candidate(
    EvaluationTicket ticket,
    core::Diagnostic diagnostic) {
    EvaluationCandidate candidate;
    candidate.receipt.node = ticket.node;
    candidate.receipt.graph_revision = ticket.graph_revision;
    candidate.receipt.parameter_revision = ticket.parameter_revision;
    candidate.receipt.dependency_digest = ticket.dependency_digest;
    candidate.receipt.status = EvaluationStatus::failed;
    candidate.diagnostic = std::move(diagnostic);
    candidate.ticket = std::move(ticket);
    return candidate;
}

} // namespace

EvaluationScheduler::EvaluationScheduler(
    std::size_t worker_count,
    std::size_t max_outstanding)
    : max_outstanding_(std::clamp(
          max_outstanding == 0U ? std::size_t{1U} : max_outstanding,
          std::size_t{1U},
          kMaxOutstanding)) {
    if (worker_count == 0U) worker_count = 1U;
    worker_count = std::min(worker_count, kMaxWorkers);
    workers_.reserve(worker_count);
    for (std::size_t index = 0U; index < worker_count; ++index) {
        workers_.emplace_back([this](std::stop_token stop_token) {
            worker(stop_token);
        });
    }
}

EvaluationScheduler::~EvaluationScheduler() {
    stop();
}

core::Result<void> EvaluationScheduler::submit(
    EvaluationTicket ticket,
    EvaluationFunction evaluate) {
    if (auto result = validate(ticket); !result) return result;
    if (!evaluate) return invalid("evaluation scheduler requires an evaluator");

    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            return core::Result<void>::failure(stopped_diagnostic());
        }
        if (outstanding_ >= max_outstanding_) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "evaluation scheduler outstanding-job limit is full"));
        }
        jobs_.push_back(Job{std::move(ticket), std::move(evaluate)});
        ++outstanding_;
    }
    work_available_.notify_one();
    return core::Result<void>::success();
}

std::optional<EvaluationCandidate> EvaluationScheduler::poll() {
    std::lock_guard lock(mutex_);
    if (completed_.empty()) return std::nullopt;
    EvaluationCandidate candidate = std::move(completed_.front());
    completed_.pop_front();
    --outstanding_;
    return candidate;
}

std::size_t EvaluationScheduler::outstanding() const noexcept {
    std::lock_guard lock(mutex_);
    return outstanding_;
}

void EvaluationScheduler::stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    work_available_.notify_all();
    workers_.clear();
}

void EvaluationScheduler::worker(std::stop_token stop_token) {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            work_available_.wait(lock, stop_token, [this] {
                return stopping_ || !jobs_.empty();
            });
            if (jobs_.empty() && (stopping_ || stop_token.stop_requested())) return;
            if (jobs_.empty()) continue;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        EvaluationCandidate candidate;
        candidate.ticket = job.ticket;
        candidate.receipt.node = job.ticket.node;
        candidate.receipt.graph_revision = job.ticket.graph_revision;
        candidate.receipt.parameter_revision = job.ticket.parameter_revision;
        candidate.receipt.dependency_digest = job.ticket.dependency_digest;
        try {
            auto output = job.evaluate();
            if (!output) {
                candidate.receipt.status = EvaluationStatus::failed;
                candidate.diagnostic = output.error();
            } else {
                candidate.receipt.status = EvaluationStatus::evaluated;
                candidate.receipt.output_digest = output.value().output_digest;
                candidate.output = std::move(output.value());
                if (auto result = validate(candidate.receipt); !result) {
                    candidate.receipt.status = EvaluationStatus::failed;
                    candidate.receipt.output_digest.clear();
                    candidate.output.reset();
                    candidate.diagnostic = result.error();
                }
            }
        } catch (const std::exception& error) {
            candidate = failed_candidate(
                std::move(job.ticket),
                core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    std::string("evaluation worker threw: ") + error.what()));
        } catch (...) {
            candidate = failed_candidate(
                std::move(job.ticket),
                core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "evaluation worker threw an unknown exception"));
        }

        {
            std::lock_guard lock(mutex_);
            completed_.push_back(std::move(candidate));
        }
    }
}

} // namespace carto::eval
