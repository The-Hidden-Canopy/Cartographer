#include <carto/assets/blob_store.hpp>
#include <carto/core/revision.hpp>
#include <carto/production/capability_profile.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

using namespace carto;

production::DestinationCapabilityProfile make_profile() {
    production::DestinationCapabilityProfile profile;
    profile.identity = "consumer.destination.handheld.v1";
    profile.profile_revision = core::Revision{2U};
    profile.capabilities.push_back(production::DestinationCapability{
        "destination.artifact.material-package",
        production::PreparedCapabilityEvidence::native_realization,
        "native material graph realized on device"});
    profile.capabilities.push_back(production::DestinationCapability{
        "destination.artifact.render-mesh",
        production::PreparedCapabilityEvidence::runtime_validated,
        "render mesh path validated on device"});
    profile.capabilities.push_back(production::DestinationCapability{
        "destination.artifact.residency-page",
        production::PreparedCapabilityEvidence::derived_product,
        "residency pages rebuilt from prepared sources"});
    require(static_cast<bool>(profile.validate()),
        "fixture destination capability profile validates");
    return profile;
}

void profile_round_trips_with_stable_digest() {
    const auto profile = make_profile();
    const std::string encoded = profile.serialize();
    const auto reopened = production::DestinationCapabilityProfile::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "destination capability profile survives deterministic round trip");
    const auto first = profile.canonical_digest();
    const auto second = reopened.value().canonical_digest();
    require(first && second && first.value() == second.value(),
        "destination capability profile identity survives reopen");

    auto rebuilt = make_profile();
    require(rebuilt.canonical_digest() && first &&
            rebuilt.canonical_digest().value() == first.value(),
        "identically constructed profiles share one digest");
    rebuilt.profile_revision = core::Revision{3U};
    const auto changed = rebuilt.canonical_digest();
    require(changed && first && changed.value() != first.value(),
        "revision drift changes the profile digest");

    require(profile.find("destination.artifact.render-mesh") != nullptr &&
            profile.find("destination.artifact.publication-package") == nullptr,
        "lookup distinguishes offered features from absent ones");
}

void capability_checks_carry_explicit_evidence() {
    const auto profile = make_profile();
    const std::vector<production::RequestedCapability> requested{
        {"destination.artifact.render-mesh", true},
        {"destination.artifact.publication-package", true},
        {"destination.artifact.evidence-bundle", false},
    };
    const auto results = production::check_destination_capabilities(profile, requested);
    require(results && results.value().size() == requested.size(),
        "every requested feature receives exactly one result");
    require(results.value()[0].evidence ==
            production::PreparedCapabilityEvidence::runtime_validated &&
        results.value()[0].required &&
        static_cast<bool>(results.value()[0].validate()),
        "supported required features reuse the destination evidence");
    require(results.value()[1].evidence ==
            production::PreparedCapabilityEvidence::unsupported &&
        !results.value()[1].required &&
        static_cast<bool>(results.value()[1].validate()) &&
        results.value()[1].detail.find("consumer.destination.handheld.v1") != std::string::npos,
        "unsupported required features carry explicit unsupported evidence");
    require(results.value()[2].evidence ==
            production::PreparedCapabilityEvidence::unsupported &&
        !results.value()[2].required,
        "unsupported optional features carry explicit unsupported evidence");

    require(!production::check_destination_capabilities(profile,
                std::vector<production::RequestedCapability>{{"bad feature!", true}}),
        "unsafe requested features fail closed");
    production::DestinationCapabilityProfile invalid;
    invalid.identity = "consumer.destination.handheld.v1";
    invalid.profile_revision = core::Revision{1U};
    invalid.capabilities.push_back(production::DestinationCapability{
        "destination.artifact.render-mesh",
        production::PreparedCapabilityEvidence::unsupported, "must not appear"});
    require(!invalid.validate(),
        "profiles never carry unsupported evidence; absence is the negative answer");
    require(!production::check_destination_capabilities(invalid, requested),
        "invalid profiles fail closed before any check runs");
}

void hostile_profiles_fail_closed() {
    const auto profile = make_profile();
    const std::string encoded = profile.serialize();

    std::string future = encoded;
    future.replace(0U,
        std::string{"CARTOGRAPHER_DESTINATION_CAPABILITIES 1"}.size(),
        "CARTOGRAPHER_DESTINATION_CAPABILITIES 99");
    require(!production::DestinationCapabilityProfile::deserialize(future),
        "future profile versions fail precisely");
    require(!production::DestinationCapabilityProfile::deserialize(encoded + "trailing"),
        "profile parser rejects trailing data");
    require(!production::DestinationCapabilityProfile::deserialize(
                "CARTOGRAPHER_DESTINATION_CAPABILITIES 1\n"
                "PROFILE \"consumer.destination.handheld.v1\" 2\n"
                "CAPABILITIES 4097\n"),
        "hostile capability counts are rejected before allocation");

    auto duplicate = profile;
    duplicate.capabilities.push_back(duplicate.capabilities.front());
    require(!duplicate.validate(),
        "duplicate capability features fail closed");
    auto unsorted = profile;
    std::swap(unsorted.capabilities.front(), unsorted.capabilities.back());
    require(!unsorted.validate(),
        "unsorted capability features fail closed");
    auto unsafe = profile;
    unsafe.capabilities.front().feature = "not a safe feature!";
    require(!unsafe.validate(),
        "unsafe capability features fail closed");
}

} // namespace

int main() {
    profile_round_trips_with_stable_digest();
    capability_checks_carry_explicit_evidence();
    hostile_profiles_fail_closed();
    std::cout << "production capability profile tests passed\n";
    return EXIT_SUCCESS;
}
