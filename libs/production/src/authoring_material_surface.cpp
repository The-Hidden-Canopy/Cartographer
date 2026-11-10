#include <carto/production/authoring_material.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>

namespace carto::production {
namespace {
using Value = std::array<double, 4>;
using Kind = project::MaterialGraphNodeKind;
using Type = project::MaterialGraphValueType;
template <class T> T need(core::Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(result.value());
}
void need(core::Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}
core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::validation_failed,
            "authoring surface material: " + std::move(message)};
}
} // namespace
assets::TextureSourceLimits authoring_texture_limits() {
    assets::TextureSourceLimits l;
    l.max_source_bytes = 32U * 1024U * 1024U;
    l.max_dimension = 2048;
    l.max_pixels = 4'194'304;
    return l;
}
namespace {
constexpr std::uint64_t kDecodedBudget = 64U * 1024U * 1024U, kMipBudget = 64U * 1024U * 1024U;
struct BoundTexture {
    render::TextureRef binding;
    project::SamplerIntent sampler;
    std::shared_ptr<const assets::Rgba8MipChain> mips;
};
double linear(double x) { return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); }
Value lerp(Value a, const Value &b, double t) {
    for (unsigned i = 0; i < 4; ++i)
        a[i] = a[i] * (1 - t) + b[i] * t;
    return a;
}
std::int64_t address(std::int64_t i, std::int64_t n, project::SamplerAddressMode mode) {
    if (mode == project::SamplerAddressMode::clamp_to_edge)
        return std::clamp(i, std::int64_t{0}, n - 1);
    const auto period = mode == project::SamplerAddressMode::repeat ? n : 2 * n;
    i = (i % period + period) % period;
    return i < n ? i : period - 1 - i;
}
core::Vec2d transform_uv(core::Vec2d v, const render::TextureTransform &t, bool derivative) {
    if (!derivative)
        v = {v.x - t.pivot.x, v.y - t.pivot.y};
    v = {v.x * t.scale.x, v.y * t.scale.y};
    const double c = std::cos(t.rotation), s = std::sin(t.rotation);
    v = {c * v.x - s * v.y, s * v.x + c * v.y};
    return derivative ? v : core::Vec2d{v.x + t.pivot.x + t.offset.x, v.y + t.pivot.y + t.offset.y};
}
Value texel(const BoundTexture &t, const assets::Rgba8MipLevel &level, std::int64_t x,
            std::int64_t y) {
    x = address(x, level.width, t.sampler.address_u);
    y = address(y, level.height, t.sampler.address_v);
    const auto offset =
        (static_cast<std::size_t>(y) * level.width + static_cast<std::size_t>(x)) * 4;
    Value value{};
    for (unsigned c = 0; c < 4; ++c)
        value[c] = level.pixels[offset + c] / 255.0;
    if (t.mips->color_space == assets::ColorSpace::srgb)
        for (unsigned c = 0; c < 3; ++c)
            value[c] = linear(value[c]);
    return value;
}
Value level_sample(const BoundTexture &t, unsigned mip, core::Vec2d uv, bool minifying) {
    const auto &level = t.mips->levels[mip];
    const double x = uv.x * level.width - .5, y = uv.y * level.height - .5;
    const auto filter = minifying ? t.sampler.minification : t.sampler.magnification;
    if (filter == project::SamplerFilter::nearest)
        return texel(t, level, static_cast<std::int64_t>(std::floor(x + .5)),
                     static_cast<std::int64_t>(std::floor(y + .5)));
    const auto ix = static_cast<std::int64_t>(std::floor(x)),
               iy = static_cast<std::int64_t>(std::floor(y));
    return lerp(
        lerp(texel(t, level, ix, iy), texel(t, level, ix + 1, iy), x - std::floor(x)),
        lerp(texel(t, level, ix, iy + 1), texel(t, level, ix + 1, iy + 1), x - std::floor(x)),
        y - std::floor(y));
}
Value sample(const BoundTexture &t, core::Vec2d uv, core::Vec2d dx, core::Vec2d dy) {
    uv = transform_uv(uv, t.binding.transform, false);
    dx = transform_uv(dx, t.binding.transform, true);
    dy = transform_uv(dy, t.binding.transform, true);
    if (!uv.finite() || !dx.finite() || !dy.finite() || std::abs(uv.x) > 1e6 ||
        std::abs(uv.y) > 1e6)
        throw std::runtime_error(
            "texture coordinates/derivatives exceed the finite sampling bound");
    const auto &base = t.mips->levels.front();
    const auto rho = std::max(std::hypot(dx.x * base.width, dx.y * base.height),
                              std::hypot(dy.x * base.width, dy.y * base.height));
    if (!std::isfinite(rho))
        throw std::runtime_error("texture footprint overflow");
    const auto last = static_cast<unsigned>(t.mips->levels.size() - 1);
    const double lod = std::clamp(std::log2(std::max(1.0, rho)), 0.0, double(last));
    const auto lo = static_cast<unsigned>(std::floor(lod));
    Value value = t.sampler.mip == project::SamplerFilter::nearest
                      ? level_sample(t, static_cast<unsigned>(std::round(lod)), uv, rho > 1)
                      : lerp(level_sample(t, lo, uv, rho > 1),
                             level_sample(t, std::min(lo + 1, last), uv, rho > 1), lod - lo);
    Value mapped{};
    for (unsigned c = 0; c < 4; ++c) {
        const auto channel = static_cast<unsigned>(t.binding.channels.rgba[c]);
        mapped[c] = channel < 4    ? value[channel]
                    : channel < 8  ? 1 - value[channel - 4]
                    : channel == 8 ? 0
                                   : 1;
    }
    return mapped;
}
enum class Parameter { base, emission, alpha, roughness, metallic, f0, coat, coat_roughness, u, v };
Parameter parameter(std::string_view name, Type type) {
    if (type == Type::color) {
        if (name == "base_color")
            return Parameter::base;
        if (name == "emissive")
            return Parameter::emission;
    } else {
        if (name == "alpha")
            return Parameter::alpha;
        if (name == "roughness")
            return Parameter::roughness;
        if (name == "metallic")
            return Parameter::metallic;
        if (name == "dielectric_f0")
            return Parameter::f0;
        if (name == "clearcoat")
            return Parameter::coat;
        if (name == "clearcoat_roughness")
            return Parameter::coat_roughness;
        if (name == "uv.u")
            return Parameter::u;
        if (name == "uv.v")
            return Parameter::v;
    }
    throw std::runtime_error("unbound graph parameter: " + std::string(name));
}
Value parameter_value(Parameter p, const render::StandardMaterial &m, core::Vec2d uv) {
    switch (p) {
    case Parameter::base:
        return {m.base_color_factor.x, m.base_color_factor.y, m.base_color_factor.z,
                m.base_color_factor.w};
    case Parameter::emission:
        return {m.emissive_factor.x * m.emissive_strength,
                m.emissive_factor.y * m.emissive_strength,
                m.emissive_factor.z * m.emissive_strength, 1};
    case Parameter::alpha:
        return {m.base_color_factor.w};
    case Parameter::roughness:
        return {m.roughness};
    case Parameter::metallic:
        return {m.metallic};
    case Parameter::f0:
        return {m.dielectric_f0};
    case Parameter::coat:
        return {m.clearcoat};
    case Parameter::coat_roughness:
        return {m.clearcoat_roughness};
    case Parameter::u:
        return {uv.x};
    case Parameter::v:
        return {uv.y};
    }
    return {};
}
} // namespace

