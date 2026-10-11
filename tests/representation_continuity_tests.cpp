#include <carto/representation/continuity.hpp>
#include <carto/representation/error_envelope.hpp>
#include <carto/representation/family.hpp>
#include <carto/representation/runtime_evidence.hpp>
#include <carto/representation/selection.hpp>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace carto;
using representation::RepresentationErrorEnvelope;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

assets::Sha256Digest digest(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

RepresentationErrorEnvelope envelope(
    double position,
    double silhouette,
    double normal,
    double radiance) {
    return RepresentationErrorEnvelope{position, silhouette, normal, radiance};
}

representation::RepresentationVariant variant(
    std::uint64_t id,
    representation::RepresentationPurpose purpose,
    const RepresentationErrorEnvelope& error,
    std::uint64_t resident_bytes,
    std::uint64_t continuity) {
    const auto error_digest = error.canonical_digest();
    require(static_cast<bool>(error_digest), "fixture envelope digest binds");
    return representation::RepresentationVariant{
        representation::RepresentationVariantId{id},
        purpose,
        error_digest.value(),
        resident_bytes,
        representation::RepresentationContinuityId{continuity},
    };
}

representation::RepresentationFamily previous_family() {
    representation::RepresentationFamily family;
    family.set_id(representation::RepresentationFamilyId{30U});
    family.set_source(representation::AuthoredSourceIdentity{
        "docuseries/continuity-fixture",
        core::Revision{3U},
        digest("continuity-source-v3"),
    });
    require(static_cast<bool>(family.upsert_variant(variant(
                1U, representation::RepresentationPurpose::render,
                envelope(0.05, 0.08, 0.04, 0.12), 16U * 1024U * 1024U, 201U))),
        "previous render variant enters");
    require(static_cast<bool>(family.upsert_variant(variant(
                2U, representation::RepresentationPurpose::collision,
                envelope(0.10, 0.10, 0.20, 0.00), 2U * 1024U * 1024U, 202U))),
        "previous collision variant enters");
    return family;
}

// Simulated reimport: the authored source advanced, render was re-cooked as a
// new byte product under a new variant id, but the continuity identity the
// runtime world holds is preserved.
representation::RepresentationFamily reimported_family() {
    representation::RepresentationFamily family;
    family.set_id(representation::RepresentationFamilyId{30U});
    family.set_source(representation::AuthoredSourceIdentity{
        "docuseries/continuity-fixture",
        core::Revision{4U},
        digest("continuity-source-v4"),
    });
    require(static_cast<bool>(family.upsert_variant(variant(
                7U, representation::RepresentationPurpose::render,
                envelope(0.04, 0.07, 0.03, 0.10), 14U * 1024U * 1024U, 201U))),
        "reimported render variant keeps continuity 201");
    require(static_cast<bool>(family.upsert_variant(variant(
                2U, representation::RepresentationPurpose::collision,
                envelope(0.10, 0.10, 0.20, 0.00), 2U * 1024U * 1024U, 202U))),
        "untouched collision variant keeps continuity 202");
    return family;
}

void continuity_survives_variant_replacement() {
    const auto previous = previous_family();
    const auto reimported = reimported_family();
    require(static_cast<bool>(representation::verify_reimport_continuity(
                previous, reimported)),
        "reimport preserves every continuity identity");

    const auto previous_digest = previous.canonical_digest();
    const auto reimported_digest = reimported.canonical_digest();
    require(static_cast<bool>(previous_digest) &&
                static_cast<bool>(reimported_digest),
        "family digests compute on both sides");
    require(previous_digest.value() != reimported_digest.value(),
        "variant replacement changes the family digest");
    require(previous.id() == reimported.id(),
        "variant replacement never changes the stable family identity");
    require(reimported.find_continuity(representation::RepresentationContinuityId{201U})
                ->id.value == 7U,
        "continuity 201 follows the replacement byte product");
    require(reimported.find_variant(representation::RepresentationVariantId{2U})
                ->continuity == representation::RepresentationContinuityId{202U},
        "untouched variants keep their continuity identity");
}

assets::Sha256Digest previous_digest_value(
    const representation::RepresentationFamily& family) {
    const auto value = family.canonical_digest();
    require(static_cast<bool>(value), "fixture family digest computes");
    return value.value();
}

void reimport_binding_binds_stable_identity_and_revision() {
    const auto previous = previous_family();
    const auto reimported = reimported_family();
    const auto binding = representation::bind_reimport_continuity(
        previous, reimported, representation::RepresentationContinuityId{201U});
    require(static_cast<bool>(binding), "reimport continuity binds");
    require(binding.value().family == reimported.id(),
        "binding holds the stable family identity");
    require(binding.value().continuity.value == 201U,
        "binding holds the continuity identity, not the variant id");
    require(binding.value().source_revision == core::Revision{4U},
        "binding records the reimported source revision");
    require(binding.value().family_digest == reimported.canonical_digest().value(),
        "binding is digest-bound to the reimported family");

    const auto reparsed =
        representation::RepresentationContinuityBinding::deserialize(
            binding.value().serialize());
    require(static_cast<bool>(reparsed), "continuity binding round-trips");
    require(reparsed.value().family_digest == binding.value().family_digest,
        "binding digest survives the round trip");

    require(!representation::bind_reimport_continuity(
            previous, reimported,
            representation::RepresentationContinuityId{999U}),
        "unknown continuity identities are refused");
    require(!representation::bind_reimport_continuity(
            previous, reimported, representation::RepresentationContinuityId{0U}),
        "zero continuity identity is refused");

    const auto previous_binding = representation::bind_reimport_continuity(
        previous, previous, representation::RepresentationContinuityId{201U});
    require(static_cast<bool>(previous_binding),
        "rebinding over an unchanged family succeeds");
    require(previous_binding.value().family_digest == previous_digest_value(previous),
        "the historical binding keeps the pre-reimport family digest");
}

void reimport_drift_fails_closed() {
    const auto previous = previous_family();
    const auto reimported = reimported_family();

    auto identity_drift = reimported;
    identity_drift.set_id(representation::RepresentationFamilyId{31U});
    const auto identity_result =
        representation::verify_reimport_continuity(previous, identity_drift);
    require(!identity_result, "changed family identity is refused");
    require(identity_result.error().code == core::ErrorCode::stale_data,
        "family identity drift reports stale_data");

    auto namespace_drift = reimported;
    namespace_drift.set_source(representation::AuthoredSourceIdentity{
        "docuseries/renamed-fixture",
        core::Revision{4U},
        digest("continuity-source-v4"),
    });
    const auto namespace_result =
        representation::verify_reimport_continuity(previous, namespace_drift);
    require(!namespace_result, "changed source namespace is refused");
    require(namespace_result.error().code == core::ErrorCode::stale_data,
        "source namespace drift reports stale_data");

    auto regression = reimported;
    regression.set_source(representation::AuthoredSourceIdentity{
        "docuseries/continuity-fixture",
        core::Revision{2U},
        digest("continuity-source-v4"),
    });
    const auto regression_result =
        representation::verify_reimport_continuity(previous, regression);
    require(!regression_result, "source revision regression is refused");
    require(regression_result.error().code == core::ErrorCode::stale_data,
        "revision regression reports stale_data");

    auto silent_rewrite = reimported;
    silent_rewrite.set_source(representation::AuthoredSourceIdentity{
        "docuseries/continuity-fixture",
        core::Revision{3U},
        digest("continuity-source-v4"),
    });
    const auto rewrite_result =
        representation::verify_reimport_continuity(previous, silent_rewrite);
    require(!rewrite_result,
        "a changed source digest without revision advance is refused");
    require(rewrite_result.error().code == core::ErrorCode::stale_data,
        "silent source rewrite reports stale_data");

    auto dropped = reimported;
    require(static_cast<bool>(dropped.upsert_variant(variant(
                8U, representation::RepresentationPurpose::shadow,
                envelope(0.12, 0.15, 0.10, 0.25), 3U * 1024U * 1024U, 203U))),
        "additional variant enters the dropped-continuity fixture");
    dropped.set_source(representation::AuthoredSourceIdentity{
        "docuseries/continuity-fixture",
        core::Revision{5U},
        digest("continuity-source-v5"),
    });
    // Replace the render variant under a fresh continuity id: the old
    // continuity identity vanishes, which is a continuity break, not a
    // replacement.
    representation::RepresentationFamily without_render;
    without_render.set_id(dropped.id());
    without_render.set_source(dropped.source());
    for (const auto& entry : dropped.variants()) {
        if (entry.continuity != representation::RepresentationContinuityId{201U}) {
            require(static_cast<bool>(without_render.upsert_variant(entry)),
                "surviving variant carries over");
        }
    }
    const auto dropped_result =
        representation::verify_reimport_continuity(previous, without_render);
    require(!dropped_result, "dropped continuity identity is refused");
    require(dropped_result.error().code == core::ErrorCode::validation_failed,
        "continuity loss reports validation_failed");

    auto repurposed = reimported;
    require(static_cast<bool>(repurposed.upsert_variant(variant(
                7U, representation::RepresentationPurpose::sensor,
                envelope(0.04, 0.07, 0.03, 0.10), 14U * 1024U * 1024U, 201U))),
        "repurposed render continuity enters");
    const auto repurposed_result =
        representation::verify_reimport_continuity(previous, repurposed);
    require(!repurposed_result,
        "changing the purpose under a continuity identity is refused");
    require(repurposed_result.error().code == core::ErrorCode::validation_failed,
        "continuity purpose drift reports validation_failed");
}

void unchanged_reimport_keeps_continuity() {
    const auto previous = previous_family();
    require(static_cast<bool>(representation::verify_reimport_continuity(
                previous, previous)),
        "an unchanged family reimports with full continuity");
    const auto binding = representation::bind_reimport_continuity(
        previous, previous, representation::RepresentationContinuityId{202U});
    require(static_cast<bool>(binding), "unchanged reimport binds");
    require(binding.value().source_revision == core::Revision{3U},
        "unchanged reimport keeps the source revision");
}

void evidence_stays_bound_to_variant_across_replacement() {
    const auto previous = previous_family();
    const auto reimported = reimported_family();

    representation::RepresentationRuntimeEvidence legacy;
    legacy.family = previous.id();
    legacy.variant = representation::RepresentationVariantId{1U};
    legacy.hardware_profile = "hardware/p5200-class";
    legacy.profile_digest = digest("profile-p5200");
    legacy.source_revision = core::Revision{3U};
    legacy.status = representation::EvidenceStatus::admitted;
    legacy.measured_resident_bytes = 16U * 1024U * 1024U;
    legacy.observed_error = envelope(0.05, 0.08, 0.04, 0.12);
    require(static_cast<bool>(legacy.refresh_digest()), "legacy digest refreshes");

    representation::RepresentationRuntimeEvidence current;
    current.family = reimported.id();
    current.variant = representation::RepresentationVariantId{7U};
    current.hardware_profile = "hardware/p5200-class";
    current.profile_digest = digest("profile-p5200");
    current.source_revision = core::Revision{4U};
    current.status = representation::EvidenceStatus::admitted;
    current.measured_resident_bytes = 14U * 1024U * 1024U;
    current.observed_error = envelope(0.04, 0.07, 0.03, 0.10);
    require(static_cast<bool>(current.refresh_digest()), "current digest refreshes");

    representation::RepresentationEvidenceLedger ledger;
    require(static_cast<bool>(ledger.append(legacy)), "legacy evidence appends");
    require(static_cast<bool>(ledger.append(current)), "current evidence appends");

    const auto legacy_query = ledger.query(
        previous.id(), representation::RepresentationVariantId{1U},
        "hardware/p5200-class");
    require(legacy_query.value().has_value(),
        "legacy evidence remains queryable by its own variant identity");
    const auto current_query = ledger.query(
        reimported.id(), representation::RepresentationVariantId{7U},
        "hardware/p5200-class");
    require(current_query.value().has_value() &&
                current_query.value().value().measured_resident_bytes ==
                    14U * 1024U * 1024U,
        "replacement variant evidence is measured independently");

    // The ledger never projects evidence across variants: an unmeasured
    // variant identity carries no measurement even when the same family and
    // profile measured other variants.
    const auto replacement_query = ledger.query(
        reimported.id(), representation::RepresentationVariantId{8U},
        "hardware/p5200-class");
    require(static_cast<bool>(replacement_query) &&
                !replacement_query.value().has_value(),
        "evidence never falls back across variant identities");
}

void continuity_binding_hostile_inputs_fail_closed() {
    representation::RepresentationContinuityBinding binding;
    binding.family = representation::RepresentationFamilyId{30U};
    binding.continuity = representation::RepresentationContinuityId{201U};
    binding.source_revision = core::Revision{4U};
    binding.family_digest = digest("some-family");
    const std::string encoded = binding.serialize();
    require(static_cast<bool>(
                representation::RepresentationContinuityBinding::deserialize(encoded)),
        "binding round-trips");

    require(!representation::RepresentationContinuityBinding::deserialize(
                encoded + "junk"),
        "binding parser rejects trailing data");
    const auto future =
        representation::RepresentationContinuityBinding::deserialize(
            "CARTOGRAPHER_REPRESENTATION_CONTINUITY 99\n"
            "FAMILY 30\nCONTINUITY 201\nSOURCE_REVISION 4\nFAMILY_DIGEST " +
            digest("some-family").hex() + "\nEND\n");
    require(!future, "future binding versions fail precisely");
    require(future.error().code == core::ErrorCode::version_mismatch,
        "future binding version reports version_mismatch");
    require(!representation::RepresentationContinuityBinding::deserialize(
                "CARTOGRAPHER_REPRESENTATION_CONTINUITY 1\n"
                "FAMILY 30\nCONTINUITY 201\nSOURCE_REVISION 4\nFAMILY_DIGEST not-a-digest\nEND\n"),
        "malformed binding digests are rejected");
}

} // namespace

int main() {
    continuity_survives_variant_replacement();
    reimport_binding_binds_stable_identity_and_revision();
    reimport_drift_fails_closed();
    unchanged_reimport_keeps_continuity();
    evidence_stays_bound_to_variant_across_replacement();
    continuity_binding_hostile_inputs_fail_closed();
    std::cout << "representation continuity tests passed\n";
    return EXIT_SUCCESS;
}
