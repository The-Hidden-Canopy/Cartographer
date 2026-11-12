#pragma once

#include <carto/assets/texture_mip.hpp>
#include <carto/device_ir/command.hpp>
#include <carto/d3d12/device.hpp>
#include <carto/production/lookdev_draw_package.hpp>
#include <carto/project/project.hpp>
#include <carto/scene/scene.hpp>

#include <filesystem>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::d3d12 {

// V34 transports direct anisotropy axes to bounded UV0 normal-map normals.
// V33 adds UV0-tangent-bound direct anisotropy with explicit unsupported inputs.
// V32 binds scene-only direct-light sheen to request and uploaded-mesh identity.
// V31 adds one explicitly selected authored spotlight to the bounded shadow pass.
// V30 applies bounded inverse-square falloff to authored point/spot lights.
// V29 binds bounded output-pixel projection jitter for temporal accumulation.
// V28 executes explicit perspective and orthographic camera projections.
// V27 executes authored per-slot texture channel mappings in the scene shader.
// V26 adds explicit constant subpixel history reprojection to V25's
// caller-bound temporal resolve. V25 added that resolve to V24's saved-project
// point/spot lighting, V23's saved-project directional-light subset, V22's
// authored vertex-color shading, sorted alpha blending, and V20's split-sum GGX
// environment integration, per-texture affine transforms, bounded UV streams,
// primary-stream normal-map tangents, both conventions, and material samplers.
inline constexpr std::string_view kSceneRenderAlgorithm =
    "carto.d3d12.scene-direct-capture.v34";
inline constexpr std::uint32_t kSceneRenderEnvironmentPrefilterSampleCount = 32U;
inline constexpr double kSceneRenderLocalLightDistanceFloorMeters = 0.01;
inline constexpr double kSceneRenderMaximumLocalLightIntensity = 1.0e20;
inline constexpr std::string_view kSceneRenderLocalLightRadianceUnits =
    "linear_lookdev_scale_not_calibrated_lumens";
inline constexpr std::string_view kSceneRenderLocalLightDistanceAttenuation =
    "inverse_square";
inline constexpr std::string_view kSceneRenderLocalLightRangeFade =
    "quartic_smooth";
inline constexpr std::string_view kSceneRenderLocalLightShadowing =
    "at_most_one_spotlight";
inline constexpr double kSceneRenderLocalShadowMinimumRangeMeters = 0.02;
inline constexpr double kSceneRenderLocalShadowMinimumOuterConeRadians = 0.01;
inline constexpr std::uint32_t kSceneRenderMaximumDimension = 16384U;
inline constexpr std::uint64_t kSceneRenderMaximumPixels =
    8192ULL * 4320ULL;

enum class SceneRenderProjection : std::uint8_t { perspective, orthographic };

// Explicit view input, never a mutation or promotion of an authored camera.
// Right-handed world, full vertical extent/FOV, D3D depth [0,1].
struct SceneRenderCamera {
    core::Vec3d position{0.0, 0.0, 5.0};
    core::Vec3d target{};
    core::Vec3d up{0.0, 1.0, 0.0};
    SceneRenderProjection projection = SceneRenderProjection::perspective;
    double vertical_fov = 0.7853981633974483;
    double orthographic_height = 10.0;
    double near_plane = 0.01;
    double far_plane = 1000.0;
    double exposure_ev = 0.0;
};

enum class SceneRenderLocalLightKind : std::uint8_t { point = 0U, spot = 1U };

struct SceneRenderLocalLight {
    scene::ObjectId object;
    SceneRenderLocalLightKind kind = SceneRenderLocalLightKind::point;
    core::Vec3d position{};
    // Spot emitters point along this world-space direction. Point lights do
    // not use the field.
    core::Vec3d emission_direction{0.0, 0.0, -1.0};
    // Linear source intensity; it is not photometrically calibrated. The
    // shader applies inverse-square falloff with the shared 1 cm floor.
    core::Vec3d radiance{1.0, 1.0, 1.0};
    double range = 10.0;
    double inner_cone_radians = 0.0;
    double outer_cone_radians = 0.0;
    // Only a single spotlight may cast shadows per capture. Point-light
    // shadows and simultaneous key/spot shadow maps remain unsupported.
    bool casts_shadows = false;
};

