#include <carto/assets/texture_mip.hpp>
#include <carto/assets/texture_source.hpp>
#include <carto/io/gltf_material_import.hpp>
#include <carto/production/authoring_material.hpp>
#include <cstdlib>
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#include <wincodec.h>
#include <windows.h>
#endif
namespace {
std::string b64(std::span<const std::uint8_t> data) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const auto n = (std::uint32_t(data[i]) << 16) |
                       (i + 1 < data.size() ? std::uint32_t(data[i + 1]) << 8 : 0) |
                       (i + 2 < data.size() ? data[i + 2] : 0);
        out += alphabet[n >> 18];
        out += alphabet[(n >> 12) & 63];
        out += i + 1 < data.size() ? alphabet[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? alphabet[n & 63] : '=';
    }
    return out;
}
void check(bool v, const char *text) {
    if (!v) {
        std::cerr << text << '\n';
        std::exit(1);
    }
}
#ifdef _WIN32
template <class T> struct Owner {
    T *p = nullptr;
    ~Owner() {
        if (p)
            p->Release();
    }
    T **out() { return &p; }
    T *operator->() { return p; }
};
std::vector<std::uint8_t> fixture(bool gray) {
    // Generate our own flat-color raster with the operating-system JPEG
    // encoder. No fetched image, proprietary fixture or encoder implementation.
    Owner<IWICImagingFactory> factory;
    check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_IWICImagingFactory,
                                     reinterpret_cast<void **>(factory.out()))),
          "fixture WIC factory");
    Owner<IStream> stream;
    check(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, stream.out())), "fixture memory stream");
    Owner<IWICBitmapEncoder> encoder;
    check(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.out())) &&
              SUCCEEDED(encoder->Initialize(stream.p, WICBitmapEncoderNoCache)),
          "fixture JPEG encoder");
    Owner<IWICBitmapFrameEncode> frame;
    Owner<IPropertyBag2> properties;
    check(SUCCEEDED(encoder->CreateNewFrame(frame.out(), properties.out())), "fixture JPEG frame");
    PROPBAG2 option{};
    option.pstrName = const_cast<wchar_t *>(L"ImageQuality");
    VARIANT quality{};
    quality.vt = VT_R4;
    quality.fltVal = 1;
    check(SUCCEEDED(properties->Write(1, &option, &quality)), "fixture quality");
    check(SUCCEEDED(frame->Initialize(properties.p)) && SUCCEEDED(frame->SetSize(8, 8)),
          "fixture JPEG dimensions");
    auto format = gray ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat24bppBGR;
    check(SUCCEEDED(frame->SetPixelFormat(&format)) &&
              IsEqualGUID(format, gray ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat24bppBGR),
          "fixture pixel format");
    std::vector<std::uint8_t> pixels;
    if (gray)
        pixels.assign(64, 128);
    else
        for (unsigned i = 0; i < 64; ++i)
            pixels.insert(pixels.end(), {31, 79, 193});
    check(SUCCEEDED(frame->WritePixels(8, gray ? 8 : 24, static_cast<UINT>(pixels.size()),
                                       pixels.data())) &&
              SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()),
          "fixture JPEG encoding");
    STATSTG info{};
    check(SUCCEEDED(stream->Stat(&info, STATFLAG_NONAME)) && info.cbSize.QuadPart < 16384,
          "fixture size bound");
    LARGE_INTEGER zero{};
    check(SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)), "fixture rewind");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.cbSize.QuadPart));
    ULONG n = 0;
    check(SUCCEEDED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &n)) &&
              n == bytes.size(),
          "fixture bytes");
    return bytes;
}
#endif
} // namespace
int main() {
#ifdef _WIN32
    using namespace carto::assets;
    const auto status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    check(SUCCEEDED(status), "test COM apartment");
    for (bool gray : {false, true}) {
        const auto bytes = fixture(gray);
        const auto decoded = decode_texture_rgba8(bytes);
        if (!decoded)
            std::cerr << decoded.error().message << '\n';
        check(bool(decoded), "JPEG executes pixel decode");
        check(decoded.value().admission.format == TextureSourceFormat::jpeg &&
                  decoded.value().admission.stage == TextureSourceStage::decoded &&
                  decoded.value().admission.source.digest == sha256(bytes),
              "JPEG source identity and executed stage");
        check(decoded.value().pixels.size() == 8 * 8 * 4, "JPEG RGBA8 dimensions");
        const auto &p = decoded.value().pixels;
        for (std::size_t i = 0; i < p.size(); i += 4) {
            check(p[i + 3] == 255, "JPEG has opaque alpha");
            for (unsigned c = 0; c < 3; ++c) {
                const int expected = gray ? 128 : c == 0 ? 193 : c == 1 ? 79 : 31;
                check(std::abs(int(p[i + c]) - expected) <= 3,
                      "independent flat-raster decode tolerance <=3 bytes");
            }
        }
        TextureSourceLimits limit;
        limit.max_pixels = 1;
        check(!decode_texture_rgba8(bytes, limit), "JPEG pixel budget precedes decode");
        check(!decode_png_rgba8(bytes), "declared PNG decoder refuses JPEG");
        auto truncated = bytes;
        truncated.pop_back();
        check(!decode_texture_rgba8(truncated), "truncated JPEG refused");
        check(bool(generate_rgba8_mips(decoded.value(),
                                       {TextureSemantic::base_color, ColorSpace::srgb})),
              "JPEG reuses semantic mip path");
        const auto json =
            std::string(R"({"asset":{"version":"2.0"},"images":[{"uri":"data:image/jpeg;base64,)") +
            b64(bytes) +
            R"("}],"textures":[{"source":0}],"materials":[{"doubleSided":true,"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}]})";
        const auto imported = carto::io::import_gltf_materials(
            {reinterpret_cast<const std::uint8_t *>(json.data()), json.size()}, false);
        check(bool(imported),
              "standard glTF JPEG source ingests without Cartographer-specific profile");
        carto::production::AuthoringTextureLibrary library;
        const auto &record = imported.value().catalog.textures().begin()->second;
        check(record.format == TextureSourceFormat::jpeg &&
                  bool(library.admit(record, imported.value().encoded_images[0])),
              "JPEG format identity and pixels reach authoring library");
        auto program = carto::production::compile_authoring_material(imported.value().catalog, 1,
                                                                     std::nullopt, &library);
        check(bool(program), "JPEG material is executable");
        auto sample = program.value().evaluate({{.5, .5}, {.01, 0}, {0, .01}});
        check(bool(sample), "JPEG material sampling executes");
        const auto c = sample.value().material.base_color_factor;
        check(gray ? (c.x > .21 && c.x < .23)
                   : (c.x > .51 && c.x < .55 && c.y > .07 && c.y < .09 && c.z > .01 && c.z < .02),
              "JPEG color samples pass through sRGB semantic conversion exactly once");
    }
    CoUninitialize();
    std::cout << "JPEG RGB/grayscale pixel execution, digest, alpha, bounds and "
                 "mips passed\n";
#else
    std::cout << "JPEG system codec unavailable on this build; no decode support "
                 "claimed\n";
#endif
}
