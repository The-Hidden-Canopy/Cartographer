#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace carto::studio {

enum class AdapterClass { other, cpu, virtual_gpu, integrated_gpu, discrete_gpu };
struct PresentationAdapter {
    std::string name;
    AdapterClass kind = AdapterClass::other;
    std::uint32_t vendor = 0, device = 0;
    std::uint64_t local_memory = 0;
};

inline std::string adapter_name_key(std::string_view name) {
    std::string key(name);
    for (char& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return key;
}

// Only compatible graphics/present adapters enter this list. An explicit name
// must resolve uniquely; a typo must not silently choose a different GPU.
inline std::size_t select_presentation_adapter(
    std::span<const PresentationAdapter> adapters, std::string_view requested = {}) {
    if (adapters.empty()) throw std::runtime_error("no compatible Vulkan graphics/present adapter");
    std::optional<std::size_t> selected;
    const auto key = adapter_name_key(requested);
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        if (!key.empty()) {
            if (adapter_name_key(adapters[i].name) != key) continue;
            if (selected) throw std::runtime_error("Vulkan adapter name is ambiguous: " + std::string(requested));
            selected = i;
        } else if (!selected ||
            std::tuple{adapters[i].kind, adapters[i].local_memory} >
                std::tuple{adapters[*selected].kind, adapters[*selected].local_memory} ||
            (std::tuple{adapters[i].kind, adapters[i].local_memory} ==
                std::tuple{adapters[*selected].kind, adapters[*selected].local_memory} &&
             std::tuple{adapters[i].vendor, adapters[i].device, adapters[i].name} <
                std::tuple{adapters[*selected].vendor, adapters[*selected].device, adapters[*selected].name})) {
            selected = i;
        }
    }
    if (!selected) throw std::runtime_error("requested Vulkan adapter is not compatible or was not found: " + std::string(requested));
    return *selected;
}

enum class PresentationResult { ready, deferred, out_of_date, suboptimal };
struct AcquiredImage { PresentationResult result; std::uint32_t image = 0; };
enum class FrameProgress { deferred, presented, recreate };

// Production and the injected regression driver use the same state machine.
// In particular, after a successful acquire the binary semaphore is owned:
// an image-fence timeout must not cause a second acquire with that semaphore.
class PresentationSequence {
public:
    static constexpr std::uint64_t wait_budget_ns = 2'000'000;
    static constexpr std::uint64_t acquire_budget_ns = 8'000'000;
    explicit PresentationSequence(std::size_t frames = 3) : frames_(frames) {
        if (frames == 0) throw std::invalid_argument("presentation requires a frame slot");
    }
    [[nodiscard]] bool has_acquired_image() const noexcept { return acquired_.has_value(); }
    [[nodiscard]] std::size_t current_frame() const noexcept { return current_; }

    template<class Driver>
    FrameProgress step(Driver& driver, std::size_t image_count) {
        if (image_count == 0) throw std::runtime_error("presentation requires a swapchain image");
        if (!acquired_) {
            if (driver.wait_frame(current_, wait_budget_ns) == PresentationResult::deferred)
                return FrameProgress::deferred;
            const auto acquired = driver.acquire(current_, acquire_budget_ns);
            if (acquired.result == PresentationResult::deferred) return FrameProgress::deferred;
            if (acquired.result == PresentationResult::out_of_date) return FrameProgress::recreate;
            if (acquired.image >= image_count) throw std::runtime_error("acquired swapchain image is out of range");
            acquired_ = acquired.image;
            suboptimal_ = acquired.result == PresentationResult::suboptimal;
        }
        if (driver.wait_image(current_, *acquired_, wait_budget_ns) == PresentationResult::deferred)
            return FrameProgress::deferred;
        // Recording/reset/submit happen only after both finite waits succeed.
        // Any exception is fatal to this presenter; never reuse a reset fence.
        driver.submit(current_, *acquired_);
        const auto presented = driver.present(*acquired_);
        if (presented == PresentationResult::deferred)
            throw std::runtime_error("presentation returned an unsupported deferred result");
        acquired_.reset();
        current_ = (current_ + 1) % frames_;
        const bool recreate = suboptimal_ || presented == PresentationResult::suboptimal ||
                              presented == PresentationResult::out_of_date;
        suboptimal_ = false;
        return recreate ? FrameProgress::recreate : FrameProgress::presented;
    }
private:
    std::size_t frames_, current_ = 0;
    std::optional<std::uint32_t> acquired_;
    bool suboptimal_ = false;
};
} // namespace carto::studio
