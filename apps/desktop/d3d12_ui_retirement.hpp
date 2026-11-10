#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace carto::desktop {

// Ownership bookkeeping only: this does not submit, signal, or poll a GPU.
// The caller owns the fence and must keep this object and its reservations
// alive until every record is collected. Use one monotonic, exclusive fence.
// Call seal_after_platform_draws only after a successful signal on the same
// queue, ordered after every draw that could reference an unsealed record.
template <typename Resource, std::size_t Capacity>
class D3D12UiRetirement final {
    static_assert(Capacity > 0);
    static_assert(std::is_nothrow_move_constructible_v<Resource>);
    static_assert(std::is_nothrow_destructible_v<Resource>);
    struct Slot {
        bool reserved = false;
        std::optional<Resource> resource;
        std::uint64_t fence = 0; // zero means not yet sealed
    };

public:
    class Reservation final {
    public:
        Reservation() noexcept = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), index_(other.index_) {}
        Reservation& operator=(Reservation&& other) noexcept {
            if (this != &other) {
                cancel();
                owner_ = std::exchange(other.owner_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }
        ~Reservation() { cancel(); }
        [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }

        // Commit before replacing or clearing current. Failure leaves it intact.
        [[nodiscard]] bool commit(std::optional<Resource>& current) noexcept {
            if (!owner_ || !current) return false;
            auto& slot = owner_->slots_[index_];
            slot.resource.emplace(std::move(*current));
            current.reset();
            slot.reserved = false;
            owner_ = nullptr;
            return true;
        }
        void cancel() noexcept {
            if (owner_) {
                owner_->slots_[index_].reserved = false;
                owner_ = nullptr;
            }
        }
    private:
        friend class D3D12UiRetirement;
        Reservation(D3D12UiRetirement* owner, std::size_t index) noexcept
            : owner_(owner), index_(index) {}
        D3D12UiRetirement* owner_ = nullptr;
        std::size_t index_ = 0;
    };

    D3D12UiRetirement() = default;
    D3D12UiRetirement(const D3D12UiRetirement&) = delete;
    D3D12UiRetirement& operator=(const D3D12UiRetirement&) = delete;
    D3D12UiRetirement(D3D12UiRetirement&&) = delete;
    D3D12UiRetirement& operator=(D3D12UiRetirement&&) = delete;

    [[nodiscard]] Reservation reserve() noexcept {
        if (shutting_down_) return {};
        for (std::size_t i = 0; i < Capacity; ++i) {
            if (!slots_[i].reserved && !slots_[i].resource) {
                slots_[i].reserved = true;
                return Reservation(this, i);
            }
        }
        return {};
    }

    [[nodiscard]] bool begin_frame() noexcept {
        if (shutting_down_ || frame_open_) return false;
        frame_open_ = true;
        return true;
    }
    // The caller must submit all intended draws or discard all unsubmitted
    // draw data before closing the frame. This call does not imply completion.
    [[nodiscard]] bool close_frame() noexcept {
        if (!frame_open_) return false;
        frame_open_ = false;
        return true;
    }

    // Pass only a confirmed successful queue Signal value. On failure or an
    // uncertain result, do not call this; records remain unsealed. A later
    // confirmed signal after all relevant draws may seal them together.
    [[nodiscard]] bool seal_after_platform_draws(std::uint64_t fence) noexcept {
        if (frame_open_ || fence == 0 || fence == invalid_fence || fence <= last_signaled_)
            return false;
        last_signaled_ = fence;
        for (auto& slot : slots_)
            if (slot.resource && slot.fence == 0) slot.fence = fence;
        return true;
    }

    // Returns false and releases nothing on a regression, unknown future
    // value, or D3D12's device-removed UINT64_MAX sentinel.
    [[nodiscard]] bool observe_completion(std::uint64_t completed) noexcept {
        if (completed == invalid_fence || completed < last_completed_ || completed > last_signaled_)
            return false;
        last_completed_ = completed;
        for (auto& slot : slots_) {
            if (slot.resource && slot.fence != 0 && slot.fence <= completed) {
                slot.resource.reset();
                slot.fence = 0;
            }
        }
        return true;
    }

    // Close or abandon the frame first. Existing reservations can be cancelled
    // or committed after shutdown starts; no new reservation is admitted.
    [[nodiscard]] bool begin_shutdown() noexcept {
        if (frame_open_) return false;
        shutting_down_ = true;
        return true;
    }
    [[nodiscard]] bool shutdown_ready() const noexcept {
        if (!shutting_down_ || frame_open_) return false;
        for (const auto& slot : slots_)
            if (slot.reserved || slot.resource) return false;
        return true;
    }
    [[nodiscard]] std::size_t outstanding() const noexcept {
        std::size_t count = 0;
        for (const auto& slot : slots_) if (slot.reserved || slot.resource) ++count;
        return count;
    }

private:
    static constexpr std::uint64_t invalid_fence = std::numeric_limits<std::uint64_t>::max();
    std::array<Slot, Capacity> slots_{};
    std::uint64_t last_signaled_ = 0, last_completed_ = 0;
    bool frame_open_ = false, shutting_down_ = false;
};
} // namespace carto::desktop
