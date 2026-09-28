#pragma once

#include <carto/core/result.hpp>

#include <cstdint>
#include <string>

namespace carto::vulkan {

struct RuntimeInfo {
    std::uint32_t api_version = 0;
    std::string device_name;
};

class Runtime {
public:
    [[nodiscard]] static core::Result<RuntimeInfo> probe();
};

} // namespace carto::vulkan
