#include <carto/assets/texture_source.hpp>

#include <limits>
#include <new>

#ifdef _WIN32
#define NOMINMAX
#include <wincodec.h>
#include <windows.h>
#endif

namespace carto::assets {
namespace {
using Result = core::Result<DecodedRgba8Texture>;
Result failure(const char *message) {
    return Result::failure({core::ErrorCode::validation_failed, message});
}
#ifdef _WIN32
template <class T> struct ComOwner {
    T *value = nullptr;
    ~ComOwner() {
        if (value)
            value->Release();
    }
    T **put() { return &value; }
    T *operator->() const { return value; }
    ComOwner() = default;
    ComOwner(const ComOwner &) = delete;
    ComOwner &operator=(const ComOwner &) = delete;
};
struct Apartment {
    HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() {
        if (SUCCEEDED(status))
            CoUninitialize();
    }
};
#endif
} // namespace

static core::Result<DecodedRgba8Texture> decode_system_rgba8(std::span<const std::uint8_t> bytes,
                                                   const TextureSourceLimits &limits,TextureSourceFormat expected) {
    const auto inspected = inspect_texture_source(bytes, texture_source_media_type(expected), limits);
    if (!inspected)
        return Result::failure(inspected.error());
    if (inspected.value().format != expected)
        return failure("image decoder received a different container");
    if(expected==TextureSourceFormat::jpeg&&(inspected.value().bit_depth!=8||(inspected.value().channels!=1&&inspected.value().channels!=3)))return failure("JPEG decode supports eight-bit grayscale or RGB/YCbCr, not CMYK or higher precision");
    // This adapter decodes one still image, never a silently selected frame.
    for (std::size_t cursor = expected==TextureSourceFormat::png?8:bytes.size(); cursor < bytes.size();) {
        const auto size = (std::uint32_t(bytes[cursor]) << 24U) |
                          (std::uint32_t(bytes[cursor + 1]) << 16U) |
                          (std::uint32_t(bytes[cursor + 2]) << 8U) | bytes[cursor + 3];
        const std::string_view type(reinterpret_cast<const char *>(bytes.data() + cursor + 4), 4);
        if (type == "acTL" || type == "fcTL" || type == "fdAT")
            return failure("animated PNG is outside the still-image decoder contract");
        cursor += std::size_t(size) + 12;
    }
#ifdef _WIN32
    try {
        const std::uint64_t stride = std::uint64_t(inspected.value().width) * 4U;
        const std::uint64_t decoded_bytes = stride * inspected.value().height;
        if (bytes.size() > std::numeric_limits<DWORD>::max() ||
            decoded_bytes > std::numeric_limits<UINT>::max() ||
            decoded_bytes > std::numeric_limits<std::size_t>::max())
            return failure("image decoded payload exceeds the platform byte budget");
        Apartment apartment;
        if (FAILED(apartment.status) && apartment.status != RPC_E_CHANGED_MODE)
            return failure("image decoder could not initialize its COM apartment");
        ComOwner<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWICImagingFactory,
                                    reinterpret_cast<void **>(factory.put()))))
            return failure("Windows image codec factory is unavailable");
        ComOwner<IWICStream> stream;
        if (FAILED(factory->CreateStream(stream.put())) ||
            FAILED(stream->InitializeFromMemory(const_cast<BYTE *>(bytes.data()),
                                                DWORD(bytes.size()))))
            return failure("image decoder could not bind bounded source bytes");
        ComOwner<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromStream(stream.value, nullptr,
                                                    WICDecodeMetadataCacheOnDemand, decoder.put())))
            return failure("image pixel decoder rejected the source stream");
        GUID container{};
        UINT frames = 0;
        if (FAILED(decoder->GetContainerFormat(&container)) ||
            !IsEqualGUID(container, expected==TextureSourceFormat::png?GUID_ContainerFormatPng:GUID_ContainerFormatJpeg) ||
            FAILED(decoder->GetFrameCount(&frames)) || frames != 1)
            return failure("image decoder container/frame admission mismatch");
        ComOwner<IWICBitmapFrameDecode> frame;
        UINT width = 0, height = 0;
        if (FAILED(decoder->GetFrame(0, frame.put())) || FAILED(frame->GetSize(&width, &height)) ||
            width != inspected.value().width || height != inspected.value().height)
            return failure("image decoded dimensions differ from inspected source");
        ComOwner<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(converter.put())) ||
            FAILED(converter->Initialize(frame.value, GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom)))
            return failure("image pixels cannot be converted to straight RGBA8");
        DecodedRgba8Texture candidate;
        candidate.admission = inspected.value();
        candidate.admission.stage = TextureSourceStage::decoded;
        candidate.admission.channels = 4;
        candidate.admission.bit_depth = 8;
        candidate.pixels.resize(std::size_t(decoded_bytes));
        if (FAILED(converter->CopyPixels(nullptr, UINT(stride), UINT(decoded_bytes),
                                         candidate.pixels.data())))
            return failure("image pixel stream is truncated or undecodable");
        if (auto valid = candidate.validate(limits); !valid)
            return Result::failure(valid.error());
        return Result::success(std::move(candidate));
    } catch (const std::bad_alloc &) {
        return failure("image decoded allocation exceeds available memory");
    }
#else
    return Result::failure({core::ErrorCode::unsupported,
                            "image pixel decoding requires the Windows codec adapter on this build"});
#endif
}

core::Result<DecodedRgba8Texture> decode_png_rgba8(std::span<const std::uint8_t> bytes,
                                                const TextureSourceLimits& limits) {
    return decode_system_rgba8(bytes,limits,TextureSourceFormat::png);
}

core::Result<DecodedRgba8Texture> decode_texture_rgba8(std::span<const std::uint8_t> bytes,
                                                       const TextureSourceLimits &limits) {
    const auto inspected = inspect_texture_source(bytes, {}, limits);
    if (!inspected)
        return Result::failure(inspected.error());
    switch (inspected.value().format) {
    case TextureSourceFormat::png:
        return decode_png_rgba8(bytes, limits);
    case TextureSourceFormat::tga:
        return decode_tga_rgba8(bytes, limits);
    case TextureSourceFormat::jpeg:
        return decode_system_rgba8(bytes,limits,TextureSourceFormat::jpeg);
    default:
        return Result::failure({core::ErrorCode::unsupported,
                                "pixel decoding is not implemented for this inspected container"});
    }
}
} // namespace carto::assets
