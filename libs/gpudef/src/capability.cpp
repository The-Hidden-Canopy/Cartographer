#include <carto/gpudef/capability.hpp>

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace carto::gpudef {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic refused(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

} // namespace

std::string_view feature_evidence_name(FeatureEvidence evidence) noexcept {
    switch (evidence) {
    case FeatureEvidence::packed_parity: return "packed_parity";
    case FeatureEvidence::cpu_reference_only: return "cpu_reference_only";
    }
    return "unknown";
}

core::Result<void> FeatureCapability::validate() const {
    const bool bound = evidence == FeatureEvidence::packed_parity;
    if (bound != (receipt_count > 0U) || bound == latest_receipt_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "feature capability evidence, receipt count, and digest disagree"));
    }
    return core::Result<void>::success();
}

core::Result<void> CapabilitySummaryReceipt::validate() const {
    if (features.empty() || features.size() > kMaxFeatures) {
        return core::Result<void>::failure(invalid(
            "capability summary must report between one and the bound count "
            "of features"));
    }
    for (std::size_t index = 1U; index < features.size(); ++index) {
        if (features[index - 1U].feature >= features[index].feature) {
            return core::Result<void>::failure(invalid(
                "capability summary features must be sorted and unique"));
        }
    }
    bool any_bound = false;
    bool all_native = true;
    for (const FeatureCapability& feature : features) {
        if (auto result = feature.validate(); !result) {
            return core::Result<void>::failure(result.error());
        }
        if (feature.evidence == FeatureEvidence::packed_parity) {
            any_bound = true;
        } else {
            all_native = false;
        }
    }
    switch (tier) {
    case EvidenceTier::cpu_reference:
        break;
    case EvidenceTier::native_device:
        if (!any_bound || !all_native) {
            return core::Result<void>::failure(invalid(
                "native-device summary tier requires every reported feature "
                "to carry native-device parity evidence"));
        }
        break;
    case EvidenceTier::structural:
        return core::Result<void>::failure(invalid(
            "capability summary never reports structural tier: CPU references "
            "exist for every deformation case"));
    }
    return core::Result<void>::success();
}

