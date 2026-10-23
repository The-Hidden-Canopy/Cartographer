#include <carto/geometry/curve_path.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace carto;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

geometry::CurvePath line_path() {
    geometry::CurvePath path;
    path.source_identity = "road.main";
    path.source_revision = core::Revision{7U};
    path.segments.push_back({
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0},
        {2.0, 0.0, 0.0}, {3.0, 0.0, 0.0}});
    return path;
}

void source_validation_and_round_trip() {
    auto path = line_path();
    require(static_cast<bool>(path.validate()), "valid authored curve is accepted");
    const std::string encoded = path.serialize();
    const auto reopened = geometry::CurvePath::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "curve source serialization round-trips canonically");
    require(!geometry::CurvePath::deserialize(encoded + "unbound"),
        "curve source rejects trailing unbound data");
    require(!geometry::CurvePath::deserialize(
        "CARTOGRAPHER_CURVE_PATH 1\nIDENTITY \"path\" 1\nSEGMENTS 18446744073709551615\n"),
        "hostile segment count is rejected before allocation");
    require(!geometry::CurvePath::deserialize(
        "CARTOGRAPHER_CURVE_PATH 2\nIDENTITY \"path\" 1\nSEGMENTS 1\n"),
        "future curve source schema is rejected");

    path.source_identity = "../private";
    require(!path.validate(), "path-like source identities are rejected");
    path = line_path();
    path.source_revision = core::Revision{};
    require(!path.validate(), "zero source revision is rejected");
    path = line_path();
    path.segments.front().p1.x = std::numeric_limits<double>::infinity();
    require(!path.validate(), "non-finite control points are rejected");
}

void bounded_sampling_and_tangent_contract() {
    const auto path = line_path();
    const auto samples = path.sample_uniform(4U);
    require(samples && samples.value().size() == 5U,
        "uniform sampling returns the bounded expected sample count");
    require(samples.value().front().position.x == 0.0 &&
            samples.value().back().position.x == 3.0,
        "sampling preserves exact cubic endpoints");
    for (const auto& sample : samples.value()) {
        require(sample.tangent.finite() && std::abs(sample.tangent.length() - 1.0) < 1.0e-12,
            "sample tangents are finite and normalized");
        require(sample.tangent.x > 0.999999,
            "straight source has a stable forward tangent");
    }
    require(!path.sample_uniform(0U), "zero samples per segment is rejected");
    require(!path.sample_uniform(geometry::CurvePath::kMaxSamplesPerSegment + 1U),
        "oversized per-segment sampling is rejected");

    geometry::CurvePath stationary_endpoint;
    stationary_endpoint.source_identity = "road.stationary-endpoint";
    stationary_endpoint.source_revision = core::Revision{1U};
    stationary_endpoint.segments.push_back({
        {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0}, {1.0, 0.0, 0.0}});
    const auto stationary_samples = stationary_endpoint.sample_uniform(4U);
    require(stationary_samples && stationary_samples.value().front().tangent.x > 0.999999,
        "zero endpoint derivative uses a bounded one-sided tangent estimate");

    geometry::CurvePath joined = path;
    joined.source_identity = "road.joined";
    joined.segments.push_back({
        {3.0, 0.0, 0.0}, {3.0, 1.0, 0.0},
        {3.0, 2.0, 0.0}, {3.0, 3.0, 0.0}});
    const auto joined_samples = joined.sample_uniform(2U);
    require(joined_samples && joined_samples.value().size() == 6U,
        "segment boundaries retain one sample per authored segment");
    const auto& before_joint = joined_samples.value()[2U];
    const auto& after_joint = joined_samples.value()[3U];
    require(before_joint.position.x == after_joint.position.x &&
            before_joint.position.y == after_joint.position.y &&
            before_joint.segment_index == 0U && after_joint.segment_index == 1U &&
            before_joint.tangent.x > 0.999999 && after_joint.tangent.y > 0.999999,
        "tangent discontinuity is preserved at a shared source endpoint");

    geometry::CurvePath disconnected = joined;
    disconnected.segments[1U].p0.x += 0.01;
    require(!disconnected.validate(), "disconnected source segments are rejected");

    geometry::CurvePath budgeted;
    budgeted.source_identity = "road.sample-budget";
    budgeted.source_revision = core::Revision{1U};
    budgeted.segments.reserve(geometry::CurvePath::kMaxSegments);
    for (std::size_t index = 0U; index < geometry::CurvePath::kMaxSegments; ++index) {
        const double start = static_cast<double>(index);
        budgeted.segments.push_back({
            {start, 0.0, 0.0}, {start + 0.25, 0.0, 0.0},
            {start + 0.75, 0.0, 0.0}, {start + 1.0, 0.0, 0.0}});
    }
    require(static_cast<bool>(budgeted.validate()),
        "maximum bounded segment count remains valid");
    require(!budgeted.sample_uniform(16U),
        "aggregate samples across many segments cannot exceed the global budget");
}

