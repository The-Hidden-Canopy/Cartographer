#pragma once

#include <carto/assets/texture_mip.hpp>
#include <carto/core/result.hpp>
#include <carto/device_ir/command.hpp>
#include <carto/gpu/rhi.hpp>

#include <cstdint>
#include <vector>

namespace carto::d3d12 {

inline constexpr std::uint64_t kTextureDataPlacementAlignment = 512U;
inline constexpr std::uint32_t kTextureDataPitchAlignment = 256U;

struct TextureMipUploadFootprint {
    std::uint32_t mip_level = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t row_bytes = 0U;
    std::uint32_t row_pitch = 0U;
    std::uint64_t offset = 0U;
};

struct TextureUploadPlan {
    assets::Sha256Digest mip_artifact_digest;
    assets::Sha256Digest staging_digest;
    gpu::TextureDesc texture;
    std::vector<TextureMipUploadFootprint> footprints;
    std::vector<std::uint8_t> staging_bytes;
    std::uint64_t resident_bytes = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

// Converts a canonical RGBA8 mip artifact into the exact D3D12 upload-buffer
// layout. Tight source rows are copied into 256-byte-aligned rows and each
// subresource begins at a 512-byte-aligned offset.
[[nodiscard]] core::Result<TextureUploadPlan> build_texture_upload_plan(
    const assets::Rgba8MipChain& chain);

// Appends only copy commands. Resource creation, transitions, submission, and
// fence ownership remain explicit at the call site.
[[nodiscard]] core::Result<void> append_texture_upload_copies(
    device_ir::DeviceCommandStream& stream,
    gpu::BufferHandle source,
    gpu::TextureHandle destination,
    const TextureUploadPlan& plan);

} // namespace carto::d3d12
