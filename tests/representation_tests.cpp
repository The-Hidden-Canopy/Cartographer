#include <carto/representation/continuity.hpp>
#include <carto/representation/error_envelope.hpp>
#include <carto/representation/family.hpp>
#include <carto/representation/runtime_evidence.hpp>
#include <carto/representation/selection.hpp>
#include <carto/web_geometry/types.hpp>

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

representation::RepresentationFamily fixture_family() {
    representation::RepresentationFamily family;
    family.set_id(representation::RepresentationFamilyId{10U});
    family.set_source(representation::AuthoredSourceIdentity{
        "docuseries/federation-fixture",
        core::Revision{3U},
        digest("source-v3"),
    });
    require(static_cast<bool>(family.upsert_variant(variant(
                1U, representation::RepresentationPurpose::render,
                envelope(0.01, 0.02, 0.01, 0.05), 48U * 1024U * 1024U, 101U))),
        "render-full enters the family");
    require(static_cast<bool>(family.upsert_variant(variant(
                2U, representation::RepresentationPurpose::render,
                envelope(0.05, 0.08, 0.04, 0.12), 16U * 1024U * 1024U, 102U))),
        "render-mid enters the family");
    require(static_cast<bool>(family.upsert_variant(variant(
                3U, representation::RepresentationPurpose::render,
                envelope(0.20, 0.30, 0.15, 0.40), 4U * 1024U * 1024U, 103U))),
        "render-low enters the family");
    require(static_cast<bool>(family.upsert_variant(variant(
                4U, representation::RepresentationPurpose::collision,
                envelope(0.10, 0.10, 0.20, 0.00), 2U * 1024U * 1024U, 104U))),
        "collision hull enters the family");
    require(static_cast<bool>(family.upsert_variant(variant(
                5U, representation::RepresentationPurpose::navigation,
                envelope(0.15, 0.10, 0.10, 0.00), 1U * 1024U * 1024U, 105U))),
        "navigation mesh enters the family");
    require(static_cast<bool>(family.upsert_variant(variant(
                6U, representation::RepresentationPurpose::sensor,
                envelope(0.05, 0.05, 0.05, 0.10), 6U * 1024U * 1024U, 106U))),
        "sensor proxy enters the family");
    require(static_cast<bool>(family.upsert_variant(variant(
                7U, representation::RepresentationPurpose::shadow,
                envelope(0.12, 0.15, 0.10, 0.25), 3U * 1024U * 1024U, 107U))),
        "shadow proxy enters the family");
    return family;
}

representation::HardwareProfileCapability p5200_profile() {
    representation::HardwareProfileCapability capability;
    capability.profile_identity = "hardware/p5200-class";
    capability.profile_digest = digest("profile-p5200");
    capability.supported_purposes = {
        representation::RepresentationPurpose::render,
        representation::RepresentationPurpose::collision,
        representation::RepresentationPurpose::navigation,
        representation::RepresentationPurpose::shadow,
    };
    capability.resident_budget_bytes = 20U * 1024U * 1024U;
    return capability;
}

representation::HardwareProfileCapability modern_profile() {
    representation::HardwareProfileCapability capability;
    capability.profile_identity = "hardware/modern";
    capability.profile_digest = digest("profile-modern");
    capability.supported_purposes = {
        representation::RepresentationPurpose::render,
        representation::RepresentationPurpose::collision,
        representation::RepresentationPurpose::navigation,
        representation::RepresentationPurpose::sensor,
        representation::RepresentationPurpose::shadow,
    };
    capability.resident_budget_bytes = 256U * 1024U * 1024U;
    return capability;
}

std::vector<representation::RepresentationVariantEnvelope> fixture_envelopes() {
    return {
        {representation::RepresentationVariantId{1U}, envelope(0.01, 0.02, 0.01, 0.05)},
        {representation::RepresentationVariantId{2U}, envelope(0.05, 0.08, 0.04, 0.12)},
        {representation::RepresentationVariantId{3U}, envelope(0.20, 0.30, 0.15, 0.40)},
        {representation::RepresentationVariantId{4U}, envelope(0.10, 0.10, 0.20, 0.00)},
        {representation::RepresentationVariantId{5U}, envelope(0.15, 0.10, 0.10, 0.00)},
        {representation::RepresentationVariantId{6U}, envelope(0.05, 0.05, 0.05, 0.10)},
        {representation::RepresentationVariantId{7U}, envelope(0.12, 0.15, 0.10, 0.25)},
    };
}

