#pragma once

#include <cstdint>
#include <limits>

namespace carto::desktop {

enum class D3D12FrameAdmission { ready, pending, invalid };

[[nodiscard]] constexpr D3D12FrameAdmission d3d12_frame_admission(
    std::uint64_t completed, std::uint64_t submitted, std::uint64_t last_signal) noexcept {
    constexpr auto invalid = std::numeric_limits<std::uint64_t>::max();
    if (completed == invalid || submitted == invalid || last_signal >= invalid - 1U ||
        completed > last_signal || submitted > last_signal)
        return D3D12FrameAdmission::invalid;
    return completed < submitted ? D3D12FrameAdmission::pending : D3D12FrameAdmission::ready;
}

} // namespace carto::desktop
