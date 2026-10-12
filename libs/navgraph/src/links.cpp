#include <carto/navgraph/links.hpp>

#include "detail.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace carto::navgraph {

core::Result<void> TraversalAnchor::validate() const {
    if (!id || !detail::valid_medium(medium) || !position.finite() ||
        !detail::safe_text(label, kMaxLabelBytes, true)) {
        return core::Result<void>::failure(detail::invalid(
            "traversal anchor identity, medium, position, or label is invalid"));
    }
    if (region.has_value() && !region.value()) {
        return core::Result<void>::failure(detail::validation(
            "traversal anchor region reference is invalid"));
    }
    if (access.has_value() && !access.value()) {
        return core::Result<void>::failure(detail::validation(
            "traversal anchor access reference is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> OffMeshLinkCandidate::validate() const {
    if (!id || !start_anchor || !end_anchor || start_anchor == end_anchor ||
        !detail::valid_access_primitive(primitive) ||
        !detail::valid_medium(start_medium) || !detail::valid_medium(end_medium) ||
        !std::isfinite(length) || length <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "off-mesh link identity, anchors, primitive, mediums, or length is invalid"));
    }
    if (start_port_identity.has_value() &&
        !detail::safe_text(start_port_identity.value(), detail::kMaxIdentityStringBytes)) {
        return core::Result<void>::failure(detail::invalid(
            "off-mesh link start port identity is invalid"));
    }
    if (end_port_identity.has_value() &&
        !detail::safe_text(end_port_identity.value(), detail::kMaxIdentityStringBytes)) {
        return core::Result<void>::failure(detail::invalid(
            "off-mesh link end port identity is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> MediumTransition::validate() const {
    if (!link || from_medium == to_medium ||
        !detail::valid_access_primitive(primitive)) {
        return core::Result<void>::failure(detail::validation(
            "medium transition must cross distinct mediums on a valid primitive"));
    }
    if (start_port_identity.has_value() &&
        !detail::safe_text(start_port_identity.value(), detail::kMaxIdentityStringBytes)) {
        return core::Result<void>::failure(detail::invalid(
            "medium transition start port identity is invalid"));
    }
    if (end_port_identity.has_value() &&
        !detail::safe_text(end_port_identity.value(), detail::kMaxIdentityStringBytes)) {
        return core::Result<void>::failure(detail::invalid(
            "medium transition end port identity is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<OffMeshLinkCandidate>> compile_offmesh_links(
    const std::vector<PortReference>& ports,
    const std::vector<TraversalAnchor>& anchors,
    const std::vector<std::uint64_t>& port_anchor_ordinals) {
    if (ports.size() > kMaxOffMeshLinkInputs || anchors.size() > kMaxOffMeshLinkInputs) {
        return core::Result<std::vector<OffMeshLinkCandidate>>::failure(detail::validation(
            "off-mesh link inputs exceed the bounded record capacity"));
    }
    if (ports.size() < 2U || ports.size() != port_anchor_ordinals.size()) {
        return core::Result<std::vector<OffMeshLinkCandidate>>::failure(detail::invalid(
            "off-mesh link compilation needs at least two ports and one anchor ordinal per port"));
    }
    for (const auto& port : ports) {
        if (auto result = port.validate(); !result) {
            return core::Result<std::vector<OffMeshLinkCandidate>>::failure(result.error());
        }
    }
    for (const auto& anchor : anchors) {
        if (auto result = anchor.validate(); !result) {
            return core::Result<std::vector<OffMeshLinkCandidate>>::failure(result.error());
        }
    }
    std::set<std::uint64_t> anchor_identities;
    for (const auto& anchor : anchors) {
        if (!anchor_identities.insert(anchor.id.value).second) {
            return core::Result<std::vector<OffMeshLinkCandidate>>::failure(detail::invalid(
                "off-mesh link compilation contains a duplicate anchor identity"));
        }
    }
    for (const std::uint64_t ordinal : port_anchor_ordinals) {
        if (ordinal >= anchors.size()) {
            return core::Result<std::vector<OffMeshLinkCandidate>>::failure(detail::invalid(
                "off-mesh link compilation references a missing anchor"));
        }
    }

    std::set<std::pair<std::string, std::string>> seen_pairs;
    std::vector<OffMeshLinkCandidate> links;
    for (std::size_t first = 0U; first < ports.size(); ++first) {
        for (std::size_t second = first + 1U; second < ports.size(); ++second) {
            const PortReference& a = ports[first];
            const PortReference& b = ports[second];
            const TraversalAnchor& anchor_a = anchors[port_anchor_ordinals[first]];
            const TraversalAnchor& anchor_b = anchors[port_anchor_ordinals[second]];

            std::vector<std::string> shared;
            std::set_intersection(
                a.tags.begin(), a.tags.end(), b.tags.begin(), b.tags.end(),
                std::back_inserter(shared));
            const bool crosses_medium = anchor_a.medium != anchor_b.medium;
            if (shared.empty() && !crosses_medium) {
                continue; // No static compatibility evidence: not a candidate.
            }

            if (!seen_pairs.emplace(a.port_identity, b.port_identity).second) {
                return core::Result<std::vector<OffMeshLinkCandidate>>::failure(detail::invalid(
                    "off-mesh link compilation contains a duplicate port pair"));
            }

            const AccessPrimitive primitive = crosses_medium
                ? AccessPrimitive::portal
                : a.traversal_primitive().value_or(AccessPrimitive::portal);

            OffMeshLinkCandidate link;
            link.id = derive_link_id(links.size());
            link.primitive = primitive;
            link.start_anchor = anchor_a.id;
            link.end_anchor = anchor_b.id;
            link.start_medium = anchor_a.medium;
            link.end_medium = anchor_b.medium;
            link.start_port_identity = a.port_identity;
            link.end_port_identity = b.port_identity;
            link.length = (anchor_b.position - anchor_a.position).length();
            if (auto result = link.validate(); !result) {
                return core::Result<std::vector<OffMeshLinkCandidate>>::failure(result.error());
            }
            links.push_back(link);
        }
    }
    return core::Result<std::vector<OffMeshLinkCandidate>>::success(std::move(links));
}

core::Result<std::vector<MediumTransition>> compile_medium_transitions(
    const std::vector<OffMeshLinkCandidate>& links) {
    std::vector<MediumTransition> transitions;
    transitions.reserve(links.size());
    for (const auto& link : links) {
        if (link.start_medium == link.end_medium) {
            continue;
        }
        MediumTransition transition;
        transition.link = link.id;
        transition.from_medium = link.start_medium;
        transition.to_medium = link.end_medium;
        transition.primitive = link.primitive;
        transition.start_port_identity = link.start_port_identity;
        transition.end_port_identity = link.end_port_identity;
        if (auto result = transition.validate(); !result) {
            return core::Result<std::vector<MediumTransition>>::failure(result.error());
        }
        transitions.push_back(std::move(transition));
    }
    return core::Result<std::vector<MediumTransition>>::success(std::move(transitions));
}

} // namespace carto::navgraph
