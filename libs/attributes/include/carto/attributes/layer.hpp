#pragma once

#include <carto/attributes/domain.hpp>
#include <carto/attributes/type.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace carto::attributes {

struct AttributeLayerDescriptor {
    std::string id;
    AttributeDomain domain = AttributeDomain::vertex;
    AttributeType type = AttributeType::float32;

    [[nodiscard]] core::Result<void> validate() const;
};

struct AttributeLayer {
    AttributeLayerDescriptor descriptor;
    std::vector<AttributeValue> values;

    [[nodiscard]] core::Result<void> validate(std::size_t element_count) const;
};

} // namespace carto::attributes
