#include <carto/d3d12/frame_scheduler.hpp>

#include <string>
#include <utility>

namespace carto::d3d12 {
namespace {

core::Diagnostic invalid_state(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

} // namespace

D3D12FrameScheduler::D3D12FrameScheduler(
    D3D12Device& device,
    std::size_t maximum_in_flight)
    : device_(device), maximum_in_flight_(maximum_in_flight) {}

void D3D12FrameScheduler::retire_completed() noexcept {
    const gpu::SubmissionSerial completed = device_.completed_serial();
    while (in_flight_count_ > 0U &&
           in_flight_[in_flight_begin_] <= completed) {
        in_flight_[in_flight_begin_] = 0U;
        in_flight_begin_ = (in_flight_begin_ + 1U) % in_flight_.size();
        --in_flight_count_;
    }
}

core::Result<void> D3D12FrameScheduler::begin_submission() {
    if (maximum_in_flight_ == 0U ||
        maximum_in_flight_ > kMaximumFramesInFlight) {
        return core::Result<void>::failure(invalid_state(
            "D3D12 frame scheduler limit must be between one and eight frames"));
    }
    if (submission_reserved_) {
        return core::Result<void>::failure(invalid_state(
            "D3D12 frame scheduler already has an untracked submission reservation"));
    }
    if (device_.device_lost()) {
        return core::Result<void>::failure(invalid_state(
            "D3D12 frame scheduler cannot submit on a lost device"));
    }
    retire_completed();
    if (device_.device_lost()) {
        return core::Result<void>::failure(invalid_state(
            "D3D12 frame scheduler observed device loss while retiring submissions"));
    }
    while (in_flight_count_ >= maximum_in_flight_) {
        const gpu::SubmissionSerial oldest = in_flight_[in_flight_begin_];
        if (auto waited = device_.wait(oldest); !waited) return waited;
        retire_completed();
        if (in_flight_count_ > 0U && in_flight_[in_flight_begin_] == oldest) {
            return core::Result<void>::failure(invalid_state(
                "D3D12 frame scheduler wait did not retire its oldest serial"));
        }
    }
    if (device_.device_lost()) {
        return core::Result<void>::failure(invalid_state(
            "D3D12 frame scheduler cannot continue after device loss"));
    }
    submission_reserved_ = true;
    return core::Result<void>::success();
}

core::Result<void> D3D12FrameScheduler::track_submission(
    gpu::SubmissionSerial serial) {
    if (!submission_reserved_ || serial == 0U ||
        serial <= last_tracked_serial_ ||
        serial > device_.last_submitted_serial()) {
        return core::Result<void>::failure(invalid_state(
            "D3D12 frame scheduler received an unreserved, zero, nonmonotonic, or unknown serial"));
    }
    submission_reserved_ = false;
    last_tracked_serial_ = serial;
    const std::size_t append_at =
        (in_flight_begin_ + in_flight_count_) % in_flight_.size();
    in_flight_[append_at] = serial;
    ++in_flight_count_;
    retire_completed();
    return core::Result<void>::success();
}

void D3D12FrameScheduler::cancel_submission() noexcept {
    submission_reserved_ = false;
}

core::Result<void> D3D12FrameScheduler::wait_idle() {
    if (submission_reserved_) {
        return core::Result<void>::failure(invalid_state(
            "D3D12 frame scheduler cannot drain an untracked submission reservation"));
    }
    if (auto waited = device_.wait_idle(); !waited) return waited;
    in_flight_.fill(0U);
    in_flight_begin_ = 0U;
    in_flight_count_ = 0U;
    return core::Result<void>::success();
}

std::size_t D3D12FrameScheduler::in_flight_count() {
    retire_completed();
    return in_flight_count_;
}

} // namespace carto::d3d12
