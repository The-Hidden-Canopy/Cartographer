#include <carto/assets/blob_store.hpp>
#include <carto/character/bindings.hpp>
#include <carto/core/result.hpp>
#include <carto/sim/cache.hpp>
#include <carto/sim/caps.hpp>
#include <carto/sim/collision_filter.hpp>
#include <carto/sim/groom.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

bool near(double left, double right, double epsilon) {
    return std::fabs(left - right) <= epsilon;
}

bool vec_near(const carto::core::Vec3d& left, const carto::core::Vec3d& right,
              double epsilon) {
    return near(left.x, right.x, epsilon) && near(left.y, right.y, epsilon) &&
        near(left.z, right.z, epsilon);
}

carto::character::GroomBinding groom_binding(std::size_t guide_count) {
    carto::character::GroomBinding binding;
    binding.id = carto::character::GroomBindingId{9U};
    binding.source_guides =
        carto::character::PinnedSourceRef{2U, carto::core::Revision{1U},
                                          digest("groom-source-guides")};
    binding.attachments.push_back(
        {carto::kinematic::JointId{20U}, carto::character::ControlId{0U}});
    for (std::size_t index = 0U; index < guide_count; ++index) {
        binding.guides.push_back(
            carto::character::GuideRef{static_cast<std::uint64_t>(index + 1U), 0U});
    }
    binding.material_intent_ref = digest("groom-material-intent");
    binding.lod_mapping[0U] = digest("groom-lod-0");
    return binding;
}

// 3x3 grid in the XY plane, top-row corners pinned to joints 10 and 11.
carto::sim::GroomBakeRequest still_groom_request() {
    carto::sim::GroomBakeRequest request;
    request.binding = groom_binding(4U);
    request.binding_digest = digest("still-groom-binding-record");
    for (std::uint32_t guide = 0U; guide < 4U; ++guide) {
        carto::sim::GuideSpec spec;
        for (std::uint32_t point = 0U; point < 6U; ++point) {
            spec.rest_offsets.push_back(
                carto::core::Vec3d{0.0, 0.02 * static_cast<double>(point), 0.0});
        }
        request.guides.push_back(spec);
    }
    for (std::int64_t tick = 0; tick < 121; ++tick) {
        carto::sim::RootMotionSample sample;
        sample.time = carto::character::RationalTime{tick, 24U};
        request.root_motion.push_back(sample);
    }
    request.spring.stiffness = 5.0;
    request.spring.damping = 0.1;
    request.spring.mass_per_point_kg = 1.0e-3;
    request.spring.substeps = 4U;
    return request;
}

void groom_springs_settle_without_root_motion() {
    using namespace carto;
    sim::GroomBakeRequest request = still_groom_request();
    auto first = sim::bake_groom(request);
    require(static_cast<bool>(first), "still groom bake succeeds");
    auto second = sim::bake_groom(request);
    require(static_cast<bool>(second), "repeated still groom bake succeeds");

    const sim::GroomGuideCache& cache = first.value().cache;
    const sim::GroomSimReceipt& receipt = first.value().receipt;
    require(receipt.backend_identity == std::string{sim::kGroomSpringBackendIdentity},
            "groom receipt declares the honest CPU reference backend tier");
    require(receipt.settle_residual < 1.0e-3,
            "guides settle: final frame displacement is negligible");
    require(cache.positions.size() == 4U * 6U * 121U,
            "groom cache holds every guide point per frame");
    const std::size_t guide_stride = 4U * 6U;
    const std::size_t final_frame = 120U * guide_stride;
    require(vec_near(cache.positions[0U], core::Vec3d{0.0, 0.0, 0.0}, 1.0e-12) &&
                vec_near(cache.positions[final_frame], core::Vec3d{0.0, 0.0, 0.0},
                         1.0e-12),
            "root points track the motionless root across frames");
    const double sag =
        request.guides[0U].rest_offsets[5U].y - cache.positions[final_frame + 5U].y;
    require(sag > 1.0e-6, "free guide points respond to gravity");
    auto first_digest = cache.canonical_digest();
    auto second_digest = second.value().cache.canonical_digest();
    require(static_cast<bool>(first_digest) && static_cast<bool>(second_digest),
            "groom cache digests compute");
    require(first_digest.value() == second_digest.value(),
            "groom cache digest is deterministic across runs");
}

void groom_cache_promotion_binds_digests_and_stays_stable() {
    using namespace carto;
    auto bake = sim::bake_groom(still_groom_request());
    require(static_cast<bool>(bake), "groom promotion bake succeeds");
    auto promotion = sim::evaluate_groom_cache_promotion(
        bake.value().cache, bake.value().receipt);
    require(static_cast<bool>(promotion), "groom promotion evaluates");
    require(promotion.value().policy == sim::CachePromotionPolicy::promote,
            "settled groom bake promotes");
    auto repeated = sim::evaluate_groom_cache_promotion(
        bake.value().cache, bake.value().receipt);
    require(static_cast<bool>(repeated), "repeated groom promotion evaluates");
    const auto first_digest = promotion.value().canonical_digest();
    const auto second_digest = repeated.value().canonical_digest();
    require(static_cast<bool>(first_digest) && static_cast<bool>(second_digest) &&
            first_digest.value() == second_digest.value(),
            "groom promotion record remains deterministic");
    auto tampered_cache = bake.value().cache;
    tampered_cache.positions.front().x += 0.01;
    auto rejected = sim::evaluate_groom_cache_promotion(
        tampered_cache, bake.value().receipt);
    require(static_cast<bool>(rejected) &&
            rejected.value().policy == sim::CachePromotionPolicy::reject,
            "tampered groom cache bytes are never promoted");

    sim::CachePromotionInput incomplete;
    incomplete.cache_digest = digest("groom-cache");
    incomplete.source_binding_digest = digest("groom-source");
    incomplete.receipt_digest = digest("groom-receipt");
    incomplete.steps_executed = 12U;
    incomplete.steps_recorded = 48U;
    incomplete.final_residual = 1.0e-6;
    const auto incomplete_result = sim::evaluate_cache_promotion(incomplete);
    require(static_cast<bool>(incomplete_result) &&
            incomplete_result.value().policy == sim::CachePromotionPolicy::reject,
            "incomplete cache evidence fails closed");
}

void hostile_groom_inputs_fail_closed() {
    using namespace carto;
    {
        sim::GroomBakeRequest request = still_groom_request();
        request.root_motion[2U].time = character::RationalTime{1U, 24U};
        require(!sim::bake_groom(request), "non-monotonic root motion is refused");
    }
    {
        sim::GroomBakeRequest request = still_groom_request();
        request.root_motion[2U].time = character::RationalTime{4U, 24U};
        require(!sim::bake_groom(request), "non-uniform root motion ticks are refused");
    }
    {
        sim::GroomBakeRequest request = still_groom_request();
        request.guides[1U].rest_offsets[2U] = core::Vec3d{
            std::numeric_limits<double>::infinity(), 0.0, 0.0};
        require(!sim::bake_groom(request), "non-finite guide offsets are refused");
    }
    {
        auto bake = sim::bake_groom(still_groom_request());
        require(static_cast<bool>(bake), "dimension bake succeeds");
        auto cache = bake.value().cache;
        cache.positions.pop_back();
        require(!cache.validate(), "cache with missing position fails validation");
    }
}

} // namespace

int main() {
    groom_springs_settle_without_root_motion();
    groom_cache_promotion_binds_digests_and_stays_stable();
    hostile_groom_inputs_fail_closed();
    std::cout << "sim tests passed\n";
    return EXIT_SUCCESS;
}
