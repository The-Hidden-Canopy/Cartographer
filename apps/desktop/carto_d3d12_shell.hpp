#pragma once

#include <carto/d3d12/authoring_viewport.hpp>
#include <carto/d3d12/bounded_fence_wait.hpp>
#include <carto/d3d12/device.hpp>
#include <carto/device_ir/command.hpp>
#include <carto/assets/texture_source.hpp>
#include "d3d12_ui_retirement.hpp"
#include <backends/imgui_impl_dx12.h>
#include <imgui.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace carto::desktop {

// Same-device adapter for the existing workbench. No external-memory handles,
// cross-adapter sharing or editable project state live in this shell.
class D3D12Shell final {
#ifdef CARTO_D3D12_SHELL_TESTING
    friend struct D3D12ShellTestAccess;
#endif
    using Clock = std::chrono::steady_clock;
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    static constexpr unsigned kFrames = 3, kDescriptors = 512;
    template<class T> static T need(core::Result<T> result) {
        if (!result) throw std::runtime_error(result.error().message);
        return std::move(result.value());
    }
    static void need(core::Result<void> result) {
        if (!result) throw std::runtime_error(result.error().message);
    }
    static void check(HRESULT result, const char* operation) {
        if (FAILED(result)) throw std::runtime_error(std::string(operation)+" failed: "+std::to_string(result));
    }
    static double ms(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now()-start).count();
    }
    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        std::uint64_t fence = 0, id = 0;
        bool query_ready = false, record = false;
        double gpu_clock_host_ms = 0;
        std::uint64_t gpu_clock_anchor = 0;
        double clock_uncertainty = -1;
        std::shared_ptr<const d3d12::ResidentTimingReceipt> producer_timing;
        double producer_entry = 0;
    };
    struct Texture {
        gpu::TextureHandle handle;
        std::shared_ptr<const void> lease;
        unsigned descriptor = kDescriptors;
        bool owned = false;
        std::string identity;
        std::uint32_t width = 0, height = 0;
    };
    struct DescriptorPool {
        std::array<bool, kDescriptors> used{};
    };
    struct RetiredTexture {
        Texture texture;
        std::shared_ptr<DescriptorPool> descriptors;
        std::shared_ptr<d3d12::D3D12Device> device;
        RetiredTexture(Texture&& value, std::shared_ptr<DescriptorPool> pool,
                       std::shared_ptr<d3d12::D3D12Device> owner) noexcept
            : texture(std::move(value)), descriptors(std::move(pool)), device(std::move(owner)) {}
        RetiredTexture(const RetiredTexture&) = delete;
        RetiredTexture& operator=(const RetiredTexture&) = delete;
        RetiredTexture(RetiredTexture&& other) noexcept
            : texture(std::move(other.texture)), descriptors(std::move(other.descriptors)),
              device(std::move(other.device)) {
            other.texture.descriptor = kDescriptors;
            other.texture.owned = false;
        }
        ~RetiredTexture() noexcept {
            if (descriptors && texture.descriptor < kDescriptors)
                descriptors->used[texture.descriptor] = false;
            if (device && texture.owned) {
                try { static_cast<void>(device->destroy_texture(texture.handle)); } catch (...) {}
            }
        }
    };
    using TextureRetirement = D3D12UiRetirement<RetiredTexture, 128>;
    struct ViewportTiming {
        bool updated = false, resident = false;
        std::uint32_t width = 0, height = 0;
        double render_entry = 0, render_ms = 0, prepare_ms = 0, gpu_ms = -1;
        double gpu_begin = -1, gpu_end = -1, fence_wait_ms = 0, route_ms = 0;
        std::uint64_t transfer_bytes = 0;
        std::shared_ptr<const d3d12::ResidentTimingReceipt> pending;
    };
    struct AbandonedResources {
        std::shared_ptr<d3d12::D3D12Device> device;
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGISwapChain3> swapchain;
        ComPtr<ID3D12DescriptorHeap> rtv_heap, srv_heap;
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12Fence> texture_fence;
        HANDLE event = nullptr, latency_event = nullptr;
        ComPtr<ID3D12QueryHeap> queries;
        ComPtr<ID3D12Resource> query_readback;
        std::array<ComPtr<ID3D12Resource>, kFrames> back_buffers;
        std::array<Frame, kFrames> frames;
        std::optional<Texture> authoring, reference, scene_capture;
        std::array<std::optional<Texture>, 2> thumbnails;
        std::shared_ptr<DescriptorPool> descriptors;
        std::unique_ptr<TextureRetirement> retired_textures;
        ViewportTiming viewport;
    };