representation::RepresentationCapabilityRequirement requirement(
    representation::RepresentationPurpose purpose,
    double position,
    double silhouette,
    double normal,
    double radiance) {
    representation::RepresentationCapabilityRequirement value;
    value.purpose = purpose;
    value.max_position_error = position;
    value.max_silhouette_error = silhouette;
    value.max_normal_error = normal;
    value.max_radiance_error = radiance;
    return value;
}

void family_records_bind_one_authored_source() {
    const auto family = fixture_family();
    require(static_cast<bool>(family.validate()), "fixture family validates");
    require(family.variants().size() == 7U, "fixture family owns seven variants");

    auto exhausted_source = family;
    exhausted_source.set_source(representation::AuthoredSourceIdentity{
        "docuseries/federation-fixture",
        core::Revision{core::Revision::max_value()},
        digest("source-v3"),
    });
    require(!exhausted_source.validate(),
        "an exhausted source revision fails closed");

    auto zero_digest_source = family;
    zero_digest_source.set_source(representation::AuthoredSourceIdentity{
        "docuseries/federation-fixture",
        core::Revision{3U},
        assets::Sha256Digest{},
    });
    require(!zero_digest_source.validate(),
        "unbound source digest fails closed");
}

void family_serialization_round_trip_and_digest_stability() {
    const auto family = fixture_family();
    const auto first_digest = family.canonical_digest();
    require(static_cast<bool>(first_digest), "family digest computes");
    const auto reparsed = representation::RepresentationFamily::deserialize(
        family.serialize());
    require(static_cast<bool>(reparsed), "family serialization round-trips");
    const auto second_digest = reparsed.value().canonical_digest();
    require(static_cast<bool>(second_digest), "reparsed family digest computes");
    require(first_digest.value() == second_digest.value(),
        "family digest is stable across a text round trip");
}

void family_replacement_preserves_continuity() {
    auto family = fixture_family();
    const auto replacement = variant(
        2U, representation::RepresentationPurpose::render,
        envelope(0.04, 0.07, 0.03, 0.10), 12U * 1024U * 1024U, 102U);
    require(static_cast<bool>(family.upsert_variant(replacement)),
        "variant replacement preserving continuity succeeds");
    require(family.find_variant(representation::RepresentationVariantId{2U})
                ->resident_bytes == 12U * 1024U * 1024U,
        "replacement byte product is resident");

    const auto continuity_drift = variant(
        2U, representation::RepresentationPurpose::render,
        envelope(0.04, 0.07, 0.03, 0.10), 12U * 1024U * 1024U, 999U);
    require(!family.upsert_variant(continuity_drift),
        "replacement with a different continuity identity is rejected");

    const auto duplicate_continuity = variant(
        9U, representation::RepresentationPurpose::shadow,
        envelope(0.12, 0.15, 0.10, 0.25), 1U, 101U);
    require(!family.upsert_variant(duplicate_continuity),
        "duplicate continuity identity across variants is rejected");
}

void family_hostile_inputs_fail_closed() {
    const auto family = fixture_family();
    const std::string encoded = family.serialize();
    require(!representation::RepresentationFamily::deserialize(encoded + "junk"),
        "family parser rejects trailing data");

    std::string future = encoded;
    future.replace(
        0U, std::string{"CARTOGRAPHER_REPRESENTATION_FAMILY 1"}.size(),
        "CARTOGRAPHER_REPRESENTATION_FAMILY 99");
    const auto future_result =
        representation::RepresentationFamily::deserialize(future);
    require(!future_result, "unknown future family versions fail precisely");
    require(future_result.error().code == core::ErrorCode::version_mismatch,
        "future family version reports version_mismatch");

    require(!representation::RepresentationFamily::deserialize(
                "CARTOGRAPHER_REPRESENTATION_FAMILY 1\n"
                "FAMILY 10\n"
                "SOURCE \"docuseries/federation-fixture\" 3 "
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "VARIANTS 300\n"),
        "hostile variant counts are rejected before allocation");

    const std::string duplicate_continuity =
        encoded.substr(0U, encoded.size() - 4U) +
        "VARIANT 9 shadow "
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 1024 101\n"
        "END\n";
    require(!representation::RepresentationFamily::deserialize(duplicate_continuity),
        "injected duplicate continuity identity is rejected");

    require(!representation::RepresentationFamily::deserialize(
                "CARTOGRAPHER_REPRESENTATION_FAMILY 1\n"
                "FAMILY 10\n"
                "SOURCE \"docuseries/federation-fixture\" 3 not-a-digest\n"
                "VARIANTS 0\nEND\n"),
        "malformed source digest is rejected");

    std::string unknown_purpose = encoded;
    const auto purpose_pos = unknown_purpose.find(" render ");
    require(purpose_pos != std::string::npos, "fixture contains a render variant");
    unknown_purpose.replace(purpose_pos + 1U, 6U, "hologram");
    require(!representation::RepresentationFamily::deserialize(unknown_purpose),
        "unknown purpose names are rejected");
}

