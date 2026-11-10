#pragma once

#include <carto/production/authoring_viewport.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/gpu/rhi.hpp>
#include <filesystem>
#include <atomic>
#include <memory>
#include <optional>

namespace carto::d3d12 {

// Synchronous preparation for an isolated snapshot consumer. Call off the UI
// thread: this can read and decode project-local blobs. The caller must own an
// immutable project snapshot and a copied project path for the entire call.
[[nodiscard]] core::Result<production::AuthoringViewportRequest>
prepare_readback_snapshot_request(const project::ProjectDocument& source,
    const std::optional<std::filesystem::path>& project_path,
    const production::AuthoringViewportRequest& request);

class D3D12Device;
struct ResidentViewportImage {
    std::shared_ptr<D3D12Device> device;
    gpu::TextureHandle texture;
    // Retains the actual target generation, including its device, through UI
    // submission. The resource is left in shader_read on the graphics queue.
    // It is a reusable surface: submit the consumer before producing the next
    // image on that queue. The lease preserves allocation, not old pixel values.
    std::shared_ptr<const void> lifetime;
};

struct ResidentGpuTiming {
    double raster_ms = -1, display_ms = -1, envelope_ms = -1;
    double temporal_upload_ms = -1, temporal_motion_ms = -1, temporal_resolve_ms = -1,
           temporal_history_ms = -1;
    double shadow_ms = -1; // nested in raster_ms; zero-work interval when reused
    double begin_host_ms = -1, end_host_ms = -1, clock_uncertainty_ms = -1;
};
// Nonblocking receipt, retained until the consuming UI fence has completed.
// A bounded pool prevents overwriting measurements still owned by a consumer.
class ResidentTimingReceipt final {
  public:
    [[nodiscard]] core::Result<ResidentGpuTiming> resolve() const;
  private:
    struct Impl;
    explicit ResidentTimingReceipt(std::shared_ptr<Impl> storage) : impl_(std::move(storage)) {}
    std::shared_ptr<Impl> impl_;
    friend class AuthoringViewport;
};

// Consecutive host boundaries in milliseconds from render() entry. GPU clocks
// use a different epoch; their durations are nested in submission/wait, never
// added to this host timeline. No UI acquisition/upload is included here.
struct AuthoringHostTimeline {
    double prepared = 0, resources_ready = 0, geometry_ready = 0;
    double materials_ready = 0;
    double commands_ready = 0, submitted = 0, fence_complete = 0;
    double timestamps_read = 0, readback_ready = 0, outputs_allocated = 0;
    double resolved = 0, returning = 0;
};

struct AuthoringGpuFrame {
    std::optional<ResidentViewportImage> resident_image;
    std::shared_ptr<const ResidentTimingReceipt> deferred_timing;
    double gpu_temporal_upload_ms = -1, gpu_temporal_motion_ms = -1, gpu_temporal_resolve_ms = -1,
           gpu_temporal_history_ms = -1;
    double temporal_prepare_ms=0,temporal_palette_prepare_ms=0;
    std::uint64_t temporal_upload_bytes = 0, temporal_resident_bytes = 0;
    bool temporal_history_eligible = false;
    unsigned temporal_reset = 0;
    double prepare_validate_ms = 0, prepare_identity_ms = 0, prepare_meshes_ms = 0;
    double prepare_materials_ms = 0;
    std::uint64_t mesh_compile_count = 0, mesh_cache_hits = 0;
    AuthoringHostTimeline host_timeline;
    double readback_map_ms = 0, readback_allocation_ms = 0, readback_copy_ms = 0;
    double readback_unmap_ms = 0;
    double submit_validate_ms = 0, submit_context_ms = 0, submit_encode_ms = 0;
    double submit_execute_signal_ms = 0, submit_context_wait_ms = 0;
    double gpu_begin_host_ms = -1, gpu_end_host_ms = -1, clock_uncertainty_ms = -1;
    double gpu_query_resolve_ms = -1;
    production::AuthoringViewportImage image;
    std::string device_name;
    double gpu_milliseconds = 0.0;
    double submit_wait_milliseconds = 0.0;
    double cpu_submission_milliseconds = 0.0;
    double fence_wait_milliseconds = 0.0;
    double cpu_prepare_milliseconds = 0.0;
    double geometry_update_milliseconds = 0.0;
    double material_update_milliseconds = 0.0, material_upload_wait_milliseconds = 0.0;
    double material_retire_milliseconds = 0.0, material_shader_compile_milliseconds = 0.0;
    double material_gpu_upload_milliseconds = -1;
    double lighting_update_milliseconds = 0, lighting_upload_wait_milliseconds = 0;
    double lighting_gpu_upload_milliseconds = -1, gpu_shadow_milliseconds = -1;
    std::uint64_t lighting_upload_bytes = 0, shadow_resident_bytes = 0, shadow_draw_calls = 0;
    double environment_prepare_milliseconds = 0, environment_compile_milliseconds = 0;
    double environment_upload_wait_milliseconds = 0, environment_gpu_upload_milliseconds = -1;
    std::uint64_t environment_upload_bytes = 0, environment_resident_bytes = 0;
    // Consecutive CPU boundaries from material-update entry: built, submitted,
    // upload fence complete, timing read, retired, return. Shader compilation,
    // upload wait and GPU copy duration are nested measurements, not addends.
    std::array<double,6> material_timeline_ms{};
    std::uint64_t material_upload_bytes = 0, material_resident_bytes = 0;
    double allocation_milliseconds = 0.0;
    double startup_shader_pipeline_milliseconds = 0.0;
    std::uint64_t shader_cache_hits = 0, shader_cache_misses = 0;
    double shader_cache_lookup_ms = 0, shader_dxc_compile_ms = 0, shader_cache_store_ms = 0;
    // Startup totals are immutable creation receipts, not per-frame work.
    std::uint64_t startup_shader_cache_hits = 0, startup_shader_cache_misses = 0;
    double startup_device_milliseconds = 0.0;
    double gpu_transfer_milliseconds = -1.0;
    double gpu_display_milliseconds = -1.0;
    double gpu_all_stages_milliseconds = -1.0;
    // Optimized: map/unmap API time only; actual mapped memory reads and
    // reconstruction are in display_milliseconds. Reference: copied readback.
    double readback_milliseconds = 0.0;
    // Host HDR/depth/ID resolution; also includes scalar display encoding in
    // reference mode. Optimized mode encodes display pixels on the GPU.
    double display_milliseconds = 0.0;
    std::uint64_t logical_resident_bytes = 0U;
    std::uint64_t geometry_upload_bytes = 0U;
    std::uint64_t target_generation = 0U;
    std::uint64_t draw_calls = 0U, triangles = 0U, readback_bytes = 0U;
    std::uint64_t image_transfer_bytes = 0U;
};

struct ReadbackViewportIdentity {
    std::uint64_t project_generation = 0, request_id = 0;
    [[nodiscard]] bool operator==(const ReadbackViewportIdentity&) const = default;
};
namespace detail {
class ReadbackCancellation final {
public:
    void request() noexcept { requested_.store(true, std::memory_order_release); }
    [[nodiscard]] bool requested() const noexcept {
        return requested_.load(std::memory_order_acquire);
    }
private:
    std::atomic<bool> requested_{false};
};
[[nodiscard]] constexpr bool valid_readback_rgba(std::uint32_t width, std::uint32_t height,
                                                   std::size_t bytes) noexcept {
    const auto pixels = std::uint64_t(width) * height;
    return pixels != 0U && pixels <= 1'048'576U && bytes == pixels * 4U;
}
// UI-thread-only admission bookkeeping. Retire only after the native thread
// and its device ownership have been confirmed complete.
class ReadbackViewportGate final {
public:
    enum class Admission { started, busy, stopped };
    [[nodiscard]] Admission begin(ReadbackViewportIdentity identity) noexcept {
        if (stopping_) return Admission::stopped;
        latest_ = identity;
        if (occupied_) return Admission::busy;
        occupied_ = true;
        return Admission::started;
    }
    void stop() noexcept { stopping_ = true; }
    void retire() noexcept { occupied_ = false; }
    [[nodiscard]] bool stopping() const noexcept { return stopping_; }
    [[nodiscard]] bool occupied() const noexcept { return occupied_; }
    [[nodiscard]] bool publishable(ReadbackViewportIdentity identity) const noexcept {
        return occupied_ && !stopping_ && identity == latest_;
    }
private:
    ReadbackViewportIdentity latest_{};
    bool occupied_ = false, stopping_ = false;
};
} // namespace detail
struct ReadbackViewportPoll {
    enum class State { idle, pending, draining, ready, failed, stale };
    State state = State::idle;
    ReadbackViewportIdentity identity{};
    std::optional<AuthoringGpuFrame> frame;
    // Present for ready only. Computed on the producer from the validated
    // final display_rgba8 bytes, before the result is published.
    std::optional<assets::Sha256Digest> image_digest;
    std::string error;
};
// One process-owned, separate-device readback job. All methods are called by
// the same UI owner thread. Before process/module teardown the owner must call
// request_stop() and keep its event loop alive until shutdown_ready() is true.
// Snapshot copying in begin may allocate/block; poll and stop never wait for GPU.
class ReadbackViewportWorker final {
public:
    [[nodiscard]] static ReadbackViewportWorker& process_owned();
    [[nodiscard]] core::Result<bool> begin(ReadbackViewportIdentity identity,
        project::ProjectDocument source, std::optional<std::filesystem::path> project_path,
        production::AuthoringViewportRequest request);
    // The caller must stop all other access to this device and its borrowed
    // native handles until poll() confirms that the worker has retired.
    [[nodiscard]] core::Result<bool> begin_resident(ReadbackViewportIdentity identity,
        project::ProjectDocument source, std::optional<std::filesystem::path> project_path,
        production::AuthoringViewportRequest request, std::shared_ptr<D3D12Device> device);
    [[nodiscard]] bool in_flight() const noexcept;
    // Grant only after all UI and platform-window submissions for the frame.
    // Once granted, the owner must not touch the device until worker retirement.
    [[nodiscard]] bool device_requested() const noexcept;
    [[nodiscard]] bool device_in_use() const noexcept;
    [[nodiscard]] bool grant_device() noexcept;
    [[nodiscard]] ReadbackViewportPoll poll();
    // UI owner may invalidate a busy result without releasing any job-owned
    // resource. The worker cooperatively checks this flag before GPU work.
    void invalidate() noexcept;
    // For a resident worker, the owner must stop device access before calling
    // shutdown_ready() and keep it stopped until asynchronous cleanup finishes.
    void request_stop() noexcept;
    [[nodiscard]] bool shutdown_ready() noexcept;
private:
    [[nodiscard]] core::Result<bool> begin_impl(ReadbackViewportIdentity identity,
        project::ProjectDocument source, std::optional<std::filesystem::path> project_path,
        production::AuthoringViewportRequest request, std::shared_ptr<D3D12Device> resident_device);
    struct Impl;
    ReadbackViewportWorker();
    ~ReadbackViewportWorker() = default;
    ReadbackViewportWorker(const ReadbackViewportWorker&) = delete;
    ReadbackViewportWorker& operator=(const ReadbackViewportWorker&) = delete;
    Impl* impl_ = nullptr; // process-owned; never destroyed while a worker can run
};

// Persistent native raster/depth/PBR resources. render() returns numeric host
// attachments for validation/picking; render_resident() retains the display
// image on the UI device. Cross-backend clients may use the explicit bridge.
class AuthoringViewport final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<AuthoringViewport>>
    create(bool optimized = true, bool cache_meshes = true);
    [[nodiscard]] static core::Result<std::unique_ptr<AuthoringViewport>>
    create_on_device(std::shared_ptr<D3D12Device> device, bool cache_meshes = true);
    ~AuthoringViewport();
    [[nodiscard]] core::Result<AuthoringGpuFrame>
    render(const project::ProjectDocument &source,
           const production::AuthoringViewportRequest &request);

