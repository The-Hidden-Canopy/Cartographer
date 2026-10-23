#include <carto/attributes/provenance.hpp>

#include <set>
#include <string>
#include <utility>

namespace carto::attributes {

namespace {

core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, std::move(message)));
}

core::Result<void> validation(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::validation_failed, std::move(message)));
}

} // namespace

core::Result<void> StableTopologySnapshot::validate() const {
    for (const auto& [domain, ids] : element_ids) {
        if (!valid_domain(domain)) return invalid("stable topology snapshot contains an invalid domain");
        std::set<std::uint64_t> unique;
        for (const auto id : ids) {
            if (id == 0U) return invalid("stable topology snapshot ids must be non-zero");
            if (!unique.insert(id).second) {
                return validation("stable topology snapshot contains a duplicate id");
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<TopologyProvenance> derive_provenance(
    const StableTopologySnapshot& source,
    const StableTopologySnapshot& destination) {
    if (auto result = source.validate(); !result) {
        return core::Result<TopologyProvenance>::failure(result.error());
    }
    if (auto result = destination.validate(); !result) {
        return core::Result<TopologyProvenance>::failure(result.error());
    }

    TopologyProvenance provenance;
    for (const auto& [domain, source_ids] : source.element_ids) {
        std::map<std::uint64_t, std::size_t> source_indices;
        for (std::size_t index = 0U; index < source_ids.size(); ++index) {
            source_indices.emplace(source_ids[index], index);
        }

        std::vector<std::vector<std::size_t>> mappings;
        const auto destination_domain = destination.element_ids.find(domain);
        if (destination_domain != destination.element_ids.end()) {
            mappings.reserve(destination_domain->second.size());
            for (const auto id : destination_domain->second) {
                const auto source_index = source_indices.find(id);
                if (source_index == source_indices.end()) {
                    mappings.emplace_back();
                } else {
                    mappings.push_back({source_index->second});
                }
            }
        }
        provenance.source_indices.emplace(domain, std::move(mappings));
    }
    for (const auto& [domain, destination_ids] : destination.element_ids) {
        if (source.element_ids.contains(domain)) continue;
        provenance.source_indices.emplace(
            domain, std::vector<std::vector<std::size_t>>(destination_ids.size()));
    }
    return core::Result<TopologyProvenance>::success(std::move(provenance));
}

} // namespace carto::attributes