struct AuthoringTextureLibrary::Impl {
    struct Entry {
        project::TextureSourceRecord record;
        assets::DecodedRgba8Texture decoded;
        std::map<std::pair<assets::TextureSemantic, assets::ColorSpace>,
                 std::shared_ptr<const assets::Rgba8MipChain>>
            mips;
    };
    mutable std::mutex mutex;
    std::map<assets::TextureAssetId, Entry> entries;
    std::uint64_t decoded_bytes = 0;
    mutable std::uint64_t mip_bytes = 0;
};
AuthoringTextureLibrary::AuthoringTextureLibrary() : impl_(std::make_unique<Impl>()) {}
AuthoringTextureLibrary::~AuthoringTextureLibrary() = default;
core::Result<void> AuthoringTextureLibrary::admit(const project::TextureSourceRecord &record,
                                                  std::span<const std::uint8_t> encoded) {
    try {
        need(record.validate());
        auto decoded = need(assets::decode_texture_rgba8(encoded, authoring_texture_limits()));
        if (decoded.admission.source.digest != record.content_digest ||
            decoded.admission.width != record.width || decoded.admission.height != record.height ||
            decoded.admission.format != record.format)
            throw std::runtime_error(
                "decoded texture does not match its authored digest/format/extent");
        std::lock_guard lock(impl_->mutex);
        auto old = impl_->entries.find(record.id);
        const auto prior = old == impl_->entries.end() ? 0U : old->second.decoded.pixels.size();
        if ((old == impl_->entries.end() && impl_->entries.size() == 16) ||
            impl_->decoded_bytes - prior + decoded.pixels.size() > kDecodedBudget)
            throw std::runtime_error("decoded texture library budget exceeded");
        std::uint64_t retired = 0;
        if (old != impl_->entries.end())
            for (const auto &[key, chain] : old->second.mips) {
                static_cast<void>(key);
                retired += chain->resident_bytes();
            }
        impl_->entries.insert_or_assign(record.id, Impl::Entry{record, std::move(decoded), {}});
        impl_->decoded_bytes =
            impl_->decoded_bytes - prior + impl_->entries.at(record.id).decoded.pixels.size();
        impl_->mip_bytes -= retired;
        return core::Result<void>::success();
    } catch (const std::exception &e) {
        return core::Result<void>::failure(invalid(e.what()));
    }
}
core::Result<std::shared_ptr<const assets::Rgba8MipChain>>
AuthoringTextureLibrary::prepare(const project::TextureSourceRecord &record,
                                 assets::TextureSemantic semantic, assets::ColorSpace space) const {
    using Result = core::Result<std::shared_ptr<const assets::Rgba8MipChain>>;
    try {
        std::lock_guard lock(impl_->mutex);
        auto found = impl_->entries.find(record.id);
        if (found == impl_->entries.end() ||
            found->second.record.content_digest != record.content_digest ||
            found->second.record.revision != record.revision ||
            found->second.record.width != record.width ||
            found->second.record.height != record.height ||
            found->second.record.format != record.format)
            throw std::runtime_error("texture source is missing or stale in the decoded library");
        auto &entry = found->second;
        const auto key = std::pair{semantic, space};
        if (const auto ready = entry.mips.find(key); ready != entry.mips.end())
            return Result::success(ready->second);
        auto mip = std::make_shared<const assets::Rgba8MipChain>(need(assets::generate_rgba8_mips(
            entry.decoded, {semantic, space}, authoring_texture_limits())));
        if (impl_->mip_bytes + mip->resident_bytes() > kMipBudget)
            throw std::runtime_error("semantic mip library budget exceeded");
        entry.mips.emplace(key, mip);
        impl_->mip_bytes += mip->resident_bytes();
        return Result::success(std::move(mip));
    } catch (const std::exception &e) {
        return Result::failure(invalid(e.what()));
    }
}
std::uint64_t AuthoringTextureLibrary::resident_bytes() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->decoded_bytes + impl_->mip_bytes;
}

