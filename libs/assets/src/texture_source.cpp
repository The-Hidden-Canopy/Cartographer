#include <carto/assets/texture_source.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace carto::assets {
namespace {

struct HeaderInfo {
    TextureSourceFormat format = TextureSourceFormat::png;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t channels = 0U;
    std::uint32_t bit_depth = 0U;
    std::uint32_t mip_levels = 1U;
    bool compressed = false;
    bool high_dynamic_range = false;
};

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_texture_source_format(TextureSourceFormat format) {
    switch (format) {
    case TextureSourceFormat::png:
    case TextureSourceFormat::jpeg:
    case TextureSourceFormat::tga:
    case TextureSourceFormat::dds:
    case TextureSourceFormat::exr:
        return true;
    }
    return false;
}

std::uint16_t read_le16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t read_le32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0U;
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return value;
}

std::uint32_t read_be32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0U;
    for (unsigned index = 0U; index < 4U; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

std::uint32_t png_crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

bool media_type_matches(TextureSourceFormat format, std::string_view declared) {
    if (declared.empty() || declared == texture_source_media_type(format)) return true;
    switch (format) {
    case TextureSourceFormat::jpeg: return declared == "image/jpg";
    case TextureSourceFormat::tga: return declared == "image/tga";
    case TextureSourceFormat::dds: return declared == "image/dds";
    case TextureSourceFormat::exr: return declared == "image/exr";
    case TextureSourceFormat::png: return false;
    }
    return false;
}

core::Result<HeaderInfo> inspect_png(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 8U> signature{
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    if (bytes.size() < 45U || !std::equal(signature.begin(), signature.end(), bytes.begin())) {
        return core::Result<HeaderInfo>::failure(invalid("PNG source is truncated"));
    }

    HeaderInfo info;
    info.format = TextureSourceFormat::png;
    bool first = true;
    bool saw_idat = false;
    bool saw_iend = false;
    std::size_t cursor = signature.size();
    while (cursor < bytes.size()) {
        if (bytes.size() - cursor < 12U) {
            return core::Result<HeaderInfo>::failure(invalid("PNG chunk header is truncated"));
        }
        const std::uint32_t length = read_be32(bytes, cursor);
        if (length > bytes.size() - cursor - 12U) {
            return core::Result<HeaderInfo>::failure(invalid("PNG chunk exceeds source bytes"));
        }
        const std::size_t type_offset = cursor + 4U;
        const std::size_t data_offset = type_offset + 4U;
        const std::size_t crc_offset = data_offset + length;
        const std::string_view type{
            reinterpret_cast<const char*>(bytes.data() + type_offset), 4U};
        const std::uint32_t expected_crc = read_be32(bytes, crc_offset);
        const std::uint32_t actual_crc = png_crc32(bytes.subspan(type_offset, 4U + length));
        if (expected_crc != actual_crc) {
            return core::Result<HeaderInfo>::failure(validation("PNG chunk CRC mismatch"));
        }
        if (first) {
            if (type != "IHDR" || length != 13U) {
                return core::Result<HeaderInfo>::failure(validation(
                    "PNG must begin with a 13-byte IHDR chunk"));
            }
            info.width = read_be32(bytes, data_offset);
            info.height = read_be32(bytes, data_offset + 4U);
            info.bit_depth = bytes[data_offset + 8U];
            const std::uint8_t color_type = bytes[data_offset + 9U];
            switch (color_type) {
            case 0U: info.channels = 1U; break;
            case 2U: info.channels = 3U; break;
            case 3U: info.channels = 1U; break;
            case 4U: info.channels = 2U; break;
            case 6U: info.channels = 4U; break;
            default:
                return core::Result<HeaderInfo>::failure(validation(
                    "PNG color type is unsupported"));
            }
            const bool legal_depth =
                (color_type == 0U && (info.bit_depth == 1U || info.bit_depth == 2U ||
                                      info.bit_depth == 4U || info.bit_depth == 8U ||
                                      info.bit_depth == 16U)) ||
                (color_type == 2U && (info.bit_depth == 8U || info.bit_depth == 16U)) ||
                (color_type == 3U && (info.bit_depth == 1U || info.bit_depth == 2U ||
                                      info.bit_depth == 4U || info.bit_depth == 8U)) ||
                ((color_type == 4U || color_type == 6U) &&
                 (info.bit_depth == 8U || info.bit_depth == 16U));
            if (!legal_depth || bytes[data_offset + 10U] != 0U ||
                bytes[data_offset + 11U] != 0U || bytes[data_offset + 12U] > 1U) {
                return core::Result<HeaderInfo>::failure(validation(
                    "PNG IHDR fields are unsupported"));
            }
            first = false;
        } else if (type == "IHDR") {
            return core::Result<HeaderInfo>::failure(validation("PNG contains duplicate IHDR"));
        }
        if (type == "IDAT") saw_idat = true;
        if (type == "IEND") {
            if (length != 0U || !saw_idat) {
                return core::Result<HeaderInfo>::failure(validation(
                    "PNG IEND requires prior image data and an empty payload"));
            }
            saw_iend = true;
            cursor = crc_offset + 4U;
            break;
        }
        cursor = crc_offset + 4U;
    }
    if (!saw_iend || cursor != bytes.size()) {
        return core::Result<HeaderInfo>::failure(validation(
            "PNG is missing IEND or contains trailing polyglot data"));
    }
    return core::Result<HeaderInfo>::success(info);
}

bool jpeg_sof_marker(std::uint8_t marker) {
    switch (marker) {
    case 0xc0U: case 0xc1U: case 0xc2U: case 0xc3U:
    case 0xc5U: case 0xc6U: case 0xc7U:
    case 0xc9U: case 0xcaU: case 0xcbU:
    case 0xcdU: case 0xceU: case 0xcfU:
        return true;
    default:
        return false;
    }
}

core::Result<HeaderInfo> inspect_jpeg(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 6U || bytes[0U] != 0xffU || bytes[1U] != 0xd8U ||
        bytes[bytes.size() - 2U] != 0xffU || bytes.back() != 0xd9U) {
        return core::Result<HeaderInfo>::failure(invalid(
            "JPEG source requires bounded SOI and EOI markers"));
    }
    HeaderInfo info;
    info.format = TextureSourceFormat::jpeg;
    bool found_sof = false;
    std::size_t cursor = 2U;
    while (cursor + 1U < bytes.size() - 2U) {
        if (bytes[cursor] != 0xffU) {
            return core::Result<HeaderInfo>::failure(validation(
                "JPEG marker stream is malformed before scan data"));
        }
        while (cursor < bytes.size() && bytes[cursor] == 0xffU) ++cursor;
        if (cursor >= bytes.size()) break;
        const std::uint8_t marker = bytes[cursor++];
        if (marker == 0xdaU) break;
        if (marker == 0xd8U || marker == 0xd9U || (marker >= 0xd0U && marker <= 0xd7U) ||
            marker == 0x01U) {
            continue;
        }
        if (cursor + 2U > bytes.size()) {
            return core::Result<HeaderInfo>::failure(invalid("JPEG segment length is truncated"));
        }
        const std::uint16_t length = static_cast<std::uint16_t>(bytes[cursor] << 8U) |
            bytes[cursor + 1U];
        if (length < 2U || length > bytes.size() - cursor) {
            return core::Result<HeaderInfo>::failure(validation(
                "JPEG segment exceeds source bytes"));
        }
        if (jpeg_sof_marker(marker)) {
            if (length < 8U) {
                return core::Result<HeaderInfo>::failure(validation(
                    "JPEG SOF segment is too short"));
            }
            info.bit_depth = bytes[cursor + 2U];
            info.height = static_cast<std::uint32_t>(bytes[cursor + 3U] << 8U) |
                bytes[cursor + 4U];
            info.width = static_cast<std::uint32_t>(bytes[cursor + 5U] << 8U) |
                bytes[cursor + 6U];
            info.channels = bytes[cursor + 7U];
            const std::uint32_t expected_length = 8U + info.channels * 3U;
            if (length != expected_length || info.channels == 0U || info.channels > 4U ||
                info.bit_depth == 0U || info.bit_depth > 16U) {
                return core::Result<HeaderInfo>::failure(validation(
                    "JPEG SOF channel or precision contract is unsupported"));
            }
            found_sof = true;
        }
        cursor += length;
    }
    if (!found_sof) {
        return core::Result<HeaderInfo>::failure(validation(
            "JPEG source has no supported start-of-frame segment"));
    }
    return core::Result<HeaderInfo>::success(info);
}

core::Result<HeaderInfo> inspect_tga(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 18U || bytes[1U] != 0U ||
        (bytes[2U] != 2U && bytes[2U] != 10U)) {
        return core::Result<HeaderInfo>::failure(invalid(
            "source is not a supported true-color TGA container"));
    }
    HeaderInfo info;
    info.format = TextureSourceFormat::tga;
    info.width = read_le16(bytes, 12U);
    info.height = read_le16(bytes, 14U);
    info.bit_depth = bytes[16U];
    if (info.bit_depth != 24U && info.bit_depth != 32U) {
        return core::Result<HeaderInfo>::failure(validation(
            "TGA source must use 24- or 32-bit true color"));
    }
    info.channels = info.bit_depth / 8U;
    info.compressed = bytes[2U] == 10U;
    const std::uint8_t alpha_bits = bytes[17U] & 0x0fU;
    if ((info.bit_depth == 24U && alpha_bits != 0U) ||
        (info.bit_depth == 32U && alpha_bits != 0U && alpha_bits != 8U)) {
        return core::Result<HeaderInfo>::failure(validation(
            "TGA alpha-bit declaration is unsupported"));
    }
    const std::size_t pixel_bytes = info.channels;
    const std::size_t data_offset = 18U + bytes[0U];
    if (data_offset > bytes.size()) {
        return core::Result<HeaderInfo>::failure(invalid("TGA image ID exceeds source bytes"));
    }
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(info.width) * info.height;
    if (bytes[2U] == 2U) {
        const std::uint64_t required = pixel_count * pixel_bytes;
        if (required > bytes.size() - data_offset) {
            return core::Result<HeaderInfo>::failure(validation(
                "uncompressed TGA pixel payload is truncated"));
        }
    } else {
        std::uint64_t decoded_pixels = 0U;
        std::size_t cursor = data_offset;
        while (decoded_pixels < pixel_count) {
            if (cursor >= bytes.size()) {
                return core::Result<HeaderInfo>::failure(validation(
                    "RLE TGA packet stream is truncated"));
            }
            const std::uint8_t packet = bytes[cursor++];
            const std::uint64_t count = (packet & 0x7fU) + 1U;
            if (count > pixel_count - decoded_pixels) {
                return core::Result<HeaderInfo>::failure(validation(
                    "RLE TGA packet exceeds the declared pixel count"));
            }
            const std::uint64_t payload = (packet & 0x80U) != 0U
                ? pixel_bytes
                : count * pixel_bytes;
            if (payload > bytes.size() - cursor) {
                return core::Result<HeaderInfo>::failure(validation(
                    "RLE TGA packet payload is truncated"));
            }
            cursor += static_cast<std::size_t>(payload);
            decoded_pixels += count;
        }
    }
    return core::Result<HeaderInfo>::success(info);
}