struct SceneRenderMaterialBinding {
    std::optional<std::uint32_t> material_slot;
    std::string source_identity;
    render::MaterialAsset current_material;
    production::CompiledMaterialArtifact material;
    // Scene-only derived graph factor. Direct-light Charlie/Neubelt sheen uses
    // base-color tint and surface roughness; a metallic-suppressed mixture with
    // the base sits beneath clearcoat. Not part of the generic material schema.
    // Must be finite in [0, 1]; nonzero values with environment IBL are refused.
    double sheen_weight = 0.0;
    // Scene-only signed strength [-1, 1]. Uses the authored primary UV0 tangent
    // axes; sign swaps the axes and magnitude blends the legacy specular kernel
    // toward anisotropic GGX/Smith. Nonzero strength requires valid UV0 tangents
    // and refuses environment IBL. UV0 normal maps with zero rotation and
    // positive axis scales carry those axes to the mapped shading normal by
    // shortest rotation; rotated/reflected maps remain unsupported.
    double anisotropy = 0.0;
};

// D3D12 scene-only material compilation keeps the exact authored source digest
// while placing supported shared UV scale/offset in scene root constants. The
// generic material compiler remains strict for consumers without this shader.
[[nodiscard]] core::Result<production::CompiledMaterialArtifact>
compile_scene_render_material_artifact(
    std::string source_identity,
    const render::MaterialAsset& material);

struct SceneRenderObject {
    scene::ObjectId object;
    core::Transform world_transform;
    production::RenderMeshArtifact mesh;
    std::vector<SceneRenderMaterialBinding> materials;
    production::LookdevDrawPackage draw_package;
    // The artifact's primary/secondary UV payloads retain these authored set
    // identities so a material slot cannot silently sample another stream.
    render::UvSetId uv0_set = render::kUv0;
    std::optional<render::UvSetId> uv1_set;
};

// Per-request explicit inputs for graph bindings used by the saved-scene adapter.
// Every graph key must resolve to the exact current source bound to a visible
// material; when present, these values override that material binding's saved
// inputs as a complete set for this request only (there is no implicit merge).
// The exact input digest participates in material/request identity. The adapter
// caps total parameter and surface values at 262,144 across bindings.
struct SceneRenderMaterialGraphInputBinding {
    core::Revision graph_revision;
    assets::Sha256Digest graph_digest;
    project::MaterialGraphEvaluationInputs inputs;
};

using SceneRenderMaterialGraphInputs =
    std::map<std::string, SceneRenderMaterialGraphInputBinding>;

struct SceneRenderTexture {
    assets::TextureAssetId asset = 0U;
    // Supplied independently from the cached mip artifact. A valid artifact
    // from a previous source cannot silently satisfy the current source.
    assets::Sha256Digest current_source_digest;
    assets::Rgba8MipChain mips;
};

struct SceneRenderEnvironmentMipLevel {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::array<float, 3U>> linear_rgb;
};

struct SceneRenderEnvironmentPrefilteredMipChain {
    assets::Sha256Digest source_digest;
    assets::ColorSpace source_color_space = assets::ColorSpace::linear;
    std::vector<SceneRenderEnvironmentMipLevel> levels;
    assets::Sha256Digest artifact_digest;

    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] core::Result<void> validate() const;
};