core::Result<std::shared_ptr<const AuthoringTextureLibrary>>
AuthoringTextureLibrary::snapshot() const {
    using Result = core::Result<std::shared_ptr<const AuthoringTextureLibrary>>;
    try {
        auto copy = std::make_shared<AuthoringTextureLibrary>();
        std::lock_guard lock(impl_->mutex);
        if (impl_->entries.size() > 16U || impl_->decoded_bytes > kDecodedBudget)
            return Result::failure(invalid("decoded library exceeds its snapshot budget"));
        std::uint64_t copied_bytes = 0;
        for (const auto &[id, entry] : impl_->entries) {
            const auto &record = entry.record;
            const auto &decoded = entry.decoded;
            if (id != record.id || !record.validate() ||
                !decoded.validate(authoring_texture_limits()) ||
                decoded.admission.source.digest != record.content_digest ||
                decoded.admission.width != record.width ||
                decoded.admission.height != record.height ||
                decoded.admission.format != record.format)
                return Result::failure(invalid("decoded snapshot source is invalid or stale"));
            const auto size = static_cast<std::uint64_t>(decoded.pixels.size());
            if (size > kDecodedBudget - copied_bytes)
                return Result::failure(invalid("decoded snapshot exceeds its byte budget"));
            // Record and pixels are owned values. Do not copy the mutable mip
            // cache or share its maps with the source library.
            copy->impl_->entries.emplace(id, Impl::Entry{record, decoded, {}});
            copied_bytes += size;
        }
        if (copied_bytes != impl_->decoded_bytes)
            return Result::failure(invalid("decoded snapshot byte accounting is inconsistent"));
        copy->impl_->decoded_bytes = copied_bytes;
        return Result::success(std::shared_ptr<const AuthoringTextureLibrary>(std::move(copy)));
    } catch (const std::bad_alloc &) {
        return Result::failure({core::ErrorCode::invalid_state, "texture snapshot allocation failed"});
    } catch (const std::exception &e) {
        return Result::failure(invalid(e.what()));
    }
}

