#include <algorithm>
#include <carto/render/temporal_reprojection.hpp>
#include <cmath>

namespace carto::render {
namespace {
core::Diagnostic invalid(const char *message) {
    return {core::ErrorCode::invalid_argument, message};
}
core::Vec3d ycocg(core::Vec3d c) {
    return {(c.x + 2 * c.y + c.z) * .25, (c.x - c.z) * .5, (-c.x + 2 * c.y - c.z) * .25};
}
core::Vec3d rgb(core::Vec3d c) { return {c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z}; }
bool positive(double d) { return std::isfinite(d) && d > 0; }
} // namespace
core::Result<void> TemporalReprojectionSettings::validate() const {
    if (!std::isfinite(feedback) || feedback < 0 || feedback >= 1 ||
        !std::isfinite(absolute_depth) || absolute_depth < 0 || !std::isfinite(relative_depth) ||
        relative_depth < 0 || relative_depth > 1 || !std::isfinite(clip_expansion) ||
        clip_expansion < 0 || clip_expansion > 1)
        return core::Result<void>::failure(invalid("temporal reprojection settings are invalid"));
    return core::Result<void>::success();
}
core::Result<TemporalReprojectionResult>
resolve_temporal_reprojected_reference(const TemporalReprojectionInput &in) {
    using Result = core::Result<TemporalReprojectionResult>;
    if (auto valid = in.settings.validate(); !valid)
        return Result::failure(valid.error());
    const std::uint64_t n = std::uint64_t(in.width) * in.height;
    if (!n || n > 1048576 || in.current_hdr.size() != n || in.current_depth.size() != n ||
        in.current_surface.size() != n || in.previous.size() != n)
        return Result::failure(
            invalid("temporal reprojection current attachments exceed/mismatch extent"));
    TemporalRejection global = TemporalRejection::accepted;
    if (!in.history_valid || in.settings.feedback == 0)
        global = TemporalRejection::unavailable;
    else if (in.camera_cut)
        global = TemporalRejection::camera_cut;
    else if (in.content_changed || in.current_revision != in.history_revision)
        global = TemporalRejection::revision;
    const bool eligible = global == TemporalRejection::accepted;
    if (eligible && (in.history_hdr.size() != n || in.history_depth.size() != n ||
                     in.history_surface.size() != n))
        return Result::failure(
            invalid("temporal reprojection history attachments mismatch extent"));
    for (std::size_t p = 0; p < n; ++p) {
        if (!in.current_hdr[p].finite() || (eligible && !in.history_hdr[p].finite()))
            return Result::failure(invalid("temporal reprojection HDR contains nonfinite values"));
    }
    TemporalReprojectionResult out;
    out.linear_hdr.assign(in.current_hdr.begin(), in.current_hdr.end());
    out.rejection.resize(n);
    const auto reject = [&](std::size_t p, TemporalRejection reason) {
        out.rejection[p] = reason;
        ++out.counts[static_cast<unsigned>(reason)];
    };
    for (unsigned y = 0; y < in.height; ++y)
        for (unsigned x = 0; x < in.width; ++x) {
            const auto p = std::size_t(y) * in.width + x;
            if (!eligible) {
                reject(p, global);
                continue;
            }
            const auto &sample = in.previous[p];
            const auto surface = in.current_surface[p];
            if (!surface[0] || !surface[1] || !sample.supported || !positive(in.current_depth[p]) ||
                !positive(sample.depth) || !std::isfinite(sample.x) || !std::isfinite(sample.y)) {
                reject(p, TemporalRejection::unsupported_motion);
                continue;
            }
            if (sample.x < 0 || sample.y < 0 || sample.x > in.width - 1. ||
                sample.y > in.height - 1.) {
                reject(p, TemporalRejection::offscreen);
                continue;
            }
            const int bx = int(std::floor(sample.x)), by = int(std::floor(sample.y));
            const double fx = sample.x - bx, fy = sample.y - by;
            core::Vec3d history{};
            auto reason = TemporalRejection::accepted;
            for (int dy = 0; dy < 2 && reason == TemporalRejection::accepted; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const double weight = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                    if (weight == 0)
                        continue;
                    const auto q = std::size_t(by + dy) * in.width + unsigned(bx + dx);
                    if (in.history_surface[q] != surface) {
                        reason = TemporalRejection::identity;
                        break;
                    }
                    if (!positive(in.history_depth[q]) ||
                        std::abs(in.history_depth[q] - sample.depth) >
                            in.settings.absolute_depth +
                                in.settings.relative_depth * sample.depth) {
                        reason = TemporalRejection::depth;
                        break;
                    }
                    history = history + in.history_hdr[q] * weight;
                }
            if (reason != TemporalRejection::accepted) {
                reject(p, reason);
                continue;
            }
            auto low = ycocg(in.current_hdr[p]), high = low;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = int(x) + dx, ny = int(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= int(in.width) || ny >= int(in.height))
                        continue;
                    const auto q = std::size_t(ny) * in.width + unsigned(nx);
                    if (in.current_surface[q] != surface)
                        continue;
                    const auto c = ycocg(in.current_hdr[q]);
                    low = {std::min(low.x, c.x), std::min(low.y, c.y), std::min(low.z, c.z)};
                    high = {std::max(high.x, c.x), std::max(high.y, c.y), std::max(high.z, c.z)};
                }
            const auto pad =
                (high - low) * in.settings.clip_expansion + core::Vec3d{1e-6, 1e-6, 1e-6};
            low = low - pad;
            high = high + pad;
            auto c = ycocg(history);
            if (!low.finite() || !high.finite() || !c.finite())
                return Result::failure(invalid("temporal clipping overflow"));
            c = {std::clamp(c.x, low.x, high.x), std::clamp(c.y, low.y, high.y),
                 std::clamp(c.z, low.z, high.z)};
            const auto clipped = rgb(c);
            const auto color =
                in.current_hdr[p] * (1 - in.settings.feedback) + clipped * in.settings.feedback;
            if (!color.finite())
                return Result::failure(invalid("temporal blend overflow"));
            out.linear_hdr[p] = {std::max(0., color.x), std::max(0., color.y),
                                 std::max(0., color.z)};
            reject(p, TemporalRejection::accepted);
        }
    return Result::success(std::move(out));
}
} // namespace carto::render
