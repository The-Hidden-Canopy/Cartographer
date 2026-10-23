#include <carto/archetype/digest.hpp>
#include <carto/archetype/inference.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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

using carto::archetype::ArchetypeCandidate;
using carto::archetype::ArchetypeProposal;
using carto::archetype::AssemblyInput;
using carto::archetype::AssemblyLink;
using carto::archetype::AssemblyNodeContent;
using carto::archetype::AssemblyObservation;
using carto::archetype::StructuralDigest;

StructuralDigest compute_or_die(const AssemblyInput& input, const char* message) {
    const auto result = StructuralDigest::compute(input);
    require(result.has_value(), message);
    return result.value();
}

ArchetypeProposal infer_or_die(
    std::string_view source_namespace,
    carto::core::Revision source_revision,
    std::span<const AssemblyObservation> observations) {
    const auto result = carto::archetype::infer_archetypes(
        std::string(source_namespace), source_revision, observations);
    require(result.has_value(), "fixture inference succeeds");
    return result.value();
}

AssemblyNodeContent mesh_node(std::string_view mesh, std::vector<std::uint32_t> regions) {
    return AssemblyNodeContent{digest(mesh), std::move(regions)};
}

AssemblyNodeContent group_node() {
    return AssemblyNodeContent{{}, {}};
}

// A motel unit: grouping root, wall mesh with two material regions, roof mesh.
AssemblyInput motel_unit(std::uint32_t root, std::uint32_t wall, std::uint32_t roof) {
    AssemblyInput input;
    input.nodes.resize(3U);
    input.nodes[root] = group_node();
    input.nodes[wall] = mesh_node("unit-wall", {3U, 9U});
    input.nodes[roof] = mesh_node("unit-roof", {7U});
    input.links.push_back(AssemblyLink{wall, root});
    input.links.push_back(AssemblyLink{roof, root});
    return input;
}

AssemblyInput water_tower() {
    AssemblyInput input;
    input.nodes.push_back(mesh_node("water-tower", {4U}));
    return input;
}

void equal_assemblies_share_one_structural_digest() {
    const StructuralDigest unit = compute_or_die(motel_unit(0U, 1U, 2U),
        "motel unit structural digest computes");
    require(unit.validate().has_value(), "structural digest validates");

    // Same assembly under different node numbering and reversed link order.
    AssemblyInput renamed = motel_unit(2U, 1U, 0U);
    std::swap(renamed.links.front(), renamed.links.back());
    const StructuralDigest second = compute_or_die(renamed,
        "renumbered motel unit computes");
    require(second == unit && second.canonical() == unit.canonical(),
            "equal assemblies digest equally regardless of node numbering and link order");

    // Same content with the two siblings interchanged.
    const StructuralDigest third = compute_or_die(motel_unit(0U, 2U, 1U),
        "sibling-swapped motel unit computes");
    require(third == unit, "sibling order does not alter the structural digest");

    // A different material region set breaks equality.
    AssemblyInput re_regioned = motel_unit(0U, 1U, 2U);
    re_regioned.nodes[1U].material_regions = {3U, 10U};
    const StructuralDigest fourth = compute_or_die(re_regioned,
        "re-regioned motel unit computes");
    require(fourth != unit &&
                fourth.material_region_digest != unit.material_region_digest,
            "material region drift changes the structural digest");

    // A different hierarchy shape breaks equality even with identical content.
    AssemblyInput deep = motel_unit(0U, 1U, 2U);
    deep.links.clear();
    deep.links.push_back(AssemblyLink{1U, 0U});
    deep.links.push_back(AssemblyLink{2U, 1U});
    const StructuralDigest fifth = compute_or_die(deep, "chained motel unit computes");
    require(fifth != unit && fifth.hierarchy_shape_digest != unit.hierarchy_shape_digest,
            "hierarchy shape drift changes the structural digest");

    // Different mesh content breaks equality.
    AssemblyInput re_meshed = motel_unit(0U, 1U, 2U);
    re_meshed.nodes[1U].mesh_digest = digest("unit-wall-alt");
    const StructuralDigest sixth = compute_or_die(re_meshed,
        "re-meshed motel unit computes");
    require(sixth != unit, "mesh content drift changes the structural digest");

    // Hostile assembly inputs fail closed.
    AssemblyInput unsorted = motel_unit(0U, 1U, 2U);
    unsorted.nodes[1U].material_regions = {9U, 3U};
    const auto unsorted_result = StructuralDigest::compute(unsorted);
    require(!unsorted_result, "unsorted material regions are rejected");
    AssemblyInput duplicated = motel_unit(0U, 1U, 2U);
    duplicated.nodes[1U].material_regions = {3U, 3U};
    const auto duplicated_result = StructuralDigest::compute(duplicated);
    require(!duplicated_result, "duplicated material regions are rejected");
    AssemblyInput cyclic = motel_unit(0U, 1U, 2U);
    cyclic.links.push_back(AssemblyLink{0U, 1U});
    const auto cyclic_result = StructuralDigest::compute(cyclic);
    require(!cyclic_result, "assembly hierarchy cycles are rejected");
    AssemblyInput multi_parent = motel_unit(0U, 1U, 2U);
    multi_parent.links.push_back(AssemblyLink{1U, 2U});
    const auto multi_parent_result = StructuralDigest::compute(multi_parent);
    require(!multi_parent_result, "nodes with more than one parent are rejected");
    AssemblyInput self_link = motel_unit(0U, 1U, 2U);
    self_link.links.push_back(AssemblyLink{1U, 1U});
    const auto self_link_result = StructuralDigest::compute(self_link);
    require(!self_link_result, "self-referential links are rejected");
    const auto empty_result = StructuralDigest::compute(AssemblyInput{});
    require(!empty_result, "empty assemblies are rejected");
}

