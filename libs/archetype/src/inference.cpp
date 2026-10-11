#include <carto/archetype/inference.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "text_format.hpp"

namespace carto::archetype {
namespace {

constexpr std::string_view kMagic = "CARTO_ARCHETYPE_PROPOSAL";
constexpr std::size_t kMaxObservationsPerProposal = 1'000'000U;

constexpr std::string_view kMembersPrefix = "CARTO_ARCHETYPE_MEMBERS_V1";
constexpr std::string_view kObservationsPrefix = "CARTO_ARCHETYPE_OBSERVATIONS_V1";
constexpr std::string_view kCandidateSetPrefix = "CARTO_ARCHETYPE_CANDIDATE_SET_V1";

std::string member_evidence_text(const std::vector<std::string>& identities) {
    std::ostringstream output;
    output << kMembersPrefix << ' ' << identities.size();
    for (const auto& identity : identities) {
        output << ' ' << std::quoted(identity);
    }
    return output.str();
}

core::Result<void> check_identity_list(const std::vector<std::string>& identities) {
    if (identities.empty() || identities.size() > kMaxObservationsPerCandidate) {
        return core::Result<void>::failure(detail::invalid_text(
            "candidate member list is empty or exceeds its bounded capacity"));
    }
    if (!std::is_sorted(identities.begin(), identities.end()) ||
        std::adjacent_find(identities.begin(), identities.end()) != identities.end()) {
        return core::Result<void>::failure(detail::invalid_text(
            "candidate member identities must be sorted and unique"));
    }
    for (const auto& identity : identities) {
        if (!detail::safe_text(identity, kMaxObservationIdentityBytes)) {
            return core::Result<void>::failure(detail::invalid_text(
                "candidate member identity is invalid"));
        }
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> AssemblyObservation::validate() const {
    if (!detail::safe_text(observation_identity, kMaxObservationIdentityBytes)) {
        return core::Result<void>::failure(detail::invalid_text(
            "assembly observation identity is invalid"));
    }
    if (auto result = structural.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "assembly observation structural digest"));
    }
    return core::Result<void>::success();
}

std::string AssemblyObservation::canonical() const {
    std::ostringstream output;
    output << "OBSERVATION " << std::quoted(observation_identity) << ' '
           << structural.canonical();
    return output.str();
}

core::Result<void> InferenceOptions::validate() const {
    if (minimum_repetition < 2U) {
        return core::Result<void>::failure(detail::invalid_text(
            "archetype inference requires a repetition threshold of at least two"));
    }
    return core::Result<void>::success();
}

core::Result<void> ArchetypeCandidate::validate() const {
    if (auto result = structural.validate(); !result) return result;
    if (auto result = check_identity_list(observation_identities); !result) return result;
    if (repetition_count != observation_identities.size()) {
        return core::Result<void>::failure(detail::invalid_text(
            "candidate repetition count does not match its member list"));
    }
    if (member_evidence_digest.is_zero() ||
        member_evidence_digest !=
            detail::digest_text(member_evidence_text(observation_identities))) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "candidate member evidence digest is missing or does not bind its members"));
    }
    return core::Result<void>::success();
}

std::string ArchetypeCandidate::canonical() const {
    std::ostringstream output;
    output << "CANDIDATE " << structural.canonical() << ' ' << repetition_count << ' '
           << member_evidence_digest.hex() << ' ' << observation_identities.size();
    for (const auto& identity : observation_identities) {
        output << ' ' << std::quoted(identity);
    }
    return output.str();
}

core::Result<void> InferenceReceipt::validate() const {
    if (!detail::safe_text(source_namespace, kMaxSourceNamespaceBytes, true) ||
        source_digest.is_zero() || candidate_set_digest.is_zero() ||
        receipt_digest.is_zero() || candidate_count > kMaxCandidatesPerProposal ||
        unique_structure_count > kMaxObservationsPerProposal ||
        observation_count > kMaxObservationsPerProposal) {
        return core::Result<void>::failure(detail::invalid_text(
            "inference receipt source, digests, or bounded counts are invalid"));
    }
    if (receipt_digest != detail::digest_text(canonical_without_digest())) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "inference receipt digest does not bind its contents"));
    }
    return core::Result<void>::success();
}

