#include "authoring_shader.hpp"
#include <bit>
#include <carto/d3d12/authoring_temporal.hpp>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>

namespace carto::d3d12 {
namespace {
bool bounded(unsigned w, unsigned h) {
    return w && h && w <= 4096 && h <= 4096 && std::uint64_t(w) * h <= 1048576;
}
core::Diagnostic invalid(const char *s) { return {core::ErrorCode::invalid_argument, s}; }
} // namespace
core::Result<AuthoringTemporalKernelPlan>
prepare_authoring_temporal(const AuthoringTemporalEpoch &now, const AuthoringTemporalEpoch &old,
                           bool history_valid, bool cut,
                           const render::TemporalReprojectionSettings &settings) {
    using Result = core::Result<AuthoringTemporalKernelPlan>;
    if (auto v = settings.validate(); !v)
        return Result::failure(v.error());
    if (!bounded(now.width, now.height) || now.source.is_zero())
        return Result::failure(invalid("authoring temporal extent/source identity is invalid"));
    AuthoringTemporalKernelPlan plan;
    plan.constants.feedback_depth_clip = {
        static_cast<float>(settings.feedback), static_cast<float>(settings.absolute_depth),
        static_cast<float>(settings.relative_depth), static_cast<float>(settings.clip_expansion)};
    for (const auto x : plan.constants.feedback_depth_clip)
        if (!std::isfinite(x))
            return Result::failure(invalid("authoring temporal constants exceed float32 bounds"));
    plan.constants.feedback_depth_clip[0] =
        std::min(plan.constants.feedback_depth_clip[0], std::nextafter(1.F, 0.F));
    plan.constants.extent_flags = {now.width, now.height,
                                   history_valid && now.source == old.source &&
                                           now.revision == old.revision && now.width == old.width &&
                                           now.height == old.height
                                       ? 1U
                                       : 0U,
                                   cut ? 1U : 0U};
    plan.groups = {(now.width + 7) / 8, (now.height + 7) / 8, 1};
    auto &p = plan.pipeline;
    p.debug_name = "authoring.temporal.reprojection";
    p.sampled_texture_count = 7;
    p.push_constant_bytes = sizeof(AuthoringTemporalConstants);
    p.color_format = gpu::Format::unknown;
    p.depth_format = gpu::Format::unknown;
    p.depth_test = p.depth_write = false;
    for (unsigned i = 0; i < 8; ++i)
        p.descriptor_bindings.push_back({0, i, gpu::ShaderStage::compute});
    return Result::success(std::move(plan));
}
ShaderSource authoring_temporal_shader() {
    return {"authoring.temporal",
            gpu::ShaderStage::compute,
            "CSMain",
            "authoring_temporal.hlsl",
            carto_authoring_temporal_source,
            true,
            false};
}
core::Result<std::vector<std::uint32_t>>
pack_temporal_identity_atlas(std::span<const render::TemporalSurface> surfaces, unsigned w,
                             unsigned h) {
    using Result = core::Result<std::vector<std::uint32_t>>;
    if (!bounded(w, h) || surfaces.size() != std::uint64_t(w) * h)
        return Result::failure(invalid("temporal identity atlas extent mismatch"));
    std::vector<std::uint32_t> packed;
    packed.reserve(surfaces.size() * 4);
    for (const auto s : surfaces)
        for (const auto value : s) {
            packed.push_back(static_cast<std::uint32_t>(value));
            packed.push_back(static_cast<std::uint32_t>(value >> 32));
        }
    return Result::success(std::move(packed));
}
core::Result<std::vector<std::array<float, 4>>>
pack_temporal_previous_samples(std::span<const render::TemporalPreviousSample> previous, unsigned w,
                               unsigned h) {
    using Result = core::Result<std::vector<std::array<float, 4>>>;
    if (!bounded(w, h) || previous.size() != std::uint64_t(w) * h)
        return Result::failure(invalid("temporal motion extent mismatch"));
    std::vector<std::array<float, 4>> packed(previous.size());
    for (std::size_t p = 0; p < previous.size(); ++p) {
        const auto &s = previous[p];
        if (!s.supported || !std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.depth) ||
            s.depth <= 0)
            continue;
        const std::array<float, 4> value{static_cast<float>(s.x), static_cast<float>(s.y),
                                         static_cast<float>(s.depth), 1};
        if (std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]) &&
            value[2] > 0)
            packed[p] = value;
    }
    return Result::success(std::move(packed));
}
namespace {
using MatrixD = std::array<double, 16>;
MatrixD matrix(const core::Transform &t) {
    if (!t.finite() || std::abs(t.scale.x) < 1e-12 || std::abs(t.scale.y) < 1e-12 ||
        std::abs(t.scale.z) < 1e-12)
        throw std::runtime_error("temporal transform is singular or invalid");
    MatrixD m{};
    for (unsigned c = 0; c < 3; ++c) {
        core::Vec3d v{};
        if (c == 0)
            v.x = t.scale.x;
        if (c == 1)
            v.y = t.scale.y;
        if (c == 2)
            v.z = t.scale.z;
        v = t.rotation.rotate(v);
        m[c * 4] = v.x;
        m[c * 4 + 1] = v.y;
        m[c * 4 + 2] = v.z;
    }
    m[12] = t.translation.x;
    m[13] = t.translation.y;
    m[14] = t.translation.z;
    m[15] = 1;
    return m;
}
MatrixD inverse(MatrixD m) {
    double a[4][8]{};
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c) {
            a[r][c] = m[c * 4 + r];
            a[r][c + 4] = r == c;
        }
    for (unsigned c = 0; c < 4; ++c) {
        unsigned pivot = c;
        for (unsigned r = c + 1; r < 4; ++r)
            if (std::abs(a[r][c]) > std::abs(a[pivot][c]))
                pivot = r;
        if (!std::isfinite(a[pivot][c]) || std::abs(a[pivot][c]) < 1e-18)
            throw std::runtime_error("temporal matrix is singular");
        for (unsigned k = 0; k < 8; ++k)
            std::swap(a[c][k], a[pivot][k]);
        const double d = a[c][c];
        for (auto &v : a[c])
            v /= d;
        for (unsigned r = 0; r < 4; ++r)
            if (r != c) {
                const double f = a[r][c];
                for (unsigned k = 0; k < 8; ++k)
                    a[r][k] -= f * a[c][k];
            }
    }
    MatrixD out{};
    for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 4; ++c)
            out[c * 4 + r] = a[r][c + 4];
    return out;
}
MatrixD multiply(const MatrixD &a, const MatrixD &b) {
    MatrixD out{};
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned k = 0; k < 4; ++k)
                out[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    return out;
}
float checked_float(double v) {
    const float f = static_cast<float>(v);
    if (!std::isfinite(f))
        throw std::runtime_error("temporal value exceeds float32");
    return f;
}
AuthoringMatrix floats(const MatrixD &m) {
    AuthoringMatrix out{};
    for (unsigned i = 0; i < 16; ++i)
        out[i] = checked_float(m[i]);
    return out;
}
auto pose(const core::Transform &t) {
    return std::tuple{t.translation.x, t.translation.y, t.translation.z, t.rotation.x, t.rotation.y,
                      t.rotation.z,    t.rotation.w,    t.scale.x,       t.scale.y,    t.scale.z};
}
auto lens(const production::AuthoringView &v) {
    return std::tuple{v.projection, v.vertical_fov_radians, v.orthographic_height, v.near_plane,
                      v.far_plane,  v.aspect_ratio};
}
std::array<float, 4> depth_row(const production::AuthoringView &v) {
    const auto b = v.rotation.rotate({0, 0, 1});
    return {checked_float(-b.x), checked_float(-b.y), checked_float(-b.z),
            checked_float(core::dot(b, v.position))};
}
assets::Sha256Digest words_digest(const std::vector<std::uint32_t> &w) {
    return assets::sha256({reinterpret_cast<const std::uint8_t *>(w.data()), w.size() * 4});
}
} // namespace
core::Result<AuthoringMatrix> authoring_view_projection(const production::AuthoringView &view,
                                                        double aspect) {
    using R = core::Result<AuthoringMatrix>;
    if (auto v = view.validate(); !v)
        return R::failure(v.error());
    if (!std::isfinite(aspect) || aspect <= 0)
        return R::failure(invalid("temporal camera aspect invalid"));
    try {
        auto q = view.rotation.normalized();
        q = {-q.x, -q.y, -q.z, q.w};
        auto clip = [&](core::Vec3d world) {
            const auto p = q.rotate(world - view.position);
            const auto n = view.near_plane, f = view.far_plane;
            if (view.projection == project::CameraProjection::orthographic)
                return std::array<double, 4>{2 * p.x / (view.orthographic_height * aspect),
                                             2 * p.y / view.orthographic_height,
                                             (-p.z - n) / (f - n), 1};
            const auto t = std::tan(view.vertical_fov_radians * .5);
            return std::array<double, 4>{p.x / (t * aspect), p.y / t,
                                         f / (n - f) * p.z + f * n / (n - f), -p.z};
        };
        const auto o = clip({});
        MatrixD m{};
        for (unsigned c = 0; c < 4; ++c) {
            core::Vec3d axis{};
            if (c == 0)
                axis.x = 1;
            if (c == 1)
                axis.y = 1;
            if (c == 2)
                axis.z = 1;
            const auto v = c == 3 ? o : clip(axis);
            for (unsigned r = 0; r < 4; ++r)
                m[c * 4 + r] = v[r] - (c == 3 ? 0 : o[r]);
        }
        return R::success(floats(m));
    } catch (const std::exception &e) {
        return R::failure({core::ErrorCode::invalid_argument, e.what()});
    }
}
struct AuthoringTemporalHostPlan::Candidate {
    const AuthoringTemporalHostState *owner = nullptr;
    std::uint64_t generation = 0, session = 0;
    AuthoringTemporalEpoch epoch;
    production::AuthoringView view;
    assets::Sha256Digest lighting, quality, face_digest;
    std::vector<std::pair<std::uint64_t, core::Transform>> draws;
    std::shared_ptr<const std::vector<std::uint32_t>> faces;
};
core::Result<AuthoringTemporalHostPlan>
AuthoringTemporalHostState::prepare(const AuthoringTemporalHostInput &in) const {
    using R = core::Result<AuthoringTemporalHostPlan>;
    try {
        if (in.draws.size() > 256)
            throw std::runtime_error("temporal draw budget exceeded");
        const auto aspect = in.view.aspect_ratio > 0 ? in.view.aspect_ratio
                                                     : double(in.epoch.width) / in.epoch.height;
        auto vp = authoring_view_projection(in.view, aspect);
        if (!vp)
            return R::failure(vp.error());
        auto kernel = prepare_authoring_temporal(in.epoch, previous_ ? previous_->epoch : in.epoch,
                                                 bool(previous_), in.camera_cut, in.settings);
        if (!kernel)
            return R::failure(kernel.error());
        AuthoringTemporalHostPlan plan;
        plan.resolve = std::move(kernel.value());
        auto next = std::make_shared<AuthoringTemporalHostPlan::Candidate>();
        next->owner = this;
        next->generation = generation_;
        next->epoch = in.epoch;
        next->view = in.view;
        next->lighting = in.lighting;
        next->quality = in.quality;
        next->session = in.session;
        auto faces = std::make_shared<std::vector<std::uint32_t>>();
        bool moved = false;
        for (std::size_t d = 0; d < in.draws.size(); ++d) {
            const auto &draw = in.draws[d];
            if (!draw.object || draw.faces.empty() || draw.faces.size() > 65536 ||
                faces->size() + draw.faces.size() * 2 > 131072)
                throw std::runtime_error("temporal face/object budget invalid");
            const auto current = matrix(draw.transform);
            bool supported = previous_ && d < previous_->draws.size() &&
                             previous_->draws[d].first == draw.object;
            moved |= supported && pose(previous_->draws[d].second) != pose(draw.transform);
            const auto old = supported ? matrix(previous_->draws[d].second) : current;
            const auto motion = floats(multiply(old, inverse(current)));
            const auto offset = static_cast<std::uint32_t>(faces->size());
            plan.draw_words.insert(plan.draw_words.end(),
                                   {static_cast<std::uint32_t>(draw.object),
                                    static_cast<std::uint32_t>(draw.object >> 32), offset,
                                    static_cast<std::uint32_t>(draw.faces.size())});
            for (auto f : motion)
                plan.draw_words.push_back(std::bit_cast<std::uint32_t>(f));
            plan.draw_words.insert(plan.draw_words.end(), {supported ? 1U : 0U, 0, 0, 0});
            for (auto face : draw.faces) {
                if (!face.value)
                    throw std::runtime_error("temporal face identity is zero");
                faces->push_back(static_cast<std::uint32_t>(face.value));
                faces->push_back(static_cast<std::uint32_t>(face.value >> 32));
            }
            next->draws.emplace_back(draw.object, draw.transform);
        }
        plan.face_digest = words_digest(*faces);
        if (previous_ && plan.face_digest == previous_->face_digest)
            plan.face_words = previous_->faces;
        else
            plan.face_words = faces;
        next->faces = plan.face_words;
        next->face_digest = plan.face_digest;
        plan.draw_digest = words_digest(plan.draw_words);
        plan.reset = AuthoringTemporalReset::none;
        if (!previous_)
            plan.reset = AuthoringTemporalReset::initial;
        else if (in.epoch.width != previous_->epoch.width ||
                 in.epoch.height != previous_->epoch.height)
            plan.reset = AuthoringTemporalReset::extent;
        else if (in.session != previous_->session)
            plan.reset = AuthoringTemporalReset::session;
        else if (in.epoch.source != previous_->epoch.source ||
                 in.epoch.revision != previous_->epoch.revision ||
                 plan.face_digest != previous_->face_digest)
            plan.reset = AuthoringTemporalReset::source;
        else if (lens(in.view) != lens(previous_->view))
            plan.reset = AuthoringTemporalReset::lens;
        else if (in.quality != previous_->quality)
            plan.reset = AuthoringTemporalReset::quality;
        else if (in.lighting != previous_->lighting)
            plan.reset = AuthoringTemporalReset::lighting;
        else if (moved && in.spatial_shadows)
            plan.reset = AuthoringTemporalReset::moving_shadow;
        bool cut = in.camera_cut;
        if (previous_) {
            const auto a = in.view.rotation.normalized(), b = previous_->view.rotation.normalized();
            const auto dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
            // Conservative host-only cut threshold; fine camera steps are GPU reprojected.
            const auto span = in.view.projection == project::CameraProjection::orthographic
                                  ? in.view.orthographic_height
                                  : in.view.near_plane * 16;
            cut |=
                dot < .9659258262890683 || (in.view.position - previous_->view.position).length() >
                                               std::max(in.view.near_plane * 8, span * .5);
        }
        if (cut)
            plan.reset = AuthoringTemporalReset::camera_cut;
        plan.resolve.constants.extent_flags[2] =
            plan.reset == AuthoringTemporalReset::none ? 1U : 0U;
        plan.resolve.constants.extent_flags[3] = cut ? 1U : 0U;
        plan.resolve.pipeline.sampled_texture_count = 4;
        plan.resolve.pipeline.descriptor_bindings.resize(5);
        MatrixD current{};
        std::copy(vp.value().begin(), vp.value().end(), current.begin());
        plan.motion.inverse_view_projection = floats(inverse(current));
        const auto old = previous_ ? authoring_view_projection(previous_->view, aspect) : vp;
        if (!old)
            return R::failure(old.error());
        plan.motion.previous_view_projection = old.value();
        plan.motion.current_depth_row = depth_row(in.view);
        plan.motion.previous_depth_row = depth_row(previous_ ? previous_->view : in.view);
        plan.motion.extent_draws = {in.epoch.width, in.epoch.height, 256,
                                    static_cast<std::uint32_t>(in.draws.size())};
        plan.candidate = std::move(next);
        return R::success(std::move(plan));
    } catch (const std::exception &e) {
        return R::failure({core::ErrorCode::invalid_argument, e.what()});
    }
}
core::Result<void> AuthoringTemporalHostState::commit(const AuthoringTemporalHostPlan &plan) {
    if (!plan.candidate || plan.candidate->owner != this ||
        plan.candidate->generation != generation_)
        return core::Result<void>::failure(invalid("stale or foreign temporal candidate"));
    previous_ = plan.candidate;
    ++generation_;
    return core::Result<void>::success();
}
void AuthoringTemporalHostState::invalidate() {
    previous_.reset();
    ++generation_;
}
ShaderSource authoring_motion_shader() {
    return {"authoring.motion",
            gpu::ShaderStage::compute,
            "CSMain",
            "authoring_motion.hlsl",
            carto_authoring_motion_source,
            true,
            false};
}
ShaderSource authoring_packed_temporal_shader() {
    auto s = authoring_temporal_shader();
    s.debug_name = "authoring.temporal.packed";
    s.source = "#define CARTO_TEMPORAL_PACKED 1\n" + s.source;
    return s;
}
} // namespace carto::d3d12
