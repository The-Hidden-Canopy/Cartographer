#include <carto/archetype/digest.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "text_format.hpp"

namespace carto::archetype {
namespace {

constexpr std::string_view kNodePrefix = "CARTO_ARCHETYPE_NODE_V1";
constexpr std::string_view kShapeNodePrefix = "CARTO_ARCHETYPE_SHAPE_NODE_V1";
constexpr std::string_view kAssemblyPrefix = "CARTO_ARCHETYPE_ASSEMBLY_V1";
constexpr std::string_view kMeshesPrefix = "CARTO_ARCHETYPE_MESHES_V1";
constexpr std::string_view kRegionsPrefix = "CARTO_ARCHETYPE_REGIONS_V1";

assets::Sha256Digest hash_node(
    std::string_view prefix,
    const AssemblyNodeContent& node,
    std::vector<assets::Sha256Digest> child_digests) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(64U + node.material_regions.size() * 4U +
        child_digests.size() * 32U);
    detail::append_text(bytes, prefix);
    detail::append_digest(bytes, node.mesh_digest);
    detail::append_u32be(bytes, static_cast<std::uint32_t>(node.material_regions.size()));
    for (const std::uint32_t region : node.material_regions) {
        detail::append_u32be(bytes, region);
    }
    std::sort(child_digests.begin(), child_digests.end());
    detail::append_u32be(bytes, static_cast<std::uint32_t>(child_digests.size()));
    for (const auto& child : child_digests) {
        detail::append_digest(bytes, child);
    }
    return detail::digest_bytes(bytes);
}

assets::Sha256Digest hash_shape_node(std::vector<assets::Sha256Digest> child_digests) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(64U + child_digests.size() * 32U);
    detail::append_text(bytes, kShapeNodePrefix);
    std::sort(child_digests.begin(), child_digests.end());
    detail::append_u32be(bytes, static_cast<std::uint32_t>(child_digests.size()));
    for (const auto& child : child_digests) {
        detail::append_digest(bytes, child);
    }
    return detail::digest_bytes(bytes);
}

} // namespace

core::Result<void> AssemblyNodeContent::validate() const {
    if (material_regions.size() > kMaxMaterialRegionsPerNode ||
        !std::is_sorted(material_regions.begin(), material_regions.end()) ||
        std::adjacent_find(material_regions.begin(), material_regions.end()) !=
            material_regions.end()) {
        return core::Result<void>::failure(detail::invalid_text(
            "assembly node material regions must be bounded, sorted, and unique"));
    }
    return core::Result<void>::success();
}

core::Result<void> AssemblyInput::validate() const {
    if (nodes.empty() || nodes.size() > kMaxAssemblyNodes) {
        return core::Result<void>::failure(detail::invalid_text(
            "assembly node count is empty or exceeds its bounded capacity"));
    }
    if (links.size() >= nodes.size()) {
        return core::Result<void>::failure(detail::invalid_text(
            "assembly links must describe a forest with fewer links than nodes"));
    }
    for (const auto& node : nodes) {
        if (auto result = node.validate(); !result) return result;
    }
    std::vector<bool> has_parent(nodes.size(), false);
    for (const auto& link : links) {
        if (link.child >= nodes.size() || link.parent >= nodes.size() ||
            link.child == link.parent) {
            return core::Result<void>::failure(detail::invalid_text(
                "assembly link endpoint is out of range or self-referential"));
        }
        if (has_parent[link.child]) {
            return core::Result<void>::failure(detail::invalid_text(
                "assembly node has more than one parent"));
        }
        has_parent[link.child] = true;
    }
    return core::Result<void>::success();
}

core::Result<void> StructuralDigest::validate() const {
    if (node_count == 0U || mesh_content_digest.is_zero() ||
        hierarchy_shape_digest.is_zero() || material_region_digest.is_zero() ||
        value.is_zero()) {
        return core::Result<void>::failure(detail::invalid_text(
            "structural digest components and node count must be present"));
    }
    return core::Result<void>::success();
}

std::string StructuralDigest::canonical() const {
    std::ostringstream output;
    output << "STRUCTURAL " << mesh_content_digest.hex() << ' '
           << hierarchy_shape_digest.hex() << ' ' << material_region_digest.hex() << ' '
           << value.hex() << ' ' << node_count;
    return output.str();
}