void family_records_carry_no_runtime_state() {
    const auto family = fixture_family();
    // Negative space: the serialized record is the whole truth. It round-trips
    // through text alone, every cross-record reference is a digest, and no
    // runtime handle, path, or mutable state is needed or representable.
    const std::string text = family.serialize();
    require(text.find('\\') == std::string::npos,
        "family serialization contains no path or handle escape");
    const auto reparsed =
        representation::RepresentationFamily::deserialize(text);
    require(static_cast<bool>(reparsed),
        "family record reconstructs from text alone");
    for (const auto& entry : reparsed.value().variants()) {
        require(!entry.error_envelope_digest.is_zero(),
            "variant error envelope reference is always digest-bound");
        require(static_cast<bool>(entry.continuity),
            "variant continuity identity is always explicit");
    }
}

void error_envelope_surface_mapping_and_cluster_upgrade() {
    const auto neutral = envelope(0.10, 0.20, 0.30, 0.40);
    surface::SurfaceErrorEnvelope surface{};
    surface.max_position_error = 0.10;
    surface.max_silhouette_error = 0.20;
    surface.max_normal_error = 0.30;
    surface.max_radiance_error = 0.40;
    surface.diffuse_error = 0.11;
    surface.specular_error = 0.12;

    const auto lifted = representation::from_surface_envelope(surface);
    require(lifted == neutral, "surface envelope lifts to the neutral record");
    const auto lowered = representation::to_surface_envelope(lifted);
    require(lowered.max_position_error == 0.10 && lowered.max_silhouette_error == 0.20 &&
                lowered.max_normal_error == 0.30 &&
                lowered.max_radiance_error == 0.40,
        "neutral envelope lowers back onto the surface axes");

    web_geometry::ClusterRecord cluster;
    cluster.id = 7U;
    cluster.geometric_error = 0.5;
    const auto cluster_envelope = representation::envelope_for_cluster(cluster);
    require(cluster_envelope.max_position_error == 0.5 &&
                cluster_envelope.max_radiance_error == 0.0,
        "scalar cluster error lifts onto the geometric axes only");

    require(!representation::attach_cluster_error(cluster, envelope(0.3, 0.3, 0.3, 0.1)),
        "attach refuses to tighten the cluster bound without new evidence");
    require(static_cast<bool>(
                representation::attach_cluster_error(cluster, envelope(0.7, 0.6, 0.8, 0.1))),
        "attach upgrades the scalar to the envelope-derived bound");
    require(cluster.geometric_error == 0.8,
        "cluster scalar becomes the maximum geometric envelope axis");

    const std::string encoded = neutral.serialize();
    const auto reparsed = RepresentationErrorEnvelope::deserialize(encoded);
    require(static_cast<bool>(reparsed), "envelope serialization round-trips");
    require(reparsed.value() == neutral, "envelope round trip preserves values");
}