std::string InferenceReceipt::canonical_without_digest() const {
    std::ostringstream output;
    output << "RECEIPT " << kSchemaVersion << '\n';
    output << "SOURCE " << std::quoted(source_namespace) << ' '
           << source_revision.value() << ' ' << source_digest.hex() << '\n';
    output << "COUNTS " << observation_count << ' ' << unique_structure_count << ' '
           << candidate_count << ' ' << deduplicated_instance_count << '\n';
    output << "CANDIDATE_SET " << candidate_set_digest.hex() << '\n';
    output << "END_RECEIPT";
    return output.str();
}

core::Result<void> InferenceReceipt::refresh_digest() {
    receipt_digest = detail::digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

core::Result<void> ArchetypeProposal::validate() const {
    if (candidates.size() > kMaxCandidatesPerProposal) {
        return core::Result<void>::failure(detail::invalid_text(
            "archetype proposal exceeds its bounded candidate capacity"));
    }
    for (std::size_t index = 0U; index < candidates.size(); ++index) {
        if (auto result = candidates[index].validate(); !result) return result;
        if (index > 0U &&
            !(candidates[index - 1U].structural.value < candidates[index].structural.value)) {
            return core::Result<void>::failure(detail::invalid_text(
                "archetype candidates must be sorted by unique structural digest"));
        }
    }
    std::uint64_t deduplicated = 0U;
    std::ostringstream candidate_set;
    candidate_set << kCandidateSetPrefix << ' ' << candidates.size() << '\n';
    for (const auto& candidate : candidates) {
        deduplicated += candidate.repetition_count - 1U;
        candidate_set << candidate.canonical() << '\n';
    }
    if (receipt.candidate_count != candidates.size() ||
        receipt.deduplicated_instance_count != deduplicated ||
        receipt.candidate_set_digest != detail::digest_text(candidate_set.str())) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "inference receipt does not match the proposed candidate set"));
    }
    if (auto result = receipt.validate(); !result) return result;
    return core::Result<void>::success();
}

std::string ArchetypeProposal::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "SOURCE " << std::quoted(receipt.source_namespace) << ' '
           << receipt.source_revision.value() << ' ' << receipt.source_digest.hex() << '\n';
    output << "COUNTS " << receipt.observation_count << ' '
           << receipt.unique_structure_count << ' ' << receipt.candidate_count << ' '
           << receipt.deduplicated_instance_count << '\n';
    output << "CANDIDATE_SET " << receipt.candidate_set_digest.hex() << '\n';
    output << "CANDIDATES " << candidates.size() << '\n';
    for (const auto& candidate : candidates) {
        const StructuralDigest& structural = candidate.structural;
        output << "CANDIDATE " << structural.mesh_content_digest.hex() << ' '
               << structural.hierarchy_shape_digest.hex() << ' '
               << structural.material_region_digest.hex() << ' ' << structural.value.hex()
               << ' ' << structural.node_count << ' ' << candidate.repetition_count << ' '
               << candidate.member_evidence_digest.hex() << ' '
               << candidate.observation_identities.size();
        for (const auto& identity : candidate.observation_identities) {
            output << ' ' << std::quoted(identity);
        }
        output << '\n';
    }
    output << "RECEIPT " << receipt.receipt_digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> ArchetypeProposal::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(
        detail::digest_text(serialize()));
}