std::string CapabilitySummaryReceipt::serialize() const {
    std::ostringstream output;
    output << "CARTO_GPUDEF_CAPABILITY_SUMMARY 1\n";
    output << "TIER " << evidence_tier_name(tier) << '\n';
    output << "FEATURES " << features.size() << '\n';
    for (const FeatureCapability& feature : features) {
        output << "FEATURE " << deformation_case_name(feature.feature) << ' '
               << feature_evidence_name(feature.evidence) << ' '
               << feature.receipt_count << ' '
               << (feature.latest_receipt_digest.is_zero()
                       ? "-" : feature.latest_receipt_digest.hex())
               << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> CapabilitySummaryReceipt::canonical_digest()
    const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<CapabilitySummaryReceipt> CapabilitySummaryReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > 64U * 1024U) {
        return core::Result<CapabilitySummaryReceipt>::failure(invalid(
            "capability summary text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_GPUDEF_CAPABILITY_SUMMARY" || version != 1U) {
        return core::Result<CapabilitySummaryReceipt>::failure(invalid(
            "capability summary framing or schema version is invalid"));
    }
    CapabilitySummaryReceipt receipt;
    std::string token;
    std::string tier_name;
    std::size_t feature_count = 0U;
    if (!((input >> token) && token == "TIER" && (input >> tier_name)) ||
        !((input >> token) && token == "FEATURES" && (input >> feature_count)) ||
        feature_count > kMaxFeatures) {
        return core::Result<CapabilitySummaryReceipt>::failure(invalid(
            "capability summary header is malformed"));
    }
    if (tier_name == "cpu-reference") {
        receipt.tier = EvidenceTier::cpu_reference;
    } else if (tier_name == "native-device") {
        receipt.tier = EvidenceTier::native_device;
    } else {
        return core::Result<CapabilitySummaryReceipt>::failure(invalid(
            "capability summary tier is unknown"));
    }
    auto read_case = [&input](DeformationCase& field) -> bool {
        std::string name;
        if (!(input >> name)) return false;
        for (const DeformationCase candidate : {
                DeformationCase::linear_blend_skinning,
                DeformationCase::dual_quaternion_skinning,
                DeformationCase::morph_stage,
                DeformationCase::corrective_driver}) {
            if (name == deformation_case_name(candidate)) {
                field = candidate;
                return true;
            }
        }
        return false;
    };
    receipt.features.reserve(feature_count);
    for (std::size_t index = 0U; index < feature_count; ++index) {
        FeatureCapability feature;
        std::string evidence_name;
        std::string digest_hex;
        if (!((input >> token) && token == "FEATURE" &&
                read_case(feature.feature) && (input >> evidence_name) &&
                (input >> feature.receipt_count >> digest_hex))) {
            return core::Result<CapabilitySummaryReceipt>::failure(invalid(
                "capability summary feature entry is malformed"));
        }
        if (evidence_name == "packed_parity") {
            feature.evidence = FeatureEvidence::packed_parity;
        } else if (evidence_name == "cpu_reference_only") {
            feature.evidence = FeatureEvidence::cpu_reference_only;
        } else {
            return core::Result<CapabilitySummaryReceipt>::failure(invalid(
                "capability summary feature evidence is unknown"));
        }
        if (digest_hex == "-") {
            feature.latest_receipt_digest = assets::Sha256Digest{};
        } else {
            auto digest = assets::Sha256Digest::from_hex(digest_hex);
            if (!digest) {
                return core::Result<CapabilitySummaryReceipt>::failure(invalid(
                    "capability summary feature digest is malformed"));
            }
            feature.latest_receipt_digest = digest.value();
        }
        receipt.features.push_back(feature);
    }
    if (!((input >> token) && token == "END")) {
        return core::Result<CapabilitySummaryReceipt>::failure(invalid(
            "capability summary is unterminated"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<CapabilitySummaryReceipt>::failure(invalid(
            "capability summary contains trailing unbound data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<CapabilitySummaryReceipt>::failure(result.error());
    }
    return core::Result<CapabilitySummaryReceipt>::success(std::move(receipt));
}

core::Result<CapabilitySummaryReceipt> summarize_capabilities(
    std::span<const DeformationCase> reported_features,
    std::span<const CaseParityReceipt> parity_receipts,
    std::span<const ThroughputReceipt> throughput_receipts) {
    std::set<DeformationCase> feature_set(
        reported_features.begin(), reported_features.end());
    if (feature_set.empty()) {
        return core::Result<CapabilitySummaryReceipt>::failure(invalid(
            "capability summary requires at least one reported feature"));
    }

    // Validate every supplied receipt before any claim is derived from it.
    for (const CaseParityReceipt& receipt : parity_receipts) {
        if (auto result = receipt.validate(); !result) {
            return core::Result<CapabilitySummaryReceipt>::failure(result.error());
        }
    }
    for (const ThroughputReceipt& receipt : throughput_receipts) {
        if (auto result = receipt.validate(); !result) {
            return core::Result<CapabilitySummaryReceipt>::failure(result.error());
        }
    }

    CapabilitySummaryReceipt summary;
    for (const DeformationCase feature : feature_set) {
        FeatureCapability entry;
        entry.feature = feature;
        for (const CaseParityReceipt& receipt : parity_receipts) {
            if (receipt.deformation_case != feature) continue;
            auto digest = receipt.canonical_digest();
            if (!digest) {
                return core::Result<CapabilitySummaryReceipt>::failure(
                    digest.error());
            }
            ++entry.receipt_count;
            // Deterministic total order for "latest": the maximum canonical
            // digest across the bound receipts.
            if (digest.value() > entry.latest_receipt_digest) {
                entry.latest_receipt_digest = digest.value();
            }
        }
        if (entry.receipt_count > 0U) {
            entry.evidence = FeatureEvidence::packed_parity;
        }
        summary.features.push_back(entry);
    }
    // feature_set iterates in enum order, so features are already sorted.

    bool any_bound = false;
    bool all_native = true;
    for (const FeatureCapability& entry : summary.features) {
        if (entry.evidence == FeatureEvidence::packed_parity) {
            any_bound = true;
            for (const CaseParityReceipt& receipt : parity_receipts) {
                if (receipt.deformation_case == entry.feature &&
                    effective_tier(receipt) != EvidenceTier::native_device) {
                    all_native = false;
                }
            }
        } else {
            all_native = false;
        }
    }
    summary.tier = (any_bound && all_native)
        ? EvidenceTier::native_device
        : EvidenceTier::cpu_reference;
    if (auto result = summary.validate(); !result) {
        return core::Result<CapabilitySummaryReceipt>::failure(result.error());
    }
    return core::Result<CapabilitySummaryReceipt>::success(std::move(summary));
}

EvidenceTier effective_tier(const CapabilitySummaryReceipt& receipt) noexcept {
    if (receipt.tier == EvidenceTier::native_device) {
        return EvidenceTier::native_device;
    }
    return EvidenceTier::cpu_reference;
}

core::Result<void> require_evidence_tier(
    const CapabilitySummaryReceipt& receipt,
    EvidenceTier requested) {
    switch (requested) {
    case EvidenceTier::structural:
    case EvidenceTier::cpu_reference:
        return core::Result<void>::success();
    case EvidenceTier::native_device:
        if (effective_tier(receipt) == EvidenceTier::native_device) {
            return core::Result<void>::success();
        }
        return core::Result<void>::failure(refused(
            "native-device capability summary requires native-device parity "
            "evidence for every reported feature"));
    }
    return core::Result<void>::failure(refused(
        "requested evidence tier is beyond what a capability summary can hold"));
}

} // namespace carto::gpudef