void error_envelope_hostile_inputs_fail_closed() {
    const auto neutral = envelope(0.10, 0.20, 0.30, 0.40);
    const std::string encoded = neutral.serialize();
    require(!RepresentationErrorEnvelope::deserialize(encoded + "junk"),
        "envelope parser rejects trailing data");
    require(!RepresentationErrorEnvelope::deserialize(
                "CARTOGRAPHER_REPRESENTATION_ERROR_ENVELOPE 99\n"
                "POSITION 0.1\nSILHOUETTE 0.2\nNORMAL 0.3\nRADIANCE 0.4\nEND\n"),
        "future envelope versions fail precisely");
    require(!RepresentationErrorEnvelope::deserialize(
                "CARTOGRAPHER_REPRESENTATION_ERROR_ENVELOPE 1\n"
                "POSITION -0.1\nSILHOUETTE 0.2\nNORMAL 0.3\nRADIANCE 0.4\nEND\n"),
        "negative error bounds fail validation");
    require(!RepresentationErrorEnvelope::deserialize(
                "CARTOGRAPHER_REPRESENTATION_ERROR_ENVELOPE 1\n"
                "POSITION nan\nSILHOUETTE 0.2\nNORMAL 0.3\nRADIANCE 0.4\nEND\n"),
        "non-finite error bounds fail validation");
    require(!RepresentationErrorEnvelope::deserialize(
                "CARTOGRAPHER_REPRESENTATION_ERROR_ENVELOPE 1\n"
                "POSITION 0x1p3\nSILHOUETTE 0.2\nNORMAL 0.3\nRADIANCE 0.4\nEND\n"),
        "non-canonical number spellings are rejected");
}

void certification_binds_variant_to_selection_evidence() {
    const auto family = fixture_family();
    representation::RepresentationCertification certification;
    certification.family = family.id();
    certification.variant = representation::RepresentationVariantId{2U};
    certification.certified_source_revision = core::Revision{3U};
    certification.selection_evidence_digest = digest("selection-evidence-v2");
    require(static_cast<bool>(certification.refresh_digest()),
        "certification digest refreshes");
    require(static_cast<bool>(certification.validate()),
        "certification validates against its content");
    const auto reparsed =
        representation::RepresentationCertification::deserialize(certification.serialize());
    require(static_cast<bool>(reparsed), "certification serialization round-trips");
    require(reparsed.value().digest == certification.digest,
        "certification digest survives the round trip");

    auto tampered = certification;
    tampered.variant = representation::RepresentationVariantId{3U};
    require(!tampered.validate(),
        "tampered certification content fails digest verification");
}

void certification_hostile_inputs_fail_closed() {
    const std::string encoded =
        "CARTOGRAPHER_REPRESENTATION_CERTIFICATION 1\n"
        "FAMILY 10\n"
        "VARIANT 2\n"
        "SOURCE_REVISION 3\n"
        "SELECTION_EVIDENCE "
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "ALGORITHM \"cartographer-representation-certification/0.1\"\n"
        "DIGEST 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "END\n";
    require(!representation::RepresentationCertification::deserialize(encoded),
        "certification with a content/digest mismatch fails closed");
    require(!representation::RepresentationCertification::deserialize(
                "CARTOGRAPHER_REPRESENTATION_CERTIFICATION 99\n"),
        "future certification versions fail precisely");
    require(!representation::RepresentationCertification::deserialize(encoded + "junk"),
        "certification parser rejects trailing data");
}

void selection_is_deterministic_across_profiles() {
    const auto family = fixture_family();
    const auto envelopes = fixture_envelopes();
    const auto p5200 = p5200_profile();
    const auto modern = modern_profile();

    // P5200-class: 20 MiB budget and a mid error budget can only admit the
    // mid variant; the full variant exceeds residency and the low variant is
    // too loose on every axis.
    const auto legacy = representation::select_variant(
        family, p5200,
        requirement(representation::RepresentationPurpose::render, 0.06, 0.09, 0.05, 0.13),
        {}, std::span(envelopes));
    require(static_cast<bool>(legacy), "P5200-class selection succeeds");
    require(legacy.value().selected_variant.value == 2U,
        "P5200-class profile selects the cheapest satisfying variant");
    require(legacy.value().profile_identity == "hardware/p5200-class",
        "selection binds the requesting profile identity");

    const auto legacy_repeat = representation::select_variant(
        family, p5200,
        requirement(representation::RepresentationPurpose::render, 0.06, 0.09, 0.05, 0.13),
        {}, std::span(envelopes));
    require(legacy_repeat.value().selection_digest ==
            legacy.value().selection_digest,
        "identical inputs reproduce the identical selection digest");

    // Modern profile: a tight error budget admits only the full variant even
    // though it is the most expensive resident product.
    const auto current = representation::select_variant(
        family, modern,
        requirement(representation::RepresentationPurpose::render, 0.02, 0.03, 0.02, 0.06),
        {}, std::span(envelopes));
    require(static_cast<bool>(current), "modern selection succeeds");
    require(current.value().selected_variant.value == 1U,
        "modern profile buys quality when the budget allows it");
    require(current.value().selection_digest != legacy.value().selection_digest,
        "different profiles over the same family select differently");

    const auto reparsed =
        representation::RepresentationSelection::deserialize(current.value().serialize());
    require(static_cast<bool>(reparsed), "selection serialization round-trips");
    require(reparsed.value().selection_digest == current.value().selection_digest,
        "selection digest survives the round trip");
    require(!representation::RepresentationSelection::deserialize(
                current.value().serialize() + "junk"),
        "selection parser rejects trailing data");
}

