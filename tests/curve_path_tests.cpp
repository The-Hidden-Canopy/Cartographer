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

} // namespace

int main() {
    source_validation_and_round_trip();
    bounded_sampling_and_tangent_contract();
    std::cout << "curve path tests passed\n";
    return EXIT_SUCCESS;
}