core::Result<HeaderInfo> inspect_dds(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 4U> magic{'D', 'D', 'S', ' '};
    if (bytes.size() < 129U || !std::equal(magic.begin(), magic.end(), bytes.begin()) ||
        read_le32(bytes, 4U) != 124U || read_le32(bytes, 76U) != 32U) {
        return core::Result<HeaderInfo>::failure(invalid("DDS header is truncated or invalid"));
    }
    HeaderInfo info;
    info.format = TextureSourceFormat::dds;
    info.height = read_le32(bytes, 12U);
    info.width = read_le32(bytes, 16U);
    info.mip_levels = std::max(1U, read_le32(bytes, 28U));
    const std::uint32_t pixel_flags = read_le32(bytes, 80U);
    const std::uint32_t fourcc = read_le32(bytes, 84U);
    info.compressed = (pixel_flags & 0x4U) != 0U;
    std::size_t payload_offset = 128U;
    constexpr std::uint32_t dx10 =
        static_cast<std::uint32_t>('D') | (static_cast<std::uint32_t>('X') << 8U) |
        (static_cast<std::uint32_t>('1') << 16U) | (static_cast<std::uint32_t>('0') << 24U);
    if (info.compressed && fourcc == dx10) {
        if (bytes.size() < 149U) {
            return core::Result<HeaderInfo>::failure(invalid("DDS DX10 header is truncated"));
        }
        payload_offset = 148U;
        const std::uint32_t dxgi_format = read_le32(bytes, 128U);
        info.high_dynamic_range = dxgi_format == 95U || dxgi_format == 96U;
        if (dxgi_format == 98U || dxgi_format == 99U || info.high_dynamic_range) {
            info.channels = 4U;
        }
    } else if (!info.compressed) {
        const std::uint32_t rgb_bits = read_le32(bytes, 88U);
        if (rgb_bits == 0U || rgb_bits > 128U || rgb_bits % 8U != 0U) {
            return core::Result<HeaderInfo>::failure(validation(
                "DDS RGB bit depth is unsupported"));
        }
        info.channels = (pixel_flags & 0x1U) != 0U ? 4U : 3U;
        info.bit_depth = rgb_bits / info.channels;
    }
    if (payload_offset >= bytes.size()) {
        return core::Result<HeaderInfo>::failure(validation("DDS contains no image payload"));
    }
    return core::Result<HeaderInfo>::success(info);
}

