#include <algorithm>
#include <bit>
#include <carto/production/authoring_environment.hpp>
#include <cmath>
#include <numbers>

namespace carto::production {
namespace {
using core::Vec3d;
constexpr double pi = std::numbers::pi;
core::Diagnostic invalid(const char *s) { return {core::ErrorCode::invalid_argument, s}; }
double radical(unsigned n) {
    double v = 0, place = .5;
    while (n) {
        v += (n & 1) * place;
        n >>= 1;
        place *= .5;
    }
    return v;
}
core::Vec2d uv(Vec3d n) {
    return {std::atan2(n.z, n.x) / (2 * pi) + .5, std::acos(std::clamp(n.y, -1., 1.)) / pi};
}
Vec3d direction(unsigned x, unsigned y, unsigned w, unsigned h) {
    const double p = 2 * pi * ((x + .5) / w - .5), t = pi * (y + .5) / h;
    return {std::sin(t) * std::cos(p), std::cos(t), std::sin(t) * std::sin(p)};
}
template <class Fetch>
std::array<double, 4> bilinear(core::Vec2d p, unsigned w, unsigned h, bool wrap, Fetch fetch) {
    const double x = p.x * w - .5, y = p.y * h - .5;
    const auto x0 = int(std::floor(x)), y0 = int(std::floor(y));
    std::array<double, 4> out{};
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            const int sx =
                wrap ? ((x0 + dx) % int(w) + int(w)) % int(w) : std::clamp(x0 + dx, 0, int(w) - 1);
            const int sy = std::clamp(y0 + dy, 0, int(h) - 1);
            const auto c = fetch(unsigned(sx), unsigned(sy));
            const double weight = (dx ? x - x0 : 1 - (x - x0)) * (dy ? y - y0 : 1 - (y - y0));
            for (unsigned i = 0; i < 4; ++i)
                out[i] += c[i] * weight;
        }
    return out;
}
Vec3d source_sample(const project::AuthoredEnvironment &e, Vec3d d) {
    const auto c = bilinear(uv(d), e.width, e.height, true, [&](auto x, auto y) {
        const auto p = e.linear_radiance[std::size_t(y) * e.width + x];
        return std::array<double, 4>{p.x, p.y, p.z, 1};
    });
    return {c[0], c[1], c[2]};
}
Vec3d to_world(Vec3d n, Vec3d p) {
    const auto tangent =
        core::cross(std::abs(n.z) < .999 ? Vec3d{0, 0, 1} : Vec3d{1, 0, 0}, n).normalized();
    return tangent * p.x + core::cross(n, tangent) * p.y + n * p.z;
}
Vec3d ggx(unsigned i, unsigned count, double roughness) {
    // Match the current direct-light GGX alpha convention, including its floor.
    const double alpha = std::max(.045, roughness * roughness), u = (i + .5) / count,
                 v = radical(i);
    const double z = std::sqrt((1 - v) / (1 + (alpha * alpha - 1) * v)),
                 s = std::sqrt(std::max(0., 1 - z * z)), p = 2 * pi * u;
    return {s * std::cos(p), s * std::sin(p), z};
}
double smith(double nv, double roughness) {
    const double a = std::max(.045, roughness * roughness), k = (a + 1) * (a + 1) / 8;
    return nv / std::max(nv * (1 - k) + k, 1e-12);
}
std::array<float, 4> lut_value(double nv, double roughness) {
    Vec3d view{std::sqrt(std::max(0., 1 - nv * nv)), 0, nv};
    double a = 0, b = 0;
    for (unsigned i = 0; i < 256; ++i) {
        const auto h = ggx(i, 256, roughness);
        const double vh = std::max(0., core::dot(view, h));
        const auto l = h * (2 * vh) - view;
        if (l.z <= 0)
            continue;
        const double g =
            smith(l.z, roughness) * smith(nv, roughness) * vh / std::max(h.z * nv, 1e-12);
        const double f = std::pow(1 - vh, 5);
        a += (1 - f) * g;
        b += f * g;
    }
    return {float(a / 256), float(b / 256), 0, 1};
}
std::array<double, 4> atlas_sample(const EnvironmentLighting &p, unsigned row, unsigned w,
                                   unsigned h, core::Vec2d coordinate, bool wrap = true,
                                   unsigned column = 0) {
    return bilinear(coordinate, w, h, wrap, [&](auto x, auto y) {
        return p.atlas()[std::size_t(row + y) * p.width + column + x];
    });
}
} // namespace
core::Result<assets::Sha256Digest>
environment_content_digest(const project::AuthoredEnvironment &e) {
    if (auto valid = e.validate(); !valid)
        return core::Result<assets::Sha256Digest>::failure(valid.error());
    std::vector<std::uint8_t> bytes;
    const auto append = [&](std::uint64_t x) {
        for (unsigned i = 0; i < 8; ++i)
            bytes.push_back(std::uint8_t(x >> (i * 8)));
    };
    // Algorithm version is part of the derived identity. Source metadata that
    // changes orientation/power does not force rebuilding all convolutions.
    append(1);
    append(e.width);
    append(e.height);
    for (const auto c : e.linear_radiance)
        for (auto x : {c.x, c.y, c.z})
            append(std::bit_cast<std::uint64_t>(x));
    return core::Result<assets::Sha256Digest>::success(assets::sha256(bytes));
}
core::Result<std::shared_ptr<const EnvironmentLighting>>
compile_environment_lighting(const project::AuthoredEnvironment &e) {
    using Result = core::Result<std::shared_ptr<const EnvironmentLighting>>;
    auto digest = environment_content_digest(e);
    if (!digest)
        return Result::failure(digest.error());
    auto product = std::shared_ptr<EnvironmentLighting>(new EnvironmentLighting);
    product->source_digest_ = digest.value();
    product->atlas_.resize(std::size_t(product->width) * product->height);
    const auto store = [&](unsigned x, unsigned y, Vec3d c) {
        product->atlas_[std::size_t(y) * product->width + x] = {float(c.x), float(c.y), float(c.z),
                                                                1};
    };
    for (unsigned level = 0; level < product->levels; ++level) {
        const unsigned w = product->width >> std::min(level, 4U), h = w / 2;
        const unsigned row = level < 5 ? 128 - w : 120, column = level < 5 ? 0 : (level - 4) * 8;
        const double roughness = double(level) / (product->levels - 1);
        for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x) {
                const auto n = direction(x, y, w, h);
                Vec3d total{};
                double weight = 0;
                for (unsigned i = 0; i < 128; ++i) {
                    const auto half = to_world(n, ggx(i, 128, roughness));
                    const auto l = half * (2 * core::dot(n, half)) - n;
                    const double nl = std::max(0., core::dot(n, l));
                    if (nl > 0) {
                        total = total + source_sample(e, l) * nl;
                        weight += nl;
                    }
                }
                store(column + x, row + y, total * (1 / std::max(weight, 1e-12)));
            }
    }
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 32; ++x) {
            const auto n = direction(x, y, 32, 16);
            Vec3d total{};
            for (unsigned i = 0; i < 128; ++i) {
                const double u = (i + .5) / 128, v = radical(i), p = 2 * pi * u;
                total = total + source_sample(
                                    e, to_world(n, {std::sqrt(v) * std::cos(p),
                                                    std::sqrt(v) * std::sin(p), std::sqrt(1 - v)}));
            }
            store(x, product->diffuse_row + y, total * (1. / 128)); // cosine average = E/pi
        }
    static const auto lut = [] {
        std::array<std::array<float, 4>, 64 * 64> values{};
        for (unsigned y = 0; y < 64; ++y)
            for (unsigned x = 0; x < 64; ++x)
                values[y * 64 + x] = lut_value((x + .5) / 64, (y + .5) / 64);
        return values;
    }();
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x)
            product->atlas_[std::size_t(product->brdf_row + y) * product->width + x] =
                lut[y * 64 + x];
    return Result::success(std::move(product));
}
Vec3d EnvironmentLighting::diffuse(Vec3d n) const {
    n = n.normalized();
    if (!n.finite())
        return n;
    const auto c = atlas_sample(*this, diffuse_row, 32, 16, uv(n));
    return {c[0], c[1], c[2]};
}
Vec3d EnvironmentLighting::specular(Vec3d r, double roughness) const {
    r = r.normalized();
    if (!r.finite())
        return r;
    if (!std::isfinite(roughness)) {
        const auto v = std::numeric_limits<double>::quiet_NaN();
        return {v, v, v};
    }
    const double level = std::clamp(roughness, 0., 1.) * (levels - 1);
    const unsigned a = unsigned(std::floor(level)), b = std::min(a + 1, levels - 1);
    const auto sample = [&](unsigned i) {
        const unsigned w = width >> std::min(i, 4U);
        return atlas_sample(*this, i < 5 ? 128 - w : 120, w, w / 2, uv(r), true,
                            i < 5 ? 0 : (i - 4) * 8);
    };
    const auto x = sample(a), y = sample(b);
    const double t = level - a;
    return {x[0] * (1 - t) + y[0] * t, x[1] * (1 - t) + y[1] * t, x[2] * (1 - t) + y[2] * t};
}
std::array<double, 2> EnvironmentLighting::brdf(double nv, double roughness) const {
    if (!std::isfinite(nv) || !std::isfinite(roughness)) {
        const auto v = std::numeric_limits<double>::quiet_NaN();
        return {v, v};
    }
    const auto c = atlas_sample(*this, brdf_row, 64, 64,
                                {std::clamp(nv, 0., 1.), std::clamp(roughness, 0., 1.)}, false);
    return {c[0], c[1]};
}
core::Result<Vec3d> evaluate_environment_lighting(const EnvironmentLighting &p,
                                                  const project::AuthoredEnvironment &e,
                                                  const render::StandardMaterial &m, Vec3d n,
                                                  Vec3d v, double ao) {
    using Result = core::Result<Vec3d>;
    if (auto valid = m.validate(); !valid)
        return Result::failure(valid.error());
    if (!std::isfinite(e.rotation_radians) || std::abs(e.rotation_radians) > 1e6 ||
        !std::isfinite(e.intensity) || e.intensity < 0 || e.intensity > 1e4)
        return Result::failure(invalid("environment orientation/intensity is invalid"));
    n = n.normalized();
    v = v.normalized();
    if (!n.finite() || !v.finite() || !std::isfinite(ao) || ao < 0 || ao > 1)
        return Result::failure(invalid("environment surface is invalid"));
    const double nv = std::max(0., core::dot(n, v));
    if (!e.enabled || e.intensity == 0 || nv <= 0)
        return Result::success({});
    const double c = std::cos(e.rotation_radians), s = std::sin(e.rotation_radians);
    const auto rotate = [&](Vec3d d) { return Vec3d{c * d.x - s * d.z, d.y, s * d.x + c * d.z}; };
    const auto reflection = rotate(n * (2 * nv) - v);
    const auto diffuse = p.diffuse(rotate(n)), specular = p.specular(reflection, m.roughness),
               coat = p.specular(reflection, m.clearcoat_roughness);
    const auto lut = p.brdf(nv, m.roughness), coat_lut = p.brdf(nv, m.clearcoat_roughness);
    const Vec3d base{m.base_color_factor.x, m.base_color_factor.y, m.base_color_factor.z};
    const auto f0 = base * m.metallic +
                    Vec3d{m.dielectric_f0, m.dielectric_f0, m.dielectric_f0} * (1 - m.metallic);
    const auto diffuse_weight = (Vec3d{1, 1, 1} - f0) * (1 - m.metallic);
    const auto diffuse_term =
        core::componentwise_multiply(core::componentwise_multiply(diffuse, base), diffuse_weight) *
        ao;
    const auto specular_term =
        core::componentwise_multiply(specular, f0 * lut[0] + Vec3d{lut[1], lut[1], lut[1]});
    const double attenuation = 1 - m.clearcoat * (.04 + .96 * std::pow(1 - nv, 5));
    const auto hdr = ((diffuse_term + specular_term) * attenuation +
                      coat * (m.clearcoat * (.04 * coat_lut[0] + coat_lut[1]))) *
                     e.intensity;
    if (!hdr.finite())
        return Result::failure(invalid("environment radiance overflow"));
    return Result::success(hdr);
}
} // namespace carto::production
