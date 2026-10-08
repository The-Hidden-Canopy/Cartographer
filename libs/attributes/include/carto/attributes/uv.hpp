#pragma once

#include <carto/attributes/set.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace carto::attributes {

using UvSetId = std::uint64_t;

struct UvSetDescriptor {
    UvSetId id = 0U;
    std::string name;
    // The coordinate payload remains an existing corner-domain vec2 layer.
    std::string layer_id;
    bool active_for_editing = false;
    bool active_for_render = false;
    bool lightmap_candidate = false;
    std::uint32_t tile_policy_version = 1U;

    [[nodiscard]] core::Result<void> validate() const;
};

// Explicit UV identity and active-set policy layered over AttributeSet. This
// class never stores a second coordinate payload, so topology transfer remains
// governed by the generic corner-domain attribute machinery.
class UvSetTable {
public:
    [[nodiscard]] core::Result<void> add(UvSetDescriptor descriptor);
    [[nodiscard]] core::Result<void> remove(
        UvSetId id,
        std::span<const UvSetId> referenced_sets = {});
    [[nodiscard]] core::Result<void> set_active_for_editing(UvSetId id);
    [[nodiscard]] core::Result<void> set_active_for_render(UvSetId id);
    [[nodiscard]] core::Result<void> validate(const AttributeSet& attributes) const;

    [[nodiscard]] const UvSetDescriptor* find(UvSetId id) const noexcept;
    [[nodiscard]] const UvSetDescriptor* active_for_editing() const noexcept;
    [[nodiscard]] const UvSetDescriptor* active_for_render() const noexcept;
    [[nodiscard]] const std::map<UvSetId, UvSetDescriptor>& descriptors() const noexcept {
        return descriptors_;
    }

private:
    std::map<UvSetId, UvSetDescriptor> descriptors_;
};

} // namespace carto::attributes
