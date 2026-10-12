#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::archetype {

inline constexpr std::size_t kMaxAssemblyNodes = 65'536U;
inline constexpr std::size_t kMaxMaterialRegionsPerNode = 4'096U;

// Neutral description of one node inside an observed assembly. The mesh
// digest is content identity (zero for a pure grouping node); material
// regions are the stable source region identifiers carried by the mesh
// evidence. No display names, scene handles, or runtime identities.
struct AssemblyNodeContent {
    assets::Sha256Digest mesh_digest;
    std::vector<std::uint32_t> material_regions; // sorted and unique

    [[nodiscard]] core::Result<void> validate() const;
};

struct AssemblyLink {
    std::uint32_t child = 0U;
    std::uint32_t parent = 0U;
};

// A rooted forest observed in source content. Node indices are positional
// only; compute() canonicalizes them, so equal assemblies digest equally
// regardless of node naming, node numbering, sibling order, or link order.
struct AssemblyInput {
    std::vector<AssemblyNodeContent> nodes;
    std::vector<AssemblyLink> links;

    [[nodiscard]] core::Result<void> validate() const;
};

// Canonical structural identity of an assembly. `value` binds mesh content,
// hierarchy shape, and per-node material regions at once; the component
// digests are deterministic evidence that the three aspects agree with the
// bound value. Equal assemblies always produce equal digests; differing
// content, shape, or region assignment always produces differing digests.
struct StructuralDigest {
    assets::Sha256Digest mesh_content_digest;    // multiset of node mesh digests
    assets::Sha256Digest hierarchy_shape_digest; // shape with all node labels blanked
    assets::Sha256Digest material_region_digest; // union set of material region ids
    assets::Sha256Digest value;                  // full bound structural digest
    std::uint32_t node_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const StructuralDigest&) const noexcept = default;
    [[nodiscard]] std::string canonical() const;

    [[nodiscard]] static core::Result<StructuralDigest> compute(
        const AssemblyInput& input);
};

} // namespace carto::archetype