void selection_refuses_missing_capability() {
    const auto family = fixture_family();
    const auto envelopes = fixture_envelopes();
    const auto refused = representation::select_variant(
        family, p5200_profile(),
        requirement(representation::RepresentationPurpose::sensor, 0.06, 0.06, 0.06, 0.11),
        {}, std::span(envelopes));
    require(!refused, "P5200-class profile refuses sensor selection");
    require(refused.error().code == core::ErrorCode::unsupported,
        "missing profile capability refuses with unsupported");

    const auto admitted = representation::select_variant(
        family, modern_profile(),
        requirement(representation::RepresentationPurpose::sensor, 0.06, 0.06, 0.06, 0.11),
        {}, std::span(envelopes));
    require(static_cast<bool>(admitted),
        "modern profile supports the sensor purpose");
    require(admitted.value().selected_variant.value == 6U,
        "sensor proxy selected on capable hardware");
}

void selection_refuses_unsatisfiable_budgets_without_fallback() {
    const auto family = fixture_family();
    const auto envelopes = fixture_envelopes();

    auto tiny_budget = p5200_profile();
    tiny_budget.resident_budget_bytes = 1024U;
    const auto refused_budget = representation::select_variant(
        family, tiny_budget,
        requirement(representation::RepresentationPurpose::render, 1.0, 1.0, 1.0, 1.0),
        {}, std::span(envelopes));
    require(!refused_budget, "no variant fits the resident budget");
    require(refused_budget.error().code == core::ErrorCode::validation_failed,
        "unsatisfiable resident budget refuses explicitly");

    const auto refused_error = representation::select_variant(
        family, modern_profile(),
        requirement(representation::RepresentationPurpose::render, 0.005, 0.03, 0.02, 0.06),
        {}, std::span(envelopes));
    require(!refused_error,
        "no variant meets an error budget below every envelope");
    require(refused_error.error().code == core::ErrorCode::validation_failed,
        "unsatisfiable error budget refuses instead of falling back");

    const auto unresolved = representation::select_variant(
        family, p5200_profile(),
        requirement(representation::RepresentationPurpose::render, 0.06, 0.09, 0.05, 0.13),
        {}, {});
    require(!unresolved, "selection without resolved envelopes fails closed");
}

void selection_tie_breaks_by_variant_id() {
    representation::RepresentationFamily family;
    family.set_id(representation::RepresentationFamilyId{20U});
    family.set_source(representation::AuthoredSourceIdentity{
        "docuseries/federation-fixture",
        core::Revision{1U},
        digest("tie-source"),
    });
    const auto cheap = envelope(0.01, 0.01, 0.01, 0.01);
    require(static_cast<bool>(family.upsert_variant(variant(
                2U, representation::RepresentationPurpose::render, cheap, 4096U, 21U))),
        "second tied variant enters");
    require(static_cast<bool>(family.upsert_variant(variant(
                1U, representation::RepresentationPurpose::render, cheap, 4096U, 22U))),
        "first tied variant enters");
    const std::vector<representation::RepresentationVariantEnvelope> envelopes{
        {representation::RepresentationVariantId{1U}, cheap},
        {representation::RepresentationVariantId{2U}, cheap},
    };
    const auto selected = representation::select_variant(
        family, modern_profile(),
        requirement(representation::RepresentationPurpose::render, 0.5, 0.5, 0.5, 0.5),
        {}, std::span(envelopes));
    require(static_cast<bool>(selected), "tied selection succeeds");
    require(selected.value().selected_variant.value == 1U,
        "resident-byte ties break toward the lowest variant identity");
}