struct SceneRenderEnvironment {
    // Digest of original SH-interchange or equirectangular-image bytes. The
    // loaders compute it; direct request construction must bind source bytes.
    // Coefficients are real order-2 SH, convolved for Lambert diffuse and
    // normalized by pi, with +Y up and basis order:
    // Y00, Y1y, Y1z, Y1x, Y2xy, Y2yz, Y2(3z^2-1), Y2xz, Y2(x^2-y^2).
    assets::Sha256Digest source_digest;
    // Set only when coefficients were derived from an image. Its interpretation
    // participates in request identity and is included in receipts.
    std::optional<assets::ColorSpace> source_color_space;
    std::array<core::Vec3d, 9U> diffuse_irradiance_over_pi_sh{};
    // Optional digest-bound linear RGB panorama levels for GGX-prefiltered
    // roughness reflections. Both LDR and HDR sources use the same canonical
    // float representation; source_color_space binds input interpretation.
    std::optional<SceneRenderEnvironmentPrefilteredMipChain>
        specular_environment_mips;

    [[nodiscard]] core::Result<void> validate() const;
};

// Explicit temporal inputs from a caller-owned producer. History, when present,
// must contain scene-linear RGBA32F after the previous frame's temporal resolve.
// Motion vectors, when present, are current-to-history offsets in pixel units,
// with positive X right and positive Y down. With no history the pass copies the
// current frame; zero_motion=true permits history without a motion texture and
// asserts every vector is exactly zero. An optional constant_motion_pixels
// supplies a uniform current-to-history offset, useful for explicitly jittered
// samples; when a motion texture is also present, the constant offset is added
// to each per-pixel vector. It does not describe camera or object motion.
// Digests and the previous request digest are caller assertions: history must
// match the current project revision/digest, and every motion source must bind
// to that same prior request. The renderer validates
// metadata, but cannot inspect or authenticate GPU texture contents. Callers
// declare each incoming resource state so the graph can transition it to
// shader_read. The caller-owned output remains shader_read for direct-target
// renders or copy_source after an offscreen capture readback.
struct SceneRenderTemporalInputs {
    gpu::TextureHandle history_color;
    device_ir::ResourceState history_initial_state =
        device_ir::ResourceState::undefined;
    assets::Sha256Digest history_content_digest;
    assets::Sha256Digest history_request_digest;
    core::Revision history_source_revision;
    assets::Sha256Digest history_source_digest;
    gpu::TextureHandle motion_vectors;
    device_ir::ResourceState motion_initial_state =
        device_ir::ResourceState::undefined;
    assets::Sha256Digest motion_vectors_digest;
    assets::Sha256Digest motion_history_request_digest;
    gpu::TextureHandle resolved_color;
    device_ir::ResourceState resolved_initial_state =
        device_ir::ResourceState::undefined;
    float feedback = 0.9F;
    bool camera_cut = false;
    // Caller assertion that every motion vector is exactly zero. No motion map
    // or constant offset may be bound and the shader reads history at the
    // current pixel.
    bool zero_motion = false;
    std::optional<core::Vec2d> constant_motion_pixels;
};

enum class SceneRenderAntialiasing : std::uint8_t {
    none = 0U,
    fxaa = 1U,
};

struct SceneRenderGpuTiming {
    // Timestamp-derived duration for the recorded scene graph passes. Excludes
    // host setup, the timing-resolve pass, subsequent pixel readback, and any
    // presentation performed after submission.
    std::uint64_t elapsed_nanoseconds = 0U;
    std::uint64_t timestamp_frequency_hz = 0U;
};

struct SceneRenderRequest {
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    SceneRenderCamera camera;
    // Explicit sample offset in output-pixel units: positive X moves right,
    // positive Y moves down. Bounded to half a pixel per axis and included in
    // request identity. Zero preserves the unjittered projection.
    core::Vec2d subpixel_jitter_pixels{};
    render::PbrLight light{{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}};
    // Bounded point/spot sources. At most one spotlight can use the scene's
    // single shadow map; the shader applies inverse-square distance with a 1 cm
    // floor and a quartic smooth range fade. Intensity is an uncalibrated linear
    // lookdev scale, not lumens.
    std::vector<SceneRenderLocalLight> local_lights;
    core::Vec3d background_linear{};
    std::vector<SceneRenderObject> objects;
    std::vector<SceneRenderTexture> textures;
    // Sampler ID zero uses the renderer's default linear-clamp sampler. Each
    // nonzero entry is an exact descriptor required by a visible material.
    std::map<render::SamplerId, gpu::SamplerDesc> samplers;
    std::optional<SceneRenderEnvironment> environment;
    // Optional explicit temporal resolve. The caller provides exact history,
    // motion, and output textures; no cache or static-scene fallback is used.
    std::optional<SceneRenderTemporalInputs> temporal;
    // FXAA operates on tone-mapped display values. Temporal resolve blends
    // scene-linear HDR before tone mapping when explicit inputs are present.
    SceneRenderAntialiasing antialiasing = SceneRenderAntialiasing::fxaa;

