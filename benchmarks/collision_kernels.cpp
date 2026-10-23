#include <carto/assets/blob_store.hpp>
#include <carto/collision/benchmark.hpp>
#include <carto/collision/metrics.hpp>
#include <carto/collision/primitive.hpp>
#include <carto/core/math.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/compiled_mesh.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using carto::assets::Sha256Digest;
using carto::collision::CollisionBenchmarkEvidence;
using carto::collision::CollisionBenchmarkReceipt;
using carto::collision::CollisionErrorMetrics;
using carto::collision::CollisionPromotionGate;
using carto::collision::CollisionReceiptStatus;
using carto::collision::PrimitiveCollisionCandidate;
using carto::collision::PrimitiveCollisionKind;
using carto::core::Vec3d;
using carto::geometry::CompiledMesh;

constexpr std::size_t kWarmupIterations = 5U;
constexpr double kRequiredClearance = 0.05;
constexpr double kPi = 3.14159265358979323846;

struct TimingSummary {
    double minimum_microseconds = 0.0;
    double median_microseconds = 0.0;
    double p95_microseconds = 0.0;
    double mean_microseconds = 0.0;
    std::size_t checksum = 0U;
};

struct FixtureCase {
    std::string name;
    CompiledMesh mesh;
    std::size_t generation_iterations = 0U;
    std::size_t metric_iterations = 0U;
};

struct ReceiptEvidence {
    CollisionBenchmarkReceipt receipt;
    bool promoted = false;
    std::size_t serialized_bytes = 0U;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

[[nodiscard]] TimingSummary summarize(
    const std::vector<double>& samples,
    std::size_t checksum) {
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double rank) {
        const double position = rank * static_cast<double>(sorted.size() - 1U);
        const std::size_t lower = static_cast<std::size_t>(std::floor(position));
        const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
        const double fraction = position - static_cast<double>(lower);
        return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
    };
    const double total = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    return TimingSummary{
        sorted.front(), percentile(0.50), percentile(0.95),
        total / static_cast<double>(sorted.size()), checksum};
}

template <typename Run>
[[nodiscard]] TimingSummary time_loop(std::size_t iterations, Run&& run) {
    std::vector<double> samples;
    samples.reserve(iterations);
    std::size_t checksum = 0U;
    for (std::size_t index = 0U; index < kWarmupIterations + iterations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        const std::uint64_t units = run();
        const auto finished = std::chrono::steady_clock::now();
        if (index >= kWarmupIterations) {
            checksum += static_cast<std::size_t>(units);
            samples.push_back(
                std::chrono::duration<double, std::micro>(finished - started).count());
        }
    }
    return summarize(samples, checksum);
}

struct MeshBuilder {
    CompiledMesh mesh;
    std::uint64_t next_vertex_id = 1U;
    std::uint64_t next_face_id = 1U;

    [[nodiscard]] std::uint32_t add_vertex(const Vec3d& position, const Vec3d& normal) {
        const auto index = static_cast<std::uint32_t>(mesh.positions.size());
        mesh.positions.push_back(position);
        mesh.normals.push_back(normal.normalized());
        mesh.vertex_ids.push_back(carto::geometry::VertexId{next_vertex_id++});
        mesh.bounds.include(position);
        return index;
    }

    void add_triangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        mesh.indices.push_back(a);
        mesh.indices.push_back(b);
        mesh.indices.push_back(c);
        mesh.triangle_faces.push_back(carto::geometry::FaceId{next_face_id++});
        mesh.triangle_edges.emplace_back();
    }

    [[nodiscard]] CompiledMesh finish() {
        mesh.source_revision = carto::core::Revision{1U};
        require(mesh.valid(), "analytic fixture mesh is invalid");
        return std::move(mesh);
    }
};

void require_analytic_volume(const CompiledMesh& mesh, double expected) {
    const auto volume = carto::collision::mesh_volume(mesh);
    require(static_cast<bool>(volume), "fixture volume computes");
    require(volume.value() > 0.0, "fixture mesh must be wound outward");
    const double residual = std::abs(volume.value() - expected) / expected;
    if (residual >= 0.05) {
        throw std::runtime_error(
            "fixture volume deviates from its analytic solid by more than 5 percent: "
            "measured=" + std::to_string(volume.value()) +
            " expected=" + std::to_string(expected));
    }
}

