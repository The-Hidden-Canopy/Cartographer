#include <carto/production/material_binding.hpp>
#include <carto/production/authoring_light_state.hpp>
#include "authoring_shader.hpp"
#include <carto/d3d12/authoring_temporal.hpp>
#include <carto/d3d12/authoring_viewport.hpp>
#include <carto/d3d12/builtin_shader_cache.hpp>
#include <carto/d3d12/device.hpp>
#include <carto/d3d12/texture_upload.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/device_ir/command.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/project/mesh_digest.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>

namespace carto::d3d12 {
namespace {
using Clock = std::chrono::steady_clock;
using core::Vec3d;
template <class T> T need(core::Result<T> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
    return std::move(value.value());
}
void need(core::Result<void> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
}
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
float narrow(double v) {
    if (!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max())
        throw std::runtime_error("viewport value cannot be represented as finite float32");
    return static_cast<float>(v);
}
template <class T> std::vector<std::uint8_t> bytes(const T &value) {
    const auto *first = reinterpret_cast<const std::uint8_t *>(&value);
    return {first, first + sizeof(value)};
}
template <class T> std::vector<std::uint8_t> bytes(const std::vector<T> &value) {
    const auto *first = reinterpret_cast<const std::uint8_t *>(value.data());
    return {first, first + value.size() * sizeof(T)};
}
struct Resources {
    D3D12Device *device;
    std::shared_ptr<D3D12Device> keep_alive;
    double allocation_ms = 0.0;
    std::vector<gpu::BufferHandle> buffers;
    std::vector<gpu::TextureHandle> textures;
    std::optional<gpu::SubmissionSerial> last_use;
    explicit Resources(D3D12Device &owner) : device(&owner) {}
    explicit Resources(std::shared_ptr<D3D12Device> owner)
        : device(owner.get()), keep_alive(std::move(owner)) {}
    ~Resources() noexcept {
        try {
            const auto drained = last_use ? device->wait(*last_use) : device->wait_idle();
            // Keep handles in the device registry on any unconfirmed wait. The
            // device owner must itself remain alive or retain its native Impl.
            if (!drained && !device->device_lost()) return;
            for (auto h : buffers)
                static_cast<void>(device->destroy_buffer(h));
            for (auto h : textures)
                static_cast<void>(device->destroy_texture(h));
        } catch (...) {
            // No destructor exception may turn an uncertain GPU wait into
            // process termination. Unremoved handles remain device-owned.
        }
    }
    gpu::BufferHandle buffer(gpu::BufferDesc desc) {
        const auto started = Clock::now();
        auto h = need(device->create_buffer(desc));
        allocation_ms += elapsed(started);
        buffers.push_back(h);
        return h;
    }
    gpu::TextureHandle texture(gpu::TextureDesc desc) {
        const auto started = Clock::now();
        auto h = need(device->create_texture(desc));
        allocation_ms += elapsed(started);
        textures.push_back(h);
        return h;
    }
};
std::array<float, 16> model_matrix(core::Transform t) {
    std::array<float, 16> m{};
    for (std::size_t c = 0; c < 3; ++c) {
        Vec3d axis{};
        if (c == 0)
            axis.x = t.scale.x;
        if (c == 1)
            axis.y = t.scale.y;
        if (c == 2)
            axis.z = t.scale.z;
        const auto p = t.rotation.rotate(axis);
        m[c * 4] = narrow(p.x);
        m[c * 4 + 1] = narrow(p.y);
        m[c * 4 + 2] = narrow(p.z);
    }
    m[12] = narrow(t.translation.x);
    m[13] = narrow(t.translation.y);
    m[14] = narrow(t.translation.z);
    m[15] = 1;
    return m;
}
std::array<float, 16> camera_matrix(const production::AuthoringView &view, double aspect) {
    return need(authoring_view_projection(view, aspect));
}
struct Draw {
    scene::ObjectId object;
    std::shared_ptr<const production::RenderMeshArtifact> mesh;
    production::RenderMeshSubmesh submesh;
    render::StandardMaterial material;
    core::Transform transform;
    std::string geometry_key;
    std::shared_ptr<const production::AuthoringMaterialGpuPacket> surface;
    std::string surface_key;
};
struct Prepared {
    assets::Sha256Digest source_digest;
    production::AuthoringView view;
    render::PbrLight light{{0, 0, 1}, {0, 0, 0}};
    bool casts_shadows = false;
    std::vector<Draw> draws;
};
using MeshArtifactCache =
    std::map<std::string, std::shared_ptr<const production::RenderMeshArtifact>>;
production::AuthoringView resolve_view(const project::ProjectDocument &document,
                                       const production::AuthoringViewportRequest &request) {
    production::AuthoringView view;
    view = request.view;
    if (request.camera) {
        const auto it = document.presentation_catalog().cameras().find(*request.camera);
        if (it == document.presentation_catalog().cameras().end())
            throw std::runtime_error("authored GPU camera is missing");
        const auto t = need(document.scene().world_transform(*request.camera));
        const auto &c = it->second;
        view = {t.translation,         t.rotation,   c.projection, c.vertical_fov_radians,
                c.orthographic_height, c.near_plane, c.far_plane,  request.view.aspect_ratio};
    }
    need(view.validate());
    return view;
}
Prepared prepare(const project::ProjectDocument &document,
                 const production::AuthoringViewportRequest &request, AuthoringGpuFrame &timing,
                 MeshArtifactCache *cache) {
    auto phase = Clock::now();
    need(document.validate());
    timing.prepare_validate_ms += elapsed(phase);
    Prepared out;
    out.view = resolve_view(document, request);
    std::size_t lights = 0;
    for (const auto &[id, l] : document.presentation_catalog().lights()) {
        const auto *object = document.scene().find(id);
        if (!object || !object->visible)
            continue;
        if (!need(production::authoring_light_is_active(l)))
            continue;
        if (l.kind != project::AuthoredLightKind::directional || ++lights > 1)
            throw std::runtime_error("GPU G1 supports one directional light");
        out.light = {need(document.scene().world_transform(id)).rotation.rotate({0, 0, 1}),
                     l.linear_color * l.intensity};
        out.casts_shadows = l.casts_shadows;
    }
    phase = Clock::now();
    const auto canonical = document.serialize();
    if (canonical.size() > 128U * 1024U * 1024U)
        throw std::runtime_error("GPU viewport project byte budget exceeded");
    const production::AuthoritativeSource source{
        "project/gpu.viewport", document.revision(),
        assets::sha256(std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t *>(canonical.data()), canonical.size()})};
    out.source_digest = source.content_digest;
    timing.prepare_identity_ms += elapsed(phase);
    MeshArtifactCache next_cache;
    std::map<std::pair<render::MaterialId, std::optional<project::MaterialVariantId>>,
             production::AuthoringMaterialProgram>
        programs;
    std::map<std::pair<render::MaterialId, std::optional<project::MaterialVariantId>>,
             std::shared_ptr<const production::AuthoringMaterialGpuPacket>>
        packets;
    std::uint64_t triangles = 0, geometry_bytes = 0;
    const auto artifact_for = [&](std::uint64_t id, std::optional<render::UvSetId> uv,
                                  bool tangents) {
        const auto &mesh = document.meshes().at(id);
        const auto key = project::canonical_mesh_source_digest(mesh).hex() + "/uv/" +
                         std::to_string(uv.value_or(0)) + "/t/" + std::to_string(tangents);
        if (const auto ready = next_cache.find(key); ready != next_cache.end()) {
            // A second source asset may reuse identical content in this frame.
            // Count the successful request, as for retained prior-frame hits.
            ++timing.mesh_cache_hits;
            return ready->second;
        }
        std::shared_ptr<const production::RenderMeshArtifact> artifact;
        if (cache)
            if (const auto prior = cache->find(key); prior != cache->end()) {
                auto rebound = std::make_shared<production::RenderMeshArtifact>(*prior->second);
                rebound->source_revision = source.revision;
                rebound->source_digest = source.content_digest;
                artifact = std::move(rebound);
                ++timing.mesh_cache_hits;
            }
        if (!artifact) {
            production::RenderMeshCompileOptions options;
            options.uv_set = uv;
            options.generate_tangents = tangents;
            options.material_slot_layer = need(production::authoring_material_slot_layer(mesh));
            artifact = std::make_shared<production::RenderMeshArtifact>(
                need(production::compile_render_mesh_artifact(source, mesh, options)));
            ++timing.mesh_compile_count;
        }
        geometry_bytes += artifact->vertices.size() * sizeof(production::RenderMeshVertex) +
                          artifact->indices.size() * 4;
        if (geometry_bytes > 64U * 1024U * 1024U)
            throw std::runtime_error("GPU prepared geometry byte budget exceeded");
        next_cache.emplace(key, artifact);
        return artifact;
    };
    for (const auto &object : document.scene().objects_sorted()) {
        if (!object.visible || !object.mesh_asset)
            continue;
        auto t = need(document.scene().world_transform(object.id));
        if (const auto override = request.transform_overrides.find(object.id);
            override != request.transform_overrides.end())
            t = override->second;
        if (!t.finite() || std::abs(t.scale.x) < 1e-12 || std::abs(t.scale.y) < 1e-12 ||
            std::abs(t.scale.z) < 1e-12)
            throw std::runtime_error("GPU viewport transform is singular");
        phase = Clock::now();
        const auto base = artifact_for(*object.mesh_asset, {}, false);
        timing.prepare_meshes_ms += elapsed(phase);
        triangles += base->indices.size() / 3;
        if (triangles > 65536)
            throw std::runtime_error("GPU viewport triangle budget exceeded");
        for (const auto &submesh : base->submeshes) {
            phase = Clock::now();
            production::AuthoringMaterialProgram program;
            std::shared_ptr<const production::AuthoringMaterialGpuPacket> packet;
            render::StandardMaterial material;
            const auto &catalog = document.material_catalog();
            if (const auto assignment = catalog.assignments().find(
                    {*object.mesh_asset, submesh.material_slot.value_or(0)});
                assignment != catalog.assignments().end()) {
                const auto key = std::pair{assignment->second.material, assignment->second.variant};
                auto ready = programs.find(key);
                if (ready == programs.end()) {
                    if (programs.size() == 32)
                        throw std::runtime_error(
                            "GPU viewport supports at most 32 material programs");
                    ready =
                        programs
                            .emplace(key,
                                     need(production::compile_authoring_material(
                                         catalog, key.first, key.second, request.textures.get())))
                            .first;
                    packets.emplace(key,
                                    std::make_shared<const production::AuthoringMaterialGpuPacket>(
                                        need(ready->second.gpu_packet())));
                }
                program = ready->second;
                packet = packets.at(key);
                material = packet->material;
                if (!program.per_fragment())
                    packet.reset();
            }
            auto active = base;
            auto range = submesh;
            if (program.uv_set()) {
                active =
                    artifact_for(*object.mesh_asset, program.uv_set(), program.normal_mapping());
                const auto region = std::find_if(
                    active->submeshes.begin(), active->submeshes.end(),
                    [&](const auto &r) { return r.material_slot == submesh.material_slot; });
                if (region == active->submeshes.end())
                    throw std::runtime_error("GPU textured material region missing");
                range = *region;
            }
            if (program.normal_mapping())
                for (std::size_t i = range.first_index; i < range.first_index + range.index_count;
                     i += 3) {
                    const auto sign = active->vertices[active->indices[i]].tangent->w;
                    if (active->vertices[active->indices[i + 1]].tangent->w != sign ||
                        active->vertices[active->indices[i + 2]].tangent->w != sign)
                        throw std::runtime_error(
                            "GPU normal-map tangent handedness changes within a primitive");
                }
            if (out.draws.size() == 256)
                throw std::runtime_error("GPU viewport draw budget exceeded");
            out.draws.push_back({object.id,
                                 std::move(active),
                                 range,
                                 material,
                                 t,
                                 {},
                                 packet,
                                 packet ? packet->digest.hex() : std::string{}});
            timing.prepare_materials_ms += elapsed(phase);
        }
    }
    if (cache)
        *cache = std::move(next_cache);
    return out;
}
} // namespace

struct ResidentTimingReceipt::Impl {
    std::shared_ptr<D3D12Device> device;
    std::shared_ptr<Resources> resources;
    gpu::BufferHandle buffer;
    gpu::SubmissionSerial serial = 0;
    std::optional<QueueClockCalibration> calibration;
    Clock::time_point entry;
};

core::Result<ResidentGpuTiming> ResidentTimingReceipt::resolve() const {
    try {
        if (!impl_->serial || impl_->device->completed_serial() < impl_->serial)
            throw std::runtime_error("Resident GPU timing is pending; no wait was performed");
        const auto raw = need(impl_->device->read_buffer(impl_->buffer, 0, 120));
        std::array<std::uint64_t, 15> ticks{};
        std::memcpy(ticks.data(), raw.data(), raw.size());
        const auto frequency = impl_->device->timestamp_frequency();
        if (!frequency)
            throw std::runtime_error("Resident timestamp frequency unavailable");
        ResidentGpuTiming result;
        const auto duration = [&](unsigned a, unsigned b) {
            return double(ticks[b] - ticks[a]) * 1000.0 / double(frequency);
        };
        result.raster_ms = duration(0, 1);
        result.display_ms = duration(2, 3);
        result.envelope_ms = duration(13, 6);
        result.shadow_ms = duration(7, 8);
        result.temporal_upload_ms = duration(13, 14);
        result.temporal_motion_ms = duration(9, 10);
        result.temporal_resolve_ms = duration(10, 11);
        result.temporal_history_ms = duration(11, 12);
        if (impl_->calibration) {
            const auto aligned = [&](auto tick) {
                return std::chrono::duration<double, std::milli>(impl_->calibration->host_epoch -
                                                                 impl_->entry)
                           .count() +
                       double((static_cast<long double>(tick) - impl_->calibration->gpu_ticks) *
                              1000.0L / frequency);
            };
            result.begin_host_ms = aligned(ticks[13]);
            result.end_host_ms = aligned(ticks[6]);
            result.clock_uncertainty_ms = impl_->calibration->uncertainty_ms;
        }
        return core::Result<ResidentGpuTiming>::success(result);
    } catch (const std::exception &error) {
        return core::Result<ResidentGpuTiming>::failure(
            {core::ErrorCode::invalid_state, error.what()});
    }
}