core::Result<ArchetypeProposal> ArchetypeProposal::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedProposalBytes) {
        return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
            "archetype proposal text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !detail::read_integer(input, version) ||
        magic != kMagic || version != kSchemaVersion) {
        return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
            "archetype proposal framing or schema version is invalid"));
    }

    ArchetypeProposal proposal;
    std::uint64_t revision = 0U;
    if (!detail::require_token(input, "SOURCE") ||
        !(input >> std::quoted(proposal.receipt.source_namespace)) ||
        !detail::read_integer(input, revision)) {
        return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
            "archetype proposal source record is malformed"));
    }
    proposal.receipt.source_revision = core::Revision{revision};
    const auto source_digest = detail::read_digest(input, "archetype proposal", "source digest");
    if (!source_digest) {
        return core::Result<ArchetypeProposal>::failure(source_digest.error());
    }
    proposal.receipt.source_digest = source_digest.value();

    std::size_t observations = 0U;
    std::size_t unique_structures = 0U;
    std::size_t candidate_count = 0U;
    std::uint64_t deduplicated = 0U;
    if (!detail::require_token(input, "COUNTS") ||
        !detail::read_count(input, kMaxObservationsPerProposal, observations) ||
        !detail::read_count(input, kMaxObservationsPerProposal, unique_structures) ||
        !detail::read_count(input, kMaxCandidatesPerProposal, candidate_count) ||
        !detail::read_integer(input, deduplicated)) {
        return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
            "archetype proposal receipt counts are malformed"));
    }
    proposal.receipt.observation_count = static_cast<std::uint32_t>(observations);
    proposal.receipt.unique_structure_count = static_cast<std::uint32_t>(unique_structures);
    proposal.receipt.candidate_count = static_cast<std::uint32_t>(candidate_count);
    proposal.receipt.deduplicated_instance_count = deduplicated;

    if (!detail::require_token(input, "CANDIDATE_SET")) {
        return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
            "archetype proposal candidate-set record is malformed"));
    }
    const auto candidate_set = detail::read_digest(
        input, "archetype proposal", "candidate-set digest");
    if (!candidate_set) {
        return core::Result<ArchetypeProposal>::failure(candidate_set.error());
    }
    proposal.receipt.candidate_set_digest = candidate_set.value();

    std::size_t parsed_candidates = 0U;
    if (!detail::require_token(input, "CANDIDATES") ||
        !detail::read_count(input, kMaxCandidatesPerProposal, parsed_candidates)) {
        return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
            "archetype proposal candidate header is malformed"));
    }
    for (std::size_t index = 0U; index < parsed_candidates; ++index) {
        ArchetypeCandidate candidate;
        StructuralDigest& structural = candidate.structural;
        std::size_t node_count = 0U;
        std::size_t repetition = 0U;
        std::size_t member_count = 0U;
        if (!detail::require_token(input, "CANDIDATE")) {
            return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
                "archetype candidate record is malformed"));
        }
        const auto mesh = detail::read_digest(input, "archetype candidate", "mesh digest");
        const auto shape = detail::read_digest(input, "archetype candidate", "shape digest");
        const auto regions = detail::read_digest(
            input, "archetype candidate", "material-region digest");
        const auto value = detail::read_digest(input, "archetype candidate", "value digest");
        if (!mesh || !shape || !regions || !value) {
            const auto* failure = !mesh ? &mesh : !shape ? &shape : !regions ? &regions
                : &value;
            return core::Result<ArchetypeProposal>::failure(failure->error());
        }
        if (!detail::read_count(input, kMaxAssemblyNodes, node_count) ||
            !detail::read_count(input, kMaxObservationsPerCandidate, repetition)) {
            return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
                "archetype candidate record is malformed"));
        }
        const auto members = detail::read_digest(
            input, "archetype candidate", "member evidence digest");
        if (!members) {
            return core::Result<ArchetypeProposal>::failure(members.error());
        }
        if (!detail::read_count(input, kMaxObservationsPerCandidate, member_count)) {
            return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
                "archetype candidate member count is malformed"));
        }
        structural.mesh_content_digest = mesh.value();
        structural.hierarchy_shape_digest = shape.value();
        structural.material_region_digest = regions.value();
        structural.value = value.value();
        structural.node_count = static_cast<std::uint32_t>(node_count);
        candidate.repetition_count = static_cast<std::uint64_t>(repetition);
        candidate.member_evidence_digest = members.value();
        candidate.observation_identities.reserve(member_count);
        for (std::size_t member = 0U; member < member_count; ++member) {
            std::string identity;
            if (!(input >> std::quoted(identity))) {
                return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
                    "archetype candidate member identity is malformed"));
            }
            candidate.observation_identities.push_back(std::move(identity));
        }
        proposal.candidates.push_back(std::move(candidate));
    }

    if (!detail::require_token(input, "RECEIPT")) {
        return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
            "archetype proposal receipt record is malformed"));
    }
    const auto receipt_digest = detail::read_digest(
        input, "archetype proposal", "receipt digest");
    if (!receipt_digest) {
        return core::Result<ArchetypeProposal>::failure(receipt_digest.error());
    }
    proposal.receipt.receipt_digest = receipt_digest.value();

    if (!detail::require_token(input, "END")) {
        return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
            "archetype proposal is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ArchetypeProposal>::failure(detail::malformed_text(
            "archetype proposal contains trailing unbound data"));
    }
    if (auto result = proposal.validate(); !result) {
        return core::Result<ArchetypeProposal>::failure(result.error());
    }
    return core::Result<ArchetypeProposal>::success(std::move(proposal));
}

