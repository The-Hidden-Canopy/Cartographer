#include <algorithm>
#include <array>
#include <bit>
#include <carto/core/json.hpp>
#include <carto/io/gltf_material_import.hpp>
#include <carto/io/gltf_scene_import.hpp>
#include <carto/assets/texture_image_decoder.hpp>
#include "gltf_dependency_reader.hpp"
#include <cctype>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>
namespace carto::io {
namespace {
// Preserve the bounded realism importer profile without coupling IO to a
// renderer-specific texture cache. Pixel admission uses the current digest-
// bound decoder, including its corruption and color-metadata refusals.
assets::TextureSourceLimits gltf_texture_limits() {
    assets::TextureSourceLimits limits;
    limits.max_source_bytes = 32U * 1024U * 1024U;
    limits.max_dimension = 2048U;
    limits.max_pixels = 4'194'304U;
    return limits;
}

template <class T> T need(core::Result<T> r) {
    if (!r)
        throw std::runtime_error(r.error().message);
    return std::move(r.value());
}
using Object = std::vector<core::json::Member>;
Object object(std::string_view s) { return need(core::json::parse_object(s)); }
std::string_view get(const Object &o, std::string_view key, std::string_view fallback = "") {
    const auto *p = core::json::find_member(o, key);
    return p ? p->raw_value : fallback;
}
void fields(const Object &o, std::initializer_list<std::string_view> supported) {
    for (const auto &v : o)
        if (std::find(supported.begin(), supported.end(), v.key) == supported.end())
            throw std::runtime_error("unsupported glTF material field: " + v.key);
}
std::vector<std::string_view> array(std::string_view s, std::size_t maximum) {
    if (s.size() < 2 || s.front() != '[' || s.back() != ']')
        throw std::runtime_error("expected bounded glTF array");
    std::vector<std::string_view> out;
    std::size_t start = 1;
    unsigned depth = 0;
    bool quote = false, escape = false;
    const auto add = [&](std::size_t end) {
        auto part = s.substr(start, end - start);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front())))
            part.remove_prefix(1);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back())))
            part.remove_suffix(1);
        if (part.empty() || out.size() >= maximum)
            throw std::runtime_error("empty or excessive glTF array entry");
        out.push_back(part);
    };
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        const char c = s[i];
        if (quote) {
            if (escape)
                escape = false;
            else if (c == '\\')
                escape = true;
            else if (c == '"')
                quote = false;
            continue;
        }
        if (c == '"')
            quote = true;
        else if (c == '[' || c == '{')
            ++depth;
        else if (c == ']' || c == '}') {
            if (!depth)
                throw std::runtime_error("invalid array nesting");
            --depth;
        } else if (c == ',' && !depth) {
            add(i);
            start = i + 1;
        }
    }
    auto tail = s.substr(start, s.size() - 1 - start);
    if (tail.find_first_not_of(" \r\n\t") != std::string_view::npos)
        add(s.size() - 1);
    else if (!out.empty())
        throw std::runtime_error("trailing glTF array separator");
    return out;
}
double number(std::string_view s) {
    double v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v))
        throw std::runtime_error("invalid finite glTF scalar");
    return v;
}
std::uint64_t integer(std::string_view s) { return need(core::json::parse_uint(s)); }
std::string text(std::string_view s) { return need(core::json::decode_string(s)); }
std::vector<double> vector(std::string_view s, std::size_t n) {
    const auto a = array(s, n);
    if (a.size() != n)
        throw std::runtime_error("glTF vector cardinality mismatch");
    std::vector<double> v;
    for (auto x : a)
        v.push_back(number(x));
    return v;
}
std::vector<std::uint8_t> base64(std::string_view s, std::size_t limit = 24U * 1024U * 1024U) {
    if (s.empty() || s.size() % 4 || s.size() > limit)
        throw std::runtime_error("embedded base64 size exceeds bound");
    const auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(s.size() / 4 * 3);
    for (std::size_t i = 0; i < s.size(); i += 4) {
        int a = digit(s[i]), b = digit(s[i + 1]), c = s[i + 2] == '=' ? -2 : digit(s[i + 2]),
            d = s[i + 3] == '=' ? -2 : digit(s[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1 || (c == -2 && d != -2) ||
            (i + 4 < s.size() && (c < 0 || d < 0)) || (c == -2 && (b & 15)) ||
            (d == -2 && c >= 0 && (c & 3)))
            throw std::runtime_error("invalid PNG base64 padding");
        const auto v = (unsigned(a) << 18) | (unsigned(b) << 12) | (unsigned(std::max(0, c)) << 6) |
                       unsigned(std::max(0, d));
        out.push_back(std::uint8_t(v >> 16));
        if (c >= 0)
            out.push_back(std::uint8_t(v >> 8));
        if (d >= 0)
            out.push_back(std::uint8_t(v));
    }
    return out;
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t p) {
    if (p + 4 > b.size())
        throw std::runtime_error("truncated GLB");
    return std::uint32_t(b[p]) | (std::uint32_t(b[p + 1]) << 8) | (std::uint32_t(b[p + 2]) << 16) |
           (std::uint32_t(b[p + 3]) << 24);
}
project::SamplerAddressMode wrap(std::uint64_t n) {
    if (n == 10497)
        return project::SamplerAddressMode::repeat;
    if (n == 33648)
        return project::SamplerAddressMode::mirrored_repeat;
    if (n == 33071)
        return project::SamplerAddressMode::clamp_to_edge;
    throw std::runtime_error("unsupported sampler wrap");
}
struct GltfContainer {
    std::string_view json;
    std::span<const std::uint8_t> binary;
    struct External { std::string uri; std::span<const std::uint8_t> bytes; };
    std::vector<External> external;
};
constexpr std::string_view kSourcePackageMagic = "CARTOGLTFPK1";
GltfContainer container(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > 128U * 1024U * 1024U)
        throw std::runtime_error("glTF source must be within 128 MiB");
    if (bytes.size() >= kSourcePackageMagic.size() &&
        std::equal(kSourcePackageMagic.begin(), kSourcePackageMagic.end(), bytes.begin())) {
        std::size_t cursor = kSourcePackageMagic.size();
        const auto take_u32 = [&]() {
            const auto value = u32(bytes, cursor);
            cursor += 4U;
            return value;
        };
        const auto document_size = take_u32();
        const auto count = take_u32();
        if (!document_size || count == 0U || count > 16U || document_size > bytes.size() - cursor)
            throw std::runtime_error("invalid glTF source package header");
        const auto document = bytes.subspan(cursor, document_size);
        cursor += document_size;
        if (document.size() >= kSourcePackageMagic.size() &&
            std::equal(kSourcePackageMagic.begin(), kSourcePackageMagic.end(), document.begin()))
            throw std::runtime_error("nested glTF source package is not supported");
        auto c = container(document);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto name_size = take_u32(), data_size = take_u32();
            if (!name_size || name_size > 240U || !data_size || name_size > bytes.size() - cursor)
                throw std::runtime_error("invalid glTF source package resource");
            const std::string name(reinterpret_cast<const char*>(bytes.data() + cursor), name_size);
            cursor += name_size;
            if (data_size > bytes.size() - cursor ||
                std::any_of(c.external.begin(), c.external.end(), [&](const auto& entry) { return entry.uri == name; }))
                throw std::runtime_error("invalid or duplicate glTF source package resource");
            c.external.push_back({name, bytes.subspan(cursor, data_size)});
            cursor += data_size;
        }
        if (cursor != bytes.size()) throw std::runtime_error("trailing glTF source package data");
        return c;
    }
    GltfContainer c{{reinterpret_cast<const char *>(bytes.data()), bytes.size()}, {}, {}};
    if (bytes.size() >= 4 && u32(bytes, 0) == 0x46546c67) {
        if (bytes.size() < 20 || u32(bytes, 4) != 2 || u32(bytes, 8) != bytes.size() ||
            u32(bytes, 16) != 0x4e4f534a)
            throw std::runtime_error("invalid GLB container");
        const auto n = u32(bytes, 12);
        if (n % 4 || n > bytes.size() - 20)
            throw std::runtime_error("invalid GLB JSON chunk");
        c.json = {reinterpret_cast<const char *>(bytes.data() + 20), n};
        const std::size_t end = 20U + n;
        if (end < bytes.size()) {
            if (bytes.size() - end < 8 || u32(bytes, end + 4) != 0x004e4942 ||
                u32(bytes, end) % 4 || u32(bytes, end) != bytes.size() - end - 8)
                throw std::runtime_error("unsupported or malformed GLB trailing chunks");
            c.binary = bytes.subspan(end + 8);
        }
    }
    return c;
}
std::vector<std::vector<std::uint8_t>> buffers(const Object &root, GltfContainer c) {
    std::vector<std::vector<std::uint8_t>> result;
    std::uint64_t total = 0;
    for (auto raw : array(get(root, "buffers", "[]"), 16)) {
        const auto b = object(raw);
        fields(b, {"byteLength", "uri", "name"});
        const auto n = integer(get(b, "byteLength"));
        if (n == 0 || n > 64U * 1024U * 1024U || (total += n) > 64U * 1024U * 1024U)
            throw std::runtime_error("glTF buffers exceed 64 MiB");
        std::vector<std::uint8_t> data;
        if (auto uri = get(b, "uri"); !uri.empty()) {
            const auto source = text(uri);
            constexpr std::string_view prefix = "data:application/octet-stream;base64,";
            constexpr std::string_view alternate = "data:application/gltf-buffer;base64,";
            const auto skip = source.starts_with(prefix)      ? prefix.size()
                              : source.starts_with(alternate) ? alternate.size()
                                                              : 0;
            if (skip) {
                data = base64(std::string_view(source).substr(skip), 90U * 1024U * 1024U);
            } else {
                const auto found = std::find_if(c.external.begin(), c.external.end(),
                    [&](const auto& entry) { return entry.uri == source; });
                if (found == c.external.end())
                    throw std::runtime_error("external glTF buffer was not captured in the source package");
                data.assign(found->bytes.begin(), found->bytes.end());
            }
            if (data.size() != n)
                throw std::runtime_error("glTF buffer byteLength mismatch");
        } else {
            if (!result.empty() || c.binary.size() < n || c.binary.size() - n > 3)
                throw std::runtime_error("GLB BIN buffer byteLength mismatch");
            data.assign(c.binary.begin(), c.binary.begin() + static_cast<std::ptrdiff_t>(n));
        }
        result.push_back(std::move(data));
    }
    return result;
}
std::span<const std::uint8_t> buffer_view(const std::vector<std::vector<std::uint8_t>> &data,
                                          std::string_view raw) {
    const auto v = object(raw);
    fields(v, {"buffer", "byteOffset", "byteLength", "byteStride", "target", "name"});
    const auto &b = data.at(static_cast<std::size_t>(integer(get(v, "buffer"))));
    const auto offset = integer(get(v, "byteOffset", "0")), size = integer(get(v, "byteLength"));
    if (offset > b.size() || size > b.size() - offset)
        throw std::runtime_error("glTF buffer view outside its buffer");
    return std::span<const std::uint8_t>(b).subspan(static_cast<std::size_t>(offset),
                                                    static_cast<std::size_t>(size));
}
} // namespace
core::Result<std::vector<std::uint8_t>> capture_gltf_scene_source(
    const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    using Result = core::Result<std::vector<std::uint8_t>>;
    try {
        const auto contents = container(bytes);
        if (!contents.external.empty()) throw std::runtime_error("source packages cannot be recaptured as files");
        const auto root = object(contents.json);
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> captured;
        std::uint64_t total = bytes.size();
        for (auto raw : array(get(root, "buffers", "[]"), 16)) {
            const auto entry = object(raw);
            const auto uri = get(entry, "uri");
            if (uri.empty()) continue;
            const auto name = text(uri);
            if (name.starts_with("data:")) continue;
            if (std::any_of(captured.begin(), captured.end(), [&](const auto& item) { return item.first == name; }))
                continue;
            auto data = detail::read_gltf_sibling(path, name, 64U * 1024U * 1024U);
            if (!data) return Result::failure(data.error());
            if (data.value().empty() || (total += data.value().size() + name.size() + 8U) > 128U * 1024U * 1024U - 20U)
                throw std::runtime_error("glTF source package exceeds its size limit");
            captured.emplace_back(name, std::move(data.value()));
        }
        if (captured.empty()) return Result::success({bytes.begin(), bytes.end()});
        std::vector<std::uint8_t> result(kSourcePackageMagic.begin(), kSourcePackageMagic.end());
        const auto append_u32 = [&](std::uint32_t value) {
            for (unsigned shift = 0; shift != 32U; shift += 8U)
                result.push_back(static_cast<std::uint8_t>(value >> shift));
        };
        append_u32(static_cast<std::uint32_t>(bytes.size()));
        append_u32(static_cast<std::uint32_t>(captured.size()));
        result.insert(result.end(), bytes.begin(), bytes.end());
        for (const auto& [name, data] : captured) {
            append_u32(static_cast<std::uint32_t>(name.size()));
            append_u32(static_cast<std::uint32_t>(data.size()));
            result.insert(result.end(), name.begin(), name.end());
            result.insert(result.end(), data.begin(), data.end());
        }
        return Result::success(std::move(result));
    } catch (const std::exception& e) {
        return Result::failure({core::ErrorCode::validation_failed,
            std::string("glTF source capture: ") + e.what()});
    }
}
core::Result<ImportedGltfMaterials> import_gltf_materials(std::span<const std::uint8_t> bytes,
                                                          bool require_cartographer_profile) {
    using Result = core::Result<ImportedGltfMaterials>;
    try {
        if (bytes.empty() || bytes.size() > 128U * 1024U * 1024U)
            throw std::runtime_error("material library source must be within 128 MiB");
        const auto contents = container(bytes);
        const auto root = object(contents.json);
        const auto asset = object(get(root, "asset"));
        const auto profile = object(get(object(get(root, "extras", "{}")), "cartographer", "{}"));
        if (text(get(asset, "version")) != "2.0" ||
            (require_cartographer_profile &&
             (text(get(profile, "profile", "\"\"")) != "cartographer-gltf" ||
              integer(get(profile, "profile_version", "0")) != 1)))
            throw std::runtime_error(
                "material reimport requires Cartographer glTF profile version 1");
        for (auto e : array(get(root, "extensionsRequired", "[]"), 16)) {
            const auto name = text(e);
            if (name != "KHR_texture_transform" && name != "KHR_materials_ior" &&
                name != "KHR_materials_clearcoat" && name != "KHR_materials_emissive_strength")
                throw std::runtime_error("unsupported required material extension: " + name);
        }
        ImportedGltfMaterials result;
        result.artifact_digest = assets::sha256(bytes);
        std::uint64_t image_bytes = 0, decoded_image_bytes = 0;
        std::optional<std::vector<std::vector<std::uint8_t>>> image_buffers;
        const auto views = array(get(root, "bufferViews", "[]"), 4096);
        for (auto raw : array(get(root, "images", "[]"), 64)) {
            const auto image = object(raw);
            fields(image, {"uri", "name", "bufferView", "mimeType"});
            std::vector<std::uint8_t> encoded;
            std::string declared;
            if (auto uri_raw = get(image, "uri"); !uri_raw.empty()) {
                if (!get(image, "bufferView").empty())
                    throw std::runtime_error("image cannot specify both URI and bufferView");
                const auto uri = text(uri_raw);
                constexpr std::string_view png = "data:image/png;base64,",
                                           jpeg = "data:image/jpeg;base64,";
                const auto skip = uri.starts_with(png)    ? png.size()
                                  : uri.starts_with(jpeg) ? jpeg.size()
                                                          : 0;
                if (!skip)
                    throw std::runtime_error("material import accepts embedded PNG/JPEG "
                                             "only; external images are not read");
                declared = uri.starts_with(png) ? "image/png" : "image/jpeg";
                if (auto mime = get(image, "mimeType"); !mime.empty() && text(mime) != declared)
                    throw std::runtime_error("image MIME type conflicts with embedded URI");
                encoded = base64(std::string_view(uri).substr(skip));
            } else {
                const auto mime = text(get(image, "mimeType"));
                if (mime != "image/png" && mime != "image/jpeg")
                    throw std::runtime_error("unsupported glTF image MIME type");
                declared = mime;
                if (!image_buffers)
                    image_buffers = buffers(root, contents);
                const auto view = buffer_view(
                    *image_buffers,
                    views.at(static_cast<std::size_t>(integer(get(image, "bufferView")))));
                if (view.size() > 18U * 1024U * 1024U)
                    throw std::runtime_error("embedded image exceeds source limit");
                encoded.assign(view.begin(), view.end());
            }
            image_bytes += encoded.size();
            if (image_bytes > 64U * 1024U * 1024U)
                throw std::runtime_error("material images exceed 64 MiB");
            const auto inspected=need(assets::inspect_texture_source(encoded,declared,gltf_texture_limits()));
            decoded_image_bytes+=std::uint64_t(inspected.width)*inspected.height*4;
            if(decoded_image_bytes>256U*1024U*1024U)throw std::runtime_error("glTF decoded image set exceeds 256 MiB before mip generation");
            const auto decoded =
                need(assets::decode_texture_image_rgba8(encoded, inspected.source.digest,
                    inspected.width, inspected.height, gltf_texture_limits()));
            if (decoded.admission.format != assets::TextureSourceFormat::png &&
                decoded.admission.format != assets::TextureSourceFormat::jpeg)
                throw std::runtime_error("glTF image is not PNG/JPEG");
            const auto id = result.encoded_images.size() + 1;
            project::TextureSourceRecord record;
            record.id = id;
            record.name = "Imported image " + std::to_string(id);
            record.relative_locator =
                "textures/imported-" + std::to_string(id) +
                (decoded.admission.format == assets::TextureSourceFormat::png ? ".png" : ".jpg");
            record.content_digest = assets::sha256(encoded);
            record.width = decoded.admission.width;
            record.height = decoded.admission.height;
            record.format = decoded.admission.format;
            record.revision = core::Revision{1};
            record.provenance =
                "glTF material import; source artifact " + result.artifact_digest.hex();
            auto admitted = result.catalog.insert_texture(std::move(record));
            if (!admitted)
                throw std::runtime_error(admitted.error().message);
            result.encoded_images.push_back(std::move(encoded));
        }
        const auto source_samplers = array(get(root, "samplers", "[]"), 64);
        const auto source_sampler_count = source_samplers.size();
        for (auto raw : source_samplers) {
            const auto s = object(raw);
            fields(s, {"name", "wrapS", "wrapT", "magFilter", "minFilter"});
            project::SamplerIntent sampler;
            sampler.id = result.catalog.samplers().size() + 1;
            sampler.name = "Imported sampler " + std::to_string(sampler.id);
            sampler.revision = core::Revision{1};
            sampler.address_u = wrap(integer(get(s, "wrapS", "10497")));
            sampler.address_v = wrap(integer(get(s, "wrapT", "10497")));
            const auto min = integer(get(s, "minFilter", "9987")),
                       mag = integer(get(s, "magFilter", "9729"));
            if (min < 9984 || min > 9987 || (mag != 9728 && mag != 9729))
                throw std::runtime_error(
                    "sampler without representable mip filtering requires an explicit "
                    "conversion; not silently substituted");
            sampler.minification = (min == 9984 || min == 9986) ? project::SamplerFilter::nearest
                                                                : project::SamplerFilter::linear;
            sampler.magnification =
                mag == 9728 ? project::SamplerFilter::nearest : project::SamplerFilter::linear;
            sampler.mip =
                min >= 9986 ? project::SamplerFilter::linear : project::SamplerFilter::nearest;
            auto admitted = result.catalog.insert_sampler(sampler);
            if (!admitted)
                throw std::runtime_error(admitted.error().message);
        }
        const auto textures = array(get(root, "textures", "[]"), 128);
        const auto default_sampler = result.catalog.samplers().size() + 1;
        if (std::any_of(textures.begin(), textures.end(),
                        [](auto raw) { return get(object(raw), "sampler").empty(); })) {
            project::SamplerIntent s;
            s.id = default_sampler;
            s.name = "glTF default sampler";
            s.revision = core::Revision{1};
            if (auto added = result.catalog.insert_sampler(s); !added)
                throw std::runtime_error(added.error().message);
        }
        const auto binding = [&](std::string_view raw, render::UvSetId uv) {
            const auto info = object(raw);
            fields(info, {"index", "texCoord", "extensions", "scale", "strength"});
            if (integer(get(info, "texCoord", "0")) != 0)
                throw std::runtime_error(
                    "only the explicit TEXCOORD_0 material profile is supported");
            const auto texture =
                object(textures.at(static_cast<std::size_t>(integer(get(info, "index")))));
            fields(texture, {"source", "sampler", "name"});
            render::TextureRef ref;
            ref.asset = integer(get(texture, "source")) + 1;
            if (auto sampler = get(texture, "sampler"); !sampler.empty()) {
                const auto index = integer(sampler);
                if (index >= source_sampler_count)
                    throw std::runtime_error("texture sampler index exceeds source sampler count");
                ref.sampler = index + 1;
            } else {
                ref.sampler = default_sampler;
            }
            ref.uv_set = uv;
            ref.normal_convention = render::NormalConvention::opengl_y_plus;
            if (!result.catalog.textures().contains(ref.asset) ||
                !result.catalog.samplers().contains(ref.sampler))
                throw std::runtime_error("material texture refers to a missing image or sampler");
            const auto ext = object(get(info, "extensions", "{}"));
            fields(ext, {"KHR_texture_transform"});
            if (auto transform = get(ext, "KHR_texture_transform"); !transform.empty()) {
                const auto t = object(transform);
                fields(t, {"offset", "scale", "rotation", "texCoord"});
                if (integer(get(t, "texCoord", "0")) != 0)
                    throw std::runtime_error("texture transform selects unsupported UV stream");
                const auto offset = vector(get(t, "offset", "[0,0]"), 2),
                           scale = vector(get(t, "scale", "[1,1]"), 2);
                ref.transform.pivot = {0, 0};
                ref.transform.offset = {offset[0], offset[1]};
                ref.transform.scale = {scale[0], scale[1]};
                ref.transform.rotation = number(get(t, "rotation", "0"));
            }
            return ref;
        };
        for (auto raw : array(get(root, "materials", "[]"), 32)) {
            const auto material = object(raw);
            fields(material, {"name", "pbrMetallicRoughness", "normalTexture", "occlusionTexture",
                              "emissiveTexture", "emissiveFactor", "doubleSided", "alphaMode",
                              "alphaCutoff", "extensions", "extras"});
            if (get(material, "doubleSided", "false") != "true")
                throw std::runtime_error("single-sided material culling is not "
                                         "represented by the authoring viewport");
            render::MaterialAsset m;
            m.id = result.catalog.materials().size() + 1;
            m.name =
                text(get(material, "name", "\"Imported material\"")) + " " + std::to_string(m.id);
            m.revision = core::Revision{1};
            auto &v = m.definition;
            const auto pbr = object(get(material, "pbrMetallicRoughness", "{}"));
            fields(pbr, {"baseColorFactor", "metallicFactor", "roughnessFactor", "baseColorTexture",
                         "metallicRoughnessTexture"});
            const auto color = vector(get(pbr, "baseColorFactor", "[1,1,1,1]"), 4);
            for (double c : color)
                if (c < 0 || c > 1)
                    throw std::runtime_error("glTF base color outside [0,1]");
            v.base_color_factor = {color[0], color[1], color[2], color[3]};
            v.metallic = number(get(pbr, "metallicFactor", "1"));
            v.roughness = number(get(pbr, "roughnessFactor", "1"));
            auto uv = render::kUv0;
            const auto extras = object(get(material, "extras", "{}"));
            if (auto id = get(extras, "cartographer_uv_set"); !id.empty())
                uv = integer(text(id));
            const auto put = [&](const Object &o, std::string_view key, render::TextureRef &ref) {
                if (auto value = get(o, key); !value.empty())
                    ref = binding(value, uv);
            };
            put(pbr, "baseColorTexture", v.base_color);
            put(pbr, "metallicRoughnessTexture", v.metallic_roughness);
            put(material, "normalTexture", v.normal);
            put(material, "occlusionTexture", v.occlusion);
            put(material, "emissiveTexture", v.emissive);
            if (v.normal)
                v.normal_scale = number(get(object(get(material, "normalTexture")), "scale", "1"));
            if (v.occlusion)
                v.occlusion_strength =
                    number(get(object(get(material, "occlusionTexture")), "strength", "1"));
            const auto emission = vector(get(material, "emissiveFactor", "[0,0,0]"), 3);
            for (double c : emission)
                if (c < 0 || c > 1)
                    throw std::runtime_error("glTF emissive factor outside [0,1]");
            v.emissive_factor = {emission[0], emission[1], emission[2]};
            v.emissive_strength = 1;
            const auto alpha = text(get(material, "alphaMode", "\"OPAQUE\""));
            if (alpha == "MASK")
                v.alpha_mode = render::AlphaMode::masked;
            else if (alpha == "BLEND")
                v.alpha_mode = render::AlphaMode::blended;
            else if (alpha != "OPAQUE")
                throw std::runtime_error("unsupported alpha mode");
            v.alpha_cutoff = number(get(material, "alphaCutoff", "0.5"));
            const auto ext = object(get(material, "extensions", "{}"));
            fields(ext, {"KHR_materials_ior", "KHR_materials_clearcoat",
                         "KHR_materials_emissive_strength"});
            if (auto value = get(ext, "KHR_materials_ior"); !value.empty()) {
                const auto e = object(value);
                fields(e, {"ior"});
                const auto ior = number(get(e, "ior", "1.5"));
                if (ior < 1)
                    throw std::runtime_error("IOR below one is unsupported");
                v.dielectric_f0 = std::pow((ior - 1) / (ior + 1), 2);
            }
            if (auto value = get(ext, "KHR_materials_clearcoat"); !value.empty()) {
                const auto e = object(value);
                fields(e, {"clearcoatFactor", "clearcoatRoughnessFactor"});
                v.clearcoat = number(get(e, "clearcoatFactor", "0"));
                v.clearcoat_roughness = number(get(e, "clearcoatRoughnessFactor", "0"));
            }
            if (auto value = get(ext, "KHR_materials_emissive_strength"); !value.empty()) {
                const auto e = object(value);
                fields(e, {"emissiveStrength"});
                v.emissive_strength = number(get(e, "emissiveStrength", "1"));
            }
            auto admitted = result.catalog.insert_material(std::move(m));
            if (!admitted)
                throw std::runtime_error(admitted.error().message);
        }
        if (require_cartographer_profile && result.catalog.materials().empty())
            throw std::runtime_error("source contains no visual materials");
        if (auto valid = result.catalog.validate(); !valid)
            return Result::failure(valid.error());
        return Result::success(std::move(result));
    } catch (const std::exception &e) {
        return Result::failure({core::ErrorCode::validation_failed,
                                std::string("glTF material reimport: ") + e.what()});
    }
}
namespace {
core::Transform node_transform(const Object &n) {
    core::Transform t;
    if (auto matrix = get(n, "matrix"); !matrix.empty()) {
        if (!get(n, "translation").empty() || !get(n, "rotation").empty() ||
            !get(n, "scale").empty())
            throw std::runtime_error("node cannot combine matrix and TRS");
        const auto m = vector(matrix, 16);
        if (std::abs(m[3]) + std::abs(m[7]) + std::abs(m[11]) + std::abs(m[15] - 1) > 1e-8)
            throw std::runtime_error("non-affine node matrix");
        core::Vec3d x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
        t.translation = {m[12], m[13], m[14]};
        t.scale = {std::sqrt(x.length_squared()), std::sqrt(y.length_squared()),
                   std::sqrt(z.length_squared())};
        if (std::min({t.scale.x, t.scale.y, t.scale.z}) < 1e-12)
            throw std::runtime_error("singular node matrix");
        if (core::dot(core::cross(x, y), z) < 0)
            t.scale.x = -t.scale.x;
        x = x * (1 / t.scale.x);
        y = y * (1 / t.scale.y);
        z = z * (1 / t.scale.z);
        if (std::abs(core::dot(x, y)) + std::abs(core::dot(x, z)) + std::abs(core::dot(y, z)) >
            1e-7)
            throw std::runtime_error("sheared node matrix cannot be represented by authoring TRS");
        const double trace = x.x + y.y + z.z;
        if (trace > 0) {
            const double s = std::sqrt(trace + 1) * 2;
            t.rotation = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4};
        } else if (x.x > y.y && x.x > z.z) {
            const double s = std::sqrt(1 + x.x - y.y - z.z) * 2;
            t.rotation = {s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
        } else if (y.y > z.z) {
            const double s = std::sqrt(1 + y.y - x.x - z.z) * 2;
            t.rotation = {(y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s};
        } else {
            const double s = std::sqrt(1 + z.z - x.x - y.y) * 2;
            t.rotation = {(z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s};
        }
    } else {
        const auto p = vector(get(n, "translation", "[0,0,0]"), 3),
                   q = vector(get(n, "rotation", "[0,0,0,1]"), 4),
                   s = vector(get(n, "scale", "[1,1,1]"), 3);
        t.translation = {p[0], p[1], p[2]};
        t.rotation = {q[0], q[1], q[2], q[3]};
        t.scale = {s[0], s[1], s[2]};
        const auto qn = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (std::abs(qn - 1) > 1e-5)
            throw std::runtime_error("glTF rotation is not a unit quaternion");
    }
    if (!t.finite() ||
        std::min({std::abs(t.scale.x), std::abs(t.scale.y), std::abs(t.scale.z)}) < 1e-12)
        throw std::runtime_error("invalid or singular node transform");
    t.rotation = t.rotation.normalized();
    return t;
}
} // namespace
core::Result<ImportedGltfScene> import_gltf_scene(std::span<const std::uint8_t> bytes,
                                                  GltfSceneImportOptions options) {
    try {
        const auto contents = container(bytes);
        const auto root = object(contents.json);
        fields(root,
               {"asset", "scene", "scenes", "nodes", "meshes", "buffers", "bufferViews",
                "accessors", "materials", "textures", "images", "samplers", "extensionsUsed",
                "extensionsRequired", "extras", "extensions", "animations", "skins", "cameras"});
        const auto asset = object(get(root, "asset"));
        if (text(get(asset, "version")) != "2.0" ||
            text(get(asset, "minVersion", "\"2.0\"")) != "2.0")
            throw std::runtime_error("requires glTF 2.0 without a newer minimum version");
        for (auto key : {"animations", "skins", "cameras"})
            if (!array(get(root, key, "[]"), 4096).empty())
                throw std::runtime_error(std::string(key) +
                                         " require an explicit compatible adapter; "
                                         "static import will not drop them");
        if (!object(get(root, "extensions", "{}")).empty())
            throw std::runtime_error("unsupported scene extension (including punctual lights)");
        ImportedGltfScene result;
        result.materials = need(import_gltf_materials(bytes, false));
        const auto data = buffers(root, contents);
        const auto views = array(get(root, "bufferViews", "[]"), 4096),
                   accessors = array(get(root, "accessors", "[]"), 4096);
        struct Accessor {
            std::span<const std::uint8_t> bytes;
            std::size_t count = 0, stride = 0, width = 0, components = 0;
            std::uint64_t type = 0;
            bool normalized = false;
        };
        const auto accessor = [&](std::uint64_t index, std::string_view shape) {
            const auto a = object(accessors.at(static_cast<std::size_t>(index)));
            fields(a, {"bufferView", "byteOffset", "componentType", "normalized", "count", "type",
                       "min", "max", "name"});
            if (text(get(a, "type")) != shape)
                throw std::runtime_error("accessor shape mismatch");
            Accessor out;
            out.components = shape == "SCALAR" ? 1 : shape == "VEC2" ? 2 : shape == "VEC3" ? 3 : 4;
            out.type = integer(get(a, "componentType"));
            out.width = out.type == 5126 || out.type == 5125 ? 4
                        : out.type == 5123                   ? 2
                        : out.type == 5121                   ? 1
                                                             : 0;
            if (!out.width)
                throw std::runtime_error("unsupported accessor component type");
            if (get(a, "normalized", "false") != "true" && get(a, "normalized", "false") != "false")
                throw std::runtime_error("invalid normalized flag");
            out.normalized = get(a, "normalized", "false") == "true";
            out.count = static_cast<std::size_t>(integer(get(a, "count")));
            if (!out.count || out.count > 600000)
                throw std::runtime_error("accessor count exceeds 600000");
            const auto raw = views.at(static_cast<std::size_t>(integer(get(a, "bufferView"))));
            const auto v = object(raw);
            out.bytes = buffer_view(data, raw);
            const auto offset = integer(get(a, "byteOffset", "0")),
                       stride = integer(
                           get(v, "byteStride", std::to_string(out.width * out.components)));
            if ((shape == "SCALAR" && !get(v, "byteStride").empty()) ||
                stride < out.width * out.components || stride > 252 || stride % out.width ||
                offset % out.width || integer(get(v, "byteOffset", "0")) % out.width ||
                offset > out.bytes.size() ||
                (out.count - 1) > (out.bytes.size() - offset) / stride ||
                (out.count - 1) * stride + out.width * out.components > out.bytes.size() - offset)
                throw std::runtime_error("accessor range, stride or alignment is invalid");
            out.stride = static_cast<std::size_t>(stride);
            out.bytes = out.bytes.subspan(static_cast<std::size_t>(offset));
            return out;
        };
        const auto value = [](const Accessor &a, std::size_t i, std::size_t c) {
            const auto p = i * a.stride + c * a.width;
            double v = 0;
            if (a.type == 5126)
                v = std::bit_cast<float>(u32(a.bytes, p));
            else if (a.type == 5125)
                v = u32(a.bytes, p);
            else if (a.type == 5123)
                v = std::uint32_t(a.bytes[p]) | (std::uint32_t(a.bytes[p + 1]) << 8);
            else
                v = a.bytes[p];
            if (a.normalized) {
                if (a.type == 5126 || a.type == 5125)
                    throw std::runtime_error("invalid normalized component type");
                v /= a.type == 5123 ? 65535 : 255;
            }
            if (!std::isfinite(v))
                throw std::runtime_error("non-finite accessor component");
            return v;
        };
        const auto mesh_sources = array(get(root, "meshes", "[]"), 64);
        std::size_t total_vertices = 0, total_faces = 0;
        bool omitted_source_regions = false;
        bool has_vanta_material_slots = false;
        const auto source_material_count = result.materials.catalog.materials().size();
        std::optional<std::uint64_t> default_material;
        for (auto raw_mesh : mesh_sources) {
            const auto source = object(raw_mesh);
            fields(source, {"name", "primitives", "extras"});
            if (const auto extra = get(source, "extras"); !extra.empty() && extra.front() == '{')
                omitted_source_regions = omitted_source_regions ||
                    !get(object(extra), "condition_material_region").empty();
            std::vector<geometry::Vertex> vertices;
            std::vector<geometry::Face> faces;
            std::vector<attributes::AttributeValue> normals, tangents, uvs, slots;
            std::map<std::pair<std::uint64_t, std::size_t>, geometry::VertexId> ids;
            std::set<attributes::UvSetId> uv_ids;
            bool any_uv = false, any_tangent = false, missing_tangent = false;
            for (auto raw : array(get(source, "primitives"), 128)) {
                const auto primitive = object(raw);
                fields(primitive, {"attributes", "indices", "material", "mode", "extras"});
                if (const auto extra = get(primitive, "extras"); !extra.empty() && extra.front() == '{') {
                    const auto metadata = object(extra);
                    omitted_source_regions = omitted_source_regions ||
                        !get(metadata, "cartographer_material_region").empty() ||
                        !get(metadata, "condition_material_region").empty() ||
                        !get(metadata, "cartographer_triangle_faces").empty();
                    has_vanta_material_slots = has_vanta_material_slots ||
                        !get(metadata, "vanta_material_slot").empty();
                }
                if (integer(get(primitive, "mode", "4")) != 4)
                    throw std::runtime_error("only triangle-list primitives are supported");
                const auto attrs = object(get(primitive, "attributes"));
                fields(attrs, {"POSITION", "NORMAL", "TANGENT", "TEXCOORD_0"});
                const auto position_id = integer(get(attrs, "POSITION"));
                const auto positions = accessor(position_id, "VEC3");
                if (positions.type != 5126 || positions.normalized)
                    throw std::runtime_error("POSITION requires float32");
                // Preserve authored orphan vertices too; render compilation may omit
                // them from its derived triangle stream, but editing/reopen must not.
                for (std::size_t index = 0; index < positions.count; ++index) {
                    const auto key = std::pair{position_id, index};
                    if (ids.contains(key))
                        continue;
                    if (++total_vertices > 200000)
                        throw std::runtime_error("scene exceeds 200000 vertices");
                    const geometry::VertexId id{vertices.size() + 1};
                    ids.emplace(key, id);
                    vertices.push_back({id,
                                        {value(positions, index, 0), value(positions, index, 1),
                                         value(positions, index, 2)}});
                }
                const auto optional_accessor =
                    [&](std::string_view name, std::string_view shape) -> std::optional<Accessor> {
                    if (auto v = get(attrs, name); !v.empty()) {
                        auto a = accessor(integer(v), shape);
                        if (a.count != positions.count)
                            throw std::runtime_error("vertex attribute count mismatch");
                        return a;
                    }
                    return {};
                };
                const auto ns = optional_accessor("NORMAL", "VEC3"),
                           ts = optional_accessor("TANGENT", "VEC4"),
                           us = optional_accessor("TEXCOORD_0", "VEC2");
                if ((ns && (ns->type != 5126 || ns->normalized)) ||
                    (ts && (ts->type != 5126 || ts->normalized)) ||
                    (us && us->type != 5126 && !us->normalized))
                    throw std::runtime_error("unsupported normal/tangent/UV component encoding");
                if (ts && (!ns || !us))
                    throw std::runtime_error("authored tangents require normals and UVs");
                any_uv = any_uv || us.has_value();
                any_tangent = any_tangent || ts.has_value();
                missing_tangent = missing_tangent || !ts;
                std::uint64_t material = 0;
                if (auto material_index = get(primitive, "material"); !material_index.empty()) {
                    const auto index = integer(material_index);
                    if (index >= source_material_count)
                        throw std::runtime_error("primitive refers to missing material");
                    material = index + 1;
                } else {
                    if (!options.allow_default_material_projection)
                        throw std::runtime_error(
                            "glTF default material is single-sided; explicitly accept the "
                            "two-sided default-material projection to import it");
                    if (!default_material) {
                        render::MaterialAsset projected;
                        projected.id = source_material_count + 1U;
                        projected.name = "glTF default material (two-sided projection)";
                        projected.revision = core::Revision{1U};
                        projected.definition.metallic = 1.0;
                        projected.definition.roughness = 1.0;
                        if (auto added = result.materials.catalog.insert_material(projected); !added)
                            throw std::runtime_error(added.error().message);
                        default_material = projected.id;
                        result.projected_default_material = projected.id;
                    }
                    material = *default_material;
                }
                const auto &mat = result.materials.catalog.materials().at(material).definition;
                for (const auto *ref : {&mat.base_color, &mat.metallic_roughness, &mat.normal,
                                        &mat.occlusion, &mat.emissive})
                    if (*ref) {
                        if (!us)
                            throw std::runtime_error("textured primitive has no TEXCOORD_0");
                        uv_ids.insert(ref->uv_set);
                    }
                std::optional<Accessor> indices;
                if (auto i = get(primitive, "indices"); !i.empty()) {
                    indices = accessor(integer(i), "SCALAR");
                    if (indices->type == 5126 || indices->normalized)
                        throw std::runtime_error("indices must be unsigned integers");
                }
                const auto count = indices ? indices->count : positions.count;
                if (count % 3 || count < 3 || total_faces + count / 3 > 200000)
                    throw std::runtime_error("triangle count exceeds scene bound or is incomplete");
                for (std::size_t i = 0; i < count; i += 3) {
                    geometry::Face face;
                    face.id = {faces.size() + 1};
                    std::array<std::size_t, 3> source_indices{};
                    std::array<core::Vec3d, 3> points{};
                    for (std::size_t c = 0; c < 3; ++c) {
                        const auto index =
                            indices ? static_cast<std::size_t>(value(*indices, i + c, 0)) : i + c;
                        if (index >= positions.count)
                            throw std::runtime_error("triangle index exceeds POSITION count");
                        source_indices[c] = index;
                        points[c] = {value(positions, index, 0), value(positions, index, 1),
                                     value(positions, index, 2)};
                        face.vertices.push_back(ids.at(std::pair{position_id, index}));
                    }
                    const auto derived = core::cross(points[1] - points[0], points[2] - points[0]);
                    if (!derived.finite() || derived.length_squared() < 1e-24)
                        throw std::runtime_error("degenerate source triangle");
                    for (auto index : source_indices) {
                        auto normal = derived.normalized();
                        if (ns) {
                            normal = {value(*ns, index, 0), value(*ns, index, 1),
                                      value(*ns, index, 2)};
                            if (std::abs(normal.length_squared() - 1) > 1e-4)
                                throw std::runtime_error("authored normal is not unit length");
                            normal = normal.normalized();
                        }
                        normals.push_back(normal);
                        uvs.push_back(us ? core::Vec2d{value(*us, index, 0), value(*us, index, 1)}
                                         : core::Vec2d{});
                        core::Vec4d tangent{};
                        if (ts) {
                            tangent = {value(*ts, index, 0), value(*ts, index, 1),
                                       value(*ts, index, 2), value(*ts, index, 3)};
                            const core::Vec3d direction{tangent.x, tangent.y, tangent.z};
                            if (std::abs(direction.length_squared() - 1) > 1e-5 ||
                                std::abs(core::dot(direction, normal)) > 1e-5 ||
                                std::abs(tangent.w) != 1)
                                throw std::runtime_error("invalid authored tangent frame");
                        }
                        tangents.push_back(tangent);
                    }
                    faces.push_back(std::move(face));
                    slots.push_back(static_cast<std::uint32_t>(material));
                    ++total_faces;
                }
                const auto mesh_id = result.meshes.size() + 1;
                const project::MaterialRegionKey key{mesh_id, static_cast<std::uint32_t>(material)};
                if (!result.materials.catalog.assignments().contains(key)) {
                    auto ok = result.materials.catalog.insert_assignment({key, material, {}});
                    if (!ok)
                        throw std::runtime_error(ok.error().message);
                }
            }
            if (faces.empty())
                throw std::runtime_error("empty glTF mesh");
            if (any_tangent && missing_tangent)
                throw std::runtime_error("mixed authored/missing tangents need "
                                         "explicit regeneration before import");
            geometry::EditableMesh mesh;
            if (auto ok = mesh.insert_bulk(std::move(vertices), std::move(faces)); !ok)
                throw std::runtime_error(ok.error().message);
            attributes::AttributeSet attributes(
                {{attributes::AttributeDomain::corner, normals.size()},
                 {attributes::AttributeDomain::face, slots.size()}});
            const auto layer = [&](std::string name, attributes::AttributeDomain domain,
                                   attributes::AttributeType type, const auto &values) {
                if (auto ok = attributes.add_layer({{std::move(name), domain, type}, values}); !ok)
                    throw std::runtime_error(ok.error().message);
            };
            layer("cartographer.normal0", attributes::AttributeDomain::corner,
                  attributes::AttributeType::vec3, normals);
            layer("cartographer.material_slot", attributes::AttributeDomain::face,
                  attributes::AttributeType::uint32, slots);
            attributes::UvSetTable sets;
            if (any_uv) {
                if (uv_ids.empty())
                    uv_ids.insert(render::kUv0);
                bool first = true;
                for (auto id : uv_ids) {
                    const auto name = "gltf.uv." + std::to_string(id);
                    layer(name, attributes::AttributeDomain::corner,
                          attributes::AttributeType::vec2, uvs);
                    if (auto ok = sets.add({id, "Imported UV " + std::to_string(id), name, first,
                                            first, false, 1});
                        !ok)
                        throw std::runtime_error(ok.error().message);
                    if (any_tangent) {
                        layer("cartographer.tangent." + std::to_string(id),
                              attributes::AttributeDomain::corner, attributes::AttributeType::vec4,
                              tangents);
                        layer("cartographer.tangent_uv." + std::to_string(id),
                              attributes::AttributeDomain::corner, attributes::AttributeType::vec2,
                              uvs);
                    }
                    first = false;
                }
            }
            if (auto ok = mesh.set_attribute_payload(std::move(attributes), std::move(sets)); !ok)
                throw std::runtime_error(ok.error().message);
            result.meshes.push_back(std::move(mesh));
        }
        const auto nodes = array(get(root, "nodes", "[]"), 4096);
        scene::Scene validation;
        bool has_vanta_object_ids = false;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto n = object(nodes[i]);
            fields(n, {"name", "mesh", "children", "translation", "rotation", "scale", "matrix",
                       "extras"});
            scene::SceneObject node;
            node.id = {i + 1};
            node.name = text(get(n, "name", "\"Imported node\""));
            if (node.name.empty() || node.name.size() > 256)
                throw std::runtime_error("invalid node name");
            node.local_transform = node_transform(n);
            if (auto m = get(n, "mesh"); !m.empty()) {
                const auto id = integer(m);
                if (id >= result.meshes.size())
                    throw std::runtime_error("node mesh index is invalid");
                node.mesh_asset = id + 1;
            }
            const auto extras = object(get(n, "extras", "{}"));
            const auto authoring_flag = [&](std::string_view key, std::string_view vanta_key,
                                            bool default_value) {
                const auto own = get(extras, key);
                const auto vanta = get(extras, vanta_key);
                const auto valid = [](std::string_view flag) {
                    return flag.empty() || flag == "true" || flag == "false";
                };
                if (!valid(own) || !valid(vanta))
                    throw std::runtime_error("invalid authoring node flag");
                if (!own.empty() && !vanta.empty() && own != vanta)
                    throw std::runtime_error("conflicting Cartographer and VANTA node flags");
                const auto flag = own.empty() ? vanta : own;
                return flag.empty() ? default_value : flag == "true";
            };
            node.visible = authoring_flag("visible", "vanta_visible", node.visible);
            node.locked = authoring_flag("locked", "vanta_locked", node.locked);
            has_vanta_object_ids = has_vanta_object_ids || !get(extras, "vanta_object_id").empty();
            result.nodes.push_back(std::move(node));
        }
        for (std::size_t i = 0; i < nodes.size(); ++i)
            for (auto raw : array(get(object(nodes[i]), "children", "[]"), 4096)) {
                const auto child = integer(raw);
                if (child >= nodes.size() || result.nodes[child].parent)
                    throw std::runtime_error("invalid or multiply parented scene node");
                result.nodes[child].parent = scene::ObjectId{i + 1};
            }
        // The project admits a single complete authoring hierarchy. Multiple
        // glTF scene selections must be made explicitly in an external adapter.
        const auto scenes = array(get(root, "scenes", "[]"), 2);
        if (scenes.size() != 1 || integer(get(root, "scene", "0")) != 0)
            throw std::runtime_error("import requires exactly one selected glTF scene");
        std::set<std::uint64_t> roots;
        for (auto raw : array(get(object(scenes[0]), "nodes", "[]"), 4096)) {
            auto i = integer(raw);
            if (i >= nodes.size() || result.nodes[i].parent || !roots.insert(i + 1).second)
                throw std::runtime_error("invalid scene root selection");
        }
        for (const auto &node : result.nodes) {
            if (!node.parent && !roots.contains(node.id.value))
                throw std::runtime_error("unselected nodes would be lost; select one "
                                         "complete scene before import");
            auto copy = node;
            copy.parent.reset();
            if (auto ok = validation.insert_object(copy); !ok)
                throw std::runtime_error(ok.error().message);
        }
        for (const auto &node : result.nodes)
            if (node.parent) {
                if (auto ok = validation.set_parent(node.id, node.parent); !ok)
                    throw std::runtime_error(ok.error().message);
            }
        if (result.nodes.empty() || result.meshes.empty())
            throw std::runtime_error("source has no static mesh scene");
        std::uint64_t instanced_triangles=0;
        for(const auto& node:result.nodes)if(node.mesh_asset){instanced_triangles+=result.meshes.at(*node.mesh_asset-1).face_count();if(instanced_triangles>1000000)throw std::runtime_error("scene exceeds one million instanced triangles");}
        if (omitted_source_regions)
            result.warnings.push_back(
                "Cartographer source face IDs and material-region identity are not reconstructed; imported regions are assigned by material and may merge distinct source regions.");
        result.warnings.push_back(
            "Static glTF import uses meters, right-handed Y-up coordinates; no "
            "external resource is fetched during admission. Exported triangles remain triangles; "
            "original pre-export polygon topology is not recoverable.");
        if (!contents.external.empty())
            result.warnings.push_back("The exact glTF document and its external buffers were captured together as the retained source.");
        if (has_vanta_object_ids)
            result.warnings.push_back(
                "VANTA object IDs are not used as Cartographer IDs; imported objects "
                "receive new IDs and are mapped by source node index.");
        if (has_vanta_material_slots)
            result.warnings.push_back(
                "VANTA material-slot IDs are not transferred; imported material "
                "assignments use glTF material indices instead.");
        if (default_material)
            result.warnings.push_back(
                "glTF's implicit material is single-sided; it was projected to a "
                "two-sided default PBR material, so back-face appearance can differ. "
                "Projects containing this projection require an updated Cartographer reader.");
        return core::Result<ImportedGltfScene>::success(std::move(result));
    } catch (const std::exception &e) {
        return core::Result<ImportedGltfScene>::failure(
            {core::ErrorCode::validation_failed, std::string("glTF scene import: ") + e.what()});
    }
}
} // namespace carto::io