void repeated_assemblies_infer_one_candidate() {
    const StructuralDigest unit = compute_or_die(motel_unit(0U, 1U, 2U),
        "motel unit structural digest computes");
    const StructuralDigest tower = compute_or_die(water_tower(),
        "water tower structural digest computes");

    std::vector<AssemblyObservation> observations{
        AssemblyObservation{"pine-ridge/motel/unit-b", unit},
        AssemblyObservation{"pine-ridge/motel/unit-a", unit},
        AssemblyObservation{"pine-ridge/water/tower", tower},
    };
    const ArchetypeProposal proposal = infer_or_die(
        "public.pine-ridge", carto::core::Revision{3U}, std::span{observations});
    require(proposal.validate().has_value(),
            "repeated assembly inference validates");
    require(proposal.candidates.size() == 1U,
            "two identically structured assemblies under different names produce one archetype candidate");
    const ArchetypeCandidate& candidate = proposal.candidates.front();
    require(candidate.repetition_count == 2U,
            "candidate repetition count is the deterministic dedup count");
    require((candidate.observation_identities ==
                std::vector<std::string>{"pine-ridge/motel/unit-a",
                                         "pine-ridge/motel/unit-b"}),
            "candidate member identities are sorted and name-bound");
    require(candidate.structural == unit,
            "candidate binds the shared structural digest");
    require(proposal.receipt.observation_count == 3U &&
                proposal.receipt.unique_structure_count == 2U &&
                proposal.receipt.candidate_count == 1U &&
                proposal.receipt.deduplicated_instance_count == 1U,
            "receipt records deterministic observation, structure, and dedup counts");
    require(proposal.receipt.source_namespace == "public.pine-ridge" &&
                proposal.receipt.source_revision == carto::core::Revision{3U},
            "receipt binds the source namespace and revision");

    // Determinism: input order must not change any digest or byte.
    std::vector<AssemblyObservation> reordered{
        observations[2], observations[0], observations[1]};
    const ArchetypeProposal again = infer_or_die(
        "public.pine-ridge", carto::core::Revision{3U}, std::span{reordered});
    require(again.serialize() == proposal.serialize(),
            "observation input order does not alter the proposal");
    const auto first_digest = proposal.canonical_digest();
    const auto second_digest = again.canonical_digest();
    require(first_digest && second_digest &&
                first_digest.value() == second_digest.value(),
            "proposal digest is stable across input orderings");

    const std::string encoded = proposal.serialize();
    const auto reopened = ArchetypeProposal::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
            "archetype proposal survives deterministic round trip");
    const auto reopened_digest = reopened.value().canonical_digest();
    require(reopened_digest && reopened_digest.value() == first_digest.value(),
            "proposal digest survives reopen");

    // The water tower is unique: counted, never proposed.
    require(std::none_of(proposal.candidates.begin(), proposal.candidates.end(),
                [&tower](const ArchetypeCandidate& entry) {
                    return entry.structural.value == tower.value;
                }),
            "unique structures are counted but never proposed");
}

