#pragma once

#include <carto/core/result.hpp>
#include <carto/d3d12/device.hpp>

#include <array>
#include <cstddef>

namespace carto::d3d12 {

// Single-threaded fence scheduler for bounded frames in flight. Call
// begin_submission before recording/submitting work, then track a serial
// returned by this device or cancel the reservation if submission failed.
class D3D12FrameScheduler final {
public:
    static constexpr std::size_t kMaximumFramesInFlight = 8U;

    explicit D3D12FrameScheduler(
        D3D12Device& device,
        std::size_t maximum_in_flight = 2U);

    D3D12FrameScheduler(const D3D12FrameScheduler&) = delete;
    D3D12FrameScheduler& operator=(const D3D12FrameScheduler&) = delete;

    [[nodiscard]] core::Result<void> begin_submission();
    [[nodiscard]] core::Result<void> track_submission(gpu::SubmissionSerial serial);
    void cancel_submission() noexcept;
    [[nodiscard]] core::Result<void> wait_idle();
    [[nodiscard]] std::size_t in_flight_count();

private:
    void retire_completed() noexcept;

    D3D12Device& device_;
    std::size_t maximum_in_flight_ = 0U;
    // Fixed storage avoids allocating after a successful GPU submission.
    std::array<gpu::SubmissionSerial, kMaximumFramesInFlight> in_flight_{};
    std::size_t in_flight_begin_ = 0U;
    std::size_t in_flight_count_ = 0U;
    gpu::SubmissionSerial last_tracked_serial_ = 0U;
    bool submission_reserved_ = false;
};

} // namespace carto::d3d12