    // Snapshot identity is retained by shared ownership; cached preparation
    // cannot outlive or alias a newly allocated source document.
    [[nodiscard]] core::Result<AuthoringGpuFrame>
    render(std::shared_ptr<const project::ProjectDocument> immutable_source,
           const production::AuthoringViewportRequest &request);
    // Transfers ownership of disposable host attachment storage for reuse.
    // Other retained frames remain independent. On failure the transferred
    // scratch storage is discarded; source and GPU resources stay valid.
    [[nodiscard]] core::Result<AuthoringGpuFrame>
    render(std::shared_ptr<const project::ProjectDocument> immutable_source,
           const production::AuthoringViewportRequest &request,
           production::AuthoringViewportImage recycled_output);

    // Same-device GPU presentation. image contains extent/view/source metadata
    // only; no full-frame CPU readback or display upload occurs. This initial
    // Synchronous timing is the correctness reference. With deferred timing the
    // caller resolves its receipt after its own same-queue consumer fence.
    [[nodiscard]] core::Result<AuthoringGpuFrame>
    render_resident(std::shared_ptr<const project::ProjectDocument> immutable_source,
                    const production::AuthoringViewportRequest& request,
                    bool synchronous_timing = true);

  private:
    [[nodiscard]] core::Result<AuthoringGpuFrame>
    render_impl(const project::ProjectDocument &source,
                const production::AuthoringViewportRequest &request,
                std::shared_ptr<const project::ProjectDocument> immutable_source,
                production::AuthoringViewportImage recycled_output = {}, bool resident = false,
                bool synchronous_timing = true);
    struct Impl;
    explicit AuthoringViewport(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> impl_;
};

} // namespace carto::d3d12