void selection_certification_filter() {
    const auto family = fixture_family();
    const auto envelopes = fixture_envelopes();

    representation::RepresentationCertification certification;
    certification.family = family.id();
    certification.variant = representation::RepresentationVariantId{2U};
    certification.certified_source_revision = core::Revision{3U};
    certification.selection_evidence_digest = digest("selection-evidence-v2");
    require(static_cast<bool>(certification.refresh_digest()),
        "certification digest refreshes");
    const std::vector<representation::RepresentationCertification> certifications{
        certification};

    // Unrestricted, the loose requirement would pick render-low; the
    // certification filter restricts eligibility to the certified variant.
    const auto restricted = representation::select_variant(
        family, modern_profile(),
        requirement(representation::RepresentationPurpose::render, 0.3, 0.4, 0.2, 0.5),
        std::span(certifications), std::span(envelopes));
    require(static_cast<bool>(restricted), "certified selection succeeds");
    require(restricted.value().selected_variant.value == 2U,
        "only certified variants are eligible");

    auto stale_certification = certification;
    stale_certification.certified_source_revision = core::Revision{2U};
    require(static_cast<bool>(stale_certification.refresh_digest()),
        "stale certification digest refreshes");
    const std::vector<representation::RepresentationCertification> stale{
        stale_certification};
    require(!representation::select_variant(
            family, modern_profile(),
            requirement(representation::RepresentationPurpose::render, 0.3, 0.4, 0.2, 0.5),
            std::span(stale), std::span(envelopes)),
        "certifications for a different source revision are rejected");

    representation::RepresentationCertification outsider = certification;
    outsider.variant = representation::RepresentationVariantId{42U};
    require(static_cast<bool>(outsider.refresh_digest()),
        "outsider certification digest refreshes");
    const std::vector<representation::RepresentationCertification> outsiders{outsider};
    require(!representation::select_variant(
            family, modern_profile(),
            requirement(representation::RepresentationPurpose::render, 0.3, 0.4, 0.2, 0.5),
            std::span(outsiders), std::span(envelopes)),
        "certifications outside the family are rejected");
}

representation::RepresentationRuntimeEvidence evidence(
    std::uint64_t variant_id,
    representation::EvidenceStatus status,
    std::uint64_t resident_bytes) {
    representation::RepresentationRuntimeEvidence record;
    record.family = representation::RepresentationFamilyId{10U};
    record.variant = representation::RepresentationVariantId{variant_id};
    record.hardware_profile = "hardware/p5200-class";
    record.profile_digest = digest("profile-p5200");
    record.source_revision = core::Revision{3U};
    record.status = status;
    record.measured_resident_bytes = resident_bytes;
    record.observed_error = envelope(0.05, 0.08, 0.04, 0.12);
    require(static_cast<bool>(record.refresh_digest()),
        "evidence digest refreshes");
    return record;
}

void evidence_ledger_append_query_and_no_fallback() {
    representation::RepresentationEvidenceLedger ledger;
    require(static_cast<bool>(ledger.append(
                evidence(2U, representation::EvidenceStatus::admitted, 16U * 1024U * 1024U))),
        "admitted evidence appends");
    const auto admitted = ledger.query(
        representation::RepresentationFamilyId{10U},
        representation::RepresentationVariantId{2U}, "hardware/p5200-class");
    require(static_cast<bool>(admitted), "ledger query succeeds");
    require(admitted.value().has_value(), "latest admitted evidence is returned");
    require(admitted.value().value().measured_resident_bytes == 16U * 1024U * 1024U,
        "measured resident bytes survive the ledger round trip");

    // A later failed measurement supersedes the admitted one; the query never
    // falls back to the earlier admitted record.
    require(static_cast<bool>(ledger.append(
                evidence(2U, representation::EvidenceStatus::failed, 16U * 1024U * 1024U))),
        "failed evidence appends");
    const auto superseded = ledger.query(
        representation::RepresentationFamilyId{10U},
        representation::RepresentationVariantId{2U}, "hardware/p5200-class");
    require(static_cast<bool>(superseded), "superseded query succeeds");
    require(!superseded.value().has_value(),
        "a failed latest record yields nullopt instead of falling back");

    require(static_cast<bool>(ledger.append(
                evidence(2U, representation::EvidenceStatus::admitted, 14U * 1024U * 1024U))),
        "recovery evidence appends");
    const auto recovered = ledger.query(
        representation::RepresentationFamilyId{10U},
        representation::RepresentationVariantId{2U}, "hardware/p5200-class");
    require(recovered.value().has_value() &&
                recovered.value().value().measured_resident_bytes == 14U * 1024U * 1024U,
        "the newest admitted record wins after recovery");

    const auto missing = ledger.query(
        representation::RepresentationFamilyId{10U},
        representation::RepresentationVariantId{7U}, "hardware/p5200-class");
    require(static_cast<bool>(missing) && !missing.value().has_value(),
        "unmeasured keys yield nullopt");
    require(!ledger.query(representation::RepresentationFamilyId{0U},
                          representation::RepresentationVariantId{2U},
                          "hardware/p5200-class"),
        "incomplete query keys fail closed");
}