void inference_inputs_fail_closed() {
    const StructuralDigest unit = compute_or_die(motel_unit(0U, 1U, 2U),
        "motel unit structural digest computes");

    carto::archetype::InferenceOptions threshold_one;
    threshold_one.minimum_repetition = 1U;
    std::vector<AssemblyObservation> pair{
        AssemblyObservation{"site/unit-a", unit},
        AssemblyObservation{"site/unit-b", unit},
    };
    const auto low_threshold = carto::archetype::infer_archetypes(
        "public.site", {}, std::span{pair}, threshold_one);
    require(!low_threshold, "repetition thresholds below two are rejected");

    std::vector<AssemblyObservation> duplicate_identities{
        AssemblyObservation{"site/unit-a", unit},
        AssemblyObservation{"site/unit-a", unit},
    };
    const auto duplicates = carto::archetype::infer_archetypes(
        "public.site", {}, std::span{duplicate_identities});
    require(!duplicates, "duplicate observation identities are rejected");

    std::vector<AssemblyObservation> invalid{
        AssemblyObservation{"", unit},
    };
    const auto empty_identity = carto::archetype::infer_archetypes(
        "public.site", {}, std::span{invalid});
    require(!empty_identity, "empty observation identities are rejected");

    std::vector<AssemblyObservation> invalid_structure{
        AssemblyObservation{"site/unit-a", {}},
    };
    const auto no_structure = carto::archetype::infer_archetypes(
        "public.site", {}, std::span{invalid_structure});
    require(!no_structure, "observations without structural evidence are rejected");
}

void proposal_bytes_fail_closed() {
    const StructuralDigest unit = compute_or_die(motel_unit(0U, 1U, 2U),
        "motel unit structural digest computes");
    std::vector<AssemblyObservation> observations{
        AssemblyObservation{"site/unit-a", unit},
        AssemblyObservation{"site/unit-b", unit},
    };
    const ArchetypeProposal proposal = infer_or_die(
        "public.site", carto::core::Revision{1U}, std::span{observations});
    const std::string encoded = proposal.serialize();

    const auto trailing = ArchetypeProposal::deserialize(encoded + "trailing");
    require(!trailing, "trailing proposal data is rejected");

    std::string future = encoded;
    future.replace(0U, std::string{"CARTO_ARCHETYPE_PROPOSAL 1"}.size(),
        "CARTO_ARCHETYPE_PROPOSAL 99");
    require(!ArchetypeProposal::deserialize(future),
            "unknown future proposal versions fail precisely");

    require(!ArchetypeProposal::deserialize(
                "CARTO_ARCHETYPE_PROPOSAL 1\n"
                "SOURCE \"x\" 0 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "COUNTS 1 1 1 0\n"
                "CANDIDATE_SET 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "CANDIDATES 65537\n"),
            "hostile candidate counts are rejected before allocation");

    std::string bad_member = encoded;
    const std::string member_hex = proposal.candidates.front().member_evidence_digest.hex();
    const auto member_pos = bad_member.find(member_hex);
    require(member_pos != std::string::npos, "fixture locates the member evidence digest");
    bad_member.replace(member_pos, member_hex.size(),
        proposal.receipt.candidate_set_digest.hex());
    require(!ArchetypeProposal::deserialize(bad_member),
            "member evidence that does not bind its identities is rejected");

    std::string bad_receipt = encoded;
    const std::string receipt_hex = proposal.receipt.receipt_digest.hex();
    const auto receipt_pos = bad_receipt.rfind(receipt_hex);
    require(receipt_pos != std::string::npos, "fixture locates the receipt digest");
    bad_receipt.replace(receipt_pos, receipt_hex.size(),
        proposal.receipt.source_digest.hex());
    require(!ArchetypeProposal::deserialize(bad_receipt),
            "receipt digest drift is rejected");

    ArchetypeProposal tampered = proposal;
    tampered.receipt.deduplicated_instance_count += 1U;
    require(!tampered.validate(),
            "receipt counts that drift from the candidate set are rejected");
    ArchetypeProposal reordered = proposal;
    std::reverse(reordered.candidates.front().observation_identities.begin(),
        reordered.candidates.front().observation_identities.end());
    require(!reordered.validate(),
            "unsorted candidate member identities are rejected");
}

} // namespace

int main() {
    equal_assemblies_share_one_structural_digest();
    repeated_assemblies_infer_one_candidate();
    inference_inputs_fail_closed();
    proposal_bytes_fail_closed();
    std::cout << "archetype inference tests passed\n";
    return EXIT_SUCCESS;
}
