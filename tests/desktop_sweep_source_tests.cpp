#include "studio_sweep_source.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {

#define REQUIRE(condition) \
    do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

void exact_update_preserves_owner() {
    carto::geometry::SweepSource existing;
    existing.source_identity = "sweep.bound";
    existing.source_revision = carto::core::Revision{4U};
    existing.owner_object_id = 88U;

    carto::geometry::SweepSource candidate;
    candidate.source_identity = existing.source_identity;
    candidate.source_revision = carto::core::Revision{5U};
    const auto retained = carto::desktop::preserve_exact_sweep_source_owner(
        candidate, existing, true);
    REQUIRE(retained);
    REQUIRE(candidate.owner_object_id == 88U);
}

void ownerless_update_stays_ownerless() {
    carto::geometry::SweepSource existing;
    existing.source_identity = "sweep.world";
    existing.source_revision = carto::core::Revision{1U};

    carto::geometry::SweepSource candidate;
    candidate.source_identity = existing.source_identity;
    candidate.source_revision = carto::core::Revision{2U};
    const auto retained = carto::desktop::preserve_exact_sweep_source_owner(
        candidate, existing, true);
    REQUIRE(retained);
    REQUIRE(candidate.owner_object_id == 0U);
}

void stale_or_different_source_refuses_without_rebinding() {
    carto::geometry::SweepSource existing;
    existing.source_identity = "sweep.bound";
    existing.source_revision = carto::core::Revision{4U};
    existing.owner_object_id = 88U;

    carto::geometry::SweepSource candidate;
    candidate.source_identity = "sweep.other";
    candidate.source_revision = carto::core::Revision{1U};
    candidate.owner_object_id = 17U;
    const auto changed_identity = carto::desktop::preserve_exact_sweep_source_owner(
        candidate, existing, true);
    REQUIRE(!changed_identity);
    REQUIRE(candidate.owner_object_id == 17U);

    candidate.source_identity = existing.source_identity;
    candidate.source_revision = existing.source_revision;
    const auto stale_load = carto::desktop::preserve_exact_sweep_source_owner(
        candidate, existing, false);
    REQUIRE(!stale_load);
    REQUIRE(candidate.owner_object_id == 17U);
}

void new_source_can_bind_only_a_current_owner() {
    carto::geometry::SweepSource candidate;
    candidate.source_identity = "sweep.local";
    candidate.source_revision = carto::core::Revision{1U};

    const auto bound = carto::desktop::bind_new_sweep_source_owner(
        candidate, 42U, true, true, 7U, 7U);
    REQUIRE(bound);
    REQUIRE(candidate.owner_object_id == 42U);

    carto::geometry::SweepSource project_frame_candidate;
    project_frame_candidate.source_identity = "sweep.project";
    project_frame_candidate.source_revision = carto::core::Revision{1U};
    const auto ownerless = carto::desktop::bind_new_sweep_source_owner(
        project_frame_candidate, 0U, true, false, 0U, 1U);
    REQUIRE(ownerless);
    REQUIRE(project_frame_candidate.owner_object_id == 0U);

    carto::geometry::SweepSource stale_owner_candidate;
    stale_owner_candidate.source_identity = "sweep.stale-owner";
    stale_owner_candidate.source_revision = carto::core::Revision{1U};
    const auto stale_owner = carto::desktop::bind_new_sweep_source_owner(
        stale_owner_candidate, 99U, true, false, 7U, 7U);
    REQUIRE(!stale_owner);
    REQUIRE(stale_owner_candidate.owner_object_id == 0U);

    carto::geometry::SweepSource switched_project_candidate;
    switched_project_candidate.source_identity = "sweep.switched-project";
    switched_project_candidate.source_revision = carto::core::Revision{1U};
    const auto stale_generation = carto::desktop::bind_new_sweep_source_owner(
        switched_project_candidate, 42U, true, true, 7U, 8U);
    REQUIRE(!stale_generation);
    REQUIRE(switched_project_candidate.owner_object_id == 0U);

    const auto attempted_rebind = carto::desktop::bind_new_sweep_source_owner(
        candidate, 99U, false, true, 7U, 7U);
    REQUIRE(!attempted_rebind);
    REQUIRE(candidate.owner_object_id == 42U);
}

} // namespace

int main() {
    try {
        exact_update_preserves_owner();
        ownerless_update_stays_ownerless();
        stale_or_different_source_refuses_without_rebinding();
        new_source_can_bind_only_a_current_owner();
        std::cout << "desktop sweep source regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "desktop sweep source regression failed: " << error.what() << '\n';
        return 1;
    }
}