bool read_cstring(
    std::span<const std::uint8_t> bytes,
    std::size_t& cursor,
    std::size_t end,
    std::string_view& value) {
    if (cursor >= end) return false;
    const std::size_t begin = cursor;
    while (cursor < end && bytes[cursor] != 0U) {
        if (cursor - begin >= 255U) return false;
        ++cursor;
    }
    if (cursor >= end) return false;
    value = std::string_view{
        reinterpret_cast<const char*>(bytes.data() + begin), cursor - begin};
    ++cursor;
    return true;
}

core::Result<HeaderInfo> inspect_exr(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 10U || read_le32(bytes, 0U) != 0x01312f76U) {
        return core::Result<HeaderInfo>::failure(invalid("EXR magic or header is invalid"));
    }
    HeaderInfo info;
    info.format = TextureSourceFormat::exr;
    info.high_dynamic_range = true;
    bool saw_data_window = false;
    bool saw_header_end = false;
    std::size_t cursor = 8U;
    while (cursor < bytes.size()) {
        std::string_view name;
        if (!read_cstring(bytes, cursor, bytes.size(), name)) {
            return core::Result<HeaderInfo>::failure(invalid("EXR attribute name is truncated"));
        }
        if (name.empty()) {
            saw_header_end = true;
            break;
        }
        std::string_view type;
        if (!read_cstring(bytes, cursor, bytes.size(), type) ||
            bytes.size() - cursor < 4U) {
            return core::Result<HeaderInfo>::failure(invalid("EXR attribute type is truncated"));
        }
        const std::uint32_t size = read_le32(bytes, cursor);
        cursor += 4U;
        if (size > 16U * 1024U * 1024U || size > bytes.size() - cursor) {
            return core::Result<HeaderInfo>::failure(validation(
                "EXR attribute payload exceeds its bounds"));
        }
        const std::size_t value_end = cursor + size;
        if (name == "dataWindow" && type == "box2i") {
            if (size != 16U) {
                return core::Result<HeaderInfo>::failure(validation(
                    "EXR dataWindow must be a box2i"));
            }
            const std::int32_t minimum_x = static_cast<std::int32_t>(read_le32(bytes, cursor));
            const std::int32_t minimum_y = static_cast<std::int32_t>(read_le32(bytes, cursor + 4U));
            const std::int32_t maximum_x = static_cast<std::int32_t>(read_le32(bytes, cursor + 8U));
            const std::int32_t maximum_y = static_cast<std::int32_t>(read_le32(bytes, cursor + 12U));
            const std::int64_t width =
                static_cast<std::int64_t>(maximum_x) - minimum_x + 1;
            const std::int64_t height =
                static_cast<std::int64_t>(maximum_y) - minimum_y + 1;
            if (width <= 0 || height <= 0 ||
                width > std::numeric_limits<std::uint32_t>::max() ||
                height > std::numeric_limits<std::uint32_t>::max()) {
                return core::Result<HeaderInfo>::failure(validation(
                    "EXR dataWindow dimensions are invalid"));
            }
            info.width = static_cast<std::uint32_t>(width);
            info.height = static_cast<std::uint32_t>(height);
            saw_data_window = true;
        } else if (name == "channels" && type == "chlist") {
            std::size_t channel_cursor = cursor;
            std::uint32_t channels = 0U;
            std::uint32_t bit_depth = 0U;
            while (channel_cursor < value_end) {
                std::string_view channel_name;
                if (!read_cstring(bytes, channel_cursor, value_end, channel_name)) {
                    return core::Result<HeaderInfo>::failure(validation(
                        "EXR channel list is malformed"));
                }
                if (channel_name.empty()) break;
                if (value_end - channel_cursor < 16U || channels == 4U) {
                    return core::Result<HeaderInfo>::failure(validation(
                        "EXR channel list exceeds the public channel bound"));
                }
                const std::uint32_t pixel_type = read_le32(bytes, channel_cursor);
                if (pixel_type > 2U) {
                    return core::Result<HeaderInfo>::failure(validation(
                        "EXR channel pixel type is unsupported"));
                }
                bit_depth = std::max(bit_depth, pixel_type == 1U ? 16U : 32U);
                channel_cursor += 16U;
                ++channels;
            }
            info.channels = channels;
            info.bit_depth = bit_depth;
        }
        cursor = value_end;
    }
    if (!saw_header_end || !saw_data_window || cursor >= bytes.size()) {
        return core::Result<HeaderInfo>::failure(validation(
            "EXR requires a terminated header, dataWindow, and pixel payload"));
    }
    return core::Result<HeaderInfo>::success(info);
}

} // namespace

