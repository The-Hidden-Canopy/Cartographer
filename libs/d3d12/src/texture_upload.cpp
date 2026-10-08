#include <carto/d3d12/texture_upload.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace carto::d3d12 {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    const std::uint64_t remainder = value % alignment;
    return remainder == 0U ? value : value + alignment - remainder;
}

} // namespace

core::Result<void> TextureUploadPlan::validate() const {
    if (mip_artifact_digest.is_zero() || staging_digest.is_zero() ||
        assets::sha256(staging_bytes) != staging_digest ||
        texture.dimension != gpu::TextureDimension::texture_2d ||
        texture.depth != 1U || texture.layers != 1U ||
        (texture.format != gpu::Format::rgba8_unorm &&
         texture.format != gpu::Format::rgba8_srgb) ||
        !gpu::has_usage(texture.usage, gpu::TextureUsage::sampled) ||
        !gpu::has_usage(texture.usage, gpu::TextureUsage::transfer_destination) ||
        footprints.empty() || footprints.size() != texture.mip_levels ||
        staging_bytes.empty() ||
        staging_bytes.size() > assets::kMaxRgba8MipArtifactBytes) {
        return core::Result<void>::failure(validation(
            "D3D12 texture upload plan identity or descriptor is invalid"));
    }
    std::uint32_t expected_width = texture.width;
    std::uint32_t expected_height = texture.height;
    std::uint64_t expected_resident_bytes = 0U;
    std::uint64_t previous_end = 0U;
    for (std::size_t index = 0U; index < footprints.size(); ++index) {
        const TextureMipUploadFootprint& footprint = footprints[index];
        const std::uint64_t expected_row_bytes =
            static_cast<std::uint64_t>(expected_width) * 4U;
        if (footprint.mip_level != index || footprint.width != expected_width ||
            footprint.height != expected_height ||
            footprint.row_bytes != expected_row_bytes ||
            footprint.row_pitch < footprint.row_bytes ||
            footprint.row_pitch % kTextureDataPitchAlignment != 0U ||
            footprint.offset % kTextureDataPlacementAlignment != 0U ||
            footprint.offset < previous_end) {
            return core::Result<void>::failure(validation(
                "D3D12 texture upload footprint is non-canonical"));
        }
        const std::uint64_t footprint_bytes =
            static_cast<std::uint64_t>(footprint.row_pitch) * footprint.height;
        if (footprint.offset > staging_bytes.size() ||
            footprint_bytes > staging_bytes.size() - footprint.offset ||
            expected_resident_bytes > std::numeric_limits<std::uint64_t>::max() -
                expected_row_bytes * expected_height) {
            return core::Result<void>::failure(validation(
                "D3D12 texture upload footprint exceeds staging bounds"));
        }
        previous_end = footprint.offset + footprint_bytes;
        expected_resident_bytes += expected_row_bytes * expected_height;
        expected_width = std::max(1U, expected_width / 2U);
        expected_height = std::max(1U, expected_height / 2U);
    }
    if (previous_end != staging_bytes.size() ||
        expected_resident_bytes != resident_bytes ||
        footprints.back().width != 1U || footprints.back().height != 1U) {
        return core::Result<void>::failure(validation(
            "D3D12 texture upload totals or terminal mip are invalid"));
    }
    return gpu::validate(texture);
}

core::Result<TextureUploadPlan> build_texture_upload_plan(
    const assets::Rgba8MipChain& chain) {
    if (auto result = chain.validate(); !result) {
        return core::Result<TextureUploadPlan>::failure(result.error());
    }
    TextureUploadPlan plan;
    plan.mip_artifact_digest = chain.artifact_digest;
    plan.texture = gpu::TextureDesc{
        chain.levels.front().width,
        chain.levels.front().height,
        1U,
        1U,
        static_cast<std::uint32_t>(chain.levels.size()),
        chain.color_space == assets::ColorSpace::srgb
            ? gpu::Format::rgba8_srgb
            : gpu::Format::rgba8_unorm,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
    };

    std::uint64_t cursor = 0U;
    for (std::size_t index = 0U; index < chain.levels.size(); ++index) {
        const assets::Rgba8MipLevel& level = chain.levels[index];
        const std::uint64_t row_bytes = static_cast<std::uint64_t>(level.width) * 4U;
        const std::uint64_t row_pitch = align_up(row_bytes, kTextureDataPitchAlignment);
        const std::uint64_t offset = align_up(cursor, kTextureDataPlacementAlignment);
        const std::uint64_t footprint_bytes = row_pitch * level.height;
        if (row_bytes > std::numeric_limits<std::uint32_t>::max() ||
            row_pitch > std::numeric_limits<std::uint32_t>::max() ||
            offset > assets::kMaxRgba8MipArtifactBytes ||
            footprint_bytes > assets::kMaxRgba8MipArtifactBytes - offset) {
            return core::Result<TextureUploadPlan>::failure(validation(
                "D3D12 texture upload layout exceeds bounded address space"));
        }
        plan.footprints.push_back({
            static_cast<std::uint32_t>(index),
            level.width,
            level.height,
            static_cast<std::uint32_t>(row_bytes),
            static_cast<std::uint32_t>(row_pitch),
            offset,
        });
        cursor = offset + footprint_bytes;
        plan.resident_bytes += level.pixels.size();
    }
    plan.staging_bytes.assign(static_cast<std::size_t>(cursor), 0U);
    for (std::size_t index = 0U; index < chain.levels.size(); ++index) {
        const assets::Rgba8MipLevel& level = chain.levels[index];
        const TextureMipUploadFootprint& footprint = plan.footprints[index];
        for (std::uint32_t row = 0U; row < level.height; ++row) {
            const std::size_t source_offset =
                static_cast<std::size_t>(row) * footprint.row_bytes;
            const std::size_t destination_offset =
                static_cast<std::size_t>(footprint.offset) +
                static_cast<std::size_t>(row) * footprint.row_pitch;
            std::copy_n(
                level.pixels.begin() + static_cast<std::ptrdiff_t>(source_offset),
                footprint.row_bytes,
                plan.staging_bytes.begin() +
                    static_cast<std::ptrdiff_t>(destination_offset));
        }
    }
    plan.staging_digest = assets::sha256(plan.staging_bytes);
    if (auto result = plan.validate(); !result) {
        return core::Result<TextureUploadPlan>::failure(result.error());
    }
    return core::Result<TextureUploadPlan>::success(std::move(plan));
}

core::Result<void> append_texture_upload_copies(
    device_ir::DeviceCommandStream& stream,
    gpu::BufferHandle source,
    gpu::TextureHandle destination,
    const TextureUploadPlan& plan) {
    if (!source || !destination) {
        return core::Result<void>::failure(invalid(
            "D3D12 texture upload copies require live resource handles"));
    }
    if (auto result = plan.validate(); !result) return result;
    for (const TextureMipUploadFootprint& footprint : plan.footprints) {
        stream.append(device_ir::CmdCopyBufferToTexture{
            source, destination, footprint.offset, footprint.mip_level});
    }
    return core::Result<void>::success();
}

} // namespace carto::d3d12
