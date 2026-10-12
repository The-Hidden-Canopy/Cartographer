#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/collision/primitive.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::collision {

// Deterministic partitioning strategies for compound primitive search.
enum class CompoundPartitionStrategy : std::uint8_t {
    // Recursively bisect vertex sets along the longest bounds axis at the
    // median vertex position. Ties resolve toward the earlier axis and the
    // lower median, keeping generation byte-stable.
    longest_axis_median_split,
};

[[nodiscard]] std::string_view compound_partition_strategy_name(
    CompoundPartitionStrategy strategy) noexcept;

struct CompoundSearchOptions {
    CompoundPartitionStrategy strategy =
        CompoundPartitionStrategy::longest_axis_median_split;
    std::uint32_t max_parts = 8U;
    std::uint32_t max_depth = 4U;
    std::uint32_t target_vertices_per_part = 64U;

    static constexpr std::uint32_t kHardMaxParts = 64U;
    static constexpr std::uint32_t kHardMaxDepth = 16U;
    static constexpr std::uint32_t kMinVerticesPerPart = 8U;

    [[nodiscard]] core::Result<void> validate() const;
};

// A compound candidate approximates one source mesh with several fitted
// primitives over a deterministic partition. Aggregate error combines the
// worst per-part fit with inter-part volume overlap so a runaway part count
// cannot hide overlap cost. Resident cost is explicit evidence, not a claim
// about any runtime embodiment.
struct CompoundCollisionCandidate {
    CompoundPartitionStrategy strategy =
        CompoundPartitionStrategy::longest_axis_median_split;
    std::vector<PrimitiveCollisionCandidate> parts;
    double aggregate_error = 0.0;
    double total_volume = 0.0;
    std::uint64_t fitted_vertex_count = 0U;
    assets::Sha256Digest candidate_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

// Searches one compound candidate. Each part receives the cheaper of a box
// and a sphere fit, chosen by fit error with a deterministic tie-break toward
// the box. Fails closed on invalid meshes, options, or unbounded recursion.
[[nodiscard]] core::Result<CompoundCollisionCandidate> search_compound_candidate(
    const geometry::CompiledMesh& mesh,
    const CompoundSearchOptions& options = {});

} // namespace carto::collision
