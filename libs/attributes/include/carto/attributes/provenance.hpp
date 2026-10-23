#pragma once

#include <carto/attributes/transfer.hpp>

#include <cstdint>
#include <map>
#include <vector>

namespace carto::attributes {

// A kernel-owned snapshot of stable element identities. The kernel supplies
// the domain ordering; this adapter returns indices into that ordering.
struct StableTopologySnapshot {
    std::map<AttributeDomain, std::vector<std::uint64_t>> element_ids;

    [[nodiscard]] core::Result<void> validate() const;
};

// Builds source-index provenance by stable identity. An absent destination id
// produces an empty mapping, allowing an explicit invalidate policy to assign a
// typed default to newly-created elements. Removed source elements simply have
// no destination mapping.
[[nodiscard]] core::Result<TopologyProvenance> derive_provenance(
    const StableTopologySnapshot& source,
    const StableTopologySnapshot& destination);

} // namespace carto::attributes
