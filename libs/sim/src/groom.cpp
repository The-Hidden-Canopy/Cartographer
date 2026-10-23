#include <carto/sim/groom.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace carto::sim {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

assets::Sha256Digest digest_of(const std::string& canonical) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
}

void write_double(std::ostringstream& output, double value) {
    output << std::setprecision(17) << value;
}

// Deterministic normalized linear blend between two unit quaternions.
core::Quaternion blend_orientations(
    core::Quaternion from,
    core::Quaternion to,
    double fraction) {
    const double dot =
        from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
    if (dot < 0.0) {
        to = core::Quaternion{-to.x, -to.y, -to.z, -to.w};
    }
    core::Quaternion blended{
        from.x + (to.x - from.x) * fraction,
        from.y + (to.y - from.y) * fraction,
        from.z + (to.z - from.z) * fraction,
        from.w + (to.w - from.w) * fraction,
    };
    return blended.normalized();
}

} // namespace

core::Result<void> GroomGuideCache::validate() const {
    if (binding_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "groom cache must be content-addressed to its source binding"));
    }
    if (guide_count == 0U || guide_count > SimCaps::kMaxGuides ||
        points_per_guide < 2U || points_per_guide > SimCaps::kMaxGuidePoints ||
        frame_count < 2U || frame_count > SimCaps::kMaxFrames ||
        ticks_per_frame == 0U || ticks_per_second == 0U) {
        return core::Result<void>::failure(validation(
            "groom cache dimensions exceed the declared simulation caps"));
    }
    const std::size_t expected = static_cast<std::size_t>(guide_count) *
        points_per_guide * frame_count;
    if (positions.size() != expected) {
        return core::Result<void>::failure(validation(
            "groom cache position table does not match its declared dimensions"));
    }
    if (positions.size() > kMaxPositions) {
        return core::Result<void>::failure(unsupported(
            "groom cache exceeds its declared bounded capacity"));
    }
    for (const core::Vec3d position : positions) {
        if (!position.finite()) {
            return core::Result<void>::failure(validation(
                "groom cache contains a non-finite position"));
        }
    }
    return core::Result<void>::success();
}

std::string GroomGuideCache::serialize() const {
    std::ostringstream output;
    output << "CARTO_SIM_GROOM_CACHE " << kSchemaVersion << '\n';
    output << "BINDING " << binding_digest.hex() << '\n';
    output << "GUIDES " << guide_count << '\n';
    output << "POINTS_PER_GUIDE " << points_per_guide << '\n';
    output << "FRAMES " << frame_count << '\n';
    output << "TICKS_PER_FRAME " << ticks_per_frame << '\n';
    output << "TICKS_PER_SECOND " << ticks_per_second << '\n';
    output << "POSITIONS " << positions.size() << '\n';
    for (const core::Vec3d position : positions) {
        write_double(output, position.x);
        output << ' ';
        write_double(output, position.y);
        output << ' ';
        write_double(output, position.z);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> GroomGuideCache::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

core::Result<void> GroomSimReceipt::validate() const {
    if (backend_identity.empty() || backend_identity.size() > kMaxIdentityBytes ||
        precision_identity.empty() || precision_identity.size() > kMaxIdentityBytes) {
        return core::Result<void>::failure(validation(
            "groom receipt requires bounded backend and precision identities"));
    }
    if (source_binding_digest.is_zero() || cache_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "groom receipt must bind its source and its output cache"));
    }
    if (guide_count == 0U || guide_count > SimCaps::kMaxGuides ||
        points_per_guide < 2U || points_per_guide > SimCaps::kMaxGuidePoints ||
        frames_executed < 2U || frames_executed > SimCaps::kMaxFrames ||
        substeps_per_frame == 0U ||
        substeps_per_frame > SimCaps::kMaxSubsteps) {
        return core::Result<void>::failure(validation(
            "groom receipt caps-used exceed the declared simulation caps"));
    }
    if (!std::isfinite(settle_residual) || settle_residual < 0.0) {
        return core::Result<void>::failure(validation(
            "groom receipt settle residual must be finite and non-negative"));
    }
    return core::Result<void>::success();
}

