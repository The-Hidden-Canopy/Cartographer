#include <algorithm>
#include <carto/production/authoring_temporal.hpp>
#include <chrono>
#include <cmath>
#include <tuple>

namespace carto::production {
namespace {
core::Diagnostic invalid(const char *s) { return {core::ErrorCode::invalid_argument, s}; }
core::Quaternion inverse(core::Quaternion q) {
    q = q.normalized();
    return {-q.x, -q.y, -q.z, q.w};
}
bool invertible(const core::Transform &t) {
    return t.finite() && std::abs(t.scale.x) > 1e-12 && std::abs(t.scale.y) > 1e-12 &&
           std::abs(t.scale.z) > 1e-12;
}
core::Vec3d local_point(core::Vec3d p, const core::Transform &t) {
    p = inverse(t.rotation).rotate(p - t.translation);
    return {p.x / t.scale.x, p.y / t.scale.y, p.z / t.scale.z};
}
core::Vec3d world_point(core::Vec3d p, const core::Transform &t) {
    return t.translation + t.rotation.rotate(core::componentwise_multiply(p, t.scale));
}
auto transform_values(const core::Transform &t) {
    return std::tuple{t.translation.x, t.translation.y, t.translation.z, t.rotation.x, t.rotation.y,
                      t.rotation.z,    t.rotation.w,    t.scale.x,       t.scale.y,    t.scale.z};
}
auto lens(const AuthoringView &v) {
    return std::tuple{v.projection, v.vertical_fov_radians, v.orthographic_height, v.near_plane,
                      v.far_plane,  v.aspect_ratio};
}
auto quality(const AuthoringViewportRequest::Shadows &s) {
    return std::tuple{s.enabled, s.resolution, s.radius, s.depth_bias, s.slope_bias};
}
core::Vec3d unproject(const AuthoringViewportImage &image, unsigned x, unsigned y, double depth) {
    const auto &v = image.view;
    const double span = v.projection == project::CameraProjection::perspective
                            ? 2 * depth * std::tan(v.vertical_fov_radians * .5)
                            : v.orthographic_height;
    const double aspect = v.aspect_ratio > 0 ? v.aspect_ratio : double(image.width) / image.height;
    return v.position + v.rotation.rotate({((x + .5) / image.width - .5) * span * aspect,
                                           (.5 - (y + .5) / image.height) * span, -depth});
}
} // namespace
void AuthoringTemporalHistory::clear() {
    valid_ = false;
    width_ = height_ = 0;
    hdr_.clear();
    depth_.clear();
    surfaces_.clear();
    transforms_.clear();
    textures_.reset();
}
core::Result<AuthoringTemporalFrame>
AuthoringTemporalHistory::render(const project::ProjectDocument &doc,
                                 const AuthoringViewportRequest &request, bool cut,
                                 const render::TemporalReprojectionSettings &settings) {
    auto image = render_authoring_viewport(doc, request);
    if (!image)
        return core::Result<AuthoringTemporalFrame>::failure(image.error());
    return resolve(doc, request, std::move(image.value()), cut, settings);
}
core::Result<AuthoringTemporalFrame> AuthoringTemporalHistory::resolve(
    const project::ProjectDocument &doc, const AuthoringViewportRequest &request,
    AuthoringViewportImage image, bool cut, const render::TemporalReprojectionSettings &settings) {
    using Result = core::Result<AuthoringTemporalFrame>;
    using Clock = std::chrono::steady_clock;
    const auto entry = Clock::now();
    const auto milliseconds = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    if (auto check = settings.validate(); !check)
        return Result::failure(check.error());
    if (image.width != request.width || image.height != request.height ||
        image.source_revision != doc.revision())
        return Result::failure(invalid("temporal authoring frame source/extent is stale"));
    if (!image.width || !image.height) {
        clear();
        return Result::success({std::move(image), AuthoringHistoryReset::suspended, {}, {}});
    }
    const auto count = std::uint64_t(image.width) * image.height;
    if (count > 1048576 || image.linear_hdr.size() != count || image.depth.size() != count ||
        image.objects.size() != count || image.faces.size() != count)
        return Result::failure(invalid("temporal authoring frame attachments mismatch extent"));
    if (auto check = image.view.validate(); !check)
        return Result::failure(check.error());
    if (auto check = request.shadows.validate(); !check)
        return Result::failure(check.error());
    const auto display = resolve_authoring_display(doc, request);
    if (!display)
        return Result::failure(display.error());
    // CPU oracle cost only. Different documents can share revision numbers;
    // compare canonical source content, not only a revision or pointer address.
    const auto source = doc.serialize();
    if (source.empty() || source.size() > 128U * 1024U * 1024U)
        return Result::failure(invalid("temporal authoring source exceeds byte budget"));
    const auto digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t *>(source.data()), source.size()});
    auto reset = AuthoringHistoryReset::none;
    if (!valid_)
        reset = AuthoringHistoryReset::initial;
    else if (image.width != width_ || image.height != height_)
        reset = AuthoringHistoryReset::extent;
    else if (digest != digest_ || image.source_revision != revision_)
        reset = AuthoringHistoryReset::source;
    else if (lens(image.view) != lens(view_))
        reset = AuthoringHistoryReset::lens;
    else if (quality(request.shadows) != quality(shadows_) || request.textures != textures_)
        reset = AuthoringHistoryReset::quality;
    if (valid_ && reset == AuthoringHistoryReset::none) {
        const auto a = image.view.rotation.normalized(), b = view_.rotation.normalized();
        const double dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        double nearest = image.view.far_plane;
        for (std::size_t p = 0; p < count; ++p)
            if (image.objects[p] && std::isfinite(image.depth[p]))
                nearest = std::min(nearest, image.depth[p]);
        // A cut can remove every current object. Previous visible geometry
        // still bounds a plausible camera step; the far plane must not hide it.
        for (std::size_t p = 0; p < depth_.size(); ++p)
            if (surfaces_[p][0] && std::isfinite(depth_[p]))
                nearest = std::min(nearest, depth_[p]);
        const double span = image.view.projection == project::CameraProjection::orthographic
                                ? image.view.orthographic_height
                                : nearest;
        if (dot < .9659258262890683 || (image.view.position - view_.position).length() >
                                           std::max(image.view.near_plane * 8, span * .5))
            cut = true;
    }
    if (cut)
        reset = AuthoringHistoryReset::camera_cut;
    std::map<scene::ObjectId, core::Transform> transforms;
    bool changed_mesh = false, changed_light = false;
    for (const auto &object : doc.scene().objects_sorted()) {
        const bool light = doc.presentation_catalog().lights().contains(object.id);
        if (!object.mesh_asset && !light)
            continue;
        auto pose = doc.scene().world_transform(object.id);
        if (!pose)
            return Result::failure(pose.error());
        if (auto found = request.transform_overrides.find(object.id);
            found != request.transform_overrides.end())
            pose = core::Result<core::Transform>::success(found->second);
        if (!pose.value().finite())
            return Result::failure(invalid("temporal object transform is invalid"));
        transforms.emplace(object.id, pose.value());
        const auto prior = transforms_.find(object.id);
        if (prior != transforms_.end() &&
            transform_values(prior->second) != transform_values(pose.value())) {
            changed_mesh |= object.mesh_asset.has_value();
            changed_light |= light;
        }
    }
    bool spatial_shadows = false;
    for (const auto &[id, l] : doc.presentation_catalog().lights()) {
        static_cast<void>(id);
        spatial_shadows |= request.shadows.enabled && l.casts_shadows;
    }
    // No per-light shadow motion field exists. Reject globally instead of
    // smearing moving shadows across otherwise stationary receivers.
    if (reset == AuthoringHistoryReset::none &&
        (changed_light || (changed_mesh && spatial_shadows)))
        reset = AuthoringHistoryReset::lighting_motion;
    std::vector<render::TemporalSurface> surfaces(count);
    std::vector<render::TemporalPreviousSample> previous(count);
    for (unsigned y = 0; y < image.height; ++y)
        for (unsigned x = 0; x < image.width; ++x) {
            const auto p = std::size_t(y) * image.width + x;
            surfaces[p] = {image.objects[p].value, image.faces[p].value};
            if (reset != AuthoringHistoryReset::none || !image.objects[p] ||
                !std::isfinite(image.depth[p]) || image.depth[p] <= 0)
                continue;
            const auto now = transforms.find(image.objects[p]),
                       old = transforms_.find(image.objects[p]);
            if (now == transforms.end() || old == transforms_.end() || !invertible(now->second) ||
                !invertible(old->second))
                continue;
            const auto world = unproject(image, x, y, image.depth[p]);
            const auto projected = view_.project(
                world_point(local_point(world, now->second), old->second), width_, height_);
            if (projected)
                previous[p] = {projected.value().x - .5, projected.value().y - .5,
                               projected.value().z, true};
        }
    render::TemporalReprojectionInput input;
    input.width = image.width;
    input.height = image.height;
    input.current_hdr = image.linear_hdr;
    input.history_hdr = hdr_;
    input.current_depth = image.depth;
    input.history_depth = depth_;
    input.current_surface = surfaces;
    input.history_surface = surfaces_;
    input.previous = previous;
    input.current_revision = image.source_revision;
    input.history_revision = revision_;
    input.history_valid = valid_ && reset == AuthoringHistoryReset::none;
    input.settings = settings;
    const auto prepared = Clock::now();
    auto resolved = render::resolve_temporal_reprojected_reference(input);
    if (!resolved)
        return Result::failure(resolved.error());
    image.linear_hdr = std::move(resolved.value().linear_hdr);
    const auto resolved_at = Clock::now();
    std::vector<std::uint8_t> coverage(count);
    for (std::size_t p = 0; p < count; ++p)
        coverage[p] = image.objects[p] ? 255 : 0;
    auto encoded = render::encode_display_rgba8(
        image.linear_hdr, coverage, display.value().exposure_ev, display.value().tone_mode);
    if (!encoded)
        return Result::failure(encoded.error());
    image.display_rgba8 = std::move(encoded.value());
    const auto displayed = Clock::now();
    // Publish derived history only after the entire resolve/display succeeds.
    auto next_hdr = image.linear_hdr;
    auto next_depth = image.depth;
    hdr_ = std::move(next_hdr);
    depth_ = std::move(next_depth);
    surfaces_ = std::move(surfaces);
    transforms_ = std::move(transforms);
    width_ = image.width;
    height_ = image.height;
    revision_ = image.source_revision;
    digest_ = digest;
    view_ = image.view;
    shadows_ = request.shadows;
    textures_ = request.textures;
    valid_ = true;
    image.diagnostics.push_back(
        "CPU temporal reference: subpixel rigid/camera reprojection and conservative history "
        "rejection; native GPU history not integrated");
    AuthoringTemporalFrame frame{
        std::move(image), reset, resolved.value().counts,
        count * (sizeof(core::Vec3d) + sizeof(double) + sizeof(render::TemporalSurface))};
    const auto published = Clock::now();
    frame.cpu_prepare_ms = milliseconds(entry, prepared);
    frame.cpu_resolve_ms = milliseconds(prepared, resolved_at);
    frame.cpu_display_ms = milliseconds(resolved_at, displayed);
    frame.cpu_publish_ms = milliseconds(displayed, published);
    frame.cpu_total_ms = milliseconds(entry, published);
    frame.image.host_milliseconds += frame.cpu_total_ms;
    return Result::success(std::move(frame));
}
} // namespace carto::production