void evidence_ledger_round_trip_and_digest_stability() {
    representation::RepresentationEvidenceLedger ledger;
    require(static_cast<bool>(ledger.append(
                evidence(2U, representation::EvidenceStatus::admitted, 16U * 1024U * 1024U))),
        "first evidence appends");
    require(static_cast<bool>(ledger.append(
                evidence(2U, representation::EvidenceStatus::failed, 16U * 1024U * 1024U))),
        "second evidence appends");
    require(static_cast<bool>(ledger.append(
                evidence(6U, representation::EvidenceStatus::stale, 6U * 1024U * 1024U))),
        "third evidence appends");

    const auto first_digest = ledger.canonical_digest();
    require(static_cast<bool>(first_digest), "ledger digest computes");
    const auto reparsed =
        representation::RepresentationEvidenceLedger::deserialize(ledger.serialize());
    require(static_cast<bool>(reparsed), "ledger serialization round-trips");
    require(reparsed.value().size() == 3U, "all records survive the round trip");
    const auto second_digest = reparsed.value().canonical_digest();
    require(static_cast<bool>(second_digest), "reparsed ledger digest computes");
    require(first_digest.value() == second_digest.value(),
        "ledger digest is stable across a text round trip");
}

void evidence_ledger_hostile_inputs_fail_closed() {
    const std::string valid_record =
        "RECORD 10 2 \"hardware/p5200-class\" "
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 3 admitted 16 "
        "0.05 0.08 0.04 0.12 "
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    // The digest above cannot match the canonical content; even a perfectly
    // framed record fails closed while its digest disagrees.
    const std::string framed =
        std::string{"CARTOGRAPHER_REPRESENTATION_EVIDENCE_LEDGER 1\nRECORDS 1\n"} +
        valid_record + "\nEND\n";
    require(!representation::RepresentationEvidenceLedger::deserialize(framed),
        "evidence records with mismatched digests fail closed");

    require(!representation::RepresentationEvidenceLedger::deserialize(
                "CARTOGRAPHER_REPRESENTATION_EVIDENCE_LEDGER 99\nRECORDS 0\nEND\n"),
        "future ledger versions fail precisely");
    require(!representation::RepresentationEvidenceLedger::deserialize(
                "CARTOGRAPHER_REPRESENTATION_EVIDENCE_LEDGER 1\nRECORDS 5000\n"),
        "hostile record counts are rejected before allocation");
    require(!representation::RepresentationEvidenceLedger::deserialize(
                std::string{"CARTOGRAPHER_REPRESENTATION_EVIDENCE_LEDGER 1\nRECORDS 0\nEND\n"} +
                "junk"),
        "ledger parser rejects trailing data");
    require(!representation::RepresentationEvidenceLedger::deserialize(
                "CARTOGRAPHER_REPRESENTATION_EVIDENCE_LEDGER 1\nRECORDS 1\n"
                "RECORD 10 2 \"hardware/p5200-class\" "
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef "
                "3 bogus 16 0.05 0.08 0.04 0.12 "
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\nEND\n"),
        "unknown evidence status names are rejected");
}

} // namespace

int main() {
    family_records_bind_one_authored_source();
    family_serialization_round_trip_and_digest_stability();
    family_replacement_preserves_continuity();
    family_hostile_inputs_fail_closed();
    family_records_carry_no_runtime_state();
    error_envelope_surface_mapping_and_cluster_upgrade();
    error_envelope_hostile_inputs_fail_closed();
    certification_binds_variant_to_selection_evidence();
    certification_hostile_inputs_fail_closed();
    selection_is_deterministic_across_profiles();
    selection_refuses_missing_capability();
    selection_refuses_unsatisfiable_budgets_without_fallback();
    selection_tie_breaks_by_variant_id();
    selection_certification_filter();
    evidence_ledger_append_query_and_no_fallback();
    evidence_ledger_round_trip_and_digest_stability();
    evidence_ledger_hostile_inputs_fail_closed();
    std::cout << "representation tests passed\n";
    return EXIT_SUCCESS;
}