struct AuthoringViewport::Impl {
    std::shared_ptr<D3D12Device> device;
    bool optimized = true;
    bool cache_meshes = true;
    std::shared_ptr<const project::ProjectDocument> cached_source;
    std::shared_ptr<const production::AuthoringTextureLibrary> cached_textures;
    Prepared cached_scene;
    MeshArtifactCache mesh_artifacts;
    std::uint64_t startup_cache_hits = 0, startup_cache_misses = 0;
    device::ShaderBinary cached_shader(const ShaderSource &source,
                                       AuthoringGpuFrame *frame = nullptr) {
        static const BuiltinShaderCache cache(BuiltinShaderCache::default_directory());
        auto result = need(cache.compile(source));
        if (frame) {
            frame->shader_cache_hits += result.hit ? 1 : 0;
            frame->shader_cache_misses += result.hit ? 0 : 1;
            frame->shader_cache_lookup_ms += result.lookup_ms;
            frame->shader_dxc_compile_ms += result.compile_ms;
            frame->shader_cache_store_ms += result.store_ms;
        } else {
            startup_cache_hits += result.hit ? 1 : 0;
            startup_cache_misses += result.hit ? 0 : 1;
        }
        return std::move(result.binary);
    }
    struct Copy {
        gpu::TextureHandle texture;
        std::uint32_t channels;
        std::uint64_t offset;
        std::uint32_t pitch;
        device_ir::ResourceState state;
        bool display = false;
    };
    std::vector<Copy> copies;
    gpu::BufferHandle readback;
    std::uint64_t readback_bytes = 0;
    double startup_device_ms = 0, startup_compile_ms = 0, target_allocation_ms = 0;
    std::unique_ptr<Resources> defaults;
    std::shared_ptr<Resources> targets;
    bool display_shader_read = false;
    gpu::PipelineHandle pbr, identity, display_pipeline, surface_pbr, shadow_pbr, shadow_surface;
    gpu::SamplerHandle sampler;
    gpu::BufferHandle timestamps;
    std::array<std::shared_ptr<ResidentTimingReceipt::Impl>, 4> timing_slots;
    gpu::TextureHandle hdr, depth, ids, display_target;
    std::array<gpu::TextureHandle, 5> textures{};
    std::uint32_t width = 0, height = 0;
    std::uint64_t generation = 0;
    struct Mesh {
        std::unique_ptr<Resources> resources;
        gpu::BufferHandle vertex, index;
        std::uint64_t size = 0;
    };
    std::map<std::string, std::unique_ptr<Mesh>> meshes;
    struct MaterialTexture {
        std::shared_ptr<Resources> resources;
        gpu::TextureHandle texture;
        std::string key;
        std::uint64_t size = 0;
    };
    struct MaterialSet {
        std::shared_ptr<Resources> resources;
        gpu::TextureHandle program;
        std::array<std::shared_ptr<MaterialTexture>, 5> maps;
        std::uint64_t size = 0;
    };
    std::map<std::string, std::shared_ptr<MaterialTexture>> material_textures;
    std::map<std::string, std::shared_ptr<MaterialSet>> material_sets;
    std::string surface_pipeline_error;
    std::shared_ptr<Resources> lighting, shadow_targets;
    gpu::TextureHandle lighting_data, shadow_map, shadow_depth;
    std::array<float, 16> shadow_vp{};
    std::vector<std::array<float, 4>> lighting_words;
    std::string shadow_content, next_shadow_content;
    std::string shadow_fit_key;
    std::optional<production::DirectionalShadowFrame> shadow_fit;
    std::uint32_t shadow_extent = 0;
    bool shadow_enabled = false, shadow_dirty = false, shadow_shader_read = false;
    std::shared_ptr<const production::EnvironmentLighting> environment_product,
        environment_requested;
    std::shared_ptr<const project::ProjectDocument> environment_snapshot;
    std::shared_ptr<Resources> environment_resources;
    gpu::TextureHandle environment_atlas;
    std::string uploaded_environment;
    bool environment_enabled = false;

    AuthoringTemporalHostState temporal_host;
    std::shared_ptr<Resources> temporal_resources;
    gpu::TextureHandle temporal_geometry, temporal_history_geometry, temporal_hdr,
        temporal_history_hdr;
    gpu::PipelineHandle motion_pipeline, temporal_pipeline;
    bool temporal_initialized = false;
    struct Palette {
        std::shared_ptr<Resources> resources;
        gpu::TextureHandle texture;
        assets::Sha256Digest digest;
        std::uint64_t size = 0;
    };
    Palette temporal_draws, temporal_faces;
    std::vector<std::shared_ptr<Resources>> temporal_retired;
    void ensure_temporal(AuthoringGpuFrame &frame) {
        if (!motion_pipeline) {
            auto make = [&](ShaderSource source, unsigned constants) {
                gpu::PipelineDesc p;
                p.debug_name = source.debug_name;
                p.sampled_texture_count = 4;
                p.push_constant_bytes = constants;
                p.depth_test = p.depth_write = false;
                p.color_format = p.depth_format = gpu::Format::unknown;
                for (unsigned i = 0; i < 5; ++i)
                    p.descriptor_bindings.push_back({0, i, gpu::ShaderStage::compute});
                p.shaders = {need(device->create_shader(cached_shader(source, &frame)))};
                return need(device->create_pipeline(p));
            };
            const auto motion = make(authoring_motion_shader(), sizeof(AuthoringMotionConstants));
            try {
                temporal_pipeline =
                    make(authoring_packed_temporal_shader(), sizeof(AuthoringTemporalConstants));
            } catch (...) {
                static_cast<void>(device->destroy_pipeline(motion));
                throw;
            }
            motion_pipeline = motion;
        }
        if (temporal_resources)
            return;
        auto candidate = std::make_shared<Resources>(device);
        auto texture = [&](unsigned w, unsigned h, gpu::Format format, gpu::TextureUsage usage) {
            return candidate->texture(
                {w, h, 1, 1, 1, format, gpu::TextureDimension::texture_2d, usage});
        };
        const auto sampled = gpu::TextureUsage::sampled;
        const auto output =
            sampled | gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source;
        const auto history = sampled | gpu::TextureUsage::transfer_destination;
        const auto geometry = texture(width * 3, height * 3, gpu::Format::r32_uint, output);
        const auto old_geometry = texture(width * 3, height * 3, gpu::Format::r32_uint, history);
        const auto resolved = texture(width, height, gpu::Format::rgba32_float, output);
        const auto old_hdr = texture(width, height, gpu::Format::rgba32_float, history);
        frame.allocation_milliseconds += candidate->allocation_ms;
        temporal_geometry = geometry;
        temporal_history_geometry = old_geometry;
        temporal_hdr = resolved;
        temporal_history_hdr = old_hdr;
        temporal_resources = std::move(candidate);
        temporal_initialized = false;
        temporal_host.invalidate();
    }
    void temporal_palette(Palette &palette, const std::vector<std::uint32_t> &words,
                          assets::Sha256Digest digest, device_ir::DeviceCommandStream &commands,
                          std::vector<std::shared_ptr<Resources>> &uploads,
                          AuthoringGpuFrame &frame) {
        if (palette.resources && palette.digest == digest)
            return;
        auto candidate = std::make_shared<Resources>(device);
        const unsigned rows = std::max(1U, static_cast<unsigned>((words.size() + 255) / 256));
        auto texture = candidate->texture(
            {256, rows, 1, 1, 1, gpu::Format::r32_uint, gpu::TextureDimension::texture_2d,
             gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination});
        auto staging = std::make_shared<Resources>(device);
        std::vector<std::uint32_t> padded(rows * 256);
        std::copy(words.begin(), words.end(), padded.begin());
        const auto data = bytes(padded);
        auto buffer = staging->buffer(
            {data.size(), gpu::MemoryClass::upload, gpu::BufferUsage::transfer_source});
        need(device->write_buffer(buffer, 0, data));
        commands.append(device_ir::CmdTransitionResource{
            {buffer}, device_ir::ResourceState::undefined, device_ir::ResourceState::copy_source});
        commands.append(
            device_ir::CmdTransitionResource{{texture},
                                             device_ir::ResourceState::undefined,
                                             device_ir::ResourceState::copy_destination});
        commands.append(device_ir::CmdCopyBufferToTexture{buffer, texture, 0});
        commands.append(device_ir::CmdTransitionResource{{texture},
                                                         device_ir::ResourceState::copy_destination,
                                                         device_ir::ResourceState::shader_read});
        palette = {candidate, texture, digest, data.size()};
        uploads.push_back(staging);
        frame.temporal_upload_bytes += data.size();
        frame.allocation_milliseconds += candidate->allocation_ms + staging->allocation_ms;
    }
    void record_temporal(device_ir::DeviceCommandStream &commands,
                         const AuthoringTemporalHostPlan &plan, const Palette &draws,
                         const Palette &faces) {
        using S = device_ir::ResourceState;
        auto transition = [&](gpu::TextureHandle h, S before, S after) {
            commands.append(device_ir::CmdTransitionResource{{h}, before, after});
        };
        commands.append(device_ir::CmdWriteTimestamp{9});
        transition(hdr, S::render_target, S::shader_read);
        transition(depth, S::depth_write, S::shader_read);
        transition(temporal_geometry, temporal_initialized ? S::shader_read : S::undefined,
                   S::shader_write);
        transition(temporal_hdr, temporal_initialized ? S::shader_read : S::undefined,
                   S::shader_write);
        if (!temporal_initialized) {
            transition(temporal_history_geometry, S::undefined, S::shader_read);
            transition(temporal_history_hdr, S::undefined, S::shader_read);
        }
        commands.append(device_ir::CmdBindPipeline{motion_pipeline});
        commands.append(device_ir::CmdBindResources{{{0, 0, {hdr}, std::nullopt},
                                                     {0, 1, {depth}, std::nullopt},
                                                     {0, 2, {draws.texture}, std::nullopt},
                                                     {0, 3, {faces.texture}, std::nullopt},
                                                     {0, 4, {temporal_geometry}, std::nullopt}}});
        commands.append(device_ir::CmdPushConstants{bytes(plan.motion)});
        commands.append(device_ir::CmdDispatch{plan.resolve.groups[0], plan.resolve.groups[1], 1});
        commands.append(device_ir::CmdUavBarrier{{temporal_geometry}});
        transition(temporal_geometry, S::shader_write, S::shader_read);
        commands.append(device_ir::CmdWriteTimestamp{10});
        commands.append(device_ir::CmdBindPipeline{temporal_pipeline});
        commands.append(
            device_ir::CmdBindResources{{{0, 0, {hdr}, std::nullopt},
                                         {0, 1, {temporal_history_hdr}, std::nullopt},
                                         {0, 2, {temporal_geometry}, std::nullopt},
                                         {0, 3, {temporal_history_geometry}, std::nullopt},
                                         {0, 4, {temporal_hdr}, std::nullopt}}});
        commands.append(device_ir::CmdPushConstants{bytes(plan.resolve.constants)});
        commands.append(device_ir::CmdDispatch{plan.resolve.groups[0], plan.resolve.groups[1], 1});
        commands.append(device_ir::CmdUavBarrier{{temporal_hdr}});
        commands.append(device_ir::CmdWriteTimestamp{11});
        transition(temporal_geometry, S::shader_read, S::copy_source);
        transition(temporal_hdr, S::shader_write, S::copy_source);
        transition(temporal_history_geometry, S::shader_read, S::copy_destination);
        transition(temporal_history_hdr, S::shader_read, S::copy_destination);
        commands.append(device_ir::CmdCopyTexture{temporal_geometry, temporal_history_geometry});
        commands.append(device_ir::CmdCopyTexture{temporal_hdr, temporal_history_hdr});
        transition(temporal_geometry, S::copy_source, S::shader_read);
        transition(temporal_hdr, S::copy_source, S::shader_read);
        transition(temporal_history_geometry, S::copy_destination, S::shader_read);
        transition(temporal_history_hdr, S::copy_destination, S::shader_read);
        transition(hdr, S::shader_read, S::render_target);
        transition(depth, S::shader_read, S::depth_write);
        commands.append(device_ir::CmdWriteTimestamp{12});
    }

    void prepare_environment(const project::ProjectDocument &source,
                             const production::AuthoringViewportRequest &request,
                             std::shared_ptr<const project::ProjectDocument> snapshot,
                             AuthoringGpuFrame &frame) {
        const auto started = Clock::now();
        const auto &e = source.presentation_catalog().environment();
        environment_enabled = e && e->enabled && e->intensity > 0;
        if (environment_enabled && (!snapshot || environment_snapshot != snapshot ||
                                    environment_requested != request.environment_lighting)) {
            const auto digest = need(production::environment_content_digest(*e));
            auto product = request.environment_lighting;
            if (product && product->source_digest() != digest)
                throw std::runtime_error("GPU environment product is stale");
            if (!product && environment_product && environment_product->source_digest() == digest)
                product = environment_product;
            if (!product) {
                const auto baking = Clock::now();
                product = need(production::compile_environment_lighting(*e));
                frame.environment_compile_milliseconds = elapsed(baking);
            }
            if (uploaded_environment != digest.hex()) {
                auto candidate = std::make_shared<Resources>(device);
                const auto atlas = candidate->texture(
                    {production::EnvironmentLighting::width,
                     production::EnvironmentLighting::height, 1, 1, 1, gpu::Format::rgba32_float,
                     gpu::TextureDimension::texture_2d,
                     gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination});
                Resources staging(*device);
                const auto pixels = std::span<const std::uint8_t>{
                    reinterpret_cast<const std::uint8_t *>(product->atlas().data()),
                    product->atlas().size_bytes()};
                const auto upload = staging.buffer(
                    {pixels.size(), gpu::MemoryClass::upload, gpu::BufferUsage::transfer_source});
                need(device->write_buffer(upload, 0, pixels));
                const auto query = staging.buffer(
                    {16, gpu::MemoryClass::readback, gpu::BufferUsage::transfer_destination});
                device_ir::DeviceCommandStream commands;
                commands.append(device_ir::CmdWriteTimestamp{22});
                commands.append(
                    device_ir::CmdTransitionResource{{upload},
                                                     device_ir::ResourceState::undefined,
                                                     device_ir::ResourceState::copy_source});
                commands.append(
                    device_ir::CmdTransitionResource{{atlas},
                                                     device_ir::ResourceState::undefined,
                                                     device_ir::ResourceState::copy_destination});
                commands.append(device_ir::CmdCopyBufferToTexture{upload, atlas, 0});
                commands.append(
                    device_ir::CmdTransitionResource{{atlas},
                                                     device_ir::ResourceState::copy_destination,
                                                     device_ir::ResourceState::shader_read});
                commands.append(device_ir::CmdWriteTimestamp{23});
                commands.append(
                    device_ir::CmdTransitionResource{{query},
                                                     device_ir::ResourceState::undefined,
                                                     device_ir::ResourceState::copy_destination});
                commands.append(device_ir::CmdResolveTimestamps{query, 22, 2, 0});
                const auto serial = need(device->submit(commands));
                const auto waiting = Clock::now();
                need(device->wait(serial));
                frame.environment_upload_wait_milliseconds = elapsed(waiting);
                const auto raw = need(device->read_buffer(query, 0, 16));
                std::array<std::uint64_t, 2> ticks{};
                std::memcpy(ticks.data(), raw.data(), 16);
                if (const auto frequency = device->timestamp_frequency())
                    frame.environment_gpu_upload_milliseconds =
                        double(ticks[1] - ticks[0]) * 1000 / double(frequency);
                candidate->last_use = serial;
                frame.allocation_milliseconds += candidate->allocation_ms + staging.allocation_ms;
                environment_resources = std::move(candidate);
                environment_atlas = atlas;
                uploaded_environment = digest.hex();
                frame.environment_upload_bytes = pixels.size();
            }
            environment_product = std::move(product);
            environment_snapshot = std::move(snapshot);
            environment_requested = request.environment_lighting;
        }
        if (environment_resources)
            frame.environment_resident_bytes =
                std::uint64_t(production::EnvironmentLighting::width) *
                production::EnvironmentLighting::height * 16;
        frame.logical_resident_bytes += frame.environment_resident_bytes;
        frame.environment_prepare_milliseconds = elapsed(started);
    }

