#include <algorithm>
#include <carto/production/authoring_viewport.hpp>
#include <cmath>

namespace carto::production {
namespace {
core::Diagnostic invalid(const char *message) {
    return {core::ErrorCode::invalid_argument, message};
}
} // namespace
core::Result<void> AuthoringViewportRequest::Shadows::validate() const {
    if (resolution < 64 || resolution > 1024 || (resolution & (resolution - 1)) || radius > 2 ||
        !std::isfinite(depth_bias) || depth_bias < 0 || depth_bias > .05 ||
        !std::isfinite(slope_bias) || slope_bias < 0 || slope_bias > .05)
        return core::Result<void>::failure(
            invalid("directional shadow quality/bias exceeds viewport bounds"));
    return core::Result<void>::success();
}
core::Result<std::optional<DirectionalShadowFrame>>
fit_directional_shadow(const project::ProjectDocument &document,
                       const AuthoringViewportRequest &request, core::Vec3d direction) {
    using Result = core::Result<std::optional<DirectionalShadowFrame>>;
    if (auto valid = request.shadows.validate(); !valid)
        return Result::failure(valid.error());
    direction = direction.normalized();
    if (!direction.finite())
        return Result::failure(invalid("shadow light direction is invalid"));
    if (!request.shadows.enabled)
        return Result::success({});
    const core::Quaternion rotation =
        direction.z < -.999999999
            ? core::Quaternion{1, 0, 0, 0}
            : core::Quaternion{-direction.y, direction.x, 0, 1 + direction.z}.normalized();
    const core::Quaternion inverse{-rotation.x, -rotation.y, -rotation.z, rotation.w};
    core::Bounds3d bounds;
    std::uint64_t vertices = 0;
    for (const auto &object : document.scene().objects_sorted()) {
        if (!object.visible || !object.mesh_asset)
            continue;
        const auto mesh = document.meshes().find(*object.mesh_asset);
        if (mesh == document.meshes().end())
            return Result::failure(invalid("shadow mesh is missing"));
        auto resolved = document.scene().world_transform(object.id);
        if (!resolved)
            return Result::failure(resolved.error());
        const auto override = request.transform_overrides.find(object.id);
        const auto t =
            override == request.transform_overrides.end() ? resolved.value() : override->second;
        if (!t.finite() || std::abs(t.scale.x) < 1e-12 || std::abs(t.scale.y) < 1e-12 ||
            std::abs(t.scale.z) < 1e-12)
            return Result::failure(invalid("shadow transform is singular"));
        if ((vertices += mesh->second.vertex_count()) > 1'048'576)
            return Result::failure(invalid("shadow bounds vertex budget exceeded"));
        for (const auto &vertex : mesh->second.vertices_sorted()) {
            const auto world =
                t.translation +
                t.rotation.rotate(core::componentwise_multiply(t.scale, vertex.position));
            const auto light = inverse.rotate(world);
            if (!light.finite())
                return Result::failure(invalid("shadow bounds overflow"));
            bounds.include(light);
        }
    }
    if (!bounds.valid())
        return Result::success({});
    const auto span = bounds.maximum - bounds.minimum;
    // Reserve a complete PCF border and snap the center to the fitted texel grid.
    const double extent = std::max({span.x, span.y, .01}) * double(request.shadows.resolution) /
                          double(request.shadows.resolution - 2 * (request.shadows.radius + 2));
    const double texel = extent / request.shadows.resolution;
    const auto center = (bounds.minimum + bounds.maximum) * .5;
    const double cx = std::round(center.x / texel) * texel,
                 cy = std::round(center.y / texel) * texel;
    const double padding = std::max(.01, std::max(span.z, extent) * .01);
    DirectionalShadowFrame frame;
    frame.settings = request.shadows;
    frame.view.position = rotation.rotate({cx, cy, bounds.maximum.z + padding + .01});
    frame.view.rotation = rotation;
    frame.view.projection = project::CameraProjection::orthographic;
    frame.view.orthographic_height = extent;
    frame.view.aspect_ratio = 1;
    frame.view.near_plane = .01;
    frame.view.far_plane = span.z + 2 * padding + .01;
    if (auto valid = frame.view.validate(); !valid)
        return Result::failure(valid.error());
    return Result::success(frame);
}
core::Result<double> sample_directional_shadow(const DirectionalShadowFrame &frame,
                                               std::span<const float> depths, core::Vec3d world,
                                               core::Vec3d normal, core::Vec3d direction) {
    using Result = core::Result<double>;
    const auto n = frame.settings.resolution;
    if (depths.size() != std::size_t(n) * n || !normal.finite() || !direction.finite() ||
        !world.finite())
        return Result::failure(invalid("shadow sample inputs are invalid"));
    const auto q = frame.view.rotation.normalized();
    const auto local = core::Quaternion{-q.x, -q.y, -q.z, q.w}.rotate(world - frame.view.position);
    const double u = .5 + local.x / frame.view.orthographic_height,
                 v = .5 - local.y / frame.view.orthographic_height;
    const double z =
        (-local.z - frame.view.near_plane) / (frame.view.far_plane - frame.view.near_plane);
    if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(z))
        return Result::failure(invalid("shadow sample projection overflow"));
    if (u < 0 || u > 1 || v < 0 || v > 1 || z < 0 || z > 1)
        return Result::success(1);
    // Preserve the existing CPU PCF reference's floor(uv*(extent-1)) contract.
    const int x = int(std::floor(u * (n - 1))), y = int(std::floor(v * (n - 1)));
    normal=normal.normalized();
    if(!normal.finite()) return Result::failure(invalid("shadow receiver plane is degenerate"));
    const auto light_normal=core::Quaternion{-q.x,-q.y,-q.z,q.w}.rotate(normal);
    const double scale=frame.view.orthographic_height/(frame.view.far_plane-frame.view.near_plane);
    const double du=std::abs(light_normal.z)>1e-5 ? light_normal.x/light_normal.z*scale : 0;
    const double dv=std::abs(light_normal.z)>1e-5 ? -light_normal.y/light_normal.z*scale : 0;
    const double bias =
        frame.settings.depth_bias +
        frame.settings.slope_bias * (1 - std::clamp(std::abs(core::dot(normal, direction)), 0., 1.));
    unsigned lit = 0, taps = 0;
    const int radius = int(frame.settings.radius);
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            const auto sx=std::clamp(x+dx,0,int(n)-1),sy=std::clamp(y+dy,0,int(n)-1);
            const float depth = depths[std::size_t(sy)*n+std::size_t(sx)];
            if (!std::isfinite(depth) || depth < 0 || depth > 1)
                return Result::failure(invalid("shadow map contains invalid sampled depth"));
            // Compare against the receiver plane at each sampled texel, not
            // one center depth for a sloped footprint (which causes acne).
            const double receiver=z+du*((sx+.5)/n-u)+dv*((sy+.5)/n-v);
            lit += receiver<0 || receiver>1 || receiver-bias<=depth;
            ++taps;
        }
    return Result::success(double(lit) / taps);
}
} // namespace carto::production
