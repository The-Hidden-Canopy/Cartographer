#pragma once

#include <carto/attributes/set.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace carto::attributes {

enum class TransferMode {
    preserve,
    interpolate,
    duplicate,
    invalidate,
};

[[nodiscard]] bool valid_transfer_mode(TransferMode mode) noexcept;

struct AttributeTransferPolicy {
    TransferMode default_mode = TransferMode::invalidate;
    std::map<std::string, TransferMode> per_layer;

    [[nodiscard]] core::Result<void> validate(const AttributeSet& source) const;
    [[nodiscard]] TransferMode mode_for(std::string_view layer_id) const noexcept;
};

struct TopologyProvenance {
    // For each domain and new element, identify one or more source element
    // indices. The operation owning this mapping remains responsible for
    // stable domain IDs.
    std::map<AttributeDomain, std::vector<std::vector<std::size_t>>> source_indices;

    [[nodiscard]] core::Result<void> validate(const AttributeSet& source) const;
};

[[nodiscard]] core::Result<AttributeSet> transfer(
    const AttributeSet& source,
    TopologyProvenance provenance,
    const AttributeTransferPolicy& policy);

} // namespace carto::attributes