    void prepare_lighting(const project::ProjectDocument &source,
                          const production::AuthoringViewportRequest &request,
                          const Prepared &scene, AuthoringGpuFrame &frame) {
        const auto started = Clock::now();
        need(request.shadows.validate());
        std::optional<production::DirectionalShadowFrame> fit;
        if (scene.casts_shadows && request.shadows.enabled) {
            auto geometry_signature = bytes(std::array<double, 5>{
                scene.light.direction_to_light.x, scene.light.direction_to_light.y,
                scene.light.direction_to_light.z, double(request.shadows.resolution),
                double(request.shadows.radius)});
            for (const auto &draw : scene.draws) {
                geometry_signature.insert(geometry_signature.end(), draw.geometry_key.begin(),
                                          draw.geometry_key.end());
                const auto transform = bytes(model_matrix(draw.transform));
                geometry_signature.insert(geometry_signature.end(), transform.begin(),
                                          transform.end());
            }
            const auto key = assets::sha256(geometry_signature).hex();
            if (key != shadow_fit_key) {
                auto fitted = need(production::fit_directional_shadow(
                    source, request, scene.light.direction_to_light));
                shadow_fit = std::move(fitted);
                shadow_fit_key = key;
            }
            fit = shadow_fit;
            if (fit)
                fit->settings = request.shadows;
        }
        shadow_enabled = fit.has_value();
        const auto extent = shadow_enabled ? request.shadows.resolution : 1U;
        std::vector<std::array<float, 4>> words(8);
        if (fit) {
            shadow_vp = camera_matrix(fit->view, 1);
            for (unsigned row = 0; row < 4; ++row)
                for (unsigned c = 0; c < 4; ++c)
                    words[row][c] = shadow_vp[c * 4 + row];
        }
        words[4] = {narrow(scene.light.radiance.x), narrow(scene.light.radiance.y),
                    narrow(scene.light.radiance.z), 1};
        words[5] = {shadow_enabled ? 1.F : 0.F, float(extent), narrow(request.shadows.depth_bias),
                    narrow(request.shadows.slope_bias)};
        words[6][0] = float(request.shadows.radius);
        if(fit) words[6][1]=narrow(fit->view.orthographic_height/(fit->view.far_plane-fit->view.near_plane));
        if (const auto &e = source.presentation_catalog().environment(); e)
            words[7] = {narrow(std::cos(e->rotation_radians)),
                        narrow(std::sin(e->rotation_radians)), narrow(e->intensity),
                        environment_enabled ? 1.F : 0.F};
        auto shadow_words = words;
        shadow_words[4] = {};
        shadow_words[5][2] = shadow_words[5][3] = 0;
        shadow_words[7] = {};
        auto signature = bytes(shadow_words);
        for (const auto &draw : scene.draws) {
            for (const auto &key : {draw.geometry_key, draw.surface_key}) {
                const auto size = bytes(std::uint64_t(key.size()));
                signature.insert(signature.end(), size.begin(), size.end());
                signature.insert(signature.end(), key.begin(), key.end());
            }
            const auto model = bytes(model_matrix(draw.transform));
            signature.insert(signature.end(), model.begin(), model.end());
            const auto &m = draw.material;
            // A graph can derive alpha from any supported parameter. Bind all
            // root factors, not just base alpha/cutoff, into shadow invalidation.
            const auto alpha = bytes(std::array<float, 18>{
                narrow(m.base_color_factor.x), narrow(m.base_color_factor.y),
                narrow(m.base_color_factor.z), narrow(m.base_color_factor.w),
                narrow(m.emissive_factor.x), narrow(m.emissive_factor.y),
                narrow(m.emissive_factor.z), narrow(m.emissive_strength), narrow(m.metallic),
                narrow(m.roughness), narrow(m.normal_scale), narrow(m.occlusion_strength),
                narrow(m.dielectric_f0), narrow(m.clearcoat), narrow(m.clearcoat_roughness),
                narrow(m.alpha_cutoff), float(m.alpha_mode), 0});
            signature.insert(signature.end(), alpha.begin(), alpha.end());
        }
        next_shadow_content = assets::sha256(signature).hex();
        shadow_dirty =
            shadow_enabled && (next_shadow_content != shadow_content || shadow_extent != extent);
        if (words != lighting_words || !lighting || shadow_extent != extent) {
            auto candidate = std::make_shared<Resources>(device);
            const auto data = candidate->texture(
                {8, 1, 1, 1, 1, gpu::Format::rgba32_float, gpu::TextureDimension::texture_2d,
                 gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination});
            Resources staging(*device);
            const auto upload =
                staging.buffer({256, gpu::MemoryClass::upload, gpu::BufferUsage::transfer_source});
            need(device->write_buffer(upload, 0, bytes(words)));
            const auto query = staging.buffer(
                {16, gpu::MemoryClass::readback, gpu::BufferUsage::transfer_destination});
            device_ir::DeviceCommandStream commands;
            commands.append(device_ir::CmdWriteTimestamp{20});
            commands.append(
                device_ir::CmdTransitionResource{{upload},
                                                 device_ir::ResourceState::undefined,
                                                 device_ir::ResourceState::copy_source});
            commands.append(
                device_ir::CmdTransitionResource{{data},
                                                 device_ir::ResourceState::undefined,
                                                 device_ir::ResourceState::copy_destination});
            commands.append(device_ir::CmdCopyBufferToTexture{upload, data, 0});
            commands.append(
                device_ir::CmdTransitionResource{{data},
                                                 device_ir::ResourceState::copy_destination,
                                                 device_ir::ResourceState::shader_read});
            auto shadow_candidate = shadow_targets;
            auto map = shadow_map, depth_target = shadow_depth;
            if (shadow_extent != extent || !shadow_candidate) {
                shadow_candidate = std::make_shared<Resources>(device);
                map = shadow_candidate->texture(
                    {extent,
                     extent,
                     1,
                     1,
                     1,
                     gpu::Format::r32_float,
                     gpu::TextureDimension::texture_2d,
                     gpu::TextureUsage::color_attachment | gpu::TextureUsage::sampled,
                     {1, 0, 0, 1}});
                depth_target = shadow_candidate->texture(
                    {extent, extent, 1, 1, 1, gpu::Format::depth32_float,
                     gpu::TextureDimension::texture_2d, gpu::TextureUsage::depth_attachment});
                commands.append(
                    device_ir::CmdTransitionResource{{map},
                                                     device_ir::ResourceState::undefined,
                                                     device_ir::ResourceState::render_target});
                commands.append(
                    device_ir::CmdTransitionResource{{depth_target},
                                                     device_ir::ResourceState::undefined,
                                                     device_ir::ResourceState::depth_write});
                commands.append(device_ir::CmdBeginRendering{map, depth_target});
                commands.append(device_ir::CmdClearColor{map, {1, 0, 0, 1}});
                commands.append(device_ir::CmdClearDepth{depth_target, 1});
                commands.append(device_ir::CmdEndRendering{});
                commands.append(
                    device_ir::CmdTransitionResource{{map},
                                                     device_ir::ResourceState::render_target,
                                                     device_ir::ResourceState::shader_read});
            }
            commands.append(device_ir::CmdWriteTimestamp{21});
            commands.append(
                device_ir::CmdTransitionResource{{query},
                                                 device_ir::ResourceState::undefined,
                                                 device_ir::ResourceState::copy_destination});
            commands.append(device_ir::CmdResolveTimestamps{query, 20, 2, 0});
            const auto serial = need(device->submit(commands));
            const auto waiting = Clock::now();
            need(device->wait(serial));
            frame.lighting_upload_wait_milliseconds = elapsed(waiting);
            std::array<std::uint64_t, 2> ticks{};
            const auto raw = need(device->read_buffer(query, 0, 16));
            std::memcpy(ticks.data(), raw.data(), 16);
            if (const auto frequency = device->timestamp_frequency())
                frame.lighting_gpu_upload_milliseconds =
                    double(ticks[1] - ticks[0]) * 1000 / double(frequency);
            candidate->last_use = shadow_candidate->last_use = serial;
            frame.allocation_milliseconds +=
                candidate->allocation_ms + staging.allocation_ms +
                (shadow_extent != extent ? shadow_candidate->allocation_ms : 0);
            lighting = std::move(candidate);
            lighting_data = data;
            lighting_words = std::move(words);
            shadow_targets = std::move(shadow_candidate);
            shadow_map = map;
            shadow_depth = depth_target;
            shadow_extent = extent;
            shadow_shader_read = true;
            frame.lighting_upload_bytes = 256;
        }
        frame.shadow_resident_bytes = std::uint64_t(extent) * extent * 8 + 128;
        frame.logical_resident_bytes += frame.shadow_resident_bytes;
        frame.lighting_update_milliseconds = elapsed(started);
    }

    void ensure_surface_pipeline(AuthoringGpuFrame &frame) {
        if (surface_pbr)
            return;
        if (!surface_pipeline_error.empty())
            throw std::runtime_error(surface_pipeline_error);
        try {
            const auto started = Clock::now();
            const auto code =
                std::string("#define CARTO_FUSED_IDENTITY 1\n#define CARTO_AUTHORING_SURFACE 1\n") +
                carto_authoring_surface_source + carto_authoring_pbr_source;
            const auto shader = [&](gpu::ShaderStage stage, const char *entry) {
                return need(device->create_shader(
                    cached_shader({"authoring.surface", stage, entry, "authoring_surface.hlsl",
                                   code, true, false},
                                  &frame)));
            };
            gpu::PipelineDesc desc;
            desc.debug_name = "authoring.surface.hdr";
            desc.shaders = {shader(gpu::ShaderStage::vertex, "VSMain"),
                            shader(gpu::ShaderStage::fragment, "PSMain")};
            desc.sampled_texture_count = 9;
            for (std::uint32_t i = 0; i < 9; ++i)
                desc.descriptor_bindings.push_back({0, i, gpu::ShaderStage::fragment});
            desc.cull_mode = gpu::CullMode::none;
            desc.color_format = gpu::Format::rgba32_float;
            desc.depth_format = gpu::Format::depth32_float;
            desc.depth_test = desc.depth_write = true;
            desc.depth_compare = gpu::CompareOp::less;
            // Light radiance moved to t6: 52+9+1=62 DWORDs.
            desc.push_constant_bytes = 208;
            desc.vertex_stride_bytes = 52;
            desc.vertex_attributes = {{"POSITION", 0, gpu::VertexFormat::float4, 0, 0},
                                      {"NORMAL", 0, gpu::VertexFormat::float3, 16, 0},
                                      {"TEXCOORD", 0, gpu::VertexFormat::float2, 28, 0},
                                      {"TANGENT", 0, gpu::VertexFormat::float4, 36, 0}};
            const auto candidate_pbr = need(device->create_pipeline(desc));
            desc.debug_name = "authoring.shadow.surface";
            desc.color_format = gpu::Format::r32_float;
            desc.shaders = {desc.shaders[0], shader(gpu::ShaderStage::fragment, "ShadowMain")};
            auto candidate_shadow = device->create_pipeline(desc);
            if (!candidate_shadow) {
                static_cast<void>(device->destroy_pipeline(candidate_pbr));
                throw std::runtime_error(candidate_shadow.error().message);
            }
            surface_pbr = candidate_pbr;
            shadow_surface = candidate_shadow.value();
            frame.material_shader_compile_milliseconds += elapsed(started);
        } catch (const std::exception &error) {
            surface_pipeline_error = error.what();
            throw;
        }
    }
    void materials(const Prepared &scene, AuthoringGpuFrame &frame) {
        const auto started = Clock::now();
        std::map<std::string, std::shared_ptr<MaterialTexture>> next_textures;
        std::map<std::string, std::shared_ptr<MaterialSet>> next_sets;
        std::unique_ptr<Resources> staging;
        device_ir::DeviceCommandStream commands;
        commands.append(device_ir::CmdWriteTimestamp{16});
        std::vector<std::shared_ptr<Resources>> created;
        const auto upload = [&](std::shared_ptr<Resources> resources, gpu::TextureDesc desc,
                                const std::vector<std::uint8_t> &data,
                                const TextureUploadPlan *plan) {
            if (frame.material_upload_bytes + data.size() > 128U * 1024U * 1024U)
                throw std::runtime_error("GPU material staging budget exceeded");
            if (!staging)
                staging = std::make_unique<Resources>(*device);
            auto texture = resources->texture(desc);
            auto buffer = staging->buffer(
                {data.size(), gpu::MemoryClass::upload, gpu::BufferUsage::transfer_source});
            need(device->write_buffer(buffer, 0, data));
            commands.append(
                device_ir::CmdTransitionResource{{buffer},
                                                 device_ir::ResourceState::undefined,
                                                 device_ir::ResourceState::copy_source});
            commands.append(
                device_ir::CmdTransitionResource{{texture},
                                                 device_ir::ResourceState::undefined,
                                                 device_ir::ResourceState::copy_destination});
            if (plan)
                need(append_texture_upload_copies(commands, buffer, texture, *plan));
            else
                commands.append(device_ir::CmdCopyBufferToTexture{buffer, texture, 0});
            commands.append(
                device_ir::CmdTransitionResource{{texture},
                                                 device_ir::ResourceState::copy_destination,
                                                 device_ir::ResourceState::shader_read});
            frame.material_upload_bytes += data.size();
            created.push_back(resources);
            return texture;
        };
        for (const auto &draw : scene.draws)
            if (draw.surface) {
                if (!optimized)
                    throw std::runtime_error(
                        "Legacy GPU reference does not execute surface programs; use optimized "
                        "native path and CPU reference oracle");
                ensure_surface_pipeline(frame);
                const auto &packet = *draw.surface;
                const auto &key = draw.surface_key;
                if (next_sets.contains(key))
                    continue;
                if (const auto ready = material_sets.find(key); ready != material_sets.end()) {
                    next_sets.emplace(key, ready->second);
                    for (const auto &map : ready->second->maps)
                        if (map)
                            next_textures.emplace(map->key, map);
                    continue;
                }
                auto set = std::make_shared<MaterialSet>();
                for (unsigned slot = 0; slot < 5; ++slot)
                    if (packet.maps[slot]) {
                        const auto &chain = *packet.maps[slot];
                        const auto map_key = chain.artifact_digest.hex();
                        auto ready = next_textures.find(map_key);
                        if (ready == next_textures.end()) {
                            if (const auto prior = material_textures.find(map_key);
                                prior != material_textures.end())
                                ready = next_textures.emplace(map_key, prior->second).first;
                            else {
                                const auto plan = need(build_texture_upload_plan(chain));
                                auto map = std::make_shared<MaterialTexture>();
                                map->resources = std::make_shared<Resources>(device);
                                map->key = map_key;
                                map->size = plan.resident_bytes;
                                map->texture =
                                    upload(map->resources, plan.texture, plan.staging_bytes, &plan);
                                ready = next_textures.emplace(map_key, std::move(map)).first;
                            }
                        }
                        set->maps[slot] = ready->second;
                    }
                set->resources = std::make_shared<Resources>(device);
                set->size = packet.words.size() * 16;
                const gpu::TextureDesc desc{32,
                                            static_cast<std::uint32_t>(packet.words.size() / 32),
                                            1,
                                            1,
                                            1,
                                            gpu::Format::rgba32_float,
                                            gpu::TextureDimension::texture_2d,
                                            gpu::TextureUsage::sampled |
                                                gpu::TextureUsage::transfer_destination};
                set->program = upload(set->resources, desc, bytes(packet.words), nullptr);
                next_sets.emplace(key, std::move(set));
            }
        if (next_sets.size() > 32)
            throw std::runtime_error("GPU material residency exceeds 32 programs");
        for (const auto &[key, map] : next_textures) {
            static_cast<void>(key);
            frame.material_resident_bytes += map->size;
        }
        for (const auto &[key, set] : next_sets) {
            static_cast<void>(key);
            frame.material_resident_bytes += set->size;
        }
        if (frame.material_resident_bytes > 65U * 1024U * 1024U)
            throw std::runtime_error("GPU material residency byte budget exceeded");
        if (staging) {
            auto timing = staging->buffer(
                {16, gpu::MemoryClass::readback, gpu::BufferUsage::transfer_destination});
            commands.append(device_ir::CmdWriteTimestamp{17});
            commands.append(
                device_ir::CmdTransitionResource{{timing},
                                                 device_ir::ResourceState::undefined,
                                                 device_ir::ResourceState::copy_destination});
            commands.append(device_ir::CmdResolveTimestamps{timing, 16, 2, 0});
            frame.material_timeline_ms[0] = elapsed(started);
            const auto serial = need(device->submit(commands));
            frame.material_timeline_ms[1] = elapsed(started);
            const auto wait_start = Clock::now();
            need(device->wait(serial));
            frame.material_upload_wait_milliseconds = elapsed(wait_start);
            frame.material_timeline_ms[2] = elapsed(started);
            staging->last_use = serial;
            const auto raw = need(device->read_buffer(timing, 0, 16));
            std::array<std::uint64_t, 2> ticks{};
            std::memcpy(ticks.data(), raw.data(), 16);
            if (const auto frequency = device->timestamp_frequency())
                frame.material_gpu_upload_milliseconds =
                    double(ticks[1] - ticks[0]) * 1000.0 / double(frequency);
            frame.material_timeline_ms[3] = elapsed(started);
            for (const auto &resource : created) {
                resource->last_use = serial;
                frame.allocation_milliseconds += resource->allocation_ms;
            }
            frame.allocation_milliseconds += staging->allocation_ms;
        } else
            std::fill_n(frame.material_timeline_ms.begin(), 4, elapsed(started));
        // Publish only after all uploads succeed. Old resources wait for their
        // own producer fence on retirement; unchanged frames allocate/upload none.
        const auto retiring = Clock::now();
        material_sets = std::move(next_sets);
        material_textures = std::move(next_textures);
        frame.material_retire_milliseconds = elapsed(retiring);
        frame.material_timeline_ms[4] = elapsed(started);
        frame.material_update_milliseconds = elapsed(started);
        frame.logical_resident_bytes += frame.material_resident_bytes;
        frame.material_timeline_ms[5] = elapsed(started);
    }