    // Bounded synchronous direct-light rendering with optional diffuse SH and
    // source-image specular environment sampling:
    // <=4096 objects, 65536 draws, <=64 point/spot sources,
    // 1024 textures, 1024 sampler descriptors, up to 8192x4320 (8K-class),
    // 512 MiB mesh/material-mip payload, 64 MiB environment mip payload, and
    // 128 MiB environment upload. Caller-owned temporal texture allocations
    // are not included in those CPU payload bounds. Blended submeshes sort
    // back-to-front by centroid;
    // authored vertex colors multiply base color and alpha without a
    // color-space conversion because the source layer has no color-space tag.
    // No authoring authority.
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> digest() const;
};

// Reads only texture sources referenced by visible saved material assignments.
// Each locator, content digest, admitted header, decoded extent and semantic
// mip chain is checked against the current project before returning a render
// input. Missing or unsupported sources fail closed.
[[nodiscard]] core::Result<std::vector<SceneRenderTexture>>
load_project_scene_textures(
    const std::filesystem::path& project_path,
    const project::ProjectDocument& document);

// Loads a bounded text file with header CARTO_DIFFUSE_IRRADIANCE_SH_V1 and
// nine RGB triples. The file digest binds the exact supplied coefficients;
// this is diffuse IBL input, not proof of the originating environment image.
[[nodiscard]] core::Result<SceneRenderEnvironment>
load_scene_render_environment_sh(const std::filesystem::path& path);

// Decodes a bounded 2:1 equirectangular PNG/JPEG/TGA image using the caller's
// explicit color-space interpretation, or bounded Radiance RGBE data as linear,
// then derives Lambert diffuse order-2 SH and a bounded GGX-prefiltered
// reflection chain. Orientation is +Y up, +Z at image center, and +X toward
// the image's right edge. Radiance explicit primaries remain unsupported.
[[nodiscard]] core::Result<SceneRenderEnvironment>
load_scene_render_environment_equirectangular(
    const std::filesystem::path& path,
    assets::ColorSpace color_space);

// Resolves an explicitly selected saved perspective or orthographic camera
// through its current scene-object world transform.
[[nodiscard]] core::Result<SceneRenderCamera>
resolve_project_scene_render_camera(
    const project::ProjectDocument& document,
    scene::ObjectId camera_object,
    double exposure_ev = 0.0);

struct SceneRenderImage {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest request_digest;
    assets::Sha256Digest environment_digest;
    std::optional<assets::ColorSpace> environment_color_space;
    SceneRenderAntialiasing antialiasing = SceneRenderAntialiasing::fxaa;
    // PBR VS/PS, shadow VS/PS, tone-map VS/PS, then temporal CS.
    std::array<assets::Sha256Digest, 7U> shader_digests;
    // Final state of the caller-owned temporal output, after completion when
    // using an asynchronous handoff API.
    std::optional<device_ir::ResourceState> temporal_output_state;
    device::DeviceIdentity device;
    // Authored material draws in the shaded pass; shadow caster replays are
    // auxiliary work and are identified by the recorded scene.shadow pass.
    std::uint64_t draw_count = 0U;
    std::vector<float> linear_hdr_rgba;
    std::vector<float> depth;
    // Top-left origin, display-encoded ACES/sRGB, opaque RGBA8.
    std::vector<std::uint8_t> srgb_rgba;
    assets::Sha256Digest pixel_digest;
    // Fence serial for the render submission. Synchronous APIs return a
    // completed serial; the asynchronous handoff API returns before completion.
    gpu::SubmissionSerial submission_serial = 0U;
    // Present only when explicitly requested and supported by the graphics queue.
    std::optional<SceneRenderGpuTiming> gpu_timing;
    // Scene passes compiled and recorded through the shared render graph.
    std::vector<std::string> render_graph_passes;
    std::vector<std::string> limitations;
};

