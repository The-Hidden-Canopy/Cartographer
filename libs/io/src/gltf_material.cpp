#include "gltf_material.hpp"
#include <algorithm>
#include <carto/production/authoring_material.hpp>
#include <carto/production/authoring_material_bake.hpp>
#include <cmath>
#include <iomanip>
#include <map>
#include <locale>
#include <set>
#include <sstream>

namespace carto::io::detail {
namespace {
using core::Result;
core::Diagnostic unsupported(std::string s) {
    return {core::ErrorCode::validation_failed, "glTF appearance export: " + std::move(s)};
}
std::string quote(std::string_view s) {
    std::string o = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (char raw : s) {
        const auto c = static_cast<unsigned char>(raw);
        if (c == '"' || c == '\\') {
            o += '\\';
            o += char(c);
        } else if (c < 32) {
            o += "\\u00";
            o += hex[c >> 4];
            o += hex[c & 15];
        } else
            o += char(c);
    }
    return o + '"';
}
std::string b64(std::span<const std::uint8_t> v) {
    constexpr char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((v.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < v.size(); i += 3) {
        unsigned x = unsigned(v[i]) << 16;
        if (i + 1 < v.size())
            x |= unsigned(v[i + 1]) << 8;
        if (i + 2 < v.size())
            x |= v[i + 2];
        o += a[(x >> 18) & 63];
        o += a[(x >> 12) & 63];
        o += i + 1 < v.size() ? a[(x >> 6) & 63] : '=';
        o += i + 2 < v.size() ? a[x & 63] : '=';
    }
    return o;
}
void be32(std::vector<std::uint8_t> &o, std::uint32_t n) {
    for (int b = 24; b >= 0; b -= 8)
        o.push_back(std::uint8_t(n >> b));
}
void chunk(std::vector<std::uint8_t> &o, const char *tag, std::span<const std::uint8_t> data) {
    be32(o, static_cast<std::uint32_t>(data.size()));
    auto start = o.size();
    o.insert(o.end(), tag, tag + 4);
    o.insert(o.end(), data.begin(), data.end());
    std::uint32_t crc = 0xffffffff;
    for (auto i = start; i < o.size(); ++i) {
        crc ^= o[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
    }
    be32(o, ~crc);
}
// Bounded independent RGBA8 encoder: filter 0, stored DEFLATE, no gamma or
// premultiplication metadata. glTF texture usage owns color interpretation.
std::vector<std::uint8_t> png(const assets::Rgba8MipLevel &p) {
    std::vector<std::uint8_t> raw;
    raw.reserve(p.pixels.size() + p.height);
    const auto stride = std::size_t(p.width) * 4;
    for (unsigned y = 0; y < p.height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), p.pixels.data() + y * stride, p.pixels.data() + (y + 1) * stride);
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    for (std::size_t i = 0; i < raw.size();) {
        const auto n = static_cast<std::uint16_t>(std::min<std::size_t>(65535, raw.size() - i));
        z.push_back(i + n == raw.size() ? 1 : 0);
        z.push_back(std::uint8_t(n));
        z.push_back(std::uint8_t(n >> 8));
        z.push_back(std::uint8_t(~n));
        z.push_back(std::uint8_t((~n) >> 8));
        z.insert(z.end(), raw.data() + i, raw.data() + i + n);
        i += n;
    }
    std::uint32_t a = 1, b = 0;
    for (auto v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);
    std::vector<std::uint8_t> o{137, 80, 78, 71, 13, 10, 26, 10}, h;
    be32(h, p.width);
    be32(h, p.height);
    h.insert(h.end(), {8, 6, 0, 0, 0});
    chunk(o, "IHDR", h);
    chunk(o, "IDAT", z);
    chunk(o, "IEND", {});
    return o;
}
bool identity(const render::ChannelMapping &m) { return m == render::ChannelMapping{}; }
std::uint8_t channel(const std::uint8_t *p, render::TextureChannel c) {
    const auto n = static_cast<unsigned>(c);
    if (n < 4)
        return p[n];
    if (n < 8)
        return std::uint8_t(255 - p[n - 4]);
    return n == 8 ? 0 : 255;
}
int wrap(project::SamplerAddressMode a) {
    return a == project::SamplerAddressMode::repeat            ? 10497
           : a == project::SamplerAddressMode::mirrored_repeat ? 33648
                                                               : 33071;
}
std::string list(const std::vector<std::string> &v) {
    std::string s = "[";
    for (auto &x : v) {
        if (s.size() > 1)
            s += ',';
        s += x;
    }
    return s + ']';
}
} // namespace
struct GltfMaterialWriter::Impl {
    const project::MaterialCatalog &catalog;
    GltfExportOptions options;
    GltfExportReport &report;
    production::AuthoringTextureLibrary loaded;
    std::vector<std::string> materials, images, textures, samplers;
    std::map<std::pair<render::MaterialId, std::optional<project::MaterialVariantId>>,
             GltfMaterialProjection>
        projected;
    std::map<assets::Sha256Digest, std::size_t> image_ids;
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> texture_ids;
    std::map<render::SamplerId, std::size_t> sampler_ids;
    std::set<std::string> extensions;
    std::set<project::MaterialRegionKey> used_assignments;
    std::set<assets::TextureAssetId> loaded_ids;
    std::uint64_t baked_texels = 0;
    std::optional<std::size_t> bake_sampler;
    Impl(const project::ProjectDocument &d, const GltfExportOptions &o, GltfExportReport &r)
        : catalog(d.material_catalog()), options(o), report(r) {}
    Result<void> load(const project::TextureSourceRecord &rec) {
        if (options.textures || loaded_ids.contains(rec.id))
            return Result<void>::success();
        if (options.project_path.empty())
            return Result<void>::failure(
                unsupported("texture pixels unavailable; supply the saved project "
                            "path or admitted texture library"));
        std::error_code e;
        auto parent = options.project_path.parent_path();
        if (parent.empty())
            parent = std::filesystem::current_path(e);
        auto root = std::filesystem::weakly_canonical(parent, e);
        if (e || root.empty())
            return Result<void>::failure(unsupported("saved project directory unavailable"));
        const auto limits = production::authoring_texture_limits();
        assets::BlobStore store(root / ".carto-assets" / "source-blobs",
                                {limits.max_source_bytes, 256});
        const auto expected = store.path_for(rec.content_digest);
        const auto actual = std::filesystem::canonical(expected, e);
        if (e)
            return Result<void>::failure(
                unsupported("texture blob missing from project asset store"));
        auto relative = actual.lexically_relative(root);
        if (actual.lexically_normal() != expected.lexically_normal() || relative.empty() ||
            relative.is_absolute() ||
            std::any_of(relative.begin(), relative.end(),
                        [](const auto &part) { return part == ".."; }))
            return Result<void>::failure(
                unsupported("texture blob must remain at its project-local "
                            "content-addressed path"));
        const auto size = std::filesystem::file_size(actual, e);
        if (e || size > limits.max_source_bytes)
            return Result<void>::failure(unsupported("texture blob exceeds source byte bound"));
        auto bytes = store.read(rec.content_digest);
        if (!bytes)
            return Result<void>::failure(bytes.error());
        auto admitted = loaded.admit(rec, bytes.value());
        if (!admitted)
            return admitted;
        loaded_ids.insert(rec.id);
        return Result<void>::success();
    }
    Result<std::string> texture(const render::TextureRef &ref, assets::TextureSemantic semantic,
                                GltfMaterialProjection &projection) {
        if (projection.uv && *projection.uv != ref.uv_set)
            return Result<std::string>::failure(
                unsupported("one material requires a single UV identity, matching "
                            "the authoring renderer"));
        projection.uv = ref.uv_set;
        const bool color = assets::expected_color_space(semantic) == assets::ColorSpace::srgb;
        const bool normal = semantic == assets::TextureSemantic::normal;
        if ((color && ref.interpretation == render::TextureInterpretation::linear) ||
            (!color && ref.interpretation == render::TextureInterpretation::srgb))
            return Result<std::string>::failure(
                unsupported("texture color-space override cannot be represented "
                            "without a lossy color bake"));
        if (normal) {
            if (!identity(ref.channels))
                return Result<std::string>::failure(
                    unsupported("normal-map channel remapping requires a tangent-space bake"));
            // Translation does not alter the frame. Other UV transforms require
            // a normal-frame bake that is deliberately outside this profile.
            if (ref.transform.rotation != 0 || ref.transform.scale.x != 1 ||
                ref.transform.scale.y != 1)
                return Result<std::string>::failure(
                    unsupported("normal-map rotation/scale requires a tangent-frame bake"));
            projection.normal_mapping = true;
        }
        if (color) {
            for (unsigned c = 0; c < 4; ++c) {
                auto n = static_cast<unsigned>(ref.channels.rgba[c]);
                bool okay =
                    c < 3 ? (n < 3 || n == 8 || n == 9) : (n == 3 || n == 7 || n == 8 || n == 9);
                if (!okay)
                    return Result<std::string>::failure(
                        unsupported("color-map channel conversion would change sRGB "
                                    "filtering; bake explicitly"));
            }
        }
        const auto record = catalog.textures().find(ref.asset);
        if (record == catalog.textures().end())
            return Result<std::string>::failure(unsupported("texture source record missing"));
        auto admission = load(record->second);
        if (!admission)
            return Result<std::string>::failure(admission.error());
        const auto *library = options.textures ? options.textures.get() : &loaded;
        auto mip =
            library->prepare(record->second, semantic, assets::expected_color_space(semantic));
        if (!mip)
            return Result<std::string>::failure(mip.error().with_context("glTF source pixels"));
        auto pixels = mip.value()->levels.front();
        for (std::size_t p = 0; p < pixels.pixels.size(); p += 4) {
            std::array<std::uint8_t, 4> src{};
            std::copy_n(pixels.pixels.data() + p, 4, src.data());
            for (unsigned c = 0; c < 4; ++c)
                pixels.pixels[p + c] = channel(src.data(), ref.channels.rgba[c]);
            if (normal && ref.normal_convention == render::NormalConvention::directx_y_minus)
                pixels.pixels[p + 1] = std::uint8_t(255 - pixels.pixels[p + 1]);
        }
        auto encoded = png(pixels);
        auto digest = assets::sha256(encoded);
        std::size_t image;
        if (auto i = image_ids.find(digest); i != image_ids.end())
            image = i->second;
        else {
            if (images.size() >= 64 ||
                report.embedded_image_bytes + encoded.size() > 64ULL * 1024 * 1024)
                return Result<std::string>::failure(
                    unsupported("embedded images exceed 64-image/64-MiB bound"));
            image = images.size();
            image_ids.emplace(digest, image);
            report.embedded_image_bytes += encoded.size();
            images.push_back("{\"uri\":\"data:image/png;base64," + b64(encoded) + "\"}");
        }
        std::size_t sampler;
        if (auto it = sampler_ids.find(ref.sampler); it != sampler_ids.end())
            sampler = it->second;
        else {
            project::SamplerIntent s;
            if (ref.sampler) {
                auto entry = catalog.samplers().find(ref.sampler);
                if (entry == catalog.samplers().end())
                    return Result<std::string>::failure(unsupported("sampler record missing"));
                s = entry->second;
            }
            const bool lin = s.minification == project::SamplerFilter::linear;
            const bool miplin = s.mip == project::SamplerFilter::linear;
            const int min = miplin ? (lin ? 9987 : 9986) : (lin ? 9985 : 9984);
            sampler = samplers.size();
            sampler_ids.emplace(ref.sampler, sampler);
            samplers.push_back(
                "{\"wrapS\":" + std::to_string(wrap(s.address_u)) +
                ",\"wrapT\":" + std::to_string(wrap(s.address_v)) + ",\"magFilter\":" +
                std::to_string(s.magnification == project::SamplerFilter::linear ? 9729 : 9728) +
                ",\"minFilter\":" + std::to_string(min) + "}");
        }
        std::size_t tex;
        auto key = std::pair(image, sampler);
        if (auto it = texture_ids.find(key); it != texture_ids.end())
            tex = it->second;
        else {
            tex = textures.size();
            texture_ids.emplace(key, tex);
            textures.push_back("{\"source\":" + std::to_string(image) +
                               ",\"sampler\":" + std::to_string(sampler) + "}");
        }
        std::ostringstream o;
        o.imbue(std::locale::classic());
        o << std::setprecision(17) << "\"index\":" << tex << ",\"texCoord\":0";
        const auto &t = ref.transform;
        if (t.offset.x != 0 || t.offset.y != 0 || t.scale.x != 1 || t.scale.y != 1 ||
            t.rotation != 0) {
            const double c = std::cos(t.rotation), s = std::sin(t.rotation);
            const double x =
                t.pivot.x + t.offset.x - c * t.scale.x * t.pivot.x + s * t.scale.y * t.pivot.y;
            const double y =
                t.pivot.y + t.offset.y - s * t.scale.x * t.pivot.x - c * t.scale.y * t.pivot.y;
            extensions.insert("KHR_texture_transform");
            o << ",\"extensions\":{\"KHR_texture_transform\":{\"offset\":[" << x << ',' << y
              << "],\"scale\":[" << t.scale.x << ',' << t.scale.y << "],\"rotation\":" << t.rotation
              << "}}";
        }
        return Result<std::string>::success(o.str());
    }
    Result<std::string> baked_texture(const assets::Rgba8MipLevel& pixels) {
        auto encoded=png(pixels);const auto digest=assets::sha256(encoded);
        std::size_t image;
        if(auto i=image_ids.find(digest);i!=image_ids.end())image=i->second;
        else {
            if(images.size()>=64||report.embedded_image_bytes+encoded.size()>64ULL*1024*1024)
                return Result<std::string>::failure(unsupported("baked images exceed export image budget"));
            image=images.size();image_ids.emplace(digest,image);report.embedded_image_bytes+=encoded.size();
            images.push_back("{\"uri\":\"data:image/png;base64,"+b64(encoded)+"\"}");
        }
        std::size_t sampler;
        if(bake_sampler)sampler=*bake_sampler;
        else {
            sampler=samplers.size();bake_sampler=sampler;
            samplers.push_back("{\"wrapS\":33071,\"wrapT\":33071,\"magFilter\":9729,\"minFilter\":9729}");
        }
        const auto key=std::pair(image,sampler);std::size_t tex;
        if(auto t=texture_ids.find(key);t!=texture_ids.end())tex=t->second;
        else {tex=textures.size();texture_ids.emplace(key,tex);textures.push_back("{\"source\":"+std::to_string(image)+",\"sampler\":"+std::to_string(sampler)+"}");}
        return Result<std::string>::success("\"index\":"+std::to_string(tex)+",\"texCoord\":0");
    }
    Result<production::AuthoringMaterialBake> bake(render::MaterialId id, std::optional<project::MaterialVariantId> variant) {
        using R=Result<production::AuthoringMaterialBake>;
        const auto n=options.graph_bake_resolution;
        if(!production::authoring_material_bake_fits_budget(n,baked_texels))
            return R::failure(unsupported("graph baking requires power-of-two resolution "+
                std::to_string(production::kMinAuthoringMaterialBakeResolution)+".."+
                std::to_string(production::kMaxAuthoringMaterialBakeResolution)+
                " and at most 1M total texels across unique material/variant bakes"));
        auto base=catalog.resolve_material(id,variant);if(!base)return R::failure(base.error());
        for(const auto& ref:{base.value().base_color,base.value().metallic_roughness,base.value().normal,base.value().occlusion,base.value().emissive})if(ref) {
            const auto entry=catalog.textures().find(ref.asset);
            if(entry==catalog.textures().end())return R::failure(unsupported("graph bake texture source missing"));
            const auto admitted=load(entry->second);if(!admitted)return R::failure(admitted.error());
        }
        auto program=production::compile_authoring_material(catalog,id,variant,options.textures?options.textures.get():&loaded);
        if(!program)return R::failure(program.error());
        auto output=production::bake_authoring_material(program.value(),n);
        if(output)baked_texels+=std::uint64_t(n)*n;
        return output;
    }
    Result<GltfMaterialProjection> material(std::uint64_t mesh, std::optional<std::uint32_t> slot) {
        render::MaterialId id = 0;
        std::optional<project::MaterialVariantId> variant;
        if (auto a = catalog.assignments().find({mesh, slot.value_or(0)});
            a != catalog.assignments().end()) {
            id = a->second.material;
            variant = a->second.variant;
            used_assignments.insert(a->first);
        }
        return project(id, variant);
    }
    Result<GltfMaterialProjection> project(render::MaterialId id,
                                            std::optional<project::MaterialVariantId> variant) {
        auto key = std::pair(id, variant);
        if (auto it = projected.find(key); it != projected.end())
            return Result<GltfMaterialProjection>::success(it->second);
        if (materials.size() >= 32)
            return Result<GltfMaterialProjection>::failure(
                unsupported("resolved materials exceed 32-material export bound"));
        render::StandardMaterial m;
        std::optional<production::AuthoringMaterialBake> baked;
        if (id) {
            auto r = production::resolve_authoring_material(catalog, id, variant);
            if (!r && options.graph_bake_resolution) {
                auto result=bake(id,variant);
                if(!result)return Result<GltfMaterialProjection>::failure(result.error());
                baked=std::move(result.value());m=baked->factors;
            } else if (!r)
                return Result<GltfMaterialProjection>::failure(
                    unsupported("material graph requires explicit baking: " + r.error().message));
            else m = r.value();
        }
        if (m.base_color_factor.x > 1 || m.base_color_factor.y > 1 || m.base_color_factor.z > 1)
            return Result<GltfMaterialProjection>::failure(
                unsupported("HDR base-color factors exceed glTF's [0,1] range"));
        if (m.dielectric_f0 >= 1)
            return Result<GltfMaterialProjection>::failure(
                unsupported("dielectric F0=1 has no finite IOR projection"));
        GltfMaterialProjection p{materials.size(), {}, false};
        if(baked){p.uv=baked->uv;p.normal_mapping=baked->normal_mapping;p.baked=true;}
        const std::array refs{m.base_color, m.metallic_roughness, m.normal, m.occlusion,
                              m.emissive};
        const std::array semantics{
            assets::TextureSemantic::base_color, assets::TextureSemantic::metallic_roughness,
            assets::TextureSemantic::normal, assets::TextureSemantic::occlusion,
            assets::TextureSemantic::emissive};
        std::array<std::string, 5> info;
        std::string clearcoat_info;
        if(baked) {
            for(unsigned i=0;i<5;++i) {
                if(i==2&&!baked->normal_mapping)continue;
                auto t=baked_texture(baked->maps[i]);if(!t)return Result<GltfMaterialProjection>::failure(t.error());
                info[i]=std::move(t.value());
            }
            auto t=baked_texture(baked->maps[5]);if(!t)return Result<GltfMaterialProjection>::failure(t.error());clearcoat_info=std::move(t.value());
            report.warnings.push_back("Explicit material graph bake: "+std::to_string(options.graph_bake_resolution)+
                "x"+std::to_string(options.graph_bake_resolution)+" RGBA8, unit UV tile, finite-resolution/color quantization; program "+baked->program_digest.hex()+". Analytic/minification parity is not implied. Cartographer's current glTF importer cannot reimport baked clearcoat textures or non-mip samplers.");
        }
        for (unsigned i = 0; i < 5; ++i)
            if (refs[i]) {
                auto t = texture(refs[i], semantics[i], p);
                if (!t)
                    return Result<GltfMaterialProjection>::failure(t.error());
                info[i] = std::move(t.value());
            }
        std::ostringstream o;
        o.imbue(std::locale::classic());
        o << std::setprecision(17) << '{';
        if (id) {
            const auto named = catalog.materials().find(id);
            if (named == catalog.materials().end())
                return Result<GltfMaterialProjection>::failure(unsupported("material name source missing"));
            o << "\"name\":" << quote(named->second.name) << ',';
        }
        o << "\"pbrMetallicRoughness\":{\"baseColorFactor\":["
          << m.base_color_factor.x << ',' << m.base_color_factor.y << ',' << m.base_color_factor.z
          << ',' << m.base_color_factor.w << "],\"metallicFactor\":" << m.metallic
          << ",\"roughnessFactor\":" << m.roughness;
        if (!info[0].empty())
            o << ",\"baseColorTexture\":{" << info[0] << '}';
        if (!info[1].empty())
            o << ",\"metallicRoughnessTexture\":{" << info[1] << '}';
        o << '}';
        if (!info[2].empty())
            o << ",\"normalTexture\":{" << info[2] << ",\"scale\":" << m.normal_scale << '}';
        if (!info[3].empty())
            o << ",\"occlusionTexture\":{" << info[3] << ",\"strength\":" << m.occlusion_strength
              << '}';
        if (!info[4].empty())
            o << ",\"emissiveTexture\":{" << info[4] << '}';
        const double emission_scale =
            std::max({1.0, m.emissive_factor.x, m.emissive_factor.y, m.emissive_factor.z});
        const double strength = m.emissive_strength * emission_scale;
        if (!std::isfinite(strength))
            return Result<GltfMaterialProjection>::failure(
                unsupported("emissive product overflows"));
        o << ",\"emissiveFactor\":[" << m.emissive_factor.x / emission_scale << ','
          << m.emissive_factor.y / emission_scale << ',' << m.emissive_factor.z / emission_scale
          << ']';
        o << ",\"doubleSided\":true,\"alphaMode\":\""
          << (m.alpha_mode == render::AlphaMode::opaque   ? "OPAQUE"
              : m.alpha_mode == render::AlphaMode::masked ? "MASK"
                                                          : "BLEND")
          << '"';
        if (m.alpha_mode == render::AlphaMode::masked)
            o << ",\"alphaCutoff\":" << m.alpha_cutoff;
        if (m.alpha_mode == render::AlphaMode::blended)
            report.warnings.push_back(
                "BLEND alpha is exported as authored; the native authoring renderer "
                "does not yet validate blended appearance");
        const double root = std::sqrt(m.dielectric_f0), ior = (1 + root) / (1 - root);
        const bool use_ior = m.dielectric_f0 != 0.04;
        const bool use_clearcoat = m.clearcoat != 0.0 || !clearcoat_info.empty();
        const bool zero_emission = m.emissive_factor.x == 0.0 && m.emissive_factor.y == 0.0 &&
                                   m.emissive_factor.z == 0.0;
        const bool use_emission_strength = !zero_emission && strength != 1.0;
        if ((!use_clearcoat && m.clearcoat_roughness != 0.0) ||
            (zero_emission && strength != 1.0))
            report.warnings.push_back("Inactive clearcoat roughness or zero-emission strength remains in the source material; the exported appearance is unchanged.");
        if (use_ior || use_clearcoat || use_emission_strength) {
            o << ",\"extensions\":{";
            bool first = true;
            const auto separator = [&] { if (!first) o << ','; first = false; };
            if (use_ior) {
                separator();
                extensions.insert("KHR_materials_ior");
                o << "\"KHR_materials_ior\":{\"ior\":" << ior << '}';
            }
            if (use_clearcoat) {
                separator();
                extensions.insert("KHR_materials_clearcoat");
                o << "\"KHR_materials_clearcoat\":{\"clearcoatFactor\":" << m.clearcoat
                  << ",\"clearcoatRoughnessFactor\":" << m.clearcoat_roughness;
                if(!clearcoat_info.empty())o << ",\"clearcoatTexture\":{" << clearcoat_info << "},\"clearcoatRoughnessTexture\":{" << clearcoat_info << '}';
                o << '}';
            }
            if (use_emission_strength) {
                separator();
                extensions.insert("KHR_materials_emissive_strength");
                o << "\"KHR_materials_emissive_strength\":{\"emissiveStrength\":" << strength << '}';
            }
            o << '}';
        }
        o << ",\"extras\":{\"cartographer_material_id\":" << id;
        if (p.uv)
            o << ",\"cartographer_uv_set\":" << quote(std::to_string(*p.uv));
        if (variant)
            o << ",\"cartographer_variant_id\":" << quote(std::to_string(*variant));
        o << "}}";
        materials.push_back(o.str());
        projected.emplace(key, p);
        return Result<GltfMaterialProjection>::success(p);
    }
};
GltfMaterialWriter::GltfMaterialWriter(const project::ProjectDocument &d,
                                       const GltfExportOptions &o, GltfExportReport &r)
    : impl_(std::make_unique<Impl>(d, o, r)) {}
GltfMaterialWriter::~GltfMaterialWriter() = default;
Result<GltfMaterialProjection> GltfMaterialWriter::material(std::uint64_t m,
                                                            std::optional<std::uint32_t> s) {
    return impl_->material(m, s);
}
Result<void> GltfMaterialWriter::prepare_catalog() {
    // Preserve the original complete, sorted index mapping for the old bounded
    // constant-only catalog profile. Extended catalogs project only referenced
    // material/variant pairs, so an unrelated unsupported unused material cannot
    // cause a visual export to fail or masquerade as a placeholder material.
    const auto& catalog = impl_->catalog;
    if (catalog.materials().size() > 32U || !catalog.textures().empty() ||
        !catalog.samplers().empty() || !catalog.variants().empty() ||
        !catalog.material_graphs().empty() || !catalog.material_graph_bindings().empty())
        return Result<void>::success();
    for (const auto& [id, asset] : catalog.materials()) {
        static_cast<void>(id);
        const auto& m = asset.definition;
        if (m.base_color_factor.x > 1.0 || m.base_color_factor.y > 1.0 ||
            m.base_color_factor.z > 1.0 || m.alpha_mode != render::AlphaMode::opaque ||
            m.base_color || m.metallic_roughness || m.normal || m.occlusion || m.emissive ||
            m.normal_scale != 1.0 || m.occlusion_strength != 1.0 || m.dielectric_f0 != 0.04 ||
            m.clearcoat != 0.0 || m.clearcoat_roughness != 0.1 ||
            m.emissive_factor.x != 0.0 || m.emissive_factor.y != 0.0 || m.emissive_factor.z != 0.0)
            return Result<void>::success();
    }
    for (const auto& [id, material] : catalog.materials()) {
        static_cast<void>(material);
        auto result = impl_->project(id, std::nullopt);
        if (!result) return Result<void>::failure(result.error());
    }
    return Result<void>::success();
}
Result<std::string> GltfMaterialWriter::finish() {
    auto &i = *impl_;
    auto &r = i.report;
    r.materials = i.materials.size();
    r.images = i.images.size();
    r.textures = i.textures.size();
    if (i.used_assignments.size() != i.catalog.assignments().size())
        r.warnings.push_back("Catalog assignments without an exported material-region "
                             "range are not interchange materials");
    for (const auto& [id, material] : i.catalog.materials()) {
        static_cast<void>(material);
        const bool emitted = std::any_of(i.projected.begin(), i.projected.end(),
            [id](const auto& item) { return item.first.first == id; });
        if (!emitted) {
            r.warnings.push_back("Unreferenced catalog materials were omitted from this derived artifact; source material IDs remain in emitted material extras");
            break;
        }
    }
    if (!i.images.empty())
        r.warnings.push_back("Embedded PNGs preserve source level-zero pixels and "
                             "sampler intent; consumer-generated mip filtering is "
                             "not rendered minification parity evidence");
    std::string o = "\"materials\":" + list(i.materials) + ',';
    if (!i.images.empty())
        o += "\"images\":" + list(i.images) + ",\"textures\":" + list(i.textures) +
             ",\"samplers\":" + list(i.samplers) + ',';
    std::vector<std::string> ext;
    for (auto &e : i.extensions)
        ext.push_back(quote(e));
    if (!ext.empty())
        o += "\"extensionsUsed\":" + list(ext) + ",\"extensionsRequired\":" + list(ext) + ',';
    return Result<std::string>::success(std::move(o));
}
} // namespace carto::io::detail