    void initialize() {
        const auto device_started = Clock::now();
        if (!device)
            device = need(D3D12Device::create());
        startup_device_ms = elapsed(device_started);
        const auto compile_started = Clock::now();
        defaults = std::make_unique<Resources>(*device);
        auto shader = [&](gpu::ShaderStage stage, const char *entry) {
            return need(device->create_shader(cached_shader(
                {"authoring.pbr", stage, entry, "authoring_pbr.hlsl",
                 (optimized ? std::string("#define CARTO_FUSED_IDENTITY 1\n") : std::string{}) +
                     carto_authoring_pbr_source,
                 true, false})));
        };
        const auto vs = shader(gpu::ShaderStage::vertex, "VSMain");
        gpu::PipelineDesc desc;
        desc.debug_name = "authoring.hdr";
        desc.shaders = {vs, shader(gpu::ShaderStage::fragment, "PSMain")};
        for (std::uint32_t i = 0; i < 9; ++i)
            desc.descriptor_bindings.push_back({0U, i, gpu::ShaderStage::fragment});
        desc.sampled_texture_count = 9U;
        desc.cull_mode = gpu::CullMode::none;
        desc.color_format = gpu::Format::rgba32_float;
        desc.depth_format = gpu::Format::depth32_float;
        desc.depth_test = true;
        desc.depth_write = true;
        desc.depth_compare = gpu::CompareOp::less;
        desc.push_constant_bytes = 216;
        desc.vertex_stride_bytes = 52;
        desc.vertex_attributes = {{"POSITION", 0, gpu::VertexFormat::float4, 0, 0},
                                  {"NORMAL", 0, gpu::VertexFormat::float3, 16, 0},
                                  {"TEXCOORD", 0, gpu::VertexFormat::float2, 28, 0},
                                  {"TANGENT", 0, gpu::VertexFormat::float4, 36, 0}};
        pbr = need(device->create_pipeline(desc));
        desc.debug_name = "authoring.identity";
        if (!optimized) {
            desc.shaders = {vs, shader(gpu::ShaderStage::fragment, "IdentityMain")};
            identity = need(device->create_pipeline(desc));
        }
        desc.debug_name = "authoring.shadow.constant";
        desc.color_format = gpu::Format::r32_float;
        desc.shaders = {vs, shader(gpu::ShaderStage::fragment, "ShadowMain")};
        shadow_pbr = need(device->create_pipeline(desc));
        if (optimized) {
            auto display_shader = [&](gpu::ShaderStage stage, const char *entry) {
                return need(device->create_shader(
                    cached_shader({"authoring.display", stage, entry, "authoring_display.hlsl",
                                   carto_authoring_display_source, true, false})));
            };
            gpu::PipelineDesc post;
            post.debug_name = "authoring.display";
            post.shaders = {display_shader(gpu::ShaderStage::vertex, "VSMain"),
                            display_shader(gpu::ShaderStage::fragment, "PSMain")};
            post.descriptor_bindings = {{0U, 0U, gpu::ShaderStage::fragment}};
            post.sampled_texture_count = 1;
            post.cull_mode = gpu::CullMode::none;
            post.color_format = gpu::Format::rgba8_unorm;
            post.depth_format = gpu::Format::unknown;
            post.depth_test = false;
            post.depth_write = false;
            post.push_constant_bytes = 16;
            display_pipeline = need(device->create_pipeline(post));
        }
        startup_compile_ms = elapsed(compile_started);
        sampler = need(device->create_sampler({}));
        timestamps = defaults->buffer(
            {120, gpu::MemoryClass::readback, gpu::BufferUsage::transfer_destination});
        Resources uploads(*device);
        device_ir::DeviceCommandStream commands;
        for (std::size_t i = 0; i < 5; ++i) {
            textures[i] = defaults->texture(
                {1, 1, 1, 1, 1, gpu::Format::rgba8_unorm, gpu::TextureDimension::texture_2d,
                 gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination});
            auto buffer =
                uploads.buffer({256, gpu::MemoryClass::upload, gpu::BufferUsage::transfer_source});
            const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
            need(device->write_buffer(buffer, 0, white));
            commands.append(
                device_ir::CmdTransitionResource{{buffer},
                                                 device_ir::ResourceState::undefined,
                                                 device_ir::ResourceState::copy_source});
            commands.append(
                device_ir::CmdTransitionResource{{textures[i]},
                                                 device_ir::ResourceState::undefined,
                                                 device_ir::ResourceState::copy_destination});
            commands.append(device_ir::CmdCopyBufferToTexture{buffer, textures[i], 0});
            commands.append(
                device_ir::CmdTransitionResource{{textures[i]},
                                                 device_ir::ResourceState::copy_destination,
                                                 device_ir::ResourceState::shader_read});
        }
        commands.append(
            device_ir::CmdTransitionResource{{timestamps},
                                             device_ir::ResourceState::undefined,
                                             device_ir::ResourceState::copy_destination});
        need(device->wait(need(device->submit(commands))));
    }
    void resize(std::uint32_t w, std::uint32_t h, bool full_readback) {
        target_allocation_ms = 0;
        if (w == width && h == height) {
            if (full_readback && optimized && !readback) {
                const auto &last = copies.back();
                const auto required = last.offset + std::uint64_t(last.pitch) * h;
                const auto allocated_before = targets->allocation_ms;
                const auto candidate = targets->buffer(
                    {required, gpu::MemoryClass::readback, gpu::BufferUsage::transfer_destination});
                device_ir::DeviceCommandStream initialize;
                initialize.append(
                    device_ir::CmdTransitionResource{{candidate},
                                                     device_ir::ResourceState::undefined,
                                                     device_ir::ResourceState::copy_destination});
                need(device->wait(need(device->submit(initialize))));
                readback = candidate;
                readback_bytes = required;
                target_allocation_ms = targets->allocation_ms - allocated_before;
            }
            return;
        }
        auto candidate = std::make_shared<Resources>(device);
        const auto color_usage =
            gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source;
        auto new_hdr = candidate->texture({w,
                                           h,
                                           1,
                                           1,
                                           1,
                                           gpu::Format::rgba32_float,
                                           gpu::TextureDimension::texture_2d,
                                           color_usage | gpu::TextureUsage::sampled,
                                           {0, 0, 0, 0}});
        gpu::TextureHandle new_ids{};
        gpu::TextureHandle new_display{};
        if (optimized)
            new_display = candidate->texture({w, h, 1, 1, 1, gpu::Format::rgba8_unorm,
                                              gpu::TextureDimension::texture_2d,
                                              color_usage | gpu::TextureUsage::sampled});
        if (!optimized)
            new_ids = candidate->texture({w,
                                          h,
                                          1,
                                          1,
                                          1,
                                          gpu::Format::rgba32_float,
                                          gpu::TextureDimension::texture_2d,
                                          color_usage,
                                          {0, 0, 0, 0}});
        auto new_depth = candidate->texture(
            {w, h, 1, 1, 1, gpu::Format::depth32_float, gpu::TextureDimension::texture_2d,
             gpu::TextureUsage::depth_attachment | gpu::TextureUsage::transfer_source |
                 gpu::TextureUsage::sampled});
        device_ir::DeviceCommandStream commands;
        for (auto target : {new_hdr, new_ids, new_display})
            if (target)
                commands.append(
                    device_ir::CmdTransitionResource{{target},
                                                     device_ir::ResourceState::undefined,
                                                     device_ir::ResourceState::render_target});
        commands.append(device_ir::CmdTransitionResource{{new_depth},
                                                         device_ir::ResourceState::undefined,
                                                         device_ir::ResourceState::depth_write});
        std::vector<Copy> new_copies;
        std::uint64_t new_readback_bytes = 0;
        gpu::BufferHandle new_readback{};
        if (optimized) {
            for (auto [texture, channels, state, is_display] :
                 {std::tuple{new_hdr, 4U, device_ir::ResourceState::render_target, false},
                  std::tuple{new_depth, 1U, device_ir::ResourceState::depth_write, false},
                  std::tuple{new_display, 1U, device_ir::ResourceState::render_target, true}}) {
                new_readback_bytes = (new_readback_bytes + 511U) & ~std::uint64_t{511U};
                const auto pitch = (w * channels * 4U + 255U) & ~255U;
                new_copies.push_back(
                    {texture, channels, new_readback_bytes, pitch, state, is_display});
                new_readback_bytes += std::uint64_t(pitch) * h;
            }
            if (full_readback) {
                new_readback = candidate->buffer({new_readback_bytes, gpu::MemoryClass::readback,
                                                  gpu::BufferUsage::transfer_destination});
                commands.append(
                    device_ir::CmdTransitionResource{{new_readback},
                                                     device_ir::ResourceState::undefined,
                                                     device_ir::ResourceState::copy_destination});
            } else
                new_readback_bytes = 0;
        }
        need(device->wait(need(device->submit(commands))));
        target_allocation_ms = candidate->allocation_ms;
        copies = std::move(new_copies);
        readback = new_readback;
        readback_bytes = new_readback_bytes;
        temporal_resources.reset();
        temporal_host.invalidate();
        temporal_initialized = false;
        targets = std::move(candidate);
        display_shader_read = false;
        hdr = new_hdr;
        ids = new_ids;
        display_target = new_display;
        depth = new_depth;
        width = w;
        height = h;
        ++generation;
    }
    void geometry(Prepared &scene, AuthoringGpuFrame &frame) {
        std::set<std::string> used;
        for (auto &draw : scene.draws) {
            if (optimized && !draw.geometry_key.empty() && meshes.contains(draw.geometry_key)) {
                used.insert(draw.geometry_key);
                continue;
            }
            std::vector<production::PbrUploadVertex> vertices;
            for (const auto &v : draw.mesh->vertices) {
                production::PbrUploadVertex packed{};
                packed.position = {narrow(v.position.x), narrow(v.position.y), narrow(v.position.z),
                                   1};
                packed.normal = {narrow(v.normal.x), narrow(v.normal.y), narrow(v.normal.z)};
                if (v.uv)
                    packed.uv0 = {narrow(v.uv->x), narrow(v.uv->y)};
                const auto tangent =
                    core::cross(v.normal,
                                std::abs(v.normal.z) < .9 ? Vec3d{0, 0, 1} : Vec3d{0, 1, 0})
                        .normalized();
                packed.tangent =
                    v.tangent ? std::array<float, 4>{narrow(v.tangent->x), narrow(v.tangent->y),
                                                     narrow(v.tangent->z), narrow(v.tangent->w)}
                              : std::array<float, 4>{narrow(tangent.x), narrow(tangent.y),
                                                     narrow(tangent.z), 1};
                vertices.push_back(packed);
            }
            auto vertex_bytes = bytes(vertices), index_bytes = bytes(draw.mesh->indices),
                 key_bytes = vertex_bytes;
            key_bytes.insert(key_bytes.end(), index_bytes.begin(), index_bytes.end());
            draw.geometry_key = assets::sha256(key_bytes).hex();
            used.insert(draw.geometry_key);
            if (!meshes.contains(draw.geometry_key)) {
                auto candidate = std::make_unique<Mesh>();
                candidate->resources = std::make_unique<Resources>(*device);
                candidate->size = key_bytes.size();
                if (candidate->size > 64U * 1024U * 1024U)
                    throw std::runtime_error("GPU mesh upload byte budget exceeded");
                candidate->vertex = candidate->resources->buffer(
                    {vertex_bytes.size(), gpu::MemoryClass::upload,
                     gpu::BufferUsage::vertex | gpu::BufferUsage::transfer_source});
                candidate->index = candidate->resources->buffer(
                    {index_bytes.size(), gpu::MemoryClass::upload,
                     gpu::BufferUsage::index | gpu::BufferUsage::transfer_source});
                need(device->write_buffer(candidate->vertex, 0, vertex_bytes));
                need(device->write_buffer(candidate->index, 0, index_bytes));
                device_ir::DeviceCommandStream commands;
                commands.append(
                    device_ir::CmdTransitionResource{{candidate->vertex},
                                                     device_ir::ResourceState::undefined,
                                                     device_ir::ResourceState::vertex_read});
                commands.append(
                    device_ir::CmdTransitionResource{{candidate->index},
                                                     device_ir::ResourceState::undefined,
                                                     device_ir::ResourceState::index_read});
                need(device->wait(need(device->submit(commands))));
                frame.geometry_upload_bytes += candidate->size;
                frame.allocation_milliseconds += candidate->resources->allocation_ms;
                meshes.emplace(draw.geometry_key, std::move(candidate));
            }
        }
        // All previous frames have completed before this renderer returns.
        std::erase_if(meshes, [&](const auto &item) { return !used.contains(item.first); });
        for (const auto &[key, mesh] : meshes) {
            static_cast<void>(key);
            frame.logical_resident_bytes += mesh->size;
        }
        if (frame.logical_resident_bytes > 64U * 1024U * 1024U)
            throw std::runtime_error("GPU geometry residency payload budget exceeded");
    }
};
core::Result<production::AuthoringViewportRequest>
prepare_readback_snapshot_request(const project::ProjectDocument& source,
    const std::optional<std::filesystem::path>& project_path,
    const production::AuthoringViewportRequest& request) {
    using Result = core::Result<production::AuthoringViewportRequest>;
    try {
        auto resolved = request;
        if (resolved.textures) {
            auto snapshot = resolved.textures->snapshot();
            if (!snapshot) return Result::failure(snapshot.error());
            resolved.textures = std::move(snapshot.value());
            return Result::success(std::move(resolved));
        }
        if (!request.width || !request.height) return Result::success(std::move(resolved));
        std::set<assets::TextureAssetId> used;
        const auto& catalog = source.material_catalog();
        for (const auto& object : source.scene().objects_sorted()) {
            if (!object.visible || !object.mesh_asset) continue;
            for (const auto& [key, assignment] : catalog.assignments()) {
                if (key.mesh_asset != *object.mesh_asset) continue;
                auto material = catalog.resolve_material(assignment.material, assignment.variant);
                if (!material) return Result::failure(material.error());
                const auto& value = material.value();
                for (const auto* ref : {&value.base_color, &value.metallic_roughness,
                                        &value.normal, &value.occlusion, &value.emissive})
                    if (*ref) used.insert(ref->asset);
            }
        }
        if (used.empty()) return Result::success(std::move(resolved));
        if (!project_path) return Result::failure({core::ErrorCode::invalid_state,
            "save the project before loading material texture pixels"});
        std::error_code error;
        const auto requested_root = project_path->parent_path().empty()
            ? std::filesystem::current_path(error) : project_path->parent_path();
        if (error) return Result::failure({core::ErrorCode::io_error,
            "unable to resolve the saved project directory"});
        const auto root = std::filesystem::weakly_canonical(requested_root, error);
        if (error || root.empty()) return Result::failure({core::ErrorCode::io_error,
            "unable to resolve the saved project directory"});
        const auto limits = production::authoring_texture_limits();
        assets::BlobStore store(root / ".carto-assets" / "source-blobs",
                                {limits.max_source_bytes, 256U});
        auto library = std::make_shared<production::AuthoringTextureLibrary>();
        for (const auto id : used) {
            const auto source_entry = catalog.textures().find(id);
            if (source_entry == catalog.textures().end())
                return Result::failure({core::ErrorCode::not_found, "material texture source is missing"});
            const auto& texture = source_entry->second;
            const auto expected = store.path_for(texture.content_digest);
            const auto actual = std::filesystem::canonical(expected, error);
            if (error) return Result::failure({core::ErrorCode::not_found,
                "material texture source bytes are missing from the project asset store"});
            const auto expected_relative = expected.lexically_relative(root);
            const auto actual_relative = actual.lexically_relative(root);
            if (actual.lexically_normal() != expected.lexically_normal() ||
                expected_relative.empty() || expected_relative.is_absolute() ||
                actual_relative.generic_string() != expected_relative.generic_string() ||
                std::any_of(actual_relative.begin(), actual_relative.end(),
                            [](const auto& component) { return component == ".."; }))
                return Result::failure({core::ErrorCode::validation_failed,
                    "material texture blob is not at its project-local content-addressed path"});
            const auto size = std::filesystem::file_size(actual, error);
            if (error || size > limits.max_source_bytes)
                return Result::failure({core::ErrorCode::validation_failed,
                    "material texture blob is unavailable or exceeds the inspection limit"});
            auto bytes = store.read(texture.content_digest);
            if (!bytes) return Result::failure(bytes.error().with_context("material texture blob verification"));
            auto inspected = assets::inspect_texture_source(bytes.value(),
                assets::texture_source_media_type(texture.format), limits);
            if (!inspected) return Result::failure(inspected.error().with_context("material texture source inspection"));
            if (inspected.value().source.digest != texture.content_digest ||
                inspected.value().format != texture.format || inspected.value().width != texture.width ||
                inspected.value().height != texture.height)
                return Result::failure({core::ErrorCode::stale_data,
                    "material texture source metadata does not match its stored bytes"});
            if (auto admitted = library->admit(texture, bytes.value()); !admitted)
                return Result::failure(admitted.error());
        }
        resolved.textures = std::move(library);
        return Result::success(std::move(resolved));
    } catch (const std::exception& e) {
        return Result::failure({core::ErrorCode::invalid_state, e.what()});
    }
}
struct ReadbackViewportWorker::Impl {
    struct Job {
        Impl* owner;
        ReadbackViewportIdentity identity;
        project::ProjectDocument source;
        std::optional<std::filesystem::path> path;
        production::AuthoringViewportRequest request;
        std::shared_ptr<D3D12Device> resident_device;
        std::optional<AuthoringGpuFrame> frame;
        std::optional<assets::Sha256Digest> image_digest;
        std::string error;
        detail::ReadbackCancellation cancellation;
        std::atomic<bool> draining{false};
        std::atomic<bool> finished{false};
        std::atomic<bool> device_requested{false};
        std::atomic<bool> device_granted{false};
        Job(Impl* worker, ReadbackViewportIdentity id, project::ProjectDocument document,
            std::optional<std::filesystem::path> project_path,
            production::AuthoringViewportRequest input, std::shared_ptr<D3D12Device> shared_device)
            : owner(worker), identity(id), source(std::move(document)), path(std::move(project_path)),
              request(std::move(input)), resident_device(std::move(shared_device)) {}
        void fail(const char* message) noexcept {
            try { error = message ? message : "readback worker failed"; }
            catch (...) { error.clear(); }
        }
    };
    HANDLE thread = nullptr;
    std::unique_ptr<Job> job;
    detail::ReadbackViewportGate gate;
    // Accessed only by the producer thread or after its thread has retired.
    std::shared_ptr<D3D12Device> resident_device;
    std::unique_ptr<AuthoringViewport> resident_viewport;
    std::shared_ptr<const project::ProjectDocument> resident_source;
    std::uint64_t resident_source_generation = 0;
    HANDLE cleanup_thread = nullptr;
    std::atomic<bool> cleanup_finished{false};