[[nodiscard]] CompiledMesh box_mesh(const Vec3d& half_extents) {
    MeshBuilder builder;
    const double hx = half_extents.x;
    const double hy = half_extents.y;
    const double hz = half_extents.z;
    const Vec3d corners[8] = {
        {-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {-hx, hy, -hz},
        {-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz},
    };
    std::uint32_t vertex_indices[8];
    for (std::size_t index = 0U; index < 8U; ++index) {
        const Vec3d direction{
            corners[index].x / hx, corners[index].y / hy, corners[index].z / hz};
        vertex_indices[index] = builder.add_vertex(corners[index], direction);
    }
    constexpr std::uint32_t faces[12][3] = {
        {0, 2, 1}, {0, 3, 2},
        {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4},
        {3, 6, 2}, {3, 7, 6},
        {0, 4, 7}, {0, 7, 3},
        {1, 2, 6}, {1, 6, 5},
    };
    for (const auto& face : faces) {
        builder.add_triangle(
            vertex_indices[face[0]], vertex_indices[face[1]], vertex_indices[face[2]]);
    }
    CompiledMesh mesh = builder.finish();
    require_analytic_volume(
        mesh, 8.0 * half_extents.x * half_extents.y * half_extents.z);
    return mesh;
}

[[nodiscard]] CompiledMesh sphere_mesh(double radius, std::size_t rings, std::size_t segments) {
    require(rings >= 2U && segments >= 3U, "sphere fixture needs rings and segments");
    MeshBuilder builder;
    const std::uint32_t top = builder.add_vertex({0.0, radius, 0.0}, {0.0, 1.0, 0.0});
    std::vector<std::uint32_t> previous;
    previous.reserve(segments);
    for (std::size_t ring = 0U; ring < rings; ++ring) {
        const double phi = kPi * static_cast<double>(ring + 1U) /
            static_cast<double>(rings + 1U);
        const double y = radius * std::cos(phi);
        const double ring_radius = radius * std::sin(phi);
        std::vector<std::uint32_t> current;
        current.reserve(segments);
        for (std::size_t segment = 0U; segment < segments; ++segment) {
            const double theta = 2.0 * kPi * static_cast<double>(segment) /
                static_cast<double>(segments);
            const Vec3d direction{
                std::cos(theta), 0.0, std::sin(theta)};
            const Vec3d position = direction * ring_radius + Vec3d{0.0, y, 0.0};
            current.push_back(builder.add_vertex(position, position * (1.0 / radius)));
        }
        if (ring == 0U) {
            for (std::size_t segment = 0U; segment < segments; ++segment) {
                const std::size_t next = (segment + 1U) % segments;
                builder.add_triangle(top, current[next], current[segment]);
            }
        } else {
            for (std::size_t segment = 0U; segment < segments; ++segment) {
                const std::size_t next = (segment + 1U) % segments;
                builder.add_triangle(
                    previous[segment], previous[next], current[next]);
                builder.add_triangle(
                    previous[segment], current[next], current[segment]);
            }
        }
        previous = std::move(current);
    }
    const std::uint32_t bottom =
        builder.add_vertex({0.0, -radius, 0.0}, {0.0, -1.0, 0.0});
    for (std::size_t segment = 0U; segment < segments; ++segment) {
        const std::size_t next = (segment + 1U) % segments;
        builder.add_triangle(bottom, previous[segment], previous[next]);
    }
    CompiledMesh mesh = builder.finish();
    require_analytic_volume(mesh, 4.0 * kPi * radius * radius * radius / 3.0);
    return mesh;
}

[[nodiscard]] CompiledMesh cylinder_mesh(
    double radius,
    double half_height,
    std::size_t segments) {
    require(segments >= 3U, "cylinder fixture needs segments");
    MeshBuilder builder;
    std::vector<std::uint32_t> bottom;
    std::vector<std::uint32_t> top;
    bottom.reserve(segments);
    top.reserve(segments);
    for (std::size_t segment = 0U; segment < segments; ++segment) {
        const double theta = 2.0 * kPi * static_cast<double>(segment) /
            static_cast<double>(segments);
        const Vec3d direction{std::cos(theta), 0.0, std::sin(theta)};
        bottom.push_back(builder.add_vertex(
            direction * radius + Vec3d{0.0, -half_height, 0.0}, direction));
        top.push_back(builder.add_vertex(
            direction * radius + Vec3d{0.0, half_height, 0.0}, direction));
    }
    for (std::size_t segment = 0U; segment < segments; ++segment) {
        const std::size_t next = (segment + 1U) % segments;
        builder.add_triangle(bottom[segment], top[segment], top[next]);
        builder.add_triangle(bottom[segment], top[next], bottom[next]);
    }
    const std::uint32_t bottom_center =
        builder.add_vertex({0.0, -half_height, 0.0}, {0.0, -1.0, 0.0});
    const std::uint32_t top_center =
        builder.add_vertex({0.0, half_height, 0.0}, {0.0, 1.0, 0.0});
    for (std::size_t segment = 0U; segment < segments; ++segment) {
        const std::size_t next = (segment + 1U) % segments;
        builder.add_triangle(bottom_center, bottom[segment], bottom[next]);
        builder.add_triangle(top_center, top[next], top[segment]);
    }
    CompiledMesh mesh = builder.finish();
    require_analytic_volume(mesh, kPi * radius * radius * 2.0 * half_height);
    return mesh;
}

// Deterministic analytic surface samples for one fitted primitive. The
// candidates are axis-bound records: cylinder and capsule use the shared
// Y-axis convention of the neutral primitive contract.
[[nodiscard]] std::vector<Vec3d> proxy_surface_samples(
    const PrimitiveCollisionCandidate& candidate,
    std::size_t count) {
    std::vector<Vec3d> samples;
    samples.reserve(count);
    const auto push = [&samples, &candidate](const Vec3d& offset) {
        samples.push_back(candidate.center + offset);
    };
    switch (candidate.kind) {
    case PrimitiveCollisionKind::sphere: {
        constexpr double kGoldenAngle = 2.39996322972865332;
        for (std::size_t index = 0U; index < count; ++index) {
            const double y = 1.0 - 2.0 * (static_cast<double>(index) + 0.5) /
                static_cast<double>(count);
            const double ring = std::sqrt(std::max(0.0, 1.0 - y * y));
            const double theta = kGoldenAngle * static_cast<double>(index);
            push(Vec3d{
                candidate.radius * ring * std::cos(theta),
                candidate.radius * y,
                candidate.radius * ring * std::sin(theta)});
        }
        break;
    }
    case PrimitiveCollisionKind::box: {
        const std::size_t grid = std::max<std::size_t>(
            1U, static_cast<std::size_t>(
                std::ceil(std::sqrt(static_cast<double>(count) / 6.0))));
        const Vec3d& half = candidate.half_extents;
        for (std::size_t face = 0U; face < 6U; ++face) {
            const std::size_t axis = face / 2U;
            const double sign = face % 2U == 0U ? -1.0 : 1.0;
            for (std::size_t u = 0U; u < grid; ++u) {
                for (std::size_t v = 0U; v < grid; ++v) {
                    const double a =
                        (static_cast<double>(u) + 0.5) / static_cast<double>(grid) * 2.0 - 1.0;
                    const double b =
                        (static_cast<double>(v) + 0.5) / static_cast<double>(grid) * 2.0 - 1.0;
                    Vec3d offset;
                    const double axis_extent =
                        axis == 0U ? half.x : (axis == 1U ? half.y : half.z);
                    const double u_extent =
                        axis == 0U ? half.y : half.x;
                    const double v_extent =
                        axis == 2U ? half.y : half.z;
                    if (axis == 0U) offset = {sign * axis_extent, a * u_extent, b * v_extent};
                    else if (axis == 1U) offset = {a * u_extent, sign * axis_extent, b * v_extent};
                    else offset = {a * u_extent, b * v_extent, sign * axis_extent};
                    push(offset);
                }
            }
        }
        samples.resize(count);
        break;
    }
    case PrimitiveCollisionKind::cylinder:
    case PrimitiveCollisionKind::capsule: {
        const bool capsule = candidate.kind == PrimitiveCollisionKind::capsule;
        const std::size_t side_count = count * 3U / 4U;
        const std::size_t cap_count = (count - side_count) / 2U;
        for (std::size_t index = 0U; index < side_count; ++index) {
            const double t = static_cast<double>(index) /
                static_cast<double>(side_count);
            const double theta = 2.0 * kPi * static_cast<double>(index) * 0.6180339887498949;
            push(Vec3d{
                candidate.radius * std::cos(theta),
                -candidate.half_height + 2.0 * candidate.half_height * t,
                candidate.radius * std::sin(theta)});
        }
        for (std::size_t cap = 0U; cap < 2U; ++cap) {
            const double sign = cap == 0U ? -1.0 : 1.0;
            for (std::size_t index = 0U; index < cap_count; ++index) {
                const double fraction = (static_cast<double>(index) + 0.5) /
                    static_cast<double>(cap_count);
                const double theta = 2.0 * kPi * static_cast<double>(index) * 0.7548776662466927;
                double height = candidate.half_height;
                double ring_radius = candidate.radius * fraction;
                if (capsule) {
                    // Hemispherical cap: latitude from the equator to the pole.
                    const double alpha = fraction * kPi * 0.5;
                    height = candidate.half_height + candidate.radius * std::sin(alpha);
                    ring_radius = candidate.radius * std::cos(alpha);
                }
                push(Vec3d{
                    ring_radius * std::cos(theta),
                    sign * height,
                    ring_radius * std::sin(theta)});
            }
        }
        break;
    }
    }
    return samples;
}

[[nodiscard]] std::vector<FixtureCase> make_fixtures() {
    std::vector<FixtureCase> fixtures;
    fixtures.push_back(FixtureCase{
        "box.half-extents.0.75-0.5-1.25", box_mesh({0.75, 0.5, 1.25}), 4000U, 20000U});
    fixtures.push_back(FixtureCase{
        "sphere.r1.8x24", sphere_mesh(1.0, 8U, 24U), 2500U, 20000U});
    fixtures.push_back(FixtureCase{
        "sphere.r1.16x32", sphere_mesh(1.0, 16U, 32U), 1000U, 20000U});
    fixtures.push_back(FixtureCase{
        "cylinder.r0.75-h1.25.16", cylinder_mesh(0.75, 1.25, 16U), 3000U, 20000U});
    fixtures.push_back(FixtureCase{
        "cylinder.r0.75-h1.25.64", cylinder_mesh(0.75, 1.25, 64U), 1500U, 20000U});
    return fixtures;
}

[[nodiscard]] ReceiptEvidence build_receipt(
    const FixtureCase& fixture,
    const PrimitiveCollisionCandidate& selected,
    const CollisionErrorMetrics& metrics,
    double generation_mean_microseconds) {
    CollisionBenchmarkReceipt receipt;
    receipt.basis.source_identity = "benchmark.fixture." + fixture.name;
    receipt.basis.source_revision = carto::core::Revision{1U};
    receipt.basis.source_digest = digest("source-" + fixture.name);
    receipt.basis.algorithm = "primitive-fit/v1";
    receipt.basis.settings_digest =
        digest("permitted=sphere,box,capsule,cylinder;clearance=0.05;fixture=analytic-v1");
    receipt.basis.target_profile_identity = "consumer.collision.gameplay-query";
    receipt.basis.target_profile_digest = digest("consumer.collision.gameplay-query/v1");
    receipt.basis.hardware_profile = "host-clock.cpu.steady-clock";
    receipt.candidate_digest = selected.candidate_digest;
    receipt.measured_error = metrics;
    receipt.cpu_reference_milliseconds = generation_mean_microseconds / 1000.0;
    receipt.evidence = CollisionBenchmarkEvidence::cpu_reference;
    receipt.status = CollisionReceiptStatus::admitted;
    require(
        static_cast<bool>(receipt.refresh_digest()),
        "benchmark receipt refreshes its digest");
    require(
        static_cast<bool>(receipt.validate()),
        "benchmark receipt validates as CPU reference evidence");

    const std::string serialized = receipt.serialize();
    const auto reopened = CollisionBenchmarkReceipt::deserialize(serialized);
    require(
        reopened && reopened.value().canonical() == receipt.canonical(),
        "benchmark receipt survives a deterministic round trip");

    CollisionPromotionGate gate;
    gate.error_thresholds.max_volume_overlap_residual =
        metrics.volume_overlap_residual * 4.0 + 0.25;
    gate.error_thresholds.max_penetration_depth =
        metrics.max_penetration_depth * 4.0 + 0.25;
    gate.error_thresholds.max_one_sided_hausdorff =
        metrics.one_sided_hausdorff * 4.0 + 0.25;
    gate.error_thresholds.max_clearance_violation =
        metrics.clearance_violation * 4.0 + 0.25;
    gate.max_cpu_reference_milliseconds =
        receipt.cpu_reference_milliseconds * 8.0 + 1.0;
    const auto promotion = carto::collision::promote_collision_candidate(
        receipt, std::nullopt, gate);
    require(
        promotion && promotion.value().promoted_new_candidate &&
            !promotion.value().retained_last_good,
        "in-budget CPU reference candidate promotes through its gate");

    return ReceiptEvidence{std::move(receipt), true, serialized.size()};
}

void print_row(
    const FixtureCase& fixture,
    const TimingSummary& generation,
    const TimingSummary& metrics,
    const PrimitiveCollisionCandidate& selected,
    const CollisionErrorMetrics& measured,
    const ReceiptEvidence& evidence) {
    std::cout << fixture.name << ',' << fixture.mesh.positions.size() << ','
        << fixture.mesh.triangle_faces.size() << ',' << fixture.generation_iterations
        << ',' << generation.minimum_microseconds << ',' << generation.median_microseconds
        << ',' << generation.p95_microseconds << ',' << generation.mean_microseconds
        << ',' << fixture.metric_iterations << ',' << metrics.minimum_microseconds << ','
        << metrics.median_microseconds << ',' << metrics.p95_microseconds << ','
        << metrics.mean_microseconds << ','
        << carto::collision::primitive_collision_kind_name(selected.kind) << ','
        << std::setprecision(6) << selected.fit.aggregate() << ','
        << measured.one_sided_hausdorff << ',' << measured.volume_overlap_residual << ','
        << measured.max_penetration_depth << ',' << measured.clearance_violation << ','
        << carto::collision::collision_receipt_status_name(evidence.receipt.status) << ','
        << evidence.receipt.receipt_digest.hex() << ',' << evidence.promoted << ','
        << evidence.serialized_bytes << '\n';
    std::cout << std::fixed << std::setprecision(3);
}

} // namespace

