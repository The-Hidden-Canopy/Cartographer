#include <carto/gpu/lifetime.hpp>

#include <algorithm>

namespace carto::gpu {

std::size_t DeferredDestructionQueue::collect(SubmissionSerial completed) noexcept {
    const auto old_size = pending_.size();
    pending_.erase(
        std::remove_if(
            pending_.begin(), pending_.end(),
            [completed](const Entry& entry) { return entry.retire_after <= completed; }),
        pending_.end());
    return old_size - pending_.size();
}

} // namespace carto::gpu
