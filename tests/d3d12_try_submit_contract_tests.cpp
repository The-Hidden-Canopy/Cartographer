#include <carto/d3d12/device.hpp>

#include <array>
#include <iostream>
#include <limits>

namespace {
using carto::d3d12::detail::ContextFenceAdmission;
using carto::d3d12::detail::context_fence_admission;
using carto::gpu::SubmissionSerial;

// Pure fake timeline: it checks admission and records whether the caller
// would consume a context or encode. It does not call the D3D12 device.
struct FakeRing {
    std::array<SubmissionSerial, 2> submitted{3, 6};
    std::array<SubmissionSerial, 2> completed{2, 6};
    std::size_t next = 0;
    int encoded = 0;
    ContextFenceAdmission step() {
        const auto status = context_fence_admission(submitted[next], completed[next]);
        if (status == ContextFenceAdmission::ready) {
            next = (next + 1) % submitted.size();
            ++encoded;
        }
        return status;
    }
};
} // namespace

int main() {
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << message << '\n'; ok = false; }
    };
    check(context_fence_admission(0, 0) == ContextFenceAdmission::ready,
        "unused context should be available");
    check(context_fence_admission(7, 6) == ContextFenceAdmission::pending &&
          context_fence_admission(7, 7) == ContextFenceAdmission::ready &&
          context_fence_admission(7, 8) == ContextFenceAdmission::ready,
        "admission must compare the context's own submitted fence");
    check(context_fence_admission(7, std::numeric_limits<SubmissionSerial>::max()) ==
          ContextFenceAdmission::invalid_completion &&
          context_fence_admission(0, std::numeric_limits<SubmissionSerial>::max()) ==
          ContextFenceAdmission::invalid_completion,
        "device-removed sentinel must not be treated as completion");
    FakeRing ring;
    check(ring.step() == ContextFenceAdmission::pending && ring.next == 0 && ring.encoded == 0,
        "pending must not advance the ring or encode");
    ring.completed[0] = std::numeric_limits<SubmissionSerial>::max();
    check(ring.step() == ContextFenceAdmission::invalid_completion && ring.next == 0 && ring.encoded == 0,
        "invalid completion must not advance or encode");
    ring.completed[0] = 3;
    check(ring.step() == ContextFenceAdmission::ready && ring.next == 1 && ring.encoded == 1,
        "retired context may advance and encode");
    return ok ? 0 : 1;
}