int main() {
    try {
        std::cout << "# Cartographer collision kernel benchmark "
            "(docuseries 060 collision candidate benchmark and promotion gates)\n";
        std::cout << "# timing: host-clock CPU std::chrono::steady_clock, single run; "
            "loop counts in the generation_iterations and metric_iterations columns\n";
        std::cout << "# evidence tier: cpu_reference only; no device or GPU timing is "
            "claimed anywhere in this output\n";
        std::cout << "fixture,vertices,faces,generation_iterations,generation_min_us,"
            "generation_median_us,generation_p95_us,generation_mean_us,"
            "metric_iterations,metric_min_us,metric_median_us,metric_p95_us,"
            "metric_mean_us,selected_kind,fit_aggregate,hausdorff,overlap_residual,"
            "penetration,clearance_violation,receipt_status,receipt_digest,promoted,"
            "serialized_bytes\n";
        std::cout << std::fixed << std::setprecision(3);

        const std::vector<FixtureCase> fixtures = make_fixtures();
        for (const auto& fixture : fixtures) {
            const auto candidates = carto::collision::generate_primitive_candidates(
                fixture.mesh, carto::collision::PrimitiveFitOptions{});
            require(
                candidates && candidates.value().size() == 4U,
                "fixture generates every permitted primitive candidate");
            const auto& all = candidates.value();
            const auto selected = std::min_element(
                all.begin(), all.end(),
                [](const PrimitiveCollisionCandidate& left,
                   const PrimitiveCollisionCandidate& right) {
                    return left.fit.aggregate() < right.fit.aggregate();
                });
            const auto alternate = std::max_element(
                all.begin(), all.end(),
                [](const PrimitiveCollisionCandidate& left,
                   const PrimitiveCollisionCandidate& right) {
                    return left.fit.aggregate() < right.fit.aggregate();
                });
            require(
                static_cast<bool>(selected->validate()) &&
                    static_cast<bool>(alternate->validate()),
                "fitted candidates validate");

            const TimingSummary generation =
                time_loop(fixture.generation_iterations, [&]() -> std::uint64_t {
                    const auto run =
                        carto::collision::generate_primitive_candidates(
                            fixture.mesh, carto::collision::PrimitiveFitOptions{});
                    require(static_cast<bool>(run), "candidate generation failed");
                    return static_cast<std::uint64_t>(run.value().size());
                });

            const TimingSummary metric_timing =
                time_loop(fixture.metric_iterations, [&]() -> std::uint64_t {
                    const auto run = carto::collision::compute_pair_metrics(
                        *selected, *alternate, kRequiredClearance);
                    require(static_cast<bool>(run), "pair metric computation failed");
                    return static_cast<std::uint64_t>(
                        static_cast<bool>(run.value().validate()));
                });

            CollisionErrorMetrics measured = carto::collision::compute_pair_metrics(
                *selected, *alternate, kRequiredClearance).value();
            const auto proxy_samples = proxy_surface_samples(*selected, 256U);
            measured.one_sided_hausdorff = carto::collision::one_sided_hausdorff(
                std::span<const Vec3d>{fixture.mesh.positions}, proxy_samples);

            const ReceiptEvidence evidence =
                build_receipt(fixture, *selected, measured, generation.mean_microseconds);
            print_row(
                fixture, generation, metric_timing, *selected, measured, evidence);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