    // This loop is worker-only. It never releases the device on an unknown
    // retirement result; the owner must keep the process and event loop alive.
    static void drain(const std::shared_ptr<D3D12Device>& device) noexcept {
        if (!device) return;
        for (;;) {
            try {
                if (device->device_lost()) return;
                const auto resolved = device->resolve_unsignaled_execution();
                if (device->device_lost()) return;
                if (resolved) {
                    const auto idle = device->wait_idle();
                    if (idle || device->device_lost()) return;
                }
            } catch (...) {
                // The native owner remains held while retrying.
            }
            Sleep(100);
        }
    }

    static unsigned __stdcall cleanup(void* opaque) noexcept {
        auto& owner = *static_cast<Impl*>(opaque);
        drain(owner.resident_device);
        owner.resident_source.reset();
        owner.resident_viewport.reset();
        drain(owner.resident_device);
        owner.resident_device.reset();
        owner.cleanup_finished.store(true, std::memory_order_release);
        return 0;
    }

    static unsigned __stdcall run(void* opaque) noexcept {
        auto& job = *static_cast<Job*>(opaque);
        auto& owner = *job.owner;
        std::shared_ptr<D3D12Device> device;
        std::unique_ptr<AuthoringViewport> viewport;
        std::optional<AuthoringGpuFrame> result;
        std::optional<assets::Sha256Digest> digest;
        bool resident_touched = false;
        struct Cancelled {};
        try {
            // begin() already froze any supplied texture library. Otherwise
            // resolve and decode project-local blobs only on this worker.
            auto request = job.request.textures
                ? core::Result<production::AuthoringViewportRequest>::success(job.request)
                : prepare_readback_snapshot_request(job.source, job.path, job.request);
            if (job.cancellation.requested()) throw Cancelled{};
            if (!request) throw std::runtime_error(request.error().message);
            if (job.resident_device) {
                job.device_requested.store(true, std::memory_order_release);
                while (!job.device_granted.load(std::memory_order_acquire)) {
                    if (job.cancellation.requested()) throw Cancelled{};
                    Sleep(1);
                }
                if (job.cancellation.requested()) throw Cancelled{};
                device = job.resident_device;
                if (owner.resident_device && owner.resident_device != device)
                    throw std::runtime_error("resident worker is bound to a different device");
                owner.resident_device = device;
                resident_touched = true;
                if (!owner.resident_viewport) {
                    auto created_viewport = AuthoringViewport::create_on_device(device);
                    if (!created_viewport) throw std::runtime_error(created_viewport.error().message);
                    owner.resident_viewport = std::move(created_viewport.value());
                }
                if (!owner.resident_source || owner.resident_source_generation != job.identity.project_generation ||
                    owner.resident_source->revision() != job.source.revision()) {
                    owner.resident_source = std::make_shared<const project::ProjectDocument>(job.source);
                    owner.resident_source_generation = job.identity.project_generation;
                }
            } else {
                auto created_device = D3D12Device::create();
                if (!created_device) throw std::runtime_error(created_device.error().message);
                device = std::shared_ptr<D3D12Device>(std::move(created_device.value()));
                if (job.cancellation.requested()) throw Cancelled{};
                auto created_viewport = AuthoringViewport::create_on_device(device);
                if (!created_viewport) throw std::runtime_error(created_viewport.error().message);
                viewport = std::move(created_viewport.value());
            }
            if (job.cancellation.requested()) throw Cancelled{};
            auto image = job.resident_device
                ? owner.resident_viewport->render_resident(owner.resident_source, request.value(), false)
                : viewport->render(job.source, request.value());
            if (job.cancellation.requested()) throw Cancelled{};
            if (!image) throw std::runtime_error(image.error().message);
            const auto& display = image.value().image;
            if (job.resident_device) {
                if (!image.value().resident_image || !image.value().resident_image->lifetime ||
                    image.value().resident_image->device != job.resident_device ||
                    !display.width || !display.height || !display.display_rgba8.empty())
                    throw std::runtime_error("resident worker returned invalid same-device output");
            } else {
                if (image.value().resident_image || image.value().deferred_timing ||
                    !detail::valid_readback_rgba(display.width, display.height, display.display_rgba8.size()))
                    throw std::runtime_error("readback worker returned invalid RGBA8 display data");
                const auto calculated = assets::sha256(display.display_rgba8);
                if (calculated.is_zero())
                    throw std::runtime_error("readback worker returned an invalid display digest");
                digest = calculated;
            }
            result.emplace(std::move(image.value()));
        } catch (const Cancelled&) { /* Publication is withdrawn; drain below. */ }
          catch (const std::exception& e) { job.fail(e.what()); }
          catch (...) { job.fail("readback worker encountered an unknown failure"); }

        if (!job.resident_device || !result) {
            job.draining.store(true, std::memory_order_release);
            drain(device); // Preserve all resources until retirement is confirmed.
            viewport.reset();
            if (resident_touched) {
                owner.resident_source.reset();
                owner.resident_viewport.reset();
                owner.resident_device.reset();
            }
            drain(device);
        }
        device.reset();
        try {
            job.frame = std::move(result);
            job.image_digest = digest;
        }
        catch (...) {
            job.frame.reset();
            job.image_digest.reset();
            job.fail("readback result ownership transfer failed");
        }
        job.finished.store(true, std::memory_order_release);
        return 0;
    }
};