public:
    enum class TextureUploadResult { ready, pending, failed };
    ~D3D12Shell() {
        try { shutdown(); } catch (...) { abandon_resources(); }
    }
    [[nodiscard]] std::shared_ptr<d3d12::D3D12Device> authoring_device() const { return device_; }
    void initialize(HWND window) {
        window_ = window;
        const auto started = Clock::now();
        d3d12::D3D12DeviceOptions options;
        options.fence_wait_timeout = std::chrono::seconds(5);
#ifdef CARTO_DESKTOP_D3D12_DEBUG
        options.enable_debug_layer = true;
#endif
        device_ = std::shared_ptr<d3d12::D3D12Device>(need(d3d12::D3D12Device::create(options)));
        native_ = static_cast<ID3D12Device*>(device_->native_device_handle());
        queue_ = static_cast<ID3D12CommandQueue*>(device_->native_graphics_queue_handle());
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");
        RECT client{}; GetClientRect(window_, &client);
        width_ = static_cast<unsigned>(std::max<LONG>(1, client.right-client.left));
        height_ = static_cast<unsigned>(std::max<LONG>(1, client.bottom-client.top));
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width_; description.Height = height_;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = kFrames;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> initial;
        check(factory_->CreateSwapChainForHwnd(queue_, window_, &description, nullptr, nullptr, &initial), "CreateSwapChainForHwnd");
        check(initial.As(&swapchain_), "Query IDXGISwapChain3");
        check(swapchain_->SetMaximumFrameLatency(1), "Set UI maximum frame latency");
        latency_event_ = swapchain_->GetFrameLatencyWaitableObject();
        if (!latency_event_) throw std::runtime_error("UI frame latency event unavailable");
        check(factory_->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = kFrames;
        check(native_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_)), "Create RTV heap");
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = kDescriptors; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(native_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_)), "Create UI SRV heap");
        srv_stride_ = native_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        rtv_stride_ = native_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        create_back_buffers();
        check(native_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "Create UI fence");
        check(native_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&texture_fence_)), "Create UI texture retirement fence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) throw std::runtime_error("Create UI fence event failed");
        for (auto& frame : frames_) {
            check(native_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)), "Create UI allocator");
            check(native_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(), nullptr,
                IID_PPV_ARGS(&frame.commands)), "Create UI command list");
            check(frame.commands->Close(), "Close initial UI list");
        }
        D3D12_QUERY_HEAP_DESC queries{};
        queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queries.Count = kFrames*2;
        check(native_->CreateQueryHeap(&queries, IID_PPV_ARGS(&queries_)), "Create UI query heap");
        D3D12_HEAP_PROPERTIES read_heap{}; read_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC read_desc{}; read_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        read_desc.Width = kFrames*16; read_desc.Height = 1; read_desc.DepthOrArraySize = 1;
        read_desc.MipLevels = 1; read_desc.SampleDesc.Count = 1; read_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(native_->CreateCommittedResource(&read_heap, D3D12_HEAP_FLAG_NONE, &read_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&query_readback_)), "Create UI query readback");
        ImGui_ImplDX12_InitInfo init;
        init.Device = native_; init.CommandQueue = queue_; init.NumFramesInFlight = kFrames;
        init.RTVFormat = DXGI_FORMAT_B8G8R8A8_UNORM; init.DSVFormat = DXGI_FORMAT_UNKNOWN;
        init.SrvDescriptorHeap = srv_heap_.Get(); init.UserData = this;
        init.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
            auto& shell = *static_cast<D3D12Shell*>(info->UserData);
            const auto slot = shell.allocate_descriptor(); *cpu = shell.cpu_descriptor(slot); *gpu = shell.gpu_descriptor(slot);
        };
        init.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
            auto& shell = *static_cast<D3D12Shell*>(info->UserData);
            const auto first = shell.srv_heap_->GetCPUDescriptorHandleForHeapStart().ptr;
            const auto slot = static_cast<unsigned>((cpu.ptr-first)/shell.srv_stride_);
            if (slot >= kDescriptors || shell.gpu_descriptor(slot).ptr != gpu.ptr || !shell.descriptor_pool_->used[slot])
                throw std::runtime_error("Invalid UI descriptor retirement");
            shell.descriptor_pool_->used[slot] = false;
        };
        if (!ImGui_ImplDX12_Init(&init)) throw std::runtime_error("Dear ImGui DX12 initialization failed");
        initialized_ = true;
        startup_ms_ = ms(started);
    }
    void begin_host_frame() {
        // Preserve the epoch across bounded pacing retries, so their wall time
        // stays in the eventual displayed frame's CPU receipt.
        if (latency_pending_) return;
        entry_ = Clock::now(); ++frame_id_; viewport_.updated = false; viewport_.pending.reset();
        phases_ = {}; phases_[0] = 0;
        uploaded_bytes_ = 0;
        latency_wait_ms_ = 0;
    }
    [[nodiscard]] double host_elapsed_ms() const { return ms(entry_); }
    [[nodiscard]] bool new_frame() {
        if (shutdown_requested_) return false;
        phases_[1] = host_elapsed_ms();
        const auto latency_start = Clock::now();
        if (occluded_) {
            const auto status = swapchain_->Present(0,DXGI_PRESENT_TEST);
            if (status == DXGI_STATUS_OCCLUDED) {
                MsgWaitForMultipleObjectsEx(0,nullptr,16,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                latency_wait_ms_ += ms(latency_start); latency_pending_ = true; return false;
            }
            check(status,"Test UI visibility"); occluded_ = false;
        }
        const auto pacing = WaitForSingleObject(latency_event_,16);
        latency_wait_ms_ += ms(latency_start);
        if (pacing == WAIT_TIMEOUT) {
            check(native_->GetDeviceRemovedReason(),"UI device during frame pacing");
            latency_pending_ = true; return false;
        }
        if (pacing != WAIT_OBJECT_0) throw std::runtime_error("UI frame latency event wait failed");
        latency_pending_ = false;
        current_slot_ = static_cast<unsigned>((frame_id_-1)%kFrames);
        auto& frame = frames_[current_slot_];
        wait(frame.fence);
        collect_retired_textures();
        phases_[2] = host_elapsed_ms();
        collect_query(frame, current_slot_);
        static_cast<void>(device_->completed_serial());
        if (!device_->debug_receipts().empty())
            throw std::runtime_error("D3D12 native UI validation: "+device_->debug_receipts().front());
        ImGui_ImplDX12_NewFrame();
        if (!retired_textures_->begin_frame())
            throw std::runtime_error("UI texture retirement frame is already open");
        texture_frame_open_ = true;
        phases_[3] = host_elapsed_ms();
        return true;
    }
    void request_resize() noexcept { resize_pending_ = true; }
    [[nodiscard]] bool set_authoring_gpu_texture(const d3d12::ResidentViewportImage& image,
        std::uint32_t width, std::uint32_t height, std::string& diagnostic) {
        try {
            if (!device_ || !native_ || !queue_ || !srv_heap_ || !initialized_ || shutdown_requested_)
                throw std::runtime_error("UI resident texture binding is unavailable");
            if (image.device != device_ || !image.lifetime || !width || !height)
                throw std::runtime_error("Resident viewport must use the UI device and retain its target lease");
            if (authoring_ && authoring_->handle == image.texture && !authoring_->owned) {
                authoring_->lease = image.lifetime;
                return true;
            }
            auto candidate = make_view(image.texture, width, height);
            candidate.lease = image.lifetime;
            commit_texture(authoring_, std::move(candidate));
            return true;
        } catch (const std::exception& e) { diagnostic = e.what(); return false; }
    }
    [[nodiscard]] bool set_authoring_viewport_texture(const production::AuthoringViewportImage& image,
        std::string& diagnostic) {
        return upload_texture(image.width, image.height, image.display_rgba8, {}, authoring_, diagnostic);
    }
    [[nodiscard]] TextureUploadResult set_reference_image_texture(const assets::DecodedRgba8Texture& decoded,
        std::string& diagnostic) {
        assets::TextureSourceLimits limits; limits.max_pixels = 16'777'216ULL;
        const auto valid = decoded.validate(limits);
        if (!valid || decoded.admission.stage != assets::TextureSourceStage::decoded ||
            decoded.admission.source.digest.is_zero()) {
            diagnostic = valid ? "Reference image requires digest-bound decoded pixels" : valid.error().message;
            return TextureUploadResult::failed;
        }
        const auto identity = decoded.admission.source.digest.hex();
        if (reference_ && reference_->identity == identity && reference_->width == decoded.admission.width &&
            reference_->height == decoded.admission.height) return TextureUploadResult::ready;
        return upload_texture(decoded.admission.width, decoded.admission.height, decoded.pixels,
            identity, reference_, diagnostic) ? TextureUploadResult::ready : TextureUploadResult::failed;
    }
    [[nodiscard]] TextureUploadResult set_scene_capture_texture(std::uint32_t width, std::uint32_t height,
        std::span<const std::uint8_t> pixels, const assets::Sha256Digest& digest, std::string& diagnostic) {
        if (digest.is_zero()) { diagnostic = "Scene capture identity is invalid"; return TextureUploadResult::failed; }
        const auto identity = digest.hex();
        if (scene_capture_ && scene_capture_->identity == identity && scene_capture_->width == width &&
            scene_capture_->height == height) return TextureUploadResult::ready;
        return upload_texture(width, height, pixels, identity, scene_capture_, diagnostic)
            ? TextureUploadResult::ready : TextureUploadResult::failed;
    }
    template<class Raster>
    [[nodiscard]] TextureUploadResult set_source_reimport_thumbnail_texture(
        std::size_t slot, const Raster& raster, std::string& diagnostic) {
        if (slot >= thumbnails_.size() || raster.pixel_digest.is_zero() ||
            std::uint64_t(raster.width) * raster.height > 65'536ULL) {
            diagnostic = "Source thumbnail is invalid"; return TextureUploadResult::failed;
        }
        const auto identity = raster.pixel_digest.hex();
        auto& texture = thumbnails_[slot];
        if (texture && texture->identity == identity && texture->width == raster.width &&
            texture->height == raster.height) return TextureUploadResult::ready;
        return upload_texture(raster.width, raster.height, raster.rgba8, identity, texture, diagnostic)
            ? TextureUploadResult::ready : TextureUploadResult::failed;
    }
    [[nodiscard]] bool authoring_viewport_format_matches() const noexcept { return authoring_.has_value(); }
    void cancel_authoring_viewport_upload() noexcept {}
    void clear_authoring_viewport_texture() { retire_texture(authoring_); }
    void clear_scene_capture_texture(bool = true) { retire_texture(scene_capture_); }
    void clear_source_reimport_thumbnail_texture(std::size_t slot) {
        if (slot < thumbnails_.size()) retire_texture(thumbnails_[slot]);
    }
    [[nodiscard]] ImTextureID scene_capture_texture_id() const noexcept {
        return scene_capture_ ? static_cast<ImTextureID>(gpu_descriptor(scene_capture_->descriptor).ptr) : ImTextureID_Invalid;
    }
    [[nodiscard]] ImTextureID source_reimport_thumbnail_texture_id(std::size_t slot) const noexcept {
        return slot < thumbnails_.size() && thumbnails_[slot]
            ? static_cast<ImTextureID>(gpu_descriptor(thumbnails_[slot]->descriptor).ptr) : ImTextureID_Invalid;
    }
    [[nodiscard]] ImTextureID authoring_viewport_texture_id() const noexcept {
        return authoring_ ? static_cast<ImTextureID>(gpu_descriptor(authoring_->descriptor).ptr) : ImTextureID_Invalid;
    }
    [[nodiscard]] ImTextureID reference_image_texture_id() const noexcept {
        return reference_ ? static_cast<ImTextureID>(gpu_descriptor(reference_->descriptor).ptr) : ImTextureID_Invalid;
    }
    void clear_reference_image_texture() {
        if (!reference_) return;
        retire_texture(reference_);
    }
    void note_authoring(const d3d12::AuthoringGpuFrame& frame, double render_entry,
        double asset_prepare_begin=0,double asset_prepare_end=0) {
        viewport_ = {true, bool(frame.resident_image), frame.image.width, frame.image.height,
            render_entry, frame.host_timeline.returning, frame.cpu_prepare_milliseconds,
            frame.gpu_all_stages_milliseconds, frame.gpu_begin_host_ms, frame.gpu_end_host_ms,
            frame.fence_wait_milliseconds, 0, frame.image_transfer_bytes, frame.deferred_timing};
        trace_remaining_ = 30;
        auto material_log=trace_file("d3d12-material-v1.csv");
        material_log<<GetCurrentProcessId()<<','<<frame_id_<<','<<frame.image.width<<','<<frame.image.height
            <<','<<frame.cpu_prepare_milliseconds<<','<<frame.geometry_update_milliseconds
            <<','<<frame.material_update_milliseconds<<','<<frame.material_shader_compile_milliseconds
            <<','<<frame.material_upload_wait_milliseconds<<','<<frame.material_retire_milliseconds
            <<','<<frame.material_gpu_upload_milliseconds<<','<<frame.material_upload_bytes<<','<<frame.material_resident_bytes
            <<','<<frame.host_timeline.geometry_ready<<','<<frame.host_timeline.materials_ready
            <<','<<asset_prepare_begin<<','<<asset_prepare_end;
        for(auto boundary:frame.material_timeline_ms) material_log<<','<<boundary;
        material_log<<'\n';
        auto lighting_log = trace_file("d3d12-lighting-v1.csv");
        lighting_log << GetCurrentProcessId() << ',' << frame_id_ << ',' << frame.image.width << ','
                     << frame.image.height << ',' << frame.lighting_update_milliseconds << ','
                     << frame.lighting_upload_wait_milliseconds << ','
                     << frame.lighting_gpu_upload_milliseconds << ',' << frame.lighting_upload_bytes
                     << ',' << frame.shadow_resident_bytes << ',' << frame.shadow_draw_calls << ','
                     << frame.gpu_shadow_milliseconds << '\n';
        auto env_log = trace_file("d3d12-environment-v1.csv");
        env_log << GetCurrentProcessId() << ',' << frame_id_ << ',' << frame.image.width << ','
                << frame.image.height << ',' << frame.environment_prepare_milliseconds << ','
                << frame.environment_compile_milliseconds << ','
                << frame.environment_upload_wait_milliseconds << ','
                << frame.environment_gpu_upload_milliseconds << ','
                << frame.environment_upload_bytes << ',' << frame.environment_resident_bytes
                << '\n';
        auto temporal_log=trace_file("d3d12-temporal-v1.csv");
        temporal_log<<GetCurrentProcessId()<<','<<frame_id_<<','<<frame.image.width<<','<<frame.image.height
            <<','<<frame.temporal_history_eligible<<','<<frame.temporal_reset<<','<<frame.temporal_upload_bytes
            <<','<<frame.temporal_resident_bytes<<','<<frame.temporal_prepare_ms<<','<<frame.temporal_palette_prepare_ms<<','<<frame.gpu_temporal_upload_ms
            <<','<<frame.gpu_temporal_motion_ms<<','<<frame.gpu_temporal_resolve_ms<<','<<frame.gpu_temporal_history_ms<<'\n';
        auto cache_log = trace_file("d3d12-shader-cache-v1.csv");
        cache_log << GetCurrentProcessId() << ',' << frame_id_ << ','
                  << frame.startup_shader_cache_hits << ',' << frame.startup_shader_cache_misses
                  << ',' << frame.shader_cache_hits << ',' << frame.shader_cache_misses << ','
                  << frame.shader_cache_lookup_ms << ',' << frame.shader_dxc_compile_ms << ','
                  << frame.shader_cache_store_ms << '\n';
    }
    void note_route(double route_ms) { viewport_.route_ms = route_ms; }
    [[nodiscard]] bool render(ImDrawData* data) {
        if (shutdown_requested_) return false;
        phases_[4] = host_elapsed_ms();
        font_updates_ = 0;
        if (data->Textures) for (auto* texture : *data->Textures)
            if (texture->Status != ImTextureStatus_OK && texture->Status != ImTextureStatus_Destroyed) ++font_updates_;
        RECT client{}; GetClientRect(window_, &client);
        const auto width = static_cast<unsigned>(std::max<LONG>(0, client.right-client.left));
        const auto height = static_cast<unsigned>(std::max<LONG>(0, client.bottom-client.top));
        if (!width || !height || IsIconic(window_)) return false;
        if (resize_pending_ || width != width_ || height != height_) {
            wait_idle();
            // Discard command-list references only after the queue marker has
            // completed; ResizeBuffers requires all application references gone.
            for (auto& pending : frames_) {
                check(pending.allocator->Reset(), "Reset UI allocator before resize");
                check(pending.commands->Reset(pending.allocator.Get(), nullptr), "Reset UI list before resize");
                check(pending.commands->Close(), "Close UI list before resize");
            }
            for (auto& buffer : back_buffers_) buffer.Reset();
            check(swapchain_->ResizeBuffers(kFrames, width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT), "Resize UI swapchain");
            width_ = width; height_ = height; create_back_buffers(); resize_pending_ = false;
            present_pending_ = false;
        }
        auto& frame = frames_[current_slot_];
        check(frame.allocator->Reset(), "Reset UI allocator");
        check(frame.commands->Reset(frame.allocator.Get(), nullptr), "Reset UI list");
        const auto index = swapchain_->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = back_buffers_[index].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        frame.commands->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, current_slot_*2);
        frame.commands->ResourceBarrier(1, &barrier);
        auto rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += index*rtv_stride_;
        const float clear[4]{.035F,.045F,.060F,1};
        frame.commands->ClearRenderTargetView(rtv, clear, 0, nullptr);
        frame.commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[]{srv_heap_.Get()};
        frame.commands->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(data, frame.commands.Get());
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        frame.commands->ResourceBarrier(1, &barrier);
        frame.commands->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, current_slot_*2+1);
        frame.commands->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, current_slot_*2,
            2, query_readback_.Get(), current_slot_*16);
        check(frame.commands->Close(), "Close UI list");
        phases_[5] = host_elapsed_ms();
        const auto calibration = device_->clock_calibration();
        if (calibration) {
            frame.gpu_clock_anchor = calibration.value().gpu_ticks;
            frame.gpu_clock_host_ms = std::chrono::duration<double,std::milli>(calibration.value().host_epoch-entry_).count();
            frame.clock_uncertainty = calibration.value().uncertainty_ms;
        }
        ID3D12CommandList* lists[]{frame.commands.Get()}; queue_->ExecuteCommandLists(1, lists);
        frame.fence = ++fence_value_;
        check(queue_->Signal(fence_.Get(), frame.fence), "Signal UI fence");
        frame.id = frame_id_; frame.query_ready = true;
        frame.producer_timing = viewport_.pending;
        frame.producer_entry = viewport_.render_entry;
        frame.record = trace_remaining_ != 0;
        phases_[6] = host_elapsed_ms();
        present_pending_ = true;
        const auto result = swapchain_->Present(1, 0);
        occluded_ = result == DXGI_STATUS_OCCLUDED;
        if (FAILED(result)) {
            device_->record_external_device_loss("UI Present", static_cast<std::uint32_t>(result));
            check(result, "UI Present");
        }
        phases_[7] = host_elapsed_ms();
        return true;
    }
    void finish_host_frame() {
        if (!retired_textures_->close_frame())
            throw std::runtime_error("UI texture retirement frame did not close");
        texture_frame_open_ = false;
        seal_retired_textures();
        phases_[8] = host_elapsed_ms();
#ifdef CARTO_DESKTOP_STARTUP_TRACE
        if (trace_remaining_ && traced_rows_++ < 4096) {
            auto log = trace_file("d3d12-ui-host-v3.csv");
            log << GetCurrentProcessId() << ',' << frame_id_ << ',' << width_ << ',' << height_;
            for (auto phase : phases_) log << ',' << phase;
            log << ',' << viewport_.updated << ',' << viewport_.resident << ',' << viewport_.width << ',' << viewport_.height
                << ',' << (viewport_.updated ? viewport_.render_entry : 0) << ',' << (viewport_.updated ? viewport_.render_ms : 0)
                << ',' << (viewport_.updated ? viewport_.prepare_ms : 0) << ',' << (viewport_.updated ? viewport_.gpu_ms : 0)
                << ',' << (viewport_.updated ? viewport_.gpu_begin : -1) << ',' << (viewport_.updated ? viewport_.gpu_end : -1)
                << ',' << (viewport_.updated ? viewport_.fence_wait_ms : 0) << ',' << (viewport_.updated ? viewport_.route_ms : 0)
                << ',' << (viewport_.updated ? viewport_.transfer_bytes : 0) << ',' << uploaded_bytes_
                << ',' << (ImGui::GetPlatformIO().Viewports.Size-1) << ',' << font_updates_ << ',' << startup_ms_
                << ',' << bool(viewport_.pending) << ',' << latency_wait_ms_ << '\n';
            --trace_remaining_;
        }
#endif
    }
    [[nodiscard]] bool prepare_shutdown() {
        shutdown_requested_ = true;
        if (!device_) return true;
        if (initialized_ && (!ImGui::GetCurrentContext() || ImGui_ImplDX12_HasUncertainRetirement()))
            return false;
        if (initialized_ && !ImGui_ImplDX12_PreparePlatformShutdown()) return false;
        if (!fence_ || !queue_) return !initialized_;
        if (shutdown_marker_failed_) return false;
        // The host calls this only after the event loop has stopped and any
        // unsubmitted draw data has been discarded.
        if (texture_frame_open_) {
            if (!retired_textures_->close_frame()) return false;
            texture_frame_open_ = false;
        }
        if (retired_textures_->outstanding() && !texture_shutdown_sealed_) {
            try { seal_retired_textures(); }
            catch (...) { shutdown_marker_failed_ = true; return false; }
            texture_shutdown_sealed_ = true;
        }
        try { collect_retired_textures(); }
        catch (...) { shutdown_marker_failed_ = true; return false; }
        if (retired_textures_->outstanding()) return false;
        if (!shutdown_marker_) {
            if (fence_value_ >= UINT64_MAX - 1) return false;
            const auto marker = fence_value_ + 1;
            if (FAILED(queue_->Signal(fence_.Get(), marker))) {
                shutdown_marker_failed_ = true;
                return false;
            }
            fence_value_ = marker;
            shutdown_marker_ = marker;
        }
        const auto completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX || completed < shutdown_marker_) return false;
        // This proves the application's queue work is complete. DXGI retains
        // any internal references needed by its presentation pipeline.
        present_pending_ = false;
        return true;
    }
    void shutdown() {
        if (!device_) return;
        if (present_pending_ || shutdown_marker_failed_ || (initialized_ && (!ImGui::GetCurrentContext() ||
            ImGui_ImplDX12_HasUncertainRetirement()))) {
            abandon_resources();
            throw std::runtime_error("UI presentation retirement is unconfirmed; resources retained until process exit");
        }
        try { collect_retired_textures(); }
        catch (...) {
            abandon_resources();
            throw std::runtime_error("UI texture retirement is unconfirmed; resources retained until process exit");
        }
        if (texture_frame_open_ || retired_textures_->outstanding()) {
            abandon_resources();
            throw std::runtime_error("UI textures remain in use; resources retained until process exit");
        }
        if (fence_ && queue_) {
            try { wait_idle(); }
            catch (...) {
                abandon_resources();
                throw std::runtime_error("UI retirement is unconfirmed; GPU resources retained until process exit");
            }
            for (unsigned i = 0; i < kFrames; ++i) collect_query(frames_[i], i);
        } else if (initialized_) {
            abandon_resources();
            throw std::runtime_error("UI retirement cannot be checked; GPU resources retained until process exit");
        }
        if (initialized_) { ImGui_ImplDX12_Shutdown(); initialized_ = false; }
        release(authoring_); release(reference_); release(scene_capture_);
        for (auto& thumbnail : thumbnails_) release(thumbnail);
        for (auto& buffer : back_buffers_) buffer.Reset();
        query_readback_.Reset(); queries_.Reset(); swapchain_.Reset();
        if (latency_event_) { CloseHandle(latency_event_); latency_event_ = nullptr; }
        for (auto& frame : frames_) { frame.commands.Reset(); frame.allocator.Reset(); }
        srv_heap_.Reset(); rtv_heap_.Reset(); fence_.Reset(); texture_fence_.Reset(); factory_.Reset();
        if (event_) { CloseHandle(event_); event_ = nullptr; }
        native_ = nullptr; queue_ = nullptr; device_.reset();
    }