const char* texture_source_format_name(TextureSourceFormat format) noexcept {
    switch (format) {
    case TextureSourceFormat::png: return "png";
    case TextureSourceFormat::jpeg: return "jpeg";
    case TextureSourceFormat::tga: return "tga";
    case TextureSourceFormat::dds: return "dds";
    case TextureSourceFormat::exr: return "exr";
    }
    return "unknown";
}

const char* texture_source_media_type(TextureSourceFormat format) noexcept {
    switch (format) {
    case TextureSourceFormat::png: return "image/png";
    case TextureSourceFormat::jpeg: return "image/jpeg";
    case TextureSourceFormat::tga: return "image/x-tga";
    case TextureSourceFormat::dds: return "image/vnd-ms.dds";
    case TextureSourceFormat::exr: return "image/x-exr";
    }
    return "application/octet-stream";
}

core::Result<void> TextureSourceLimits::validate() const {
    if (max_source_bytes == 0U || max_dimension == 0U || max_pixels == 0U ||
        max_mip_levels == 0U ||
        max_source_bytes > kTextureSourceAbsoluteMaxBytes ||
        max_dimension > kTextureAbsoluteMaxDimension ||
        max_pixels > kTextureAbsoluteMaxPixels ||
        max_mip_levels > kTextureAbsoluteMaxMipLevels) {
        return core::Result<void>::failure(invalid(
            "texture source limits must be non-zero and within process safety bounds"));
    }
    return core::Result<void>::success();
}