ReadbackViewportWorker::ReadbackViewportWorker() : impl_(new Impl) {}
ReadbackViewportWorker& ReadbackViewportWorker::process_owned() {
    // The process owner must interlock shutdown; the singleton and active
    // worker are never detached or destructed during static teardown.
    static auto* worker = new ReadbackViewportWorker;
    return *worker;
}
core::Result<bool> ReadbackViewportWorker::begin(ReadbackViewportIdentity identity,
    project::ProjectDocument source, std::optional<std::filesystem::path> project_path,
    production::AuthoringViewportRequest request) {
    return begin_impl(identity, std::move(source), std::move(project_path), std::move(request), {});
}
core::Result<bool> ReadbackViewportWorker::begin_resident(ReadbackViewportIdentity identity,
    project::ProjectDocument source, std::optional<std::filesystem::path> project_path,
    production::AuthoringViewportRequest request, std::shared_ptr<D3D12Device> device) {
    if (!device) return core::Result<bool>::failure({core::ErrorCode::invalid_argument, "resident device is null"});
    return begin_impl(identity, std::move(source), std::move(project_path), std::move(request), std::move(device));
}
bool ReadbackViewportWorker::in_flight() const noexcept { return impl_->thread != nullptr; }
bool ReadbackViewportWorker::device_requested() const noexcept {
    return impl_->job && impl_->job->resident_device &&
        impl_->job->device_requested.load(std::memory_order_acquire);
}
bool ReadbackViewportWorker::device_in_use() const noexcept {
    return impl_->job && impl_->job->resident_device &&
        impl_->job->device_granted.load(std::memory_order_acquire);
}
bool ReadbackViewportWorker::grant_device() noexcept {
    if (!device_requested() || impl_->gate.stopping() || impl_->job->cancellation.requested()) return false;
    impl_->job->device_granted.store(true, std::memory_order_release);
    return true;
}
core::Result<bool> ReadbackViewportWorker::begin_impl(ReadbackViewportIdentity identity,
    project::ProjectDocument source, std::optional<std::filesystem::path> project_path,
    production::AuthoringViewportRequest request, std::shared_ptr<D3D12Device> resident_device) {
    using Result = core::Result<bool>;
    const auto admission = impl_->gate.begin(identity);
    if (admission == detail::ReadbackViewportGate::Admission::stopped)
        return Result::failure({core::ErrorCode::invalid_state, "readback worker is stopping"});
    if (admission == detail::ReadbackViewportGate::Admission::busy) {
        if (impl_->job && !(impl_->job->identity == identity))
            impl_->job->cancellation.request();
        return Result::success(false);
    }
    struct Reservation {
        detail::ReadbackViewportGate& gate;
        bool committed = false;
        ~Reservation() { if (!committed) gate.retire(); }
    } reservation{impl_->gate};
    try {
        if (project_path && project_path->is_relative()) {
            std::error_code error;
            auto absolute = std::filesystem::absolute(*project_path, error);
            if (error) return Result::failure({core::ErrorCode::io_error,
                "unable to fix the project path for the worker"});
            project_path = std::move(absolute);
        }
        if (request.textures) {
            auto frozen = request.textures->snapshot();
            if (!frozen) return Result::failure(frozen.error());
            request.textures = std::move(frozen.value());
        }
        auto job = std::make_unique<Impl::Job>(impl_, identity, std::move(source),
                                               std::move(project_path), std::move(request), std::move(resident_device));
        const auto handle = _beginthreadex(nullptr, 0, &Impl::run, job.get(), 0, nullptr);
        if (!handle) return Result::failure({core::ErrorCode::invalid_state,
            "unable to start the readback worker"});
        impl_->job = std::move(job);
        impl_->thread = reinterpret_cast<HANDLE>(handle);
        reservation.committed = true;
        return Result::success(true);
    } catch (const std::exception& e) {
        return Result::failure({core::ErrorCode::invalid_state, e.what()});
    }
}
ReadbackViewportPoll ReadbackViewportWorker::poll() {
    ReadbackViewportPoll out;
    if (!impl_->thread) return out;
    out.identity = impl_->job->identity;
    const auto status = WaitForSingleObject(impl_->thread, 0);
    if (status != WAIT_OBJECT_0 || !impl_->job->finished.load(std::memory_order_acquire)) {
        out.state = impl_->job->draining.load(std::memory_order_acquire)
            ? ReadbackViewportPoll::State::draining : ReadbackViewportPoll::State::pending;
        if (status == WAIT_FAILED) out.error = "worker completion could not be verified";
        return out;
    }
    if (!CloseHandle(impl_->thread)) {
        out.state = ReadbackViewportPoll::State::draining;
        out.error = "worker handle could not be retired";
        return out;
    }
    impl_->thread = nullptr;
    auto job = std::move(impl_->job);
    const bool publishable = impl_->gate.publishable(job->identity) && !job->cancellation.requested();
    impl_->gate.retire();
    if (!publishable) {
        out.state = ReadbackViewportPoll::State::stale;
    } else if (job->frame && ((job->resident_device && job->frame->resident_image &&
               job->frame->resident_image->device == job->resident_device &&
               job->frame->resident_image->lifetime && job->frame->image.width && job->frame->image.height) ||
              (!job->resident_device && job->image_digest && !job->image_digest->is_zero() &&
               detail::valid_readback_rgba(job->frame->image.width, job->frame->image.height,
                                           job->frame->image.display_rgba8.size())))) {
        out.state = ReadbackViewportPoll::State::ready;
        out.frame = std::move(job->frame);
        out.image_digest = job->image_digest;
    } else {
        out.state = ReadbackViewportPoll::State::failed;
        out.error = job->error.empty() ? "readback worker failed" : std::move(job->error);
    }
    return out;
}
void ReadbackViewportWorker::invalidate() noexcept {
    if (impl_->job) impl_->job->cancellation.request();
}
void ReadbackViewportWorker::request_stop() noexcept {
    impl_->gate.stop();
    invalidate();
}
bool ReadbackViewportWorker::shutdown_ready() noexcept {
    if (impl_->thread) {
        if (!impl_->gate.stopping() || WaitForSingleObject(impl_->thread, 0) != WAIT_OBJECT_0 ||
            !impl_->job->finished.load(std::memory_order_acquire)) return false;
        if (!CloseHandle(impl_->thread)) return false;
        impl_->thread = nullptr;
        impl_->job.reset();
        impl_->gate.retire();
    }
    if (impl_->gate.occupied()) return false;
    if (!impl_->gate.stopping()) return !impl_->resident_viewport;
    if (!impl_->cleanup_thread && impl_->resident_viewport) {
        const auto handle = _beginthreadex(nullptr, 0, &Impl::cleanup, impl_, 0, nullptr);
        if (!handle) return false;
        impl_->cleanup_thread = reinterpret_cast<HANDLE>(handle);
    }
    if (impl_->cleanup_thread) {
        if (WaitForSingleObject(impl_->cleanup_thread, 0) != WAIT_OBJECT_0 ||
            !impl_->cleanup_finished.load(std::memory_order_acquire)) return false;
        if (!CloseHandle(impl_->cleanup_thread)) return false;
        impl_->cleanup_thread = nullptr;
    }
    return !impl_->resident_viewport;
}

AuthoringViewport::AuthoringViewport(std::unique_ptr<Impl> implementation)
    : impl_(std::move(implementation)) {}
