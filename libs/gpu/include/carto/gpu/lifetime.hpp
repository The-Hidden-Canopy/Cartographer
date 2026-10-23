#pragma once

#include <carto/core/result.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace carto::gpu {

using SubmissionSerial = std::uint64_t;

// Owns retired backend resources until the queue knows that every submission
// which could reference them has completed. The queue is backend-neutral: a
// Device supplies the resource object and reports completed serials, while
// the object destructor performs the actual backend cleanup.
class DeferredDestructionQueue {
public:
    template <typename T>
    [[nodiscard]] core::Result<void> retire(SubmissionSerial retire_after,
                                             std::shared_ptr<T> resource) {
        if (!resource) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "deferred destruction requires a live resource"));
        }
        pending_.push_back({retire_after, std::move(resource)});
        return core::Result<void>::success();
    }

    [[nodiscard]] std::size_t collect(SubmissionSerial completed) noexcept;
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }

private:
    struct Entry {
        SubmissionSerial retire_after;
        std::shared_ptr<void> resource;
    };

    std::vector<Entry> pending_;
};

} // namespace carto::gpu