struct AuthoringMaterialProgram::Impl {
    struct Instruction {
        Kind kind;
        Type type;
        Value value{};
        Parameter parameter = Parameter::alpha;
        std::array<unsigned, 3> inputs{};
        unsigned texture = 0;
    };
    render::StandardMaterial source, base;
    std::vector<BoundTexture> textures;
    std::array<std::optional<unsigned>, 5> maps;
    std::vector<Instruction> instructions;
    project::MaterialGraphOutputs outputs;
    std::map<std::uint64_t, unsigned> slots;
    bool base_explicit_texture = false, emission_explicit_texture = false;
    std::optional<render::UvSetId> uv;
};
std::optional<render::UvSetId> AuthoringMaterialProgram::uv_set() const {
    return impl_ ? impl_->uv : std::nullopt;
}
bool AuthoringMaterialProgram::normal_mapping() const { return impl_ && bool(impl_->maps[2]); }
bool AuthoringMaterialProgram::per_fragment() const {
    return impl_ && (!impl_->textures.empty() || !impl_->instructions.empty());
}
std::uint64_t AuthoringMaterialProgram::texture_bytes() const {
    std::uint64_t total = 0;
    std::set<const assets::Rgba8MipChain *> seen;
    if (impl_)
        for (const auto &t : impl_->textures)
            if (seen.insert(t.mips.get()).second)
                total += t.mips->resident_bytes();
    return total;
}
core::Result<AuthoringMaterialGpuPacket> AuthoringMaterialProgram::gpu_packet() const {
    using Result = core::Result<AuthoringMaterialGpuPacket>;
    try {
        AuthoringMaterialGpuPacket packet;
        const auto count = impl_ ? impl_->instructions.size() : 0U;
        packet.material =
            impl_ ? (count ? impl_->source : impl_->base) : render::StandardMaterial{};
        packet.words.resize(((24 + count * 3 + 31) / 32) * 32);
        const auto f = [](double v) {
            if (!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max())
                throw std::runtime_error("GPU packet value exceeds finite float32");
            return static_cast<float>(v);
        };
        packet.words[0] = {1, f(double(count)),
                           f(impl_ ? unsigned(impl_->base_explicit_texture) +
                                         2U * unsigned(impl_->emission_explicit_texture)
                                   : 0),
                           0};
        packet.words[1] = packet.words[2] = {-1, -1, -1, -1};
        packet.words[3] = {f(packet.material.clearcoat), f(packet.material.clearcoat_roughness), 0,
                           0};
        std::vector<std::uint8_t> identity;
        const auto append_identity = [&](auto value) {
            const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
            identity.insert(identity.end(), bytes, bytes + sizeof(value));
        };
        if (impl_)
            for (unsigned slot = 0; slot < 5; ++slot)
                if (impl_->maps[slot]) {
                    const auto &texture = impl_->textures[*impl_->maps[slot]];
                    packet.maps[slot] = texture.mips;
                    const auto &sampler = texture.sampler;
                    const auto &t = texture.binding.transform;
                    const unsigned flags =
                        unsigned(sampler.minification == project::SamplerFilter::linear) |
                        (unsigned(sampler.magnification == project::SamplerFilter::linear) << 1) |
                        (unsigned(sampler.mip == project::SamplerFilter::linear) << 2) |
                        (unsigned(sampler.address_u) << 3) | (unsigned(sampler.address_v) << 5) |
                        (unsigned(texture.binding.normal_convention ==
                                  render::NormalConvention::opengl_y_plus)
                         << 7);
                    const auto &base = texture.mips->levels.front();
                    packet.words[4 + slot * 4] = {f(base.width), f(base.height),
                                                  f(double(texture.mips->levels.size())), f(flags)};
                    packet.words[5 + slot * 4] = {f(t.offset.x), f(t.offset.y), f(t.scale.x),
                                                  f(t.scale.y)};
                    packet.words[6 + slot * 4] = {f(std::cos(t.rotation)), f(std::sin(t.rotation)),
                                                  f(t.pivot.x), f(t.pivot.y)};
                    for (unsigned c = 0; c < 4; ++c)
                        packet.words[7 + slot * 4][c] =
                            f(unsigned(texture.binding.channels.rgba[c]));
                    append_identity(texture.binding.asset);
                    append_identity(texture.binding.uv_set);
                    append_identity(texture.binding.sampler);
                    append_identity(sampler.revision.value());
                    identity.insert(identity.end(), texture.mips->artifact_digest.bytes.begin(),
                                    texture.mips->artifact_digest.bytes.end());
                }
        if (count) {
            std::vector<unsigned> uses(count), registers(count);
            const auto arity = [](Kind kind) {
                return kind == Kind::mix ? 3U
                       : (kind == Kind::texture_sample || kind == Kind::add ||
                          kind == Kind::multiply)
                           ? 2U
                           : 0U;
            };
            for (const auto &instruction : impl_->instructions)
                for (unsigned i = 0; i < arity(instruction.kind); ++i)
                    ++uses[instruction.inputs[i]];
            const auto &o = impl_->outputs;
            const std::array outputs{o.base_color, o.alpha,     o.roughness,          o.metallic,
                                     o.emissive,   o.clearcoat, o.clearcoat_roughness};
            for (auto id : outputs)
                if (id)
                    ++uses[impl_->slots.at(id)];
            std::array<bool, 32> occupied{};
            unsigned high = 0;
            for (unsigned i = 0; i < count; ++i) {
                const auto &n = impl_->instructions[i];
                auto &inputs = packet.words[25 + i * 3];
                for (unsigned j = 0; j < arity(n.kind); ++j) {
                    const auto source = n.inputs[j];
                    inputs[1 + j] = f(registers[source]);
                    if (--uses[source] == 0)
                        occupied[registers[source]] = false;
                }
                const auto available = std::find(occupied.begin(), occupied.end(), false);
                if (available == occupied.end())
                    throw std::runtime_error("GPU graph exceeds 32 live registers");
                const auto destination = static_cast<unsigned>(available - occupied.begin());
                registers[i] = destination;
                occupied[destination] = uses[i] != 0;
                high = std::max(high, destination + 1);
                inputs[0] = f(destination);
                unsigned physical_map = 0;
                if (n.kind == Kind::texture_sample)
                    for (unsigned slot = 0; slot < 5; ++slot)
                        if (impl_->maps[slot] && *impl_->maps[slot] == n.texture) {
                            physical_map = slot;
                            break;
                        }
                packet.words[24 + i * 3] = {f(unsigned(n.kind)), f(unsigned(n.type)),
                                            f(unsigned(n.parameter)), f(physical_map)};
                for (unsigned c = 0; c < 4; ++c)
                    packet.words[26 + i * 3][c] = f(n.value[c]);
            }
            packet.words[0][3] = f(high);
            for (unsigned i = 0; i < outputs.size(); ++i)
                if (outputs[i])
                    packet.words[i < 4 ? 1 : 2][i < 4 ? i : i - 4] =
                        f(registers[impl_->slots.at(outputs[i])]);
        }
        const auto *first = reinterpret_cast<const std::uint8_t *>(packet.words.data());
        identity.insert(identity.end(), first,
                        first + packet.words.size() * sizeof(packet.words[0]));
        packet.digest = assets::sha256(identity);
        return Result::success(std::move(packet));
    } catch (const std::exception &e) {
        return Result::failure(invalid(e.what()));
    }
}
core::Result<AuthoringMaterialProgram>
compile_authoring_material(const project::MaterialCatalog &catalog, render::MaterialId id,
                           std::optional<project::MaterialVariantId> variant,
                           const AuthoringTextureLibrary *library) {
    using Result = core::Result<AuthoringMaterialProgram>;
    try {
        auto program = std::make_shared<AuthoringMaterialProgram::Impl>();
        program->source = program->base = need(catalog.resolve_material(id, variant));
        if (program->base.alpha_mode == render::AlphaMode::blended)
            throw std::runtime_error("alpha blending is not supported");
        const std::array refs{program->base.base_color, program->base.metallic_roughness,
                              program->base.normal, program->base.occlusion,
                              program->base.emissive};
        constexpr std::array semantics{
            assets::TextureSemantic::base_color, assets::TextureSemantic::metallic_roughness,
            assets::TextureSemantic::normal, assets::TextureSemantic::occlusion,
            assets::TextureSemantic::emissive};
        for (unsigned slot = 0; slot < 5; ++slot)
            if (refs[slot]) {
                if (!library)
                    throw std::runtime_error("texture pixels are not admitted");
                const auto record = catalog.textures().find(refs[slot].asset);
                if (record == catalog.textures().end())
                    throw std::runtime_error("texture source record is missing");
                if (program->uv && *program->uv != refs[slot].uv_set)
                    throw std::runtime_error("one UV identity per material program is supported");
                program->uv = refs[slot].uv_set;
                auto semantic = semantics[slot];
                auto space = assets::expected_color_space(semantic);
                if (refs[slot].interpretation == render::TextureInterpretation::linear &&
                    space == assets::ColorSpace::srgb) {
                    semantic = assets::TextureSemantic::data;
                    space = assets::ColorSpace::linear;
                }
                if (refs[slot].interpretation == render::TextureInterpretation::srgb &&
                    space != assets::ColorSpace::srgb)
                    throw std::runtime_error("normal/data bindings cannot use sRGB interpretation");
                BoundTexture bound;
                bound.binding = refs[slot];
                if (refs[slot].sampler) {
                    const auto sampler = catalog.samplers().find(refs[slot].sampler);
                    if (sampler == catalog.samplers().end())
                        throw std::runtime_error("sampler identity does not resolve");
                    bound.sampler = sampler->second;
                    need(bound.sampler.validate());
                }
                bound.mips = need(library->prepare(record->second, semantic, space));
                program->maps[slot] = static_cast<unsigned>(program->textures.size());
                program->textures.push_back(std::move(bound));
            }
        const auto binding = catalog.material_graph_bindings().find(id);
        if (binding != catalog.material_graph_bindings().end()) {
            const auto &graph = catalog.material_graphs().at(binding->second.graph_identity);
            if (graph.nodes.size() > 128)
                throw std::runtime_error("128-node graph execution budget exceeded");
            auto current = project::validate_material_graph_source(
                graph, binding->second.graph_identity, binding->second.source_revision,
                binding->second.source_digest);
            if (!current)
                return Result::failure(
                    current.error().with_context("authoring surface material graph"));
            const auto &o = graph.outputs;
            if (o.normal || o.occlusion || o.transmission || o.subsurface || o.sheen ||
                o.anisotropy || o.opacity || o.displacement)
                throw std::runtime_error(
                    "graph surface outputs outside the bounded standard subset");
            bool dynamic = false;
            for (const auto &n : graph.nodes)
                dynamic |= n.kind == Kind::texture_sample || n.parameter_name == "uv.u" ||
                           n.parameter_name == "uv.v";
            if (!dynamic)
                program->base = need(resolve_authoring_material(catalog, id, variant));
            else {
                program->outputs = o;
                std::vector<bool> texture_dependency;
                for (auto node_id : need(graph.topological_order())) {
                    const auto &n = *graph.find(node_id);
                    if (n.kind == Kind::unsupported || n.kind == Kind::surface_input ||
                        n.inputs.size() > 3)
                        throw std::runtime_error("unsupported graph node " + std::to_string(n.id));
                    AuthoringMaterialProgram::Impl::Instruction instruction{n.kind, n.value_type,
                                                                            n.constants};
                    bool uses_texture = n.kind == Kind::texture_sample;
                    for (unsigned i = 0; i < n.inputs.size(); ++i) {
                        instruction.inputs[i] = program->slots.at(n.inputs[i]);
                        uses_texture = uses_texture || texture_dependency[instruction.inputs[i]];
                    }
                    switch (n.kind) {
                    case Kind::constant_scalar:
                    case Kind::constant_color:
                    case Kind::add:
                    case Kind::multiply:
                    case Kind::mix:
                        break;
                    case Kind::parameter_scalar:
                    case Kind::parameter_color:
                        instruction.parameter = parameter(n.parameter_name, n.value_type);
                        if (instruction.parameter == Parameter::u ||
                            instruction.parameter == Parameter::v) {
                            if (!program->uv)
                                program->uv = render::kUv0;
                        }
                        break;
                    case Kind::texture_sample: {
                        std::optional<unsigned> selected;
                        for (unsigned i = 0; i < program->textures.size(); ++i) {
                            const auto &t = program->textures[i];
                            if (t.binding.asset != n.texture_asset || t.binding.uv_set != n.uv_set)
                                continue;
                            if (selected && (program->textures[*selected].binding != t.binding ||
                                             program->textures[*selected].mips != t.mips))
                                throw std::runtime_error("graph texture binding is ambiguous "
                                                         "across authored material slots");
                            selected = i;
                        }
                        if (!selected)
                            throw std::runtime_error("graph texture node requires a matching "
                                                     "authored material texture/UV binding");
                        instruction.texture = *selected;
                        break;
                    }
                    default:
                        throw std::runtime_error("unsupported graph node " + std::to_string(n.id));
                    }
                    program->slots.emplace(n.id,
                                           static_cast<unsigned>(program->instructions.size()));
                    program->instructions.push_back(instruction);
                    texture_dependency.push_back(uses_texture);
                }
                program->base_explicit_texture =
                    texture_dependency.at(program->slots.at(o.base_color));
                program->emission_explicit_texture =
                    o.emissive && texture_dependency.at(program->slots.at(o.emissive));
            }
        }
        return Result::success(AuthoringMaterialProgram(std::move(program)));
    } catch (const std::exception &e) {
        return Result::failure(invalid(e.what()));
    }
}
core::Result<AuthoringMaterialSample>
AuthoringMaterialProgram::evaluate(const AuthoringSurfaceSample &surface) const {
    using Result = core::Result<AuthoringMaterialSample>;
    try {
        if (!impl_)
            return Result::success({});
        if (!surface.uv.finite() || !surface.uv_dx.finite() || !surface.uv_dy.finite())
            throw std::runtime_error("nonfinite surface UV");
        auto result = AuthoringMaterialSample{impl_->base};
        auto &m = result.material;
        if (!impl_->instructions.empty()) {
            std::array<Value, 128> values{};
            // Coordinate derivative propagation keeps graph UV arithmetic and
            // texture minification coupled instead of silently sampling mip zero.
            std::array<Value, 128> dx{}, dy{};
            for (unsigned slot = 0; slot < impl_->instructions.size(); ++slot) {
                const auto &n = impl_->instructions[slot];
                auto &value = values[slot];
                if (n.kind == Kind::constant_scalar || n.kind == Kind::constant_color)
                    value = n.value;
                else if (n.kind == Kind::parameter_scalar || n.kind == Kind::parameter_color) {
                    value = parameter_value(n.parameter, impl_->source, surface.uv);
                    if (n.parameter == Parameter::u) {
                        dx[slot][0] = surface.uv_dx.x;
                        dy[slot][0] = surface.uv_dy.x;
                    }
                    if (n.parameter == Parameter::v) {
                        dx[slot][0] = surface.uv_dx.y;
                        dy[slot][0] = surface.uv_dy.y;
                    }
                } else if (n.kind == Kind::texture_sample) {
                    const auto a = n.inputs[0], b = n.inputs[1];
                    value = sample(impl_->textures[n.texture], {values[a][0], values[b][0]},
                                   {dx[a][0], dx[b][0]}, {dy[a][0], dy[b][0]});
                    // Screen derivatives of sampled color use adjacent samples.
                    const auto vx = sample(impl_->textures[n.texture],
                                           {values[a][0] + dx[a][0], values[b][0] + dx[b][0]},
                                           {dx[a][0], dx[b][0]}, {dy[a][0], dy[b][0]});
                    const auto vy = sample(impl_->textures[n.texture],
                                           {values[a][0] + dy[a][0], values[b][0] + dy[b][0]},
                                           {dx[a][0], dx[b][0]}, {dy[a][0], dy[b][0]});
                    for (unsigned c = 0; c < 4; ++c) {
                        dx[slot][c] = vx[c] - value[c];
                        dy[slot][c] = vy[c] - value[c];
                    }
                } else {
                    const auto a = n.inputs[0], b = n.inputs[1], w = n.inputs[2];
                    for (unsigned c = 0; c < (n.type == Type::scalar ? 1U : 4U); ++c) {
                        if (n.kind == Kind::add) {
                            value[c] = values[a][c] + values[b][c];
                            dx[slot][c] = dx[a][c] + dx[b][c];
                            dy[slot][c] = dy[a][c] + dy[b][c];
                        } else if (n.kind == Kind::multiply) {
                            value[c] = values[a][c] * values[b][c];
                            dx[slot][c] = dx[a][c] * values[b][c] + values[a][c] * dx[b][c];
                            dy[slot][c] = dy[a][c] * values[b][c] + values[a][c] * dy[b][c];
                        } else {
                            const auto weight = values[w][0];
                            value[c] = values[a][c] * (1 - weight) + values[b][c] * weight;
                            dx[slot][c] = dx[a][c] * (1 - weight) + dx[b][c] * weight +
                                          (values[b][c] - values[a][c]) * dx[w][0];
                            dy[slot][c] = dy[a][c] * (1 - weight) + dy[b][c] * weight +
                                          (values[b][c] - values[a][c]) * dy[w][0];
                        }
                    }
                }
                for (unsigned c = 0; c < 4; ++c)
                    if (!std::isfinite(value[c]) || !std::isfinite(dx[slot][c]) ||
                        !std::isfinite(dy[slot][c]) || std::abs(value[c]) > 1e12)
                        throw std::runtime_error("nonfinite/excessive graph intermediate");
            }
            const auto output = [&](auto id) -> const Value & {
                return values[impl_->slots.at(id)];
            };
            const auto &o = impl_->outputs;
            const auto color = output(o.base_color);
            m.base_color_factor = {color[0], color[1], color[2], output(o.alpha)[0]};
            m.roughness = output(o.roughness)[0];
            m.metallic = output(o.metallic)[0];
            if (o.emissive) {
                const auto e = output(o.emissive);
                m.emissive_factor = {e[0], e[1], e[2]};
                m.emissive_strength = 1;
            }
            if (o.clearcoat)
                m.clearcoat = output(o.clearcoat)[0];
            if (o.clearcoat_roughness)
                m.clearcoat_roughness = output(o.clearcoat_roughness)[0];
        }
        const auto mapped = [&](unsigned slot) {
            return sample(impl_->textures[*impl_->maps[slot]], surface.uv, surface.uv_dx,
                          surface.uv_dy);
        };
        if (impl_->maps[0]) {
            const auto t = mapped(0);
            if (!impl_->base_explicit_texture) {
                m.base_color_factor.x *= t[0];
                m.base_color_factor.y *= t[1];
                m.base_color_factor.z *= t[2];
            }
            m.base_color_factor.w *= t[3];
        }
        if (impl_->maps[1]) {
            const auto t = mapped(1);
            m.roughness *= t[1];
            m.metallic *= t[2];
        }
        if (impl_->maps[2]) {
            const auto t = mapped(2);
            const double green =
                m.normal.normal_convention == render::NormalConvention::directx_y_minus ? -1 : 1;
            result.tangent_normal =
                core::Vec3d{(2 * t[0] - 1) * m.normal_scale,
                            (2 * t[1] - 1) * m.normal_scale * green, 2 * t[2] - 1}
                    .normalized();
            if (result.tangent_normal.length_squared() < 1e-12)
                throw std::runtime_error("normal map sample is degenerate");
            result.normal_mapped = true;
        }
        if (impl_->maps[3])
            result.ambient_occlusion = m.occlusion_strength =
                1 + m.occlusion_strength * (mapped(3)[0] - 1);
        if (impl_->maps[4] && !impl_->emission_explicit_texture) {
            const auto t = mapped(4);
            m.emissive_factor = core::componentwise_multiply(m.emissive_factor, {t[0], t[1], t[2]});
        }
        // These are evaluated factors; no caller should multiply sampled maps
        // again.
        m.base_color = {};
        m.metallic_roughness = {};
        m.normal = {};
        m.occlusion = {};
        m.emissive = {};
        need(m.validate());
        return Result::success(std::move(result));
    } catch (const std::exception &e) {
        return Result::failure(invalid(e.what()));
    }
}
} // namespace carto::production