AuthoringViewport::~AuthoringViewport() = default;
core::Result<std::unique_ptr<AuthoringViewport>>
AuthoringViewport::create_on_device(std::shared_ptr<D3D12Device> device, bool cache_meshes) {
    using Result = core::Result<std::unique_ptr<AuthoringViewport>>;
    if (!device)
        return Result::failure(
            {core::ErrorCode::invalid_argument, "shared viewport device is null"});
    try {
        auto impl = std::make_unique<Impl>();
        impl->device = std::move(device);
        impl->cache_meshes = cache_meshes;
        impl->initialize();
        return Result::success(
            std::unique_ptr<AuthoringViewport>(new AuthoringViewport(std::move(impl))));
    } catch (const std::exception &e) {
        return Result::failure({core::ErrorCode::invalid_state, e.what()});
    }
}
core::Result<std::unique_ptr<AuthoringViewport>> AuthoringViewport::create(bool optimized,
                                                                           bool cache_meshes) {
    try {
        auto impl = std::make_unique<Impl>();
        impl->optimized = optimized;
        impl->cache_meshes = cache_meshes;
        impl->initialize();
        return core::Result<std::unique_ptr<AuthoringViewport>>::success(
            std::unique_ptr<AuthoringViewport>(new AuthoringViewport(std::move(impl))));
    } catch (const std::exception &e) {
        return core::Result<std::unique_ptr<AuthoringViewport>>::failure(
            {core::ErrorCode::invalid_state, e.what()});
    }
}
core::Result<AuthoringGpuFrame>
AuthoringViewport::render(const project::ProjectDocument &source,
                          const production::AuthoringViewportRequest &request) {
    return render_impl(source, request, {});
}
core::Result<AuthoringGpuFrame>
AuthoringViewport::render(std::shared_ptr<const project::ProjectDocument> source,
                          const production::AuthoringViewportRequest &request) {
    return render(std::move(source), request, {});
}
core::Result<AuthoringGpuFrame>
AuthoringViewport::render(std::shared_ptr<const project::ProjectDocument> source,
                          const production::AuthoringViewportRequest &request,
                          production::AuthoringViewportImage recycled_output) {
    if (!source)
        return core::Result<AuthoringGpuFrame>::failure(
            {core::ErrorCode::invalid_argument, "GPU viewport snapshot is null"});
    return render_impl(*source, request, source, std::move(recycled_output));
}
core::Result<AuthoringGpuFrame>
AuthoringViewport::render_resident(std::shared_ptr<const project::ProjectDocument> source,
                                   const production::AuthoringViewportRequest &request,
                                   bool synchronous_timing) {
    if (!source || !impl_->optimized)
        return core::Result<AuthoringGpuFrame>::failure(
            {core::ErrorCode::invalid_argument,
             "resident viewport requires a source and optimized GPU path"});
    return render_impl(*source, request, source, {}, true, synchronous_timing);
}
core::Result<AuthoringGpuFrame> AuthoringViewport::render_impl(
    const project::ProjectDocument &source, const production::AuthoringViewportRequest &request,
    std::shared_ptr<const project::ProjectDocument> immutable_source,
    production::AuthoringViewportImage recycled_output, bool resident, bool synchronous_timing) {
    try {
        const auto entry = Clock::now();
        AuthoringGpuFrame frame;
        frame.image = std::move(recycled_output);
        frame.device_name = impl_->device->identity().adapter_name;
        frame.startup_shader_pipeline_milliseconds = impl_->startup_compile_ms;
        frame.startup_shader_cache_hits = impl_->startup_cache_hits;
        frame.startup_shader_cache_misses = impl_->startup_cache_misses;
        frame.startup_device_milliseconds = impl_->startup_device_ms;
        auto &image = frame.image;
        image.width = request.width;
        image.height = request.height;
        image.source_revision = source.revision();
        image.diagnostics.clear();
        image.host_milliseconds = 0;
        image.attachment_bytes = 0;
        image.sample_visits = 0;
        if (!request.width || !request.height) {
            impl_->temporal_host.invalidate();
            image.linear_hdr.clear();
            image.depth.clear();
            image.objects.clear();
            image.faces.clear();
            image.display_rgba8.clear();
            return core::Result<AuthoringGpuFrame>::success(std::move(frame));
        }
        const auto display = need(production::resolve_authoring_display(source, request));
        if (std::uint64_t(request.width) * request.height > 1048576U)
            throw std::runtime_error("GPU viewport extent/display budget invalid");
        if(request.temporal.enabled) {
            need(request.temporal.settings.validate());
            if(!impl_->optimized || request.width>4096 || request.height>4096)
                throw std::runtime_error("native temporal renderer/atlas extent exceeds supported bounds");
        }
        const auto started = Clock::now();
        const bool snapshot_cached = impl_->optimized && immutable_source;
        Prepared scene;
        if (snapshot_cached) {
            if (impl_->cached_source != immutable_source ||
                impl_->cached_textures != request.textures) {
                auto base_request = request;
                base_request.transform_overrides.clear();
                auto base = prepare(source, base_request, frame,
                                    impl_->cache_meshes ? &impl_->mesh_artifacts : nullptr);
                impl_->cached_scene = std::move(base);
                impl_->cached_source = immutable_source;
                impl_->cached_textures = request.textures;
            }
            scene = impl_->cached_scene;
            scene.view = resolve_view(source, request);
            for (auto &draw : scene.draws) {
                if (auto override = request.transform_overrides.find(draw.object);
                    override != request.transform_overrides.end())
                    draw.transform = override->second;
                if (!draw.transform.finite() || std::abs(draw.transform.scale.x) < 1e-12 ||
                    std::abs(draw.transform.scale.y) < 1e-12 ||
                    std::abs(draw.transform.scale.z) < 1e-12)
                    throw std::runtime_error("GPU transient transform is singular");
            }
        } else
            scene =
                prepare(source, request, frame,
                        impl_->optimized && impl_->cache_meshes ? &impl_->mesh_artifacts : nullptr);
        // Light transform previews share the same transient override boundary
        // as mesh edits; an immutable document cache must not freeze them.
        for (const auto &[id, light] : source.presentation_catalog().lights()) {
            const auto *object = source.scene().find(id);
            if (!object || !object->visible)
                continue;
            if (!need(production::authoring_light_is_active(light)))
                continue;
            auto transform = need(source.scene().world_transform(id));
            if (const auto override = request.transform_overrides.find(id);
                override != request.transform_overrides.end())
                transform = override->second;
            if (!transform.finite())
                throw std::runtime_error("GPU light transform is invalid");
            scene.light.direction_to_light = transform.rotation.rotate({0, 0, 1});
        }
        image.view = scene.view;
        if (std::any_of(scene.draws.begin(), scene.draws.end(),
                        [](const auto &draw) { return bool(draw.surface); }))
            image.diagnostics.push_back("Numeric material errors display magenta and fail picking; "
                                        "check graph output ranges");
        frame.cpu_prepare_milliseconds = elapsed(started);
        frame.host_timeline.prepared = elapsed(entry);
        impl_->resize(request.width, request.height, !resident);
        frame.host_timeline.resources_ready = elapsed(entry);
        const auto geometry_started = Clock::now();
        impl_->geometry(scene, frame);
        if (snapshot_cached)
            for (std::size_t i = 0; i < scene.draws.size(); ++i)
                impl_->cached_scene.draws[i].geometry_key = scene.draws[i].geometry_key;
        frame.geometry_update_milliseconds = elapsed(geometry_started);
        frame.host_timeline.geometry_ready = elapsed(entry);
        impl_->materials(scene, frame);
        impl_->prepare_environment(source, request, immutable_source, frame);
        impl_->prepare_lighting(source, request, scene, frame);
        frame.host_timeline.materials_ready = elapsed(entry);
        frame.allocation_milliseconds += impl_->target_allocation_ms;
        frame.target_generation = impl_->generation;
        frame.draw_calls = scene.draws.size() * (impl_->optimized ? 1U : 2U);
        for (const auto &draw : scene.draws)
            frame.triangles += draw.submesh.index_count / 3U;
        frame.readback_bytes = impl_->readback_bytes;
        frame.image_transfer_bytes = resident ? 0U : impl_->readback_bytes;
        frame.logical_resident_bytes +=
            std::uint64_t(image.width) * image.height * (impl_->optimized ? 24U : 36U) +
            impl_->readback_bytes + 140U; // 120 timestamp bytes + five white RGBA8 texels
        const double aspect = scene.view.aspect_ratio > 0 ? scene.view.aspect_ratio
                                                          : double(image.width) / image.height;
        const auto vp = camera_matrix(scene.view, aspect);
        const bool temporal = request.temporal.enabled;
        if (temporal && !impl_->optimized)
            throw std::runtime_error("temporal requires fused native authoring renderer");
        std::optional<AuthoringTemporalHostPlan> temporal_plan;
        std::vector<AuthoringTemporalDraw> temporal_draws;
        const auto temporal_prepare_started=Clock::now();
        if (temporal) {
            impl_->ensure_temporal(frame);
            for (const auto &draw : scene.draws)
                temporal_draws.push_back(
                    {draw.object.value, draw.transform,
                     std::span(draw.mesh->triangle_faces)
                         .subspan(draw.submesh.first_index / 3, draw.submesh.index_count / 3)});
            auto lighting_bytes = bytes(impl_->lighting_words);
            const auto direction = bytes(std::array<double, 3>{scene.light.direction_to_light.x,
                                                               scene.light.direction_to_light.y,
                                                               scene.light.direction_to_light.z});
            lighting_bytes.insert(lighting_bytes.end(), direction.begin(), direction.end());
            auto quality = bytes(std::array<double, 4>{
                double(request.shadows.resolution), double(request.shadows.radius),
                request.shadows.depth_bias, request.shadows.slope_bias});
            quality.push_back(request.shadows.enabled ? 1 : 0);
            AuthoringTemporalHostInput input{
                {scene.source_digest, source.revision(), request.width, request.height},
                scene.view,
                temporal_draws,
                assets::sha256(lighting_bytes),
                assets::sha256(quality),
                request.temporal.session,
                request.temporal.camera_cut,
                impl_->shadow_enabled,
                request.temporal.settings};
            temporal_plan = need(impl_->temporal_host.prepare(input));
            frame.temporal_history_eligible = temporal_plan->resolve.constants.extent_flags[2] != 0;
            frame.temporal_reset = static_cast<unsigned>(temporal_plan->reset);
            frame.temporal_resident_bytes = std::uint64_t(request.width) * request.height * 104;
            frame.logical_resident_bytes += frame.temporal_resident_bytes;
        } else
            impl_->temporal_host.invalidate();
        frame.temporal_prepare_ms=elapsed(temporal_prepare_started);
        std::shared_ptr<ResidentTimingReceipt::Impl> deferred;
        auto query_destination = impl_->timestamps;
        if (resident && !synchronous_timing) {
            for (auto &slot : impl_->timing_slots) {
                if (!slot) {
                    slot = std::make_shared<ResidentTimingReceipt::Impl>();
                    slot->device = impl_->device;
                    slot->resources = std::make_shared<Resources>(impl_->device);
                    slot->buffer = slot->resources->buffer(
                        {120, gpu::MemoryClass::readback, gpu::BufferUsage::transfer_destination});
                }
                if (slot.use_count() == 1 && impl_->device->completed_serial() >= slot->serial) {
                    deferred = slot;
                    break;
                }
            }
            if (!deferred)
                throw std::runtime_error("Four resident timing receipts remain owned or pending; "
                                         "consume/release them before submitting again");
            query_destination = deferred->buffer;
        }
        device_ir::DeviceCommandStream commands;
        commands.append(device_ir::CmdWriteTimestamp{13});
        const auto temporal_palette_started=Clock::now();
        std::vector<std::shared_ptr<Resources>> temporal_uploads;
        auto temporal_draw_palette = impl_->temporal_draws,
             temporal_face_palette = impl_->temporal_faces;
        const auto completed = impl_->device->completed_serial();
        std::erase_if(impl_->temporal_retired,
                      [&](const auto &r) { return r->last_use && *r->last_use <= completed; });
        if (temporal_plan) {
            impl_->temporal_palette(temporal_draw_palette, temporal_plan->draw_words,
                                    temporal_plan->draw_digest, commands, temporal_uploads, frame);
            impl_->temporal_palette(temporal_face_palette, *temporal_plan->face_words,
                                    temporal_plan->face_digest, commands, temporal_uploads, frame);
            frame.temporal_resident_bytes +=
                temporal_draw_palette.size + temporal_face_palette.size;
            frame.logical_resident_bytes += temporal_draw_palette.size + temporal_face_palette.size;
        }
        frame.temporal_palette_prepare_ms=elapsed(temporal_palette_started);
        commands.append(device_ir::CmdWriteTimestamp{14});
        if (deferred && !deferred->serial)
            commands.append(
                device_ir::CmdTransitionResource{{query_destination},
                                                 device_ir::ResourceState::undefined,
                                                 device_ir::ResourceState::copy_destination});
        commands.append(device_ir::CmdWriteTimestamp{0});
        commands.append(device_ir::CmdWriteTimestamp{7});
        for (unsigned stage = 0; stage < (impl_->optimized ? 2U : 3U); ++stage) {
            const bool shadow_pass = stage == 0;
            if (shadow_pass && !impl_->shadow_dirty) {
                commands.append(device_ir::CmdWriteTimestamp{8});
                continue;
            }
            const unsigned pass = stage ? stage - 1 : 0;
            const auto target =
                shadow_pass ? impl_->shadow_map : (pass == 0 ? impl_->hdr : impl_->ids);
            const auto depth_target = shadow_pass ? impl_->shadow_depth : impl_->depth;
            const auto w = shadow_pass ? impl_->shadow_extent : image.width;
            const auto h = shadow_pass ? impl_->shadow_extent : image.height;
            if (shadow_pass && impl_->shadow_shader_read)
                commands.append(
                    device_ir::CmdTransitionResource{{impl_->shadow_map},
                                                     device_ir::ResourceState::shader_read,
                                                     device_ir::ResourceState::render_target});
            commands.append(device_ir::CmdBeginRendering{target, depth_target});
            commands.append(device_ir::CmdClearColor{
                target, {shadow_pass ? 1.F : 0.F, 0, 0, shadow_pass ? 1.F : 0.F}});
            commands.append(device_ir::CmdClearDepth{depth_target, 1});
            commands.append(device_ir::CmdSetViewport{0, 0, float(w), float(h), 0, 1});
            commands.append(device_ir::CmdSetScissor{0, 0, w, h});
            const auto base_pipeline =
                shadow_pass ? impl_->shadow_pbr : (pass == 0 ? impl_->pbr : impl_->identity);
            commands.append(device_ir::CmdBindPipeline{base_pipeline});
            auto bound_pipeline = base_pipeline;
            std::string bound_surface;
            for (std::size_t d = 0; d < scene.draws.size(); ++d) {
                const auto &draw = scene.draws[d];
                const auto &mesh = *impl_->meshes.at(draw.geometry_key);
                const auto &m = draw.material;
                const auto desired_pipeline =
                    draw.surface ? (shadow_pass ? impl_->shadow_surface : impl_->surface_pbr)
                                 : base_pipeline;
                if (bound_pipeline != desired_pipeline) {
                    commands.append(device_ir::CmdBindPipeline{desired_pipeline});
                    bound_pipeline = desired_pipeline;
                    bound_surface.clear();
                }
                const auto binding_key = draw.surface ? draw.surface_key : "constant";
                if (bound_surface != binding_key) {
                    device_ir::CmdBindResources surface_bindings;
                    const auto *set =
                        draw.surface ? impl_->material_sets.at(draw.surface_key).get() : nullptr;
                    for (std::uint32_t slot = 0; slot < 5; ++slot)
                        surface_bindings.bindings.push_back(
                            {0,
                             slot,
                             {set && set->maps[slot] ? set->maps[slot]->texture
                                                     : impl_->textures[slot]},
                             slot == 0 ? std::optional{impl_->sampler} : std::nullopt});
                    surface_bindings.bindings.push_back(
                        {0, 5, {set ? set->program : impl_->textures[0]}, std::nullopt});
                    surface_bindings.bindings.push_back(
                        {0, 6, {impl_->lighting_data}, std::nullopt});
                    // Never bind the active render target as an SRV, including
                    // unused shader bindings during the shadow producer pass.
                    surface_bindings.bindings.push_back(
                        {0,
                         7,
                         {shadow_pass ? impl_->textures[0] : impl_->shadow_map},
                         std::nullopt});
                    surface_bindings.bindings.push_back(
                        {0,
                         8,
                         {impl_->environment_atlas ? impl_->environment_atlas : impl_->textures[0]},
                         std::nullopt});
                    commands.append(std::move(surface_bindings));
                    bound_surface = binding_key;
                }
                std::array<float, 58> constants{};
                const auto &projection = shadow_pass ? impl_->shadow_vp : vp;
                std::copy(projection.begin(), projection.end(), constants.begin());
                const auto model = model_matrix(draw.transform);
                std::copy(model.begin(), model.end(), constants.begin() + 16);
                const bool ortho = scene.view.projection == project::CameraProjection::orthographic;
                const auto eye =
                    ortho ? scene.view.rotation.rotate({0, 0, 1}) : scene.view.position;
                constants[32] = narrow(eye.x);
                constants[33] = narrow(eye.y);
                constants[34] = narrow(eye.z);
                constants[35] = impl_->optimized ? (ortho ? -1.0F : 1.0F) * float(d + 1U)
                                                 : (pass == 1 ? float(d) : (ortho ? -1.0F : 0.0F));
                constants[36] = narrow(m.base_color_factor.x);
                constants[37] = narrow(m.base_color_factor.y);
                constants[38] = narrow(m.base_color_factor.z);
                constants[39] = narrow(m.base_color_factor.w);
                constants[40] = narrow(m.emissive_factor.x * m.emissive_strength);
                constants[41] = narrow(m.emissive_factor.y * m.emissive_strength);
                constants[42] = narrow(m.emissive_factor.z * m.emissive_strength);
                constants[43] = narrow(m.dielectric_f0);
                constants[44] = narrow(m.metallic);
                constants[45] = narrow(m.roughness);
                constants[46] = narrow(m.normal_scale);
                constants[47] = narrow(m.occlusion_strength);
                constants[48] = narrow(scene.light.direction_to_light.x);
                constants[49] = narrow(scene.light.direction_to_light.y);
                constants[50] = narrow(scene.light.direction_to_light.z);
                constants[51] =
                    m.alpha_mode == render::AlphaMode::masked ? -narrow(m.alpha_cutoff) : 1;
                constants[52] = narrow(m.clearcoat);
                constants[53] = narrow(m.clearcoat_roughness);
                commands.append(device_ir::CmdBindVertexBuffer{mesh.vertex, 0, 52});
                commands.append(
                    device_ir::CmdBindIndexBuffer{mesh.index, 0, device_ir::IndexFormat::uint32});
                auto push = bytes(constants);
                push.resize(draw.surface ? 208 : 216);
                commands.append(device_ir::CmdPushConstants{std::move(push)});
                commands.append(device_ir::CmdDrawIndexed{draw.submesh.index_count, 1,
                                                          draw.submesh.first_index, 0, 0});
                if (shadow_pass)
                    ++frame.shadow_draw_calls;
            }
            commands.append(device_ir::CmdEndRendering{});
            if (shadow_pass) {
                commands.append(
                    device_ir::CmdTransitionResource{{impl_->shadow_map},
                                                     device_ir::ResourceState::render_target,
                                                     device_ir::ResourceState::shader_read});
                commands.append(device_ir::CmdWriteTimestamp{8});
            }
        }
        frame.draw_calls += frame.shadow_draw_calls;
        commands.append(device_ir::CmdWriteTimestamp{1});
        if (temporal_plan)
            impl_->record_temporal(commands, *temporal_plan, temporal_draw_palette,
                                   temporal_face_palette);
        else
            for (unsigned q = 9; q < 13; ++q)
                commands.append(device_ir::CmdWriteTimestamp{q});
        if (impl_->optimized) {
            commands.append(device_ir::CmdWriteTimestamp{2});
            commands.append(
                device_ir::CmdTransitionResource{{impl_->hdr},
                                                 device_ir::ResourceState::render_target,
                                                 device_ir::ResourceState::shader_read});
            if (impl_->display_shader_read)
                commands.append(
                    device_ir::CmdTransitionResource{{impl_->display_target},
                                                     device_ir::ResourceState::shader_read,
                                                     device_ir::ResourceState::render_target});
            commands.append(device_ir::CmdBeginRendering{impl_->display_target, std::nullopt});
            commands.append(
                device_ir::CmdSetViewport{0, 0, float(image.width), float(image.height), 0, 1});
            commands.append(device_ir::CmdSetScissor{0, 0, image.width, image.height});
            commands.append(device_ir::CmdBindPipeline{impl_->display_pipeline});
            commands.append(device_ir::CmdBindResources{
                {{0, 0, {temporal_plan ? impl_->temporal_hdr : impl_->hdr}, impl_->sampler}}});
            commands.append(device_ir::CmdPushConstants{bytes(
                std::array<float, 4>{narrow(display.exposure_ev),
                                     float(static_cast<unsigned>(display.tone_mode)), 0, 0})});
            commands.append(device_ir::CmdDraw{3});
            commands.append(device_ir::CmdEndRendering{});
            commands.append(
                device_ir::CmdTransitionResource{{impl_->hdr},
                                                 device_ir::ResourceState::shader_read,
                                                 device_ir::ResourceState::render_target});
            commands.append(device_ir::CmdWriteTimestamp{3});
            commands.append(device_ir::CmdWriteTimestamp{4});
            for (auto copy : impl_->copies) {
                if (temporal_plan && copy.texture == impl_->hdr) {
                    copy.texture = impl_->temporal_hdr;
                    copy.state = device_ir::ResourceState::shader_read;
                }
                if (resident)
                    break;
                commands.append(device_ir::CmdTransitionResource{
                    {copy.texture}, copy.state, device_ir::ResourceState::copy_source});
                commands.append(
                    device_ir::CmdCopyTextureToBuffer{copy.texture, impl_->readback, copy.offset});
                commands.append(device_ir::CmdTransitionResource{
                    {copy.texture}, device_ir::ResourceState::copy_source, copy.state});
            }
            commands.append(
                device_ir::CmdTransitionResource{{impl_->display_target},
                                                 device_ir::ResourceState::render_target,
                                                 device_ir::ResourceState::shader_read});
            commands.append(device_ir::CmdWriteTimestamp{5});
        }
        commands.append(
            device_ir::CmdResolveTimestamps{query_destination, 0, impl_->optimized ? 6U : 2U, 0});
        commands.append(device_ir::CmdResolveTimestamps{query_destination, 7, 2, 56});
        commands.append(device_ir::CmdResolveTimestamps{query_destination, 9, 6, 72});
        if (impl_->optimized) {
            commands.append(device_ir::CmdWriteTimestamp{6});
            commands.append(device_ir::CmdResolveTimestamps{query_destination, 6, 1, 48});
        }

        const auto calibration = impl_->device->clock_calibration();
        const auto submit_start = Clock::now();
        frame.host_timeline.commands_ready = elapsed(entry);
        SubmissionHostTiming submit_timing;
        const auto serial = need(impl_->device->submit_timed(commands, submit_timing));
        if (temporal_plan) {
            need(impl_->temporal_host.commit(*temporal_plan));
            impl_->temporal_initialized = true;
            for (auto pair : {std::pair{&impl_->temporal_draws, &temporal_draw_palette},
                              std::pair{&impl_->temporal_faces, &temporal_face_palette}}) {
                if (pair.first->resources && pair.first->resources != pair.second->resources)
                    impl_->temporal_retired.push_back(pair.first->resources);
                *pair.first = std::move(*pair.second);
            }
            impl_->temporal_resources->last_use = impl_->temporal_draws.resources->last_use =
                impl_->temporal_faces.resources->last_use = serial;
            for (auto &upload : temporal_uploads) {
                upload->last_use = serial;
                impl_->temporal_retired.push_back(upload);
            }
        }
        impl_->targets->last_use = serial;
        impl_->lighting->last_use = impl_->shadow_targets->last_use = serial;
        if (impl_->environment_resources)
            impl_->environment_resources->last_use = serial;
        if (impl_->shadow_dirty)
            impl_->shadow_content = impl_->next_shadow_content;
        impl_->shadow_shader_read = true;
        for (const auto &draw : scene.draws)
            if (draw.surface) {
                const auto &set = impl_->material_sets.at(draw.surface_key);
                set->resources->last_use = serial;
                for (const auto &map : set->maps)
                    if (map)
                        map->resources->last_use = serial;
            }
        if (impl_->optimized)
            impl_->display_shader_read = true;
        frame.submit_validate_ms = submit_timing.validated;
        frame.submit_context_ms = submit_timing.context_ready - submit_timing.validated;
        frame.submit_encode_ms = submit_timing.encoded - submit_timing.context_ready;
        frame.submit_execute_signal_ms = submit_timing.returned - submit_timing.encoded;
        frame.submit_context_wait_ms = submit_timing.context_wait_ms;
        frame.host_timeline.submitted = elapsed(entry);
        frame.cpu_submission_milliseconds = elapsed(submit_start);
        if (deferred) {
            deferred->serial = serial;
            deferred->entry = entry;
            deferred->calibration = calibration ? std::optional{calibration.value()} : std::nullopt;
            frame.deferred_timing =
                std::shared_ptr<const ResidentTimingReceipt>(new ResidentTimingReceipt(deferred));
            frame.resident_image =
                ResidentViewportImage{impl_->device, impl_->display_target, impl_->targets};
            frame.gpu_milliseconds = frame.gpu_display_milliseconds =
                frame.gpu_all_stages_milliseconds = -1;
            frame.gpu_transfer_milliseconds = 0;
            frame.host_timeline.fence_complete = frame.host_timeline.timestamps_read = -1;
            frame.host_timeline.readback_ready = frame.host_timeline.outputs_allocated =
                frame.host_timeline.resolved = -1;
            image.diagnostics.push_back("Same-queue resident image; no image transfer or producer "
                                        "fence wait; GPU timing deferred to consumer fence");
            frame.logical_resident_bytes +=
                120U * static_cast<std::uint64_t>(
                           std::count_if(impl_->timing_slots.begin(), impl_->timing_slots.end(),
                                         [](const auto &slot) { return bool(slot); }));
            image.host_milliseconds = elapsed(started);
            frame.host_timeline.returning = elapsed(entry);
            return core::Result<AuthoringGpuFrame>::success(std::move(frame));
        }
        const auto wait_start = Clock::now();
        need(impl_->device->wait(serial));
        frame.host_timeline.fence_complete = elapsed(entry);
        frame.fence_wait_milliseconds = elapsed(wait_start);
        frame.submit_wait_milliseconds = elapsed(submit_start);
        const auto timing = need(impl_->device->read_buffer(impl_->timestamps, 0, 120));
        std::array<std::uint64_t, 15> ticks{};
        std::memcpy(ticks.data(), timing.data(), timing.size());
        frame.host_timeline.timestamps_read = elapsed(entry);
        const auto frequency = impl_->device->timestamp_frequency();
        frame.gpu_temporal_upload_ms =
            frequency ? double(ticks[14] - ticks[13]) * 1000 / double(frequency) : -1;
        frame.gpu_temporal_motion_ms =
            frequency ? double(ticks[10] - ticks[9]) * 1000 / double(frequency) : -1;
        frame.gpu_temporal_resolve_ms =
            frequency ? double(ticks[11] - ticks[10]) * 1000 / double(frequency) : -1;
        frame.gpu_temporal_history_ms =
            frequency ? double(ticks[12] - ticks[11]) * 1000 / double(frequency) : -1;
        frame.gpu_shadow_milliseconds =
            frequency ? double(ticks[8] - ticks[7]) * 1000 / double(frequency) : -1;
        if (calibration && frequency) {
            const auto host_ms = [&](std::uint64_t tick) {
                return std::chrono::duration<double, std::milli>(calibration.value().host_epoch -
                                                                 entry)
                           .count() +
                       double((static_cast<long double>(tick) - calibration.value().gpu_ticks) *
                              1000.0L / frequency);
            };
            frame.gpu_begin_host_ms = host_ms(ticks[13]);
            frame.gpu_end_host_ms = host_ms(ticks[impl_->optimized ? 6 : 1]);
            frame.clock_uncertainty_ms = calibration.value().uncertainty_ms;
        }
        frame.gpu_milliseconds =
            frequency ? double(ticks[1] - ticks[0]) * 1000.0 / double(frequency) : -1.0;
        if (resident) {
            frame.gpu_all_stages_milliseconds =
                frequency ? double(ticks[6] - ticks[13]) * 1000.0 / double(frequency) : -1;
            frame.gpu_display_milliseconds =
                frequency ? double(ticks[3] - ticks[2]) * 1000.0 / double(frequency) : -1;
            frame.gpu_transfer_milliseconds =
                0; // No image transfer; final barrier is in the envelope.
            frame.gpu_query_resolve_ms =
                frequency ? double(ticks[6] - ticks[5]) * 1000.0 / double(frequency) : -1;
            frame.resident_image =
                ResidentViewportImage{impl_->device, impl_->display_target, impl_->targets};
            image.diagnostics.push_back("Same-device GPU image; no full-frame readback or upload; "
                                        "timestamp fence wait retained");
            image.host_milliseconds = elapsed(started);
            frame.host_timeline.readback_ready = frame.host_timeline.timestamps_read;
            frame.host_timeline.outputs_allocated = frame.host_timeline.timestamps_read;
            frame.host_timeline.resolved = frame.host_timeline.timestamps_read;
            frame.host_timeline.returning = elapsed(entry);
            return core::Result<AuthoringGpuFrame>::success(std::move(frame));
        }
        const auto read_start = Clock::now();
        const auto count = std::size_t(image.width) * image.height;
        std::vector<float> hdr, ids, depth;
        if (impl_->optimized) {
            frame.gpu_all_stages_milliseconds =
                frequency ? double(ticks[6] - ticks[13]) * 1000.0 / double(frequency) : -1.0;
            frame.gpu_query_resolve_ms =
                frequency ? double(ticks[6] - ticks[5]) * 1000.0 / double(frequency) : -1.0;
            frame.gpu_display_milliseconds =
                frequency ? double(ticks[3] - ticks[2]) * 1000.0 / double(frequency) : -1.0;
            frame.gpu_transfer_milliseconds =
                frequency ? double(ticks[5] - ticks[4]) * 1000.0 / double(frequency) : -1.0;
        } else {
            hdr = need(impl_->device->read_texture_rgba32f(impl_->hdr));
            ids = need(impl_->device->read_texture_rgba32f(impl_->ids));
            depth = need(impl_->device->read_texture_depth32f(impl_->depth));
        }
        frame.readback_milliseconds = elapsed(read_start);
        frame.host_timeline.readback_ready = elapsed(entry);
        const auto display_start = Clock::now();
        image.linear_hdr.resize(count);
        image.objects.resize(count);
        image.faces.resize(count);
        image.depth.resize(count);
        image.display_rgba8.resize(count * 4U);
        frame.host_timeline.outputs_allocated = elapsed(entry);
        const auto resolve = [&](std::span<const std::uint8_t> mapped) -> core::Result<void> {
            const Impl::Copy *hdr_copy = nullptr, *depth_copy = nullptr, *display_copy = nullptr;
            if (impl_->optimized) {
                for (const auto &copy : impl_->copies) {
                    if (copy.display)
                        display_copy = &copy;
                    else if (copy.channels == 4)
                        hdr_copy = &copy;
                    else
                        depth_copy = &copy;
                }
                if (!hdr_copy || !depth_copy || !display_copy)
                    throw std::runtime_error("GPU readback layout is incomplete");
            }
            for (std::uint32_t row = 0; row < image.height; ++row) {
                if (impl_->optimized)
                    std::memcpy(image.display_rgba8.data() + std::size_t(row) * image.width * 4U,
                                mapped.data() + display_copy->offset +
                                    std::size_t(row) * display_copy->pitch,
                                std::size_t(image.width) * 4U);
                for (std::uint32_t column = 0; column < image.width; ++column) {
                    const std::size_t p = std::size_t(row) * image.width + column;
                    std::array<float, 4> hdr_pixel{};
                    float depth_pixel = 0;
                    if (impl_->optimized) {
                        std::memcpy(hdr_pixel.data(),
                                    mapped.data() + hdr_copy->offset +
                                        std::size_t(row) * hdr_copy->pitch + column * 16U,
                                    16U);
                    } else {
                        std::copy_n(hdr.data() + p * 4U, 4U, hdr_pixel.begin());
                        depth_pixel = depth[p];
                    }
                    std::uint32_t ordinal = 0U;
                    long primitive = 0;
                    const auto clear_background = [&] {
                        image.objects[p] = {};
                        image.faces[p] = {};
                        image.linear_hdr[p] = {};
                        image.depth[p] = std::numeric_limits<double>::infinity();
                        if (!impl_->optimized)
                            std::fill_n(image.display_rgba8.data() + p * 4U, 4U, std::uint8_t{});
                    };
                    if (impl_->optimized) {
                        const double packed = hdr_pixel[3];
                        if (packed < 0)
                            throw std::runtime_error(
                                "GPU material program produced invalid numeric values");
                        if (packed == 0) {
                            clear_background();
                            continue;
                        }
                        if (!std::isfinite(packed) || packed < 1 || packed > 16777216.0)
                            throw std::runtime_error("GPU packed identity is invalid");
                        const auto integer = static_cast<std::uint32_t>(packed);
                        if (double(integer) != packed)
                            throw std::runtime_error("GPU packed identity is fractional");
                        const auto value = integer - 1U;
                        ordinal = value / 65536U;
                        primitive = static_cast<long>(value % 65536U) + 1;
                        if (ordinal >= scene.draws.size())
                            throw std::runtime_error("GPU draw ordinal is invalid");
                        std::memcpy(&depth_pixel,
                                    mapped.data() + depth_copy->offset +
                                        std::size_t(row) * depth_copy->pitch + column * 4U,
                                    4U);
                    } else {
                        if (ids[p * 4U] == 0) {
                            clear_background();
                            continue;
                        }
                        if (!std::isfinite(ids[p * 4U]) || !std::isfinite(ids[p * 4U + 1U]) ||
                            ids[p * 4U] < 1 || ids[p * 4U] > static_cast<float>(scene.draws.size()))
                            throw std::runtime_error("GPU identity readback is invalid");
                        ordinal = static_cast<std::uint32_t>(std::lround(ids[p * 4U])) - 1U;
                        primitive = std::lround(ids[p * 4U + 1U]);
                    }
                    const auto &draw = scene.draws[ordinal];
                    if (primitive < 1 || std::uint64_t(primitive) > draw.submesh.index_count / 3U)
                        throw std::runtime_error("GPU face ordinal is invalid");
                    image.objects[p] = draw.object;
                    image.faces[p] = draw.mesh->triangle_faces[draw.submesh.first_index / 3U +
                                                               std::size_t(primitive) - 1U];
                    const double z = depth_pixel, n = scene.view.near_plane,
                                 f = scene.view.far_plane;
                    image.depth[p] =
                        scene.view.projection == project::CameraProjection::orthographic
                            ? n + z * (f - n)
                            : n * f / (f - z * (f - n));
                    image.linear_hdr[p] = {hdr_pixel[0], hdr_pixel[1], hdr_pixel[2]};
                    if (!image.linear_hdr[p].finite())
                        throw std::runtime_error("GPU lighting produced nonfinite HDR radiance");
                    if (!impl_->optimized) {
                        auto encoded = need(render::linear_to_srgb(need(render::tone_map(
                            need(render::apply_exposure(image.linear_hdr[p], display.exposure_ev)),
                            display.tone_mode))));
                        image.display_rgba8[p * 4U] = std::uint8_t(std::lround(encoded.x * 255));
                        image.display_rgba8[p * 4U + 1] =
                            std::uint8_t(std::lround(encoded.y * 255));
                        image.display_rgba8[p * 4U + 2] =
                            std::uint8_t(std::lround(encoded.z * 255));
                        image.display_rgba8[p * 4U + 3] = 255;
                    }
                }
            }
            return core::Result<void>::success();
        };
        if (impl_->optimized) {
            ReadbackHostTiming read_timing;
            need(impl_->device->visit_readback(impl_->readback, 0, impl_->readback_bytes, resolve,
                                               read_timing));
            frame.readback_map_ms = read_timing.map_ms;
            frame.readback_unmap_ms = read_timing.unmap_ms;
            frame.readback_milliseconds = read_timing.map_ms + read_timing.unmap_ms;
        } else
            need(resolve({}));
        frame.display_milliseconds = elapsed(display_start);
        frame.host_timeline.resolved = elapsed(entry);
        image.host_milliseconds = elapsed(started);
        image.attachment_bytes = count * 52U;
        image.diagnostics.push_back("GPU raster/depth/PBR + host HDR/depth/identity readback; "
                                    "bounded material/UV/texture graphs; directional shadow PCF "
                                    "and directional environment");
        frame.host_timeline.returning = elapsed(entry);
        return core::Result<AuthoringGpuFrame>::success(std::move(frame));
    } catch (const std::exception &e) {
        impl_->temporal_host.invalidate();
        return core::Result<AuthoringGpuFrame>::failure({core::ErrorCode::invalid_state, e.what()});
    }
}
} // namespace carto::d3d12