core::Result<void> TextureSourceAdmission::validate(
    const TextureSourceLimits& limits) const {
    if (auto result = limits.validate(); !result) return result;
    if (!valid_texture_source_format(format) ||
        (stage != TextureSourceStage::container_inspected &&
         stage != TextureSourceStage::decoded)) {
        return core::Result<void>::failure(validation(
            "texture source format or stage is invalid"));
    }
    if (source.bytes == 0U || source.bytes > limits.max_source_bytes ||
        source.digest.is_zero() || source.media_type != texture_source_media_type(format) ||
        width == 0U || height == 0U || width > limits.max_dimension ||
        height > limits.max_dimension || mip_levels == 0U ||
        mip_levels > limits.max_mip_levels || channels > 4U || bit_depth > 32U) {
        return core::Result<void>::failure(validation(
            "texture source admission exceeds its identity, extent, or format bounds"));
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (pixels == 0U || pixels > limits.max_pixels ||
        (stage == TextureSourceStage::decoded && (channels == 0U || bit_depth == 0U))) {
        return core::Result<void>::failure(validation(
            "texture source pixel count or decoded channel contract is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<TextureSourceAdmission> inspect_texture_source(
    std::span<const std::uint8_t> bytes,
    std::string_view declared_media_type,
    const TextureSourceLimits& limits) {
    if (auto result = limits.validate(); !result) {
        return core::Result<TextureSourceAdmission>::failure(result.error());
    }
    if (bytes.empty() || bytes.size() > limits.max_source_bytes ||
        declared_media_type.size() > 127U ||
        !std::all_of(declared_media_type.begin(), declared_media_type.end(), [](unsigned char value) {
            return value >= 0x21U && value <= 0x7eU;
        })) {
        return core::Result<TextureSourceAdmission>::failure(invalid(
            "texture source bytes or declared media type exceed admission bounds"));
    }

    core::Result<HeaderInfo> inspected = [&]() -> core::Result<HeaderInfo> {
        constexpr std::array<std::uint8_t, 8U> png{
            0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
        if (bytes.size() >= png.size() && std::equal(png.begin(), png.end(), bytes.begin())) {
            return inspect_png(bytes);
        }
        if (bytes.size() >= 2U && bytes[0U] == 0xffU && bytes[1U] == 0xd8U) {
            return inspect_jpeg(bytes);
        }
        if (bytes.size() >= 4U && bytes[0U] == 'D' && bytes[1U] == 'D' &&
            bytes[2U] == 'S' && bytes[3U] == ' ') {
            return inspect_dds(bytes);
        }
        if (bytes.size() >= 4U && read_le32(bytes, 0U) == 0x01312f76U) {
            return inspect_exr(bytes);
        }
        return inspect_tga(bytes);
    }();
    if (!inspected) return core::Result<TextureSourceAdmission>::failure(inspected.error());
    if (!media_type_matches(inspected.value().format, declared_media_type)) {
        return core::Result<TextureSourceAdmission>::failure(validation(
            "declared texture media type does not match inspected source bytes"));
    }

    TextureSourceAdmission admission;
    admission.format = inspected.value().format;
    admission.stage = TextureSourceStage::container_inspected;
    admission.source = {
        sha256(bytes),
        static_cast<std::uint64_t>(bytes.size()),
        texture_source_media_type(inspected.value().format),
    };
    admission.width = inspected.value().width;
    admission.height = inspected.value().height;
    admission.channels = inspected.value().channels;
    admission.bit_depth = inspected.value().bit_depth;
    admission.mip_levels = inspected.value().mip_levels;
    admission.compressed = inspected.value().compressed;
    admission.high_dynamic_range = inspected.value().high_dynamic_range;
    if (auto result = admission.validate(limits); !result) {
        return core::Result<TextureSourceAdmission>::failure(result.error());
    }
    return core::Result<TextureSourceAdmission>::success(std::move(admission));
}

core::Result<void> DecodedRgba8Texture::validate(
    const TextureSourceLimits& limits) const {
    if (auto result = admission.validate(limits); !result) return result;
    if (admission.stage != TextureSourceStage::decoded || admission.channels != 4U ||
        admission.bit_depth != 8U || admission.format != TextureSourceFormat::tga) {
        return core::Result<void>::failure(validation(
            "decoded RGBA8 texture admission does not match its pixel contract"));
    }
    const std::uint64_t expected =
        static_cast<std::uint64_t>(admission.width) * admission.height * 4U;
    if (expected != pixels.size()) {
        return core::Result<void>::failure(validation(
            "decoded RGBA8 texture byte count does not match its extent"));
    }
    return core::Result<void>::success();
}

core::Result<DecodedRgba8Texture> decode_tga_rgba8(
    std::span<const std::uint8_t> bytes,
    const TextureSourceLimits& limits) {
    auto inspected = inspect_texture_source(bytes, "image/x-tga", limits);
    if (!inspected) {
        return core::Result<DecodedRgba8Texture>::failure(inspected.error());
    }
    if (inspected.value().format != TextureSourceFormat::tga) {
        return core::Result<DecodedRgba8Texture>::failure(validation(
            "TGA decoder received a different admitted source format"));
    }

    const std::size_t source_pixel_bytes = inspected.value().bit_depth / 8U;
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(inspected.value().width) * inspected.value().height;
    const std::uint64_t source_bytes = pixel_count * source_pixel_bytes;
    if (source_bytes > std::numeric_limits<std::size_t>::max()) {
        return core::Result<DecodedRgba8Texture>::failure(validation(
            "TGA decoded payload exceeds addressable memory"));
    }
    std::vector<std::uint8_t> source_pixels;
    source_pixels.reserve(static_cast<std::size_t>(source_bytes));
    std::size_t cursor = 18U + bytes[0U];
    if (bytes[2U] == 2U) {
        source_pixels.insert(
            source_pixels.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
            bytes.begin() + static_cast<std::ptrdiff_t>(
                cursor + static_cast<std::size_t>(source_bytes)));
    } else {
        while (source_pixels.size() < source_bytes) {
            const std::uint8_t packet = bytes[cursor++];
            const std::size_t count = (packet & 0x7fU) + 1U;
            if ((packet & 0x80U) != 0U) {
                const auto pixel = bytes.subspan(cursor, source_pixel_bytes);
                cursor += source_pixel_bytes;
                for (std::size_t index = 0U; index < count; ++index) {
                    source_pixels.insert(source_pixels.end(), pixel.begin(), pixel.end());
                }
            } else {
                const std::size_t packet_bytes = count * source_pixel_bytes;
                source_pixels.insert(
                    source_pixels.end(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                    bytes.begin() + static_cast<std::ptrdiff_t>(cursor + packet_bytes));
                cursor += packet_bytes;
            }
        }
    }
    if (source_pixels.size() != source_bytes) {
        return core::Result<DecodedRgba8Texture>::failure(validation(
            "TGA decoder produced an inconsistent source-pixel count"));
    }

    DecodedRgba8Texture decoded;
    decoded.admission = inspected.value();
    decoded.admission.stage = TextureSourceStage::decoded;
    decoded.admission.channels = 4U;
    decoded.admission.bit_depth = 8U;
    decoded.pixels.resize(static_cast<std::size_t>(pixel_count * 4U));
    const bool origin_right = (bytes[17U] & 0x10U) != 0U;
    const bool origin_top = (bytes[17U] & 0x20U) != 0U;
    const bool authored_alpha = source_pixel_bytes == 4U && (bytes[17U] & 0x0fU) == 8U;
    for (std::uint64_t linear = 0U; linear < pixel_count; ++linear) {
        const std::uint32_t row = static_cast<std::uint32_t>(
            linear / inspected.value().width);
        const std::uint32_t column = static_cast<std::uint32_t>(
            linear % inspected.value().width);
        const std::uint32_t x = origin_right
            ? inspected.value().width - 1U - column
            : column;
        const std::uint32_t y = origin_top
            ? row
            : inspected.value().height - 1U - row;
        const std::size_t source_offset =
            static_cast<std::size_t>(linear) * source_pixel_bytes;
        const std::size_t destination_offset =
            (static_cast<std::size_t>(y) * inspected.value().width + x) * 4U;
        decoded.pixels[destination_offset] = source_pixels[source_offset + 2U];
        decoded.pixels[destination_offset + 1U] = source_pixels[source_offset + 1U];
        decoded.pixels[destination_offset + 2U] = source_pixels[source_offset];
        decoded.pixels[destination_offset + 3U] = authored_alpha
            ? source_pixels[source_offset + 3U]
            : 255U;
    }
    if (auto result = decoded.validate(limits); !result) {
        return core::Result<DecodedRgba8Texture>::failure(result.error());
    }
    return core::Result<DecodedRgba8Texture>::success(std::move(decoded));
}

} // namespace carto::assets