std::string GroomSimReceipt::serialize() const {
    std::ostringstream output;
    output << "CARTO_SIM_GROOM_RECEIPT " << kSchemaVersion << '\n';
    output << "BACKEND " << backend_identity << '\n';
    output << "PRECISION " << precision_identity << '\n';
    output << "BINDING " << source_binding_digest.hex() << '\n';
    output << "GUIDES " << guide_count << '\n';
    output << "POINTS_PER_GUIDE " << points_per_guide << '\n';
    output << "FRAMES " << frames_executed << '\n';
    output << "SUBSTEPS " << substeps_per_frame << '\n';
    output << "SETTLE_RESIDUAL ";
    write_double(output, settle_residual);
    output << '\n';
    output << "CACHE " << cache_digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> GroomSimReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

core::Result<GroomBakeResult> bake_groom(const GroomBakeRequest& request) {
    if (auto result = request.binding.validate(); !result) {
        return core::Result<GroomBakeResult>::failure(
            result.error().with_context("groom bake binding"));
    }
    if (request.binding_digest.is_zero()) {
        return core::Result<GroomBakeResult>::failure(invalid(
            "groom bake requires a content-addressed binding digest"));
    }
    if (request.guides.empty() || request.guides.size() > SimCaps::kMaxGuides ||
        request.guides.size() > request.binding.guides.size()) {
        return core::Result<GroomBakeResult>::failure(unsupported(
            "guide count exceeds the declared cap of " +
            std::to_string(SimCaps::kMaxGuides) +
            " or the authored binding guide table"));
    }
    if (request.root_motion.size() < 2U ||
        request.root_motion.size() > SimCaps::kMaxFrames) {
        return core::Result<GroomBakeResult>::failure(unsupported(
            "root motion frame count exceeds the declared cap of " +
            std::to_string(SimCaps::kMaxFrames)));
    }

    // Uniform point count across guides.
    const std::size_t points_per_guide = request.guides.front().rest_offsets.size();
    if (points_per_guide < 2U || points_per_guide > SimCaps::kMaxGuidePoints) {
        return core::Result<GroomBakeResult>::failure(unsupported(
            "guide point count exceeds the declared cap of " +
            std::to_string(SimCaps::kMaxGuidePoints)));
    }
    std::vector<std::vector<double>> rest_lengths(request.guides.size());
    for (std::size_t guide = 0U; guide < request.guides.size(); ++guide) {
        const GuideSpec& spec = request.guides[guide];
        if (spec.rest_offsets.size() != points_per_guide) {
            return core::Result<GroomBakeResult>::failure(validation(
                "all guides in one bake must share a uniform point count"));
        }
        rest_lengths[guide].reserve(points_per_guide);
        rest_lengths[guide].push_back(0.0);
        for (std::size_t point = 1U; point < points_per_guide; ++point) {
            if (!spec.rest_offsets[point].finite()) {
                return core::Result<GroomBakeResult>::failure(invalid(
                    "guide rest offsets must be finite"));
            }
            const double segment =
                (spec.rest_offsets[point] - spec.rest_offsets[point - 1U]).length();
            if (segment <= 1.0e-12) {
                return core::Result<GroomBakeResult>::failure(validation(
                    "guide contains a degenerate zero-length segment"));
            }
            rest_lengths[guide].push_back(segment);
        }
        if (!spec.rest_offsets.front().finite()) {
            return core::Result<GroomBakeResult>::failure(invalid(
                "guide rest offsets must be finite"));
        }
    }

    // Root motion: one timebase, strictly monotonic, uniform per-frame ticks.
    std::vector<character::RationalTime> times;
    times.reserve(request.root_motion.size());
    for (const RootMotionSample& sample : request.root_motion) {
        if (auto result = sample.time.validate(); !result) {
            return core::Result<GroomBakeResult>::failure(
                result.error().with_context("groom root motion sample"));
        }
        if (!sample.position.finite() || !sample.orientation.finite()) {
            return core::Result<GroomBakeResult>::failure(invalid(
                "root motion positions and orientations must be finite"));
        }
        times.push_back(sample.time);
    }
    if (auto result = character::validate_monotonic(times); !result) {
        return core::Result<GroomBakeResult>::failure(
            result.error().with_context("groom root motion samples"));
    }
    const std::uint32_t ticks_per_second = times.front().ticks_per_second;
    const std::int64_t first_tick = times.front().tick;
    const std::int64_t ticks_per_frame = times[1U].tick - first_tick;
    if (ticks_per_frame <= 0 ||
        ticks_per_frame >
            static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return core::Result<GroomBakeResult>::failure(validation(
            "root motion frames must advance by a positive uniform tick count "
            "inside the bounded per-frame cap"));
    }
    for (std::size_t index = 1U; index < times.size(); ++index) {
        if (times[index].ticks_per_second != ticks_per_second ||
            times[index].tick - times[index - 1U].tick != ticks_per_frame) {
            return core::Result<GroomBakeResult>::failure(validation(
                "root motion must advance by a uniform tick count per frame"));
        }
    }

    const auto& spring = request.spring;
    if (!std::isfinite(spring.stiffness) || spring.stiffness <= 0.0 ||
        !std::isfinite(spring.damping) || spring.damping < 0.0 ||
        spring.damping >= 1.0 ||
        !std::isfinite(spring.mass_per_point_kg) || spring.mass_per_point_kg <= 0.0 ||
        !spring.gravity.finite()) {
        return core::Result<GroomBakeResult>::failure(invalid(
            "groom spring parameters must be finite with positive stiffness, "
            "damping inside [0, 1) and positive point mass"));
    }
    if (spring.substeps == 0U || spring.substeps > SimCaps::kMaxSubsteps) {
        return core::Result<GroomBakeResult>::failure(unsupported(
            "substep count exceeds the declared cap of " +
            std::to_string(SimCaps::kMaxSubsteps)));
    }

    const std::size_t guide_count = request.guides.size();
    const std::size_t frame_count = request.root_motion.size();
    const double frame_seconds =
        static_cast<double>(ticks_per_frame) /
        static_cast<double>(ticks_per_second);
    const double substep_seconds =
        frame_seconds / static_cast<double>(spring.substeps);

    // Root points are infinite-mass and driven by the root motion; the
    // remaining points are chained point masses integrated in fixed order.
    std::vector<std::vector<core::Vec3d>> position(guide_count);
    std::vector<std::vector<core::Vec3d>> velocity(guide_count);
    std::vector<std::vector<core::Vec3d>> rest_world(guide_count);
    const core::Quaternion initial_orientation =
        request.root_motion.front().orientation.normalized();
    for (std::size_t guide = 0U; guide < guide_count; ++guide) {
        position[guide].resize(points_per_guide);
        velocity[guide].assign(points_per_guide, core::Vec3d{});
        rest_world[guide].resize(points_per_guide);
        const core::Vec3d root = request.root_motion.front().position;
        for (std::size_t point = 0U; point < points_per_guide; ++point) {
            rest_world[guide][point] =
                root +
                initial_orientation.rotate(request.guides[guide].rest_offsets[point]);
            position[guide][point] = rest_world[guide][point];
        }
    }

    auto root_at = [&](std::size_t frame, std::uint32_t substep) {
        const RootMotionSample& from = request.root_motion[frame];
        const RootMotionSample& to = request.root_motion[frame + 1U];
        const double fraction =
            static_cast<double>(substep) / static_cast<double>(spring.substeps);
        return std::pair<core::Vec3d, core::Quaternion>{
            from.position + (to.position - from.position) * fraction,
            blend_orientations(from.orientation, to.orientation, fraction)};
    };

    GroomGuideCache cache;
    cache.binding_digest = request.binding_digest;
    cache.guide_count = static_cast<std::uint32_t>(guide_count);
    cache.points_per_guide = static_cast<std::uint32_t>(points_per_guide);
    cache.frame_count = static_cast<std::uint32_t>(frame_count);
    cache.ticks_per_frame = static_cast<std::uint32_t>(ticks_per_frame);
    cache.ticks_per_second = ticks_per_second;
    cache.positions.reserve(guide_count * points_per_guide * frame_count);

    auto record_frame = [&]() {
        for (std::size_t guide = 0U; guide < guide_count; ++guide) {
            cache.positions.insert(
                cache.positions.end(),
                position[guide].begin(),
                position[guide].end());
        }
    };
    record_frame();

    for (std::size_t frame = 0U; frame + 1U < frame_count; ++frame) {
        for (std::uint32_t substep = 1U; substep <= spring.substeps; ++substep) {
            const auto [root_position, root_orientation] = root_at(frame, substep);
            for (std::size_t guide = 0U; guide < guide_count; ++guide) {
                position[guide][0U] =
                    root_position +
                    root_orientation.rotate(
                        request.guides[guide].rest_offsets.front());
                for (std::size_t point = 1U; point < points_per_guide; ++point) {
                    const core::Vec3d parent = position[guide][point - 1U];
                    const core::Vec3d delta = position[guide][point] - parent;
                    const double length = delta.length();
                    if (length > 1.0e-12) {
                        const double extension = length - rest_lengths[guide][point];
                        const double spring_acceleration =
                            -spring.stiffness * extension /
                            spring.mass_per_point_kg;
                        const core::Vec3d acceleration =
                            delta * (spring_acceleration / length) + spring.gravity;
                        velocity[guide][point] =
                            velocity[guide][point] + acceleration * substep_seconds;
                    } else {
                        velocity[guide][point] =
                            velocity[guide][point] + spring.gravity * substep_seconds;
                    }
                    velocity[guide][point] =
                        velocity[guide][point] * (1.0 - spring.damping);
                    position[guide][point] =
                        position[guide][point] + velocity[guide][point] * substep_seconds;
                }
            }
        }
        record_frame();
    }

    // Settle residual: mean displacement of the free points between the
    // final two recorded frames.
    double settle_sum = 0.0;
    const std::size_t free_points = points_per_guide - 1U;
    const std::size_t stride = guide_count * points_per_guide;
    for (std::size_t guide = 0U; guide < guide_count; ++guide) {
        for (std::size_t point = 1U; point < points_per_guide; ++point) {
            const std::size_t base =
                (frame_count - 2U) * stride + guide * points_per_guide + point;
            const core::Vec3d delta =
                cache.positions[base + stride] - cache.positions[base];
            settle_sum += delta.length();
        }
    }
    const double settle_residual =
        settle_sum / static_cast<double>(guide_count * free_points);

    auto cache_digest = cache.canonical_digest();
    if (!cache_digest) {
        return core::Result<GroomBakeResult>::failure(cache_digest.error());
    }

    GroomSimReceipt receipt;
    receipt.backend_identity = std::string{kGroomSpringBackendIdentity};
    receipt.precision_identity = std::string{kFloat64PrecisionIdentity};
    receipt.source_binding_digest = request.binding_digest;
    receipt.guide_count = static_cast<std::uint32_t>(guide_count);
    receipt.points_per_guide = static_cast<std::uint32_t>(points_per_guide);
    receipt.frames_executed = static_cast<std::uint32_t>(frame_count);
    receipt.substeps_per_frame = spring.substeps;
    receipt.settle_residual = settle_residual;
    receipt.cache_digest = cache_digest.value();

    GroomBakeResult result;
    result.cache = std::move(cache);
    result.receipt = std::move(receipt);
    return core::Result<GroomBakeResult>::success(std::move(result));
}

} // namespace carto::sim