// Owns compiled scene programs, immutable mesh uploads, source-bound material/
// environment textures, and bounded render targets across repeated renders. The
// cache must be reset or destroyed before its device. It is single-threaded and
// must not be shared between concurrent renders.
class SceneRenderCache final {
public:
    SceneRenderCache();
    ~SceneRenderCache();
    SceneRenderCache(const SceneRenderCache&) = delete;
    SceneRenderCache& operator=(const SceneRenderCache&) = delete;
    SceneRenderCache(SceneRenderCache&&) noexcept;
    SceneRenderCache& operator=(SceneRenderCache&&) noexcept;

    // Releases program, mesh, material, and target resources; do this before
    // destroying the device. The next render starts a new cache generation.
    void reset() noexcept;
    // Number of program-set builds since reset, saturating at uint64 max.
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend core::Result<SceneRenderImage> render_scene_cached(
        D3D12Device&,
        SceneRenderCache&,
        const SceneRenderRequest&,
        const std::filesystem::path&);
    friend core::Result<SceneRenderImage> render_scene_with_cache(
        D3D12Device&,
        SceneRenderCache&,
        const SceneRenderRequest&,
        const std::filesystem::path&,
        std::optional<gpu::TextureHandle>,
        device_ir::ResourceState,
        std::optional<gpu::TextureHandle>,
        bool,
        bool);
};

// Metadata for a GPU-rendered frame. Unlike SceneRenderImage, this does not
// read HDR, depth, or display pixels back to the host.
struct SceneRenderReceipt {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    assets::Sha256Digest request_digest;
    // Completion boundary for the GPU work that produced this receipt.
    gpu::SubmissionSerial submission_serial = 0U;
    assets::Sha256Digest environment_digest;
    std::optional<assets::ColorSpace> environment_color_space;
    SceneRenderAntialiasing antialiasing = SceneRenderAntialiasing::fxaa;
    std::array<assets::Sha256Digest, 7U> shader_digests;
    // State the caller must use when reimporting its temporal output texture.
    std::optional<device_ir::ResourceState> temporal_output_state;
    device::DeviceIdentity device;
    // Authored material draws in the shaded pass; excludes shadow caster replays.
    std::uint64_t draw_count = 0U;
    // Scene and optional desktop handoff passes in compiled execution order.
    std::vector<std::string> render_graph_passes;
    std::vector<std::string> limitations;
};

// Read-only saved-document adapter. Consumes explicit authored UV0, material
// regions, assignments and world transforms; it never invents missing UVs or
// a default material. Bound graphs with CPU-evaluable nodes and supported PBR
// outputs are lowered into material factors. Optional graph opacity multiplies
// graph alpha as coverage; both must be in [0, 1]. The authored alpha mode and
// cutoff are preserved (opaque mode still ignores coverage). This is not
// refraction or transmission. Scalar sheen lowers into the scene-only direct
// lobe described by SceneRenderMaterialBinding; sheen IBL is not supported.
// Scalar anisotropy uses that binding's explicit UV0-tangent contract.
// Durable graph inputs are used by
// default; an optional transient input map is revision/digest-bound to the exact
// source and overrides them for that request only. Missing inputs, texture
// sampling, variants on graph-bound materials, unsupported texture controls,
// and unsupported outputs fail.
[[nodiscard]] core::Result<SceneRenderRequest> prepare_project_scene_render(
    std::string_view project_identity,
    const project::ProjectDocument& document,
    SceneRenderCamera camera,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<SceneRenderTexture> textures = {},
    render::PbrLight light = {{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}},
    SceneRenderMaterialGraphInputs graph_inputs = {});

