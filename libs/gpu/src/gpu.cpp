#include <carto/gpu/rhi.hpp>

namespace carto::gpu {

static_assert(sizeof(BufferHandle) == sizeof(std::uint64_t));
static_assert(sizeof(TextureHandle) == sizeof(std::uint64_t));

} // namespace carto::gpu
