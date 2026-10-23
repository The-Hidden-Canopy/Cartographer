#pragma once

#include <carto/eval/cache.hpp>
#include <carto/eval/receipt.hpp>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace carto::eval {

// A scheduler result is a candidate only. The scheduler never publishes it to
// a graph or cache; the owner must revalidate the ticket before committing it.
struct EvaluationCandidate {
    EvaluationTicket ticket;
    EvaluationReceipt receipt;
    std::optional<EvaluationCacheValue> output;
    std::optional<core::Diagnostic> diagnostic;
};

using EvaluationFunction = std::function<core::Result<EvaluationCacheValue>()>;

class EvaluationScheduler {
public:
    explicit EvaluationScheduler(
        std::size_t worker_count = 1U,
        std::size_t max_outstanding = 64U);
    ~EvaluationScheduler();

    EvaluationScheduler(const EvaluationScheduler&) = delete;
    EvaluationScheduler& operator=(const EvaluationScheduler&) = delete;
    EvaluationScheduler(EvaluationScheduler&&) = delete;
    EvaluationScheduler& operator=(EvaluationScheduler&&) = delete;

    [[nodiscard]] core::Result<void> submit(
        EvaluationTicket ticket,
        EvaluationFunction evaluate);
    [[nodiscard]] std::optional<EvaluationCandidate> poll();
    [[nodiscard]] std::size_t outstanding() const noexcept;
    void stop() noexcept;

private:
    struct Job {
        EvaluationTicket ticket;
        EvaluationFunction evaluate;
    };

    void worker(std::stop_token stop_token);

    mutable std::mutex mutex_;
    std::condition_variable_any work_available_;
    std::deque<Job> jobs_;
    std::deque<EvaluationCandidate> completed_;
    std::vector<std::jthread> workers_;
    std::size_t max_outstanding_;
    std::size_t outstanding_ = 0U;
    bool stopping_ = false;
};

} // namespace carto::eval
