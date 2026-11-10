#include "../third_party/imgui/backends/carto_dx12_frame_admission.hpp"

#include <cstdint>
#include <limits>

int main() {
    using carto::desktop::d3d12_frame_admission;
    using carto::desktop::D3D12FrameAdmission;
    constexpr auto invalid = std::numeric_limits<std::uint64_t>::max();
    if (d3d12_frame_admission(0, 0, 0) != D3D12FrameAdmission::ready ||
        d3d12_frame_admission(4, 5, 7) != D3D12FrameAdmission::pending ||
        d3d12_frame_admission(5, 5, 7) != D3D12FrameAdmission::ready ||
        d3d12_frame_admission(invalid, 5, 7) != D3D12FrameAdmission::invalid ||
        d3d12_frame_admission(7, invalid, 7) != D3D12FrameAdmission::invalid ||
        d3d12_frame_admission(8, 7, 7) != D3D12FrameAdmission::invalid ||
        d3d12_frame_admission(7, 8, 7) != D3D12FrameAdmission::invalid ||
        d3d12_frame_admission(7, 7, invalid - 1) != D3D12FrameAdmission::invalid)
        return 1;
    return 0;
}