private:
    void abandon_resources() noexcept {
        if (!abandoned_) return;
        abandoned_->device = std::move(device_);
        abandoned_->factory = std::move(factory_);
        abandoned_->swapchain = std::move(swapchain_);
        abandoned_->rtv_heap = std::move(rtv_heap_);
        abandoned_->srv_heap = std::move(srv_heap_);
        abandoned_->fence = std::move(fence_);
        abandoned_->texture_fence = std::move(texture_fence_);
        abandoned_->event = std::exchange(event_, nullptr);
        abandoned_->latency_event = std::exchange(latency_event_, nullptr);
        abandoned_->queries = std::move(queries_);
        abandoned_->query_readback = std::move(query_readback_);
        abandoned_->back_buffers = std::move(back_buffers_);
        abandoned_->frames = std::move(frames_);
        abandoned_->authoring = std::move(authoring_);
        abandoned_->reference = std::move(reference_);
        abandoned_->scene_capture = std::move(scene_capture_);
        abandoned_->thumbnails = std::move(thumbnails_);
        abandoned_->descriptors = std::move(descriptor_pool_);
        abandoned_->retired_textures = std::move(retired_textures_);
        abandoned_->viewport = std::move(viewport_);
        native_ = nullptr;
        queue_ = nullptr;
        // The renderer backend and its descriptors also remain live. This
        // state is fatal to the UI and must not be used for further frames.
        static_cast<void>(abandoned_.release());
    }
    void wait(std::uint64_t value) {
        if (!value) return;
        const auto result = d3d12::detail::bounded_fence_wait(value,
            Clock::now() + std::chrono::seconds(5),
            [&] { return fence_->GetCompletedValue(); },
            [&](std::uint64_t target) { return SUCCEEDED(fence_->SetEventOnCompletion(target, event_)); },
            [&](std::uint32_t milliseconds) {
                const auto status = WaitForSingleObject(event_, milliseconds);
                return status == WAIT_OBJECT_0 ? d3d12::detail::FenceEventOutcome::signaled :
                    status == WAIT_TIMEOUT ? d3d12::detail::FenceEventOutcome::timeout :
                        d3d12::detail::FenceEventOutcome::failed;
            }, [] { return Clock::now(); });
        if (result != d3d12::detail::FenceWaitOutcome::complete) {
            check(native_->GetDeviceRemovedReason(), "UI device removal during wait");
            throw std::runtime_error(result == d3d12::detail::FenceWaitOutcome::timeout
                ? "UI fence did not complete within five seconds"
                : "UI fence did not prove retirement");
        }
    }
    void wait_idle() {
        const auto value = ++fence_value_;
        check(queue_->Signal(fence_.Get(), value), "Signal UI retirement fence"); wait(value);
    }
    void retire_texture(std::optional<Texture>& current) {
        if (!current) return;
        auto reservation = retired_textures_->reserve();
        if (!reservation) throw std::runtime_error("UI texture retirement capacity exhausted");
        std::optional<RetiredTexture> retiring;
        retiring.emplace(std::move(*current), descriptor_pool_, device_);
        current.reset();
        if (!reservation.commit(retiring)) {
            current.emplace(std::move(retiring->texture));
            retiring->texture.descriptor = kDescriptors;
            retiring->texture.owned = false;
            throw std::runtime_error("UI texture retirement could not be committed");
        }
    }
    void seal_retired_textures() {
        if (!texture_fence_ || !queue_) {
            if (retired_textures_->outstanding())
                throw std::runtime_error("UI texture retirement fence is unavailable");
            return;
        }
        if (texture_serial_ >= UINT64_MAX - 1)
            throw std::runtime_error("UI texture retirement fence exhausted");
        const auto next = texture_serial_ + 1;
        if (FAILED(queue_->Signal(texture_fence_.Get(), next))) {
            shutdown_marker_failed_ = true;
            throw std::runtime_error("UI texture retirement marker is unconfirmed");
        }
        texture_serial_ = next;
        if (!retired_textures_->seal_after_platform_draws(next)) {
            shutdown_marker_failed_ = true;
            throw std::runtime_error("UI texture retirement could not be sealed");
        }
    }
    void collect_retired_textures() {
        if (!texture_fence_) {
            if (retired_textures_->outstanding())
                throw std::runtime_error("UI texture retirement fence is unavailable");
            return;
        }
        if (!retired_textures_->observe_completion(texture_fence_->GetCompletedValue()))
            throw std::runtime_error("UI texture retirement completion is invalid");
    }
    unsigned allocate_descriptor() {
        for (unsigned i = 0; i < kDescriptors; ++i) if (!descriptor_pool_->used[i]) {
            descriptor_pool_->used[i] = true; return i;
        }
        throw std::runtime_error("UI texture descriptor budget exhausted");
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor(unsigned slot) const noexcept {
        auto handle = srv_heap_->GetCPUDescriptorHandleForHeapStart(); handle.ptr += slot*srv_stride_; return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor(unsigned slot) const noexcept {
        auto handle = srv_heap_->GetGPUDescriptorHandleForHeapStart(); handle.ptr += slot*srv_stride_; return handle;
    }
    void create_back_buffers() {
        auto rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        for (unsigned i = 0; i < kFrames; ++i) {
            check(swapchain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i])), "Get UI back buffer");
            native_->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, rtv); rtv.ptr += rtv_stride_;
        }
    }
    Texture make_view(gpu::TextureHandle handle, std::uint32_t width, std::uint32_t height) {
        auto* resource = static_cast<ID3D12Resource*>(need(device_->native_texture_handle(handle)));
        const auto description = resource->GetDesc();
        if (description.Format != DXGI_FORMAT_R8G8B8A8_UNORM || description.Width != width || description.Height != height)
            throw std::runtime_error("UI sampled image format/extent mismatch");
        Texture view; view.handle = handle; view.width = width; view.height = height;
        view.descriptor = allocate_descriptor();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = description.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
        native_->CreateShaderResourceView(resource, &srv, cpu_descriptor(view.descriptor));
        return view;
    }
    void release(std::optional<Texture>& texture) noexcept {
        if (!texture) return;
        if (texture->descriptor < kDescriptors) descriptor_pool_->used[texture->descriptor] = false;
        if (texture->owned) {
            try { static_cast<void>(device_->destroy_texture(texture->handle)); } catch (...) {}
        }
        texture.reset();
    }
    void commit_texture(std::optional<Texture>& destination, Texture candidate) {
        try { retire_texture(destination); }
        catch (...) { descriptor_pool_->used[candidate.descriptor] = false; throw; }
        destination = std::move(candidate);
    }
    bool upload_texture(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> pixels,
        std::string identity, std::optional<Texture>& destination, std::string& diagnostic) {
        gpu::TextureHandle texture{}; gpu::BufferHandle staging{};
        try {
            if (!device_ || !native_ || !queue_ || !srv_heap_ || !initialized_ || shutdown_requested_)
                throw std::runtime_error("UI texture upload is unavailable");
            if (!width || !height || std::uint64_t(width)*height > 16'777'216ULL || pixels.size() != std::uint64_t(width)*height*4U)
                throw std::runtime_error("UI upload image extent/payload is invalid");
            const auto pitch = (width*4U+255U)&~255U;
            std::vector<std::uint8_t> packed(std::size_t(pitch)*height);
            for (std::uint32_t row = 0; row < height; ++row)
                std::memcpy(packed.data()+std::size_t(row)*pitch, pixels.data()+std::size_t(row)*width*4U, width*4U);
            texture = need(device_->create_texture({width,height,1,1,1,gpu::Format::rgba8_unorm,
                gpu::TextureDimension::texture_2d,gpu::TextureUsage::sampled|gpu::TextureUsage::transfer_destination}));
            staging = need(device_->create_buffer({packed.size(),gpu::MemoryClass::upload,gpu::BufferUsage::transfer_source}));
            need(device_->write_buffer(staging,0,packed));
            device_ir::DeviceCommandStream commands;
            commands.append(device_ir::CmdTransitionResource{{staging},device_ir::ResourceState::undefined,device_ir::ResourceState::copy_source});
            commands.append(device_ir::CmdTransitionResource{{texture},device_ir::ResourceState::undefined,device_ir::ResourceState::copy_destination});
            commands.append(device_ir::CmdCopyBufferToTexture{staging,texture,0});
            commands.append(device_ir::CmdTransitionResource{{texture},device_ir::ResourceState::copy_destination,device_ir::ResourceState::shader_read});
            need(device_->wait(need(device_->submit(commands))));
            need(device_->destroy_buffer(staging)); staging = {};
            auto candidate = make_view(texture,width,height); candidate.owned = true; candidate.identity = std::move(identity);
            commit_texture(destination,std::move(candidate)); texture = {};
            uploaded_bytes_ += pixels.size();
            return true;
        } catch (const std::exception& e) {
            if (staging) static_cast<void>(device_->destroy_buffer(staging));
            if (texture) static_cast<void>(device_->destroy_texture(texture));
            diagnostic = e.what(); return false;
        }
    }
    static std::ofstream trace_file(const char* name) {
        std::array<wchar_t,32768> path{};
        const auto count = GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        const auto destination = std::filesystem::path(std::wstring(path.data(),count)).parent_path()/name;
        const bool header = !std::filesystem::exists(destination) || std::filesystem::file_size(destination) == 0;
        std::ofstream stream(destination,std::ios::app);
        if (header && std::strcmp(name,"d3d12-material-v1.csv") == 0)
            stream << "pid,frame_id,width,height,prepare_ms,geometry_ms,material_ms,material_shader_compile_ms,material_wait_ms,material_retire_ms,material_gpu_upload_ms,material_upload_bytes,material_resident_bytes,geometry_ready_ms,materials_ready_ms,asset_prepare_begin_ms,asset_prepare_end_ms,material_built_ms,material_submitted_ms,material_fence_ms,material_queries_read_ms,material_retired_ms,material_return_ms\n";
        if (header && std::strcmp(name, "d3d12-environment-v1.csv") == 0)
            stream << "pid,frame_id,width,height,environment_prepare_ms,environment_compile_ms,"
                      "environment_wait_ms,environment_gpu_upload_ms,environment_upload_bytes,"
                      "environment_resident_bytes\n";
        if(header && std::strcmp(name,"d3d12-temporal-v1.csv")==0)
            stream<<"pid,frame_id,width,height,history_eligible,reset_reason,palette_upload_bytes,temporal_resident_bytes,temporal_prepare_cpu_ms,palette_prepare_cpu_ms,palette_gpu_ms,motion_gpu_ms,resolve_gpu_ms,history_copy_gpu_ms\n";
        if(header && std::strcmp(name,"d3d12-temporal-gpu-v1.csv")==0)
            stream<<"pid,frame_id,palette_gpu_ms,motion_gpu_ms,resolve_gpu_ms,history_copy_gpu_ms,producer_envelope_ms\n";
        if (header && std::strcmp(name, "d3d12-shader-cache-v1.csv") == 0)
            stream << "pid,frame_id,startup_hits,startup_misses,material_hits,material_misses,"
                      "lookup_ms,dxc_compile_ms,store_ms\n";
        if (header && std::strcmp(name, "d3d12-lighting-v1.csv") == 0)
            stream << "pid,frame_id,width,height,lighting_cpu_ms,metadata_fence_wait_ms,metadata_"
                      "gpu_upload_ms,metadata_upload_bytes,shadow_resident_bytes,shadow_draw_calls,"
                      "synchronous_shadow_gpu_ms\n";
        if (header && std::strcmp(name, "d3d12-shadow-gpu-v1.csv") == 0)
            stream << "pid,frame_id,shadow_gpu_ms,raster_gpu_ms,producer_envelope_ms\n";
        if (header && std::strcmp(name,"d3d12-ui-host-v3.csv") == 0)
            stream << "pid,frame_id,window_width,window_height,entry_ms,events_done_ms,context_ready_ms,backend_new_frame_ms,ui_ready_ms,record_done_ms,submit_done_ms,present_return_ms,platform_return_ms,viewport_updated,resident,viewport_width,viewport_height,render_entry_ms,render_ms,prepare_ms,pbr_gpu_envelope_ms,pbr_gpu_begin_ms,pbr_gpu_end_ms,pbr_fence_wait_ms,viewport_route_ms,readback_bytes,upload_bytes,secondary_viewports,font_updates,startup_ms,timing_deferred,frame_latency_wait_ms\n";
        if (header && std::strcmp(name,"d3d12-ui-gpu-v3.csv") == 0)
            stream << "pid,frame_id,ui_gpu_ms,ui_gpu_begin_ms,ui_gpu_end_ms,clock_uncertainty_ms,deferred_pbr_gpu_ms,deferred_pbr_begin_ms,deferred_pbr_end_ms,deferred_pbr_clock_uncertainty_ms\n";
        stream << std::setprecision(10); return stream;
    }
    void collect_query(Frame& frame, unsigned slot) {
        if (!frame.query_ready) return;
        const D3D12_RANGE range{slot*16U,slot*16U+16U}; void* mapped = nullptr;
        check(query_readback_->Map(0,&range,&mapped),"Map UI timing receipt");
        std::array<std::uint64_t,2> ticks{};
        std::memcpy(ticks.data(),static_cast<const std::uint8_t*>(mapped)+slot*16U,16U);
        const D3D12_RANGE written{0,0}; query_readback_->Unmap(0,&written);
        std::optional<d3d12::ResidentGpuTiming> producer;
        if (frame.producer_timing) producer = need(frame.producer_timing->resolve());
#ifdef CARTO_DESKTOP_STARTUP_TRACE
        if (frame.record) {
            const auto frequency = device_->timestamp_frequency();
            if (!frequency) throw std::runtime_error("UI timestamp frequency is unavailable");
            const auto aligned = [&](auto tick) { return frame.gpu_clock_host_ms +
                double((static_cast<long double>(tick)-frame.gpu_clock_anchor)*1000.0L/frequency); };
            auto log = trace_file("d3d12-ui-gpu-v3.csv");
            if (producer) {
                auto temporal_log=trace_file("d3d12-temporal-gpu-v1.csv");
                temporal_log<<GetCurrentProcessId()<<','<<frame.id<<','<<producer->temporal_upload_ms
                    <<','<<producer->temporal_motion_ms<<','<<producer->temporal_resolve_ms<<','<<producer->temporal_history_ms
                    <<','<<producer->envelope_ms<<'\n';
                auto shadow_log = trace_file("d3d12-shadow-gpu-v1.csv");
                shadow_log << GetCurrentProcessId() << ',' << frame.id << ',' << producer->shadow_ms
                           << ',' << producer->raster_ms << ',' << producer->envelope_ms << '\n';
            }
            log << GetCurrentProcessId() << ',' << frame.id << ',' << double(ticks[1]-ticks[0])*1000.0/double(frequency) << ','
                << aligned(ticks[0]) << ',' << aligned(ticks[1]) << ',' << frame.clock_uncertainty
                << ',' << (producer ? producer->envelope_ms : -1)
                << ',' << (producer ? frame.producer_entry+producer->begin_host_ms : -1)
                << ',' << (producer ? frame.producer_entry+producer->end_host_ms : -1)
                << ',' << (producer ? producer->clock_uncertainty_ms : -1) << '\n';
        }
#endif
        frame.query_ready = false;
        frame.producer_timing.reset();
    }
    HWND window_ = nullptr;
    std::unique_ptr<AbandonedResources> abandoned_ = std::make_unique<AbandonedResources>();
    std::shared_ptr<d3d12::D3D12Device> device_;
    ID3D12Device* native_ = nullptr; ID3D12CommandQueue* queue_ = nullptr;
    ComPtr<IDXGIFactory4> factory_; ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_,srv_heap_;
    ComPtr<ID3D12Fence> fence_, texture_fence_; HANDLE event_ = nullptr;
    HANDLE latency_event_ = nullptr;
    ComPtr<ID3D12QueryHeap> queries_; ComPtr<ID3D12Resource> query_readback_;
    std::array<ComPtr<ID3D12Resource>,kFrames> back_buffers_;
    std::array<Frame,kFrames> frames_;
    std::shared_ptr<DescriptorPool> descriptor_pool_ = std::make_shared<DescriptorPool>();
    std::unique_ptr<TextureRetirement> retired_textures_ = std::make_unique<TextureRetirement>();
    std::optional<Texture> authoring_,reference_,scene_capture_;
    std::array<std::optional<Texture>, 2> thumbnails_;
    unsigned srv_stride_ = 0, rtv_stride_ = 0, width_ = 0, height_ = 0, current_slot_ = 0;
    std::uint64_t fence_value_ = 0, frame_id_ = 0;
    std::uint64_t shutdown_marker_ = 0;
    std::uint64_t texture_serial_ = 0;
    bool initialized_ = false, resize_pending_ = false;
    bool latency_pending_ = false, occluded_ = false;
    bool shutdown_requested_ = false, present_pending_ = false, shutdown_marker_failed_ = false;
    bool texture_frame_open_ = false, texture_shutdown_sealed_ = false;
    Clock::time_point entry_ = Clock::now(); std::array<double,9> phases_{};
    ViewportTiming viewport_;
    unsigned trace_remaining_ = 0, traced_rows_ = 0;
    unsigned font_updates_ = 0;
    std::uint64_t uploaded_bytes_ = 0;
    double startup_ms_ = 0;
    double latency_wait_ms_ = 0;
};

} // namespace carto::desktop
