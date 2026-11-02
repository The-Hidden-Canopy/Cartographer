// Local Cartographer integration policy. Upstream ImGui notices are preserved.
#pragma once
#include <cstdint>

namespace ImGui_ImplVulkanLifetime
{
inline std::uint32_t ClampExtent(std::uint32_t requested, std::uint32_t minimum, std::uint32_t maximum)
{
    return requested < minimum ? minimum : (requested > maximum ? maximum : requested);
}
struct SurfaceExtent { std::uint32_t Width, Height; };
inline bool SelectSurfaceExtent(int width, int height, SurfaceExtent current,
    SurfaceExtent minimum, SurfaceExtent maximum, SurfaceExtent& selected)
{
    constexpr std::uint32_t variable = 0xffffffffU;
    constexpr std::uint32_t max_int = 0x7fffffffU;
    if (width <= 0 || height <= 0 || maximum.Width == 0 || maximum.Height == 0 ||
        minimum.Width > maximum.Width || minimum.Height > maximum.Height ||
        minimum.Width > max_int || minimum.Height > max_int) return false;
    SurfaceExtent result = current;
    if (current.Width == variable && current.Height == variable)
    {
        result.Width = ClampExtent((std::uint32_t)width, minimum.Width, maximum.Width);
        result.Height = ClampExtent((std::uint32_t)height, minimum.Height, maximum.Height);
    }
    if (result.Width == 0 || result.Height == 0 || result.Width > max_int || result.Height > max_int ||
        result.Width < minimum.Width || result.Width > maximum.Width ||
        result.Height < minimum.Height || result.Height > maximum.Height) return false;
    selected = result;
    return true;
}
// Rendering completion and presentation retirement are separate events. An
// enqueued present owns its fence until a successful signal observation.
struct PresentFenceCycle
{
    bool Armed = false, Pending = false;
    bool MayRetire() const { return !Armed && !Pending; }
    bool Begin()
    {
        if (!MayRetire()) return false;
        Armed = true;
        return true;
    }
    bool Finish(bool enqueued)
    {
        if (!Armed) return false;
        Armed = false;
        Pending = enqueued;
        return true;
    }
    bool ObserveSignal()
    {
        if (!Pending) return false;
        Pending = false;
        return true;
    }
};
// A successful acquire owns both the image and its binary signal until the
// corresponding submit/present pair. Deferral must preserve that ownership.
struct AcquireCycle
{
    std::uint32_t Image = 0;
    bool Acquired = false;
    bool Submitted = false;

    bool MayAcquire() const { return !Acquired && !Submitted; }
    bool Admit(std::uint32_t image, std::uint32_t count)
    {
        if (!MayAcquire() || image >= count) return false;
        Image = image;
        Acquired = true;
        return true;
    }
    bool MarkSubmitted()
    {
        if (!Acquired || Submitted) return false;
        Submitted = true;
        return true;
    }
    bool MayPresent() const { return Acquired && Submitted; }
    bool Presented()
    {
        if (!MayPresent()) return false;
        Acquired = Submitted = false;
        return true;
    }
};
}