geometry::CurvePath half_circle_path() {
    constexpr double kRadius = 10.0;
    constexpr double kHandle = kRadius * 0.55228474983079356;
    geometry::CurvePath path;
    path.source_identity = "road.half-circle";
    path.source_revision = core::Revision{3U};
    path.segments.push_back({
        {kRadius, 0.0, 0.0}, {kRadius, kHandle, 0.0},
        {kHandle, kRadius, 0.0}, {0.0, kRadius, 0.0}});
    path.segments.push_back({
        {0.0, kRadius, 0.0}, {-kHandle, kRadius, 0.0},
        {-kRadius, kHandle, 0.0}, {-kRadius, 0.0, 0.0}});
    return path;
}

void adaptive_arc_length_contract() {
    const auto path = half_circle_path();
    require(!path.sample_arc_length(0.0) &&
            !path.sample_arc_length(-1.0) &&
            !path.sample_arc_length(std::numeric_limits<double>::infinity()) &&
            !path.sample_arc_length(std::numeric_limits<double>::quiet_NaN()),
        "non-finite or non-positive arc-length spacing is rejected");
    geometry::CurvePath invalid_identity = path;
    invalid_identity.source_identity = "../private";
    require(!invalid_identity.sample_arc_length(0.5),
        "arc-length sampling validates its source before emission");
    require(!path.sample_arc_length(1.0e-9),
        "arc-length spacing below the bounded budget fails closed");

    constexpr double kTargetSpacing = 0.25;
    const auto samples = path.sample_arc_length(kTargetSpacing);
    require(static_cast<bool>(samples),
        "adaptive arc-length sampling succeeds on a curved path");
    const auto& values = samples.value();
    require(values.size() == 127U,
        "half-circle emits the deterministic bounded station count");
    require(values.front().position.x == 10.0 && values.front().position.y == 0.0 &&
            values.back().position.x == -10.0 && values.back().position.y == 0.0,
        "arc-length sampling preserves exact cubic endpoints");
    for (std::size_t index = 0U; index < values.size(); ++index) {
        const auto& sample = values[index];
        require(sample.tangent.finite() &&
                std::abs(sample.tangent.length() - 1.0) < 1.0e-12,
            "arc-length sample tangents are finite and normalized");
        if (index > 0U) {
            const auto& previous = values[index - 1U];
            require(sample.segment_index == previous.segment_index ||
                    sample.segment_index == previous.segment_index + 1U,
                "arc-length samples stay ordered by segment");
            if (sample.segment_index == previous.segment_index) {
                require(sample.parameter >= previous.parameter,
                    "arc-length sample parameters are non-decreasing per segment");
            }
            const double gap = (sample.position - previous.position).length();
            require(gap > 0.0, "shared segment endpoints are not duplicated");
            require(gap <= kTargetSpacing * 1.01,
                "consecutive arc-length spacing stays bounded by the request");
        }
    }

    const auto repeated = path.sample_arc_length(kTargetSpacing);
    require(repeated && repeated.value().size() == values.size(),
        "adaptive arc-length sampling is deterministic in count");
    for (std::size_t index = 0U; index < values.size(); ++index) {
        require(repeated.value()[index].position.x == values[index].position.x &&
                repeated.value()[index].position.y == values[index].position.y &&
                repeated.value()[index].parameter == values[index].parameter,
            "adaptive arc-length sampling is deterministic in output");
    }

    const auto straight = line_path().sample_arc_length(0.5);
    require(straight && straight.value().size() == 7U,
        "straight source emits exact stations within the point budget");
    for (std::size_t index = 0U; index < straight.value().size(); ++index) {
        require(std::abs(straight.value()[index].position.x -
                0.5 * static_cast<double>(index)) < 1.0e-12,
            "straight source stations land at exact arc-length offsets");
    }

    geometry::CurvePath budgeted;
    budgeted.source_identity = "road.arc-budget";
    budgeted.source_revision = core::Revision{1U};
    budgeted.segments.reserve(geometry::CurvePath::kMaxSegments);
    for (std::size_t index = 0U; index < geometry::CurvePath::kMaxSegments; ++index) {
        const double start = static_cast<double>(index);
        budgeted.segments.push_back({
            {start, 0.0, 0.0}, {start + 0.25, 0.0, 0.0},
            {start + 0.75, 0.0, 0.0}, {start + 1.0, 0.0, 0.0}});
    }
    const auto budgeted_samples = budgeted.sample_arc_length(1.0);
    require(budgeted_samples && budgeted_samples.value().size() ==
            geometry::CurvePath::kMaxSegments + 1U,
        "unit arc-length stations across many segments stay within the global budget");
}

} // namespace

int main() {
    source_validation_and_round_trip();
    bounded_sampling_and_tangent_contract();
    adaptive_arc_length_contract();
    std::cout << "curve path tests passed\n";
    return EXIT_SUCCESS;
}
