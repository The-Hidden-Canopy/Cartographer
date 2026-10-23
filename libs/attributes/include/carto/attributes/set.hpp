#pragma once

#include <carto/attributes/layer.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace carto::attributes {

class AttributeSet {
public:
    AttributeSet() = default;
    explicit AttributeSet(std::map<AttributeDomain, std::size_t> domain_counts)
        : domain_counts_(std::move(domain_counts)) {}

    [[nodiscard]] core::Result<void> set_domain_count(
        AttributeDomain domain,
        std::size_t count);
    [[nodiscard]] core::Result<void> add_layer(AttributeLayer layer);
    [[nodiscard]] core::Result<void> remove_layer(std::string_view id);
    [[nodiscard]] core::Result<void> validate() const;

    [[nodiscard]] const AttributeLayer* find(std::string_view id) const noexcept;
    [[nodiscard]] std::optional<std::size_t> domain_count(AttributeDomain domain) const noexcept;
    [[nodiscard]] const std::map<AttributeDomain, std::size_t>& domain_counts() const noexcept {
        return domain_counts_;
    }
    [[nodiscard]] std::size_t layer_count() const noexcept { return layers_.size(); }
    [[nodiscard]] const std::map<std::string, AttributeLayer>& layers() const noexcept {
        return layers_;
    }

private:
    std::map<AttributeDomain, std::size_t> domain_counts_;
    std::map<std::string, AttributeLayer> layers_;
};

} // namespace carto::attributes