// Renders immutable derived sources on the actual caller-owned device and
// retires only its own temporary resources. No scene/journal mutations. The
// directory must contain the packaged scene PBR, shadow, tone-map, and temporal
// HLSL sources; the temporal shader is compiled even when a request omits
// temporal inputs. Optional GPU timing requires graphics-queue timestamps and
// adds only measurement commands; it does not participate in source or request
// identity.
[[nodiscard]] core::Result<SceneRenderImage> render_scene(
    D3D12Device& device,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory);
[[nodiscard]] core::Result<SceneRenderImage> render_scene(
    D3D12Device& device,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    bool capture_gpu_timing);

// Repeated offscreen captures reuse compiled shader/pipeline resources, mesh
// vertex/index uploads, source-bound material textures, and internal render
// targets while their identities and dimensions remain compatible. Scalar AO
// factors travel in existing material root constants without synthetic GPU
// textures. Internal targets above the bounded cache budget remain per-capture.
// The timing overload captures graphics-queue timestamps when requested;
// measurement mode does not participate in the scene request digest.
[[nodiscard]] core::Result<SceneRenderImage> render_scene_cached(
    D3D12Device& device,
    SceneRenderCache& cache,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory);
[[nodiscard]] core::Result<SceneRenderImage> render_scene_cached(
    D3D12Device& device,
    SceneRenderCache& cache,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    bool capture_gpu_timing);

// Renders directly into a caller-owned BGRA8 render target. The target must
// match the request extent and have color_attachment plus transfer_source
// usage. Its initial state must be undefined for a new target or copy_source
// when re-rendering; successful calls leave it in copy_source. No pixels are
// read back to the host, and the target remains caller-owned.
[[nodiscard]] core::Result<SceneRenderReceipt> render_scene_to_texture(
    D3D12Device& device,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    gpu::TextureHandle target,
    device_ir::ResourceState target_initial_state);

// Cached counterpart for repeated direct-target renders. The cache also binds
// the render-target format, so switching between capture and desktop formats
// rebuilds the program set. Bounded internal render targets may also be reused
// when dimensions match; requests above the target-cache budget remain
// per-render allocations.
[[nodiscard]] core::Result<SceneRenderReceipt> render_scene_to_texture_cached(
    D3D12Device& device,
    SceneRenderCache& cache,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    gpu::TextureHandle target,
    device_ir::ResourceState target_initial_state);

// Records the authored scene and its copy/present handoff in one compiled
// graph and one synchronous submission. The scene target remains caller-owned
// and is left in copy_source; the BGRA8 frame target must match the request
// extent, include transfer_destination usage, and begin in present. On success
// the frame target is left in present. No frame pixels are read back.
[[nodiscard]] core::Result<SceneRenderReceipt> render_scene_to_texture_with_frame_handoff(
    D3D12Device& device,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    gpu::TextureHandle scene_target,
    device_ir::ResourceState scene_target_initial_state,
    gpu::TextureHandle frame_target);

// Cached counterpart for the integrated scene render/copy/present submission.
[[nodiscard]] core::Result<SceneRenderReceipt>
render_scene_to_texture_with_frame_handoff_cached(
    D3D12Device& device,
    SceneRenderCache& cache,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    gpu::TextureHandle scene_target,
    device_ir::ResourceState scene_target_initial_state,
    gpu::TextureHandle frame_target);

// Asynchronous cached frame handoff. Returns after queue submission without
// waiting for the GPU. Track the receipt serial with a frame scheduler or wait
// before CPU readback. Subsequent resource uses must remain ordered on the same
// graphics queue.
[[nodiscard]] core::Result<SceneRenderReceipt>
render_scene_to_texture_with_frame_handoff_cached_async(
    D3D12Device& device,
    SceneRenderCache& cache,
    const SceneRenderRequest& request,
    const std::filesystem::path& shader_directory,
    gpu::TextureHandle scene_target,
    device_ir::ResourceState scene_target_initial_state,
    gpu::TextureHandle frame_target);

} // namespace carto::d3d12