core::Result<StructuralDigest> StructuralDigest::compute(const AssemblyInput& input) {
    if (auto result = input.validate(); !result) {
        return core::Result<StructuralDigest>::failure(result.error());
    }
    const std::size_t node_total = input.nodes.size();

    std::vector<std::uint32_t> parent(node_total, std::numeric_limits<std::uint32_t>::max());
    std::vector<std::vector<std::uint32_t>> children(node_total);
    for (const auto& link : input.links) {
        parent[link.child] = link.parent;
        children[link.parent].push_back(link.child);
    }

    // Leaves-first traversal: a node is hashed once every child is hashed, so
    // deep hierarchies never recurse. Child digests are sorted before hashing,
    // which makes sibling order and node numbering canonicalization-free.
    std::vector<assets::Sha256Digest> node_digests(node_total);
    std::vector<assets::Sha256Digest> shape_digests(node_total);
    std::vector<std::uint32_t> unhashed_children(node_total);
    std::vector<std::uint32_t> ready;
    ready.reserve(node_total);
    for (std::size_t index = 0U; index < node_total; ++index) {
        unhashed_children[index] = static_cast<std::uint32_t>(children[index].size());
        if (children[index].empty()) ready.push_back(static_cast<std::uint32_t>(index));
    }

    std::size_t hashed = 0U;
    std::vector<assets::Sha256Digest> child_digests;
    std::vector<assets::Sha256Digest> child_shapes;
    while (!ready.empty()) {
        const std::uint32_t current = ready.back();
        ready.pop_back();
        child_digests.clear();
        child_shapes.clear();
        child_digests.reserve(children[current].size());
        child_shapes.reserve(children[current].size());
        for (const std::uint32_t child : children[current]) {
            child_digests.push_back(node_digests[child]);
            child_shapes.push_back(shape_digests[child]);
        }
        node_digests[current] = hash_node(kNodePrefix, input.nodes[current], child_digests);
        shape_digests[current] = hash_shape_node(std::move(child_shapes));
        ++hashed;
        const std::uint32_t ancestor = parent[current];
        if (ancestor != std::numeric_limits<std::uint32_t>::max() &&
            --unhashed_children[ancestor] == 0U) {
            ready.push_back(ancestor);
        }
    }
    if (hashed != node_total) {
        return core::Result<StructuralDigest>::failure(detail::invalid_text(
            "assembly hierarchy contains a cycle"));
    }

    std::vector<assets::Sha256Digest> root_digests;
    std::vector<assets::Sha256Digest> mesh_multiset;
    std::vector<std::uint32_t> region_union;
    root_digests.reserve(node_total);
    mesh_multiset.reserve(node_total);
    for (std::size_t index = 0U; index < node_total; ++index) {
        if (parent[index] == std::numeric_limits<std::uint32_t>::max()) {
            root_digests.push_back(node_digests[index]);
        }
        if (!input.nodes[index].mesh_digest.is_zero()) {
            mesh_multiset.push_back(input.nodes[index].mesh_digest);
        }
        region_union.insert(region_union.end(),
            input.nodes[index].material_regions.begin(),
            input.nodes[index].material_regions.end());
    }
    std::sort(root_digests.begin(), root_digests.end());
    std::sort(mesh_multiset.begin(), mesh_multiset.end());
    std::sort(region_union.begin(), region_union.end());
    region_union.erase(std::unique(region_union.begin(), region_union.end()),
        region_union.end());

    std::vector<std::uint8_t> assembly_bytes;
    detail::append_text(assembly_bytes, kAssemblyPrefix);
    detail::append_u64be(assembly_bytes, static_cast<std::uint64_t>(node_total));
    detail::append_u32be(assembly_bytes, static_cast<std::uint32_t>(root_digests.size()));
    for (const auto& root : root_digests) {
        detail::append_digest(assembly_bytes, root);
    }

    std::vector<std::uint8_t> mesh_bytes;
    detail::append_text(mesh_bytes, kMeshesPrefix);
    detail::append_u32be(mesh_bytes, static_cast<std::uint32_t>(mesh_multiset.size()));
    for (const auto& mesh : mesh_multiset) {
        detail::append_digest(mesh_bytes, mesh);
    }

    std::vector<std::uint8_t> region_bytes;
    detail::append_text(region_bytes, kRegionsPrefix);
    detail::append_u32be(region_bytes, static_cast<std::uint32_t>(region_union.size()));
    for (const std::uint32_t region : region_union) {
        detail::append_u32be(region_bytes, region);
    }

    StructuralDigest digest;
    digest.node_count = static_cast<std::uint32_t>(node_total);
    digest.value = detail::digest_bytes(assembly_bytes);
    digest.mesh_content_digest = detail::digest_bytes(mesh_bytes);
    digest.material_region_digest = detail::digest_bytes(region_bytes);

    // The shape digest reuses the same leaves-first digests with every node
    // label blanked, so it isolates pure hierarchy shape.
    std::vector<std::uint8_t> shape_bytes;
    detail::append_text(shape_bytes, kAssemblyPrefix);
    detail::append_text(shape_bytes, " SHAPE");
    detail::append_u64be(shape_bytes, static_cast<std::uint64_t>(node_total));
    std::vector<assets::Sha256Digest> root_shapes;
    for (std::size_t index = 0U; index < node_total; ++index) {
        if (parent[index] == std::numeric_limits<std::uint32_t>::max()) {
            root_shapes.push_back(shape_digests[index]);
        }
    }
    std::sort(root_shapes.begin(), root_shapes.end());
    detail::append_u32be(shape_bytes, static_cast<std::uint32_t>(root_shapes.size()));
    for (const auto& root : root_shapes) {
        detail::append_digest(shape_bytes, root);
    }
    digest.hierarchy_shape_digest = detail::digest_bytes(shape_bytes);

    return core::Result<StructuralDigest>::success(std::move(digest));
}

} // namespace carto::archetype
