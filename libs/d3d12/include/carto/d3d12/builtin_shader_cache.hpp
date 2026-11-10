#pragma once
#include <carto/d3d12/device.hpp>
#include <filesystem>

namespace carto::d3d12 {
struct BuiltinShaderCacheResult {
    device::ShaderBinary binary;
    bool hit = false, stored = false;
    double lookup_ms = 0, compile_ms = 0, store_ms = 0;
    std::string note;
};
// A local disposable cache for fixed embedded authoring shaders only. Never
// call this for authored/generated project shaders. No source, paths or debug
// symbols are stored. Includes and debug builds bypass persistent caching.
class BuiltinShaderCache final {
  public:
    explicit BuiltinShaderCache(std::filesystem::path directory)
        : directory_(std::move(directory)) {}
    [[nodiscard]] core::Result<BuiltinShaderCacheResult> compile(const ShaderSource &) const;
    [[nodiscard]] static core::Result<std::string> key(const ShaderSource &,
                                                       std::string_view runtime_fingerprint);
    [[nodiscard]] static std::filesystem::path default_directory();

  private:
    std::filesystem::path directory_;
};
} // namespace carto::d3d12