core::Result<ArchetypeProposal> infer_archetypes(
    std::string source_namespace,
    core::Revision source_revision,
    std::span<const AssemblyObservation> observations,
    const InferenceOptions& options) {
    if (auto result = options.validate(); !result) {
        return core::Result<ArchetypeProposal>::failure(result.error());
    }
    if (!detail::safe_text(source_namespace, kMaxSourceNamespaceBytes, true)) {
        return core::Result<ArchetypeProposal>::failure(detail::invalid_text(
            "archetype inference source namespace is invalid"));
    }
    if (observations.size() > kMaxObservationsPerProposal) {
        return core::Result<ArchetypeProposal>::failure(detail::invalid_text(
            "archetype inference observation count exceeds its bounded capacity"));
    }

    std::vector<AssemblyObservation> ordered(observations.begin(), observations.end());
    for (const auto& observation : ordered) {
        if (auto result = observation.validate(); !result) {
            return core::Result<ArchetypeProposal>::failure(result.error());
        }
    }
    std::sort(ordered.begin(), ordered.end(),
        [](const AssemblyObservation& left, const AssemblyObservation& right) {
            return left.observation_identity < right.observation_identity;
        });
    if (std::adjacent_find(ordered.begin(), ordered.end(),
            [](const AssemblyObservation& left, const AssemblyObservation& right) {
                return left.observation_identity == right.observation_identity;
            }) != ordered.end()) {
        return core::Result<ArchetypeProposal>::failure(detail::invalid_text(
            "archetype inference observation identities must be unique"));
    }

    struct Group {
        StructuralDigest structural;
        std::vector<std::string> identities;
    };
    std::map<assets::Sha256Digest, Group> groups;
    for (const auto& observation : ordered) {
        auto& group = groups[observation.structural.value];
        if (group.identities.empty()) {
            group.structural = observation.structural;
        }
        group.identities.push_back(observation.observation_identity);
    }

    ArchetypeProposal proposal;
    std::uint64_t deduplicated = 0U;
    for (auto& [value, group] : groups) {
        static_cast<void>(value);
        if (group.identities.size() < options.minimum_repetition) continue;
        ArchetypeCandidate candidate;
        candidate.structural = group.structural;
        candidate.observation_identities = std::move(group.identities);
        candidate.repetition_count = candidate.observation_identities.size();
        candidate.member_evidence_digest = detail::digest_text(
            member_evidence_text(candidate.observation_identities));
        deduplicated += candidate.repetition_count - 1U;
        proposal.candidates.push_back(std::move(candidate));
    }
    std::sort(proposal.candidates.begin(), proposal.candidates.end(),
        [](const ArchetypeCandidate& left, const ArchetypeCandidate& right) {
            return left.structural.value < right.structural.value;
        });

    std::ostringstream source_text;
    source_text << kObservationsPrefix << ' ' << ordered.size() << '\n';
    for (const auto& observation : ordered) {
        source_text << observation.canonical() << '\n';
    }

    proposal.receipt.source_namespace = std::move(source_namespace);
    proposal.receipt.source_revision = source_revision;
    proposal.receipt.source_digest = detail::digest_text(source_text.str());
    proposal.receipt.observation_count = static_cast<std::uint32_t>(ordered.size());
    proposal.receipt.unique_structure_count =
        static_cast<std::uint32_t>(groups.size());
    proposal.receipt.candidate_count =
        static_cast<std::uint32_t>(proposal.candidates.size());
    proposal.receipt.deduplicated_instance_count = deduplicated;

    std::ostringstream candidate_set;
    candidate_set << kCandidateSetPrefix << ' ' << proposal.candidates.size() << '\n';
    for (const auto& candidate : proposal.candidates) {
        candidate_set << candidate.canonical() << '\n';
    }
    proposal.receipt.candidate_set_digest = detail::digest_text(candidate_set.str());
    if (auto result = proposal.receipt.refresh_digest(); !result) {
        return core::Result<ArchetypeProposal>::failure(result.error());
    }
    return core::Result<ArchetypeProposal>::success(std::move(proposal));
}

} // namespace carto::archetype
