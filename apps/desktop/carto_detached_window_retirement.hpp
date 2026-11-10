#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace carto::studio {

// Policy for a detached window whose native and GPU resources are owned as one
// bundle. The caller must route all of this window's submissions and presents
// through the frame gate, serialize those calls with its queue, and retain the
// device, queue and descriptor allocator needed by the bundle. This class does
// not install ImGui callbacks, wait on a driver, or prove a platform present
// has retired. Its caller must supply independent, nonblocking observations.
template<class ResourceBundle>
class DetachedWindowRetirement {
public:
    enum class FrameResult { not_submitted, submitted_without_present, presented, uncertain };
    enum class PresentObservation { pending, retired, uncertain };
    enum class Intent { none, resize, close };
    struct Extent { std::uint32_t width = 0, height = 0; };

    explicit DetachedWindowRetirement(std::unique_ptr<ResourceBundle> resources) noexcept
        : resources_(std::move(resources)), quarantined_(!resources_) {}
    DetachedWindowRetirement(const DetachedWindowRetirement&) = delete;
    DetachedWindowRetirement& operator=(const DetachedWindowRetirement&) = delete;
    DetachedWindowRetirement(DetachedWindowRetirement&&) = delete;
    DetachedWindowRetirement& operator=(DetachedWindowRetirement&&) = delete;

    // Dropping an owner without explicit, proven retirement must never destroy
    // potentially in-flight native/GPU objects. Such a bundle is intentionally
    // retained until process exit. Production callers must also retain its
    // device, queue and any allocator it refers to inside that bundle.
    ~DetachedWindowRetirement() { static_cast<void>(resources_.release()); }

    [[nodiscard]] bool begin_frame() noexcept {
        if (quarantined_ || !resources_ || frame_active_ || intent_ != Intent::none || resizing_)
            return false;
        frame_active_ = true;
        return true;
    }
    [[nodiscard]] bool finish_frame(FrameResult result) noexcept {
        if (!frame_active_ || quarantined_) return false;
        frame_active_ = false;
        if (result == FrameResult::uncertain) {
            quarantined_ = true;
            return false;
        }
        if (result == FrameResult::presented) present_pending_ = true;
        return true;
    }
    [[nodiscard]] bool request_resize(std::uint32_t width, std::uint32_t height) noexcept {
        if (quarantined_ || !resources_ || resizing_ || intent_ == Intent::close || !width || !height)
            return false;
        extent_ = {width, height};
        intent_ = Intent::resize;
        return true;
    }
    [[nodiscard]] bool request_close() noexcept {
        if (quarantined_ || !resources_ || resizing_) return false;
        intent_ = Intent::close;
        return true;
    }
    // With all frame submissions stopped, the owner queues a globally unique,
    // strictly increasing value on the same queue and fence, after every draw
    // for this window. A shared-fence allocator must serialize all windows and
    // never reuse or issue a previously completed value. This object alone
    // cannot prove that external ordering; do not integrate without that hook.
    // Call signal_queued only after the queue call succeeds; on failure or an
    // uncertain outcome call signal_uncertain. No submission may bypass the gate.
    [[nodiscard]] bool needs_signal() const noexcept {
        return resources_ && !quarantined_ && !frame_active_ && !resizing_ &&
               intent_ != Intent::none && !signal_pending_;
    }
    [[nodiscard]] bool signal_queued(std::uint64_t fence_value) noexcept {
        if (!needs_signal() || fence_value == 0 ||
            fence_value == std::numeric_limits<std::uint64_t>::max() || fence_value <= last_fence_)
            return false;
        last_fence_ = fence_value;
        signal_pending_ = true;
        fence_retired_ = false;
        return true;
    }
    void signal_uncertain() noexcept { quarantined_ = true; }
    // UINT64_MAX is D3D12's device-removal sentinel, never completion proof.
    // This consumes a single nonblocking observation and performs no wait.
    void observe_fence(std::uint64_t completed) noexcept {
        if (quarantined_ || !signal_pending_) return;
        if (completed == std::numeric_limits<std::uint64_t>::max() || completed < last_completed_) {
            quarantined_ = true;
        } else {
            last_completed_ = completed;
            if (completed >= last_fence_) fence_retired_ = true;
        }
    }
    void observe_present(PresentObservation observation) noexcept {
        if (quarantined_ || !signal_pending_ || !present_pending_) return;
        if (observation == PresentObservation::uncertain) quarantined_ = true;
        else if (observation == PresentObservation::retired) present_pending_ = false;
    }
    [[nodiscard]] bool ready() const noexcept {
        return resources_ && !quarantined_ && !frame_active_ && !resizing_ &&
               signal_pending_ && fence_retired_ && !present_pending_;
    }
    [[nodiscard]] bool begin_resize(Extent& extent) noexcept {
        if (!ready() || intent_ != Intent::resize) return false;
        extent = extent_;
        resizing_ = true;
        return true;
    }
    // A failed or uncertain native resize may have changed resource ownership.
    // Quarantine the complete bundle rather than attempting to reuse it.
    [[nodiscard]] bool finish_resize(bool succeeded) noexcept {
        if (!resizing_ || quarantined_) return false;
        resizing_ = false;
        if (!succeeded) { quarantined_ = true; return false; }
        intent_ = Intent::none;
        signal_pending_ = false;
        fence_retired_ = false;
        return true;
    }
    // Ownership transfers only after explicit close, queue retirement, and
    // presentation retirement. Native teardown must run on the owning thread.
    [[nodiscard]] std::unique_ptr<ResourceBundle> take_retired() noexcept {
        if (!ready() || intent_ != Intent::close) return {};
        intent_ = Intent::none;
        signal_pending_ = false;
        return std::move(resources_);
    }
    [[nodiscard]] bool quarantined() const noexcept { return quarantined_; }
    [[nodiscard]] Intent intent() const noexcept { return intent_; }
    [[nodiscard]] ResourceBundle* resources() const noexcept { return resources_.get(); }

private:
    std::unique_ptr<ResourceBundle> resources_;
    Extent extent_{};
    std::uint64_t last_fence_ = 0, last_completed_ = 0;
    Intent intent_ = Intent::none;
    bool frame_active_ = false;
    bool signal_pending_ = false;
    bool fence_retired_ = false;
    bool present_pending_ = false;
    bool resizing_ = false;
    bool quarantined_ = false;
};

} // namespace carto::studio
