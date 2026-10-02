#include <carto/d3d12/device.hpp>
#include <carto/assets/blob_store.hpp>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <span>
#include <type_traits>
#include <utility>

#if CARTO_D3D12_HAS_DXC
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-attributes"
#endif
#include CARTO_DXC_HEADER
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

#ifdef DeviceCapabilities
#undef DeviceCapabilities
#endif

namespace carto::d3d12 {

namespace {

using Microsoft::WRL::ComPtr;
using core::Diagnostic;
using core::ErrorCode;

Diagnostic unsupported(std::string message) {
    return Diagnostic(ErrorCode::unsupported, std::move(message));
}

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic validation(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

Diagnostic out_of_memory(std::string message) {
    return Diagnostic(ErrorCode::invalid_state, std::move(message));
}

Diagnostic native_failure(const char* operation, HRESULT status) {
    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex
            << static_cast<unsigned long>(status);
    return Diagnostic(ErrorCode::invalid_state, message.str());
}

Diagnostic device_lost_failure() {
    return Diagnostic(
        ErrorCode::invalid_state,
        "D3D12 device is lost; call recover() to create a fresh device");
}

constexpr std::size_t kMaxHlslSourceBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaxDebugMessageBytes = 64U * 1024U;

bool is_device_removed_hresult(HRESULT status) {
    return status == DXGI_ERROR_DEVICE_REMOVED || status == DXGI_ERROR_DEVICE_RESET ||
        status == DXGI_ERROR_DRIVER_INTERNAL_ERROR || status == DXGI_ERROR_DEVICE_HUNG;
}

std::string adapter_id(const DXGI_ADAPTER_DESC1& descriptor) {
    std::ostringstream output;
    output << "luid:" << std::hex << static_cast<std::uint32_t>(descriptor.AdapterLuid.HighPart)
           << ':' << std::hex << descriptor.AdapterLuid.LowPart;
    return output.str();
}

std::string wide_name(const wchar_t* value) {
    if (value == nullptr) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
    if (written <= 1) return {};
    result.resize(static_cast<std::size_t>(written - 1));
    return result;
}

D3D12_HEAP_TYPE heap_type(gpu::MemoryClass memory) {
    switch (memory) {
    case gpu::MemoryClass::upload: return D3D12_HEAP_TYPE_UPLOAD;
    case gpu::MemoryClass::readback: return D3D12_HEAP_TYPE_READBACK;
    case gpu::MemoryClass::device_local: return D3D12_HEAP_TYPE_DEFAULT;
    }
    return D3D12_HEAP_TYPE_DEFAULT;
}

D3D12_RESOURCE_STATES initial_state(gpu::MemoryClass memory) {
    switch (memory) {
    case gpu::MemoryClass::upload: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case gpu::MemoryClass::readback: return D3D12_RESOURCE_STATE_COPY_DEST;
    case gpu::MemoryClass::device_local: return D3D12_RESOURCE_STATE_COMMON;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

DXGI_FORMAT format(gpu::Format value) {
    switch (value) {
    case gpu::Format::rgba8_unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case gpu::Format::rgba8_srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case gpu::Format::bgra8_unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case gpu::Format::bgra8_srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case gpu::Format::rgba16_float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case gpu::Format::rgba32_float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case gpu::Format::rg16_float: return DXGI_FORMAT_R16G16_FLOAT;
    case gpu::Format::rg32_float: return DXGI_FORMAT_R32G32_FLOAT;
    case gpu::Format::r16_float: return DXGI_FORMAT_R16_FLOAT;
    case gpu::Format::r32_float: return DXGI_FORMAT_R32_FLOAT;
    case gpu::Format::r32_uint: return DXGI_FORMAT_R32_UINT;
    case gpu::Format::depth32_float: return DXGI_FORMAT_D32_FLOAT;
    case gpu::Format::depth24_stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case gpu::Format::unknown: return DXGI_FORMAT_UNKNOWN;
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT vertex_format(gpu::VertexFormat value) {
    switch (value) {
    case gpu::VertexFormat::float1: return DXGI_FORMAT_R32_FLOAT;
    case gpu::VertexFormat::float2: return DXGI_FORMAT_R32G32_FLOAT;
    case gpu::VertexFormat::float3: return DXGI_FORMAT_R32G32B32_FLOAT;
    case gpu::VertexFormat::float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case gpu::VertexFormat::uint8x4_unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case gpu::VertexFormat::uint8x4_snorm: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case gpu::VertexFormat::uint16x2: return DXGI_FORMAT_R16G16_UINT;
    case gpu::VertexFormat::uint16x4: return DXGI_FORMAT_R16G16B16A16_UINT;
    case gpu::VertexFormat::uint32: return DXGI_FORMAT_R32_UINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

D3D12_RESOURCE_STATES native_state(device_ir::ResourceState state) {
    switch (state) {
    case device_ir::ResourceState::undefined: return D3D12_RESOURCE_STATE_COMMON;
    case device_ir::ResourceState::copy_source: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case device_ir::ResourceState::copy_destination: return D3D12_RESOURCE_STATE_COPY_DEST;
    case device_ir::ResourceState::vertex_read:
        return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    case device_ir::ResourceState::index_read: return D3D12_RESOURCE_STATE_INDEX_BUFFER;
    case device_ir::ResourceState::constant_read:
        return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    case device_ir::ResourceState::shader_read:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case device_ir::ResourceState::shader_write: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case device_ir::ResourceState::render_target: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case device_ir::ResourceState::depth_write: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case device_ir::ResourceState::depth_read: return D3D12_RESOURCE_STATE_DEPTH_READ;
    case device_ir::ResourceState::present: return D3D12_RESOURCE_STATE_PRESENT;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

D3D12_TEXTURE_ADDRESS_MODE native_address_mode(gpu::AddressMode mode) {
    switch (mode) {
    case gpu::AddressMode::repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case gpu::AddressMode::mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case gpu::AddressMode::clamp_edge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case gpu::AddressMode::clamp_border: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
}

D3D12_FILTER native_filter(const gpu::SamplerDesc& descriptor) {
    if (descriptor.anisotropy) {
        return descriptor.compare
            ? D3D12_FILTER_COMPARISON_ANISOTROPIC
            : D3D12_FILTER_ANISOTROPIC;
    }
    const bool linear = descriptor.min_filter == gpu::Filter::linear ||
        descriptor.mag_filter == gpu::Filter::linear ||
        descriptor.mip_filter == gpu::MipFilter::linear;
    if (descriptor.compare) {
        return linear ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR
                      : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    }
    return linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                  : D3D12_FILTER_MIN_MAG_MIP_POINT;
}

D3D12_COMPARISON_FUNC native_compare(gpu::CompareOp operation) {
    switch (operation) {
    case gpu::CompareOp::never: return D3D12_COMPARISON_FUNC_NEVER;
    case gpu::CompareOp::less: return D3D12_COMPARISON_FUNC_LESS;
    case gpu::CompareOp::equal: return D3D12_COMPARISON_FUNC_EQUAL;
    case gpu::CompareOp::less_or_equal: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case gpu::CompareOp::greater: return D3D12_COMPARISON_FUNC_GREATER;
    case gpu::CompareOp::not_equal: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case gpu::CompareOp::greater_or_equal: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case gpu::CompareOp::always: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

#if CARTO_D3D12_HAS_DXC
const GUID kIidDxcUtils = {
    0x4605c4cb, 0x2019, 0x492a, {0xad, 0xa4, 0x65, 0xf2, 0x0b, 0xb7, 0xd6, 0x7f}};
const GUID kIidDxcCompiler3 = {
    0x228b4687, 0x5a6a, 0x4730, {0x90, 0x0c, 0x97, 0x02, 0xb2, 0x20, 0x3f, 0x54}};
const GUID kIidDxcResult = {
    0x58346cda, 0xdde7, 0x4497, {0x94, 0x61, 0x6f, 0x87, 0xaf, 0x5e, 0x06, 0x59}};

std::wstring environment_value(const wchar_t* name) {
    std::vector<wchar_t> buffer(256U, L'\0');
    for (;;) {
        const DWORD length = GetEnvironmentVariableW(name, buffer.data(),
                                                      static_cast<DWORD>(buffer.size()));
        if (length == 0U) return {};
        if (length < buffer.size() - 1U) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2U);
    }
}

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), length) != length) {
        return {};
    }
    return result;
}

HMODULE load_dxc_module() {
    std::vector<std::filesystem::path> candidates;
    std::vector<std::filesystem::path> versioned_candidates;
    const std::wstring configured = environment_value(L"CARTO_DXCOMPILER_PATH");
    if (!configured.empty()) candidates.emplace_back(configured);
    candidates.emplace_back(L"dxcompiler.dll");

    const std::wstring sdk_root = environment_value(L"WindowsSdkDir");
    if (!sdk_root.empty()) {
        const std::filesystem::path bin_root = std::filesystem::path(sdk_root) / L"bin";
        std::error_code error;
        if (std::filesystem::exists(bin_root, error)) {
            for (const auto& version : std::filesystem::directory_iterator(bin_root, error)) {
                if (error) break;
                versioned_candidates.push_back(version.path() / L"x64" / L"dxcompiler.dll");
            }
        }
    }
    const std::filesystem::path installed_root =
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin";
    std::error_code error;
    if (std::filesystem::exists(installed_root, error)) {
        for (const auto& version : std::filesystem::directory_iterator(installed_root, error)) {
            if (error) break;
            versioned_candidates.push_back(version.path() / L"x64" / L"dxcompiler.dll");
        }
    }
    // Directory enumeration order is unspecified. Prefer the newest installed
    // SDK runtime so the produced DXIL matches the current Windows validator;
    // an explicit CARTO_DXCOMPILER_PATH still wins above this list.
    std::sort(versioned_candidates.begin(), versioned_candidates.end(),
              [](const auto& left, const auto& right) {
                  return left.wstring() > right.wstring();
              });
    candidates.insert(candidates.end(), versioned_candidates.begin(), versioned_candidates.end());
    for (const auto& candidate : candidates) {
        HMODULE module = LoadLibraryW(candidate.c_str());
        if (module != nullptr) return module;
    }
    return nullptr;
}

struct DxcModuleGuard {
    HMODULE module = nullptr;
    explicit DxcModuleGuard(HMODULE value) : module(value) {}
    ~DxcModuleGuard() {
        if (module != nullptr) FreeLibrary(module);
    }
    DxcModuleGuard(const DxcModuleGuard&) = delete;
    DxcModuleGuard& operator=(const DxcModuleGuard&) = delete;
};

std::string dxc_error_text(IDxcResult* result) {
    if (result == nullptr) return {};
    Microsoft::WRL::ComPtr<IDxcBlobEncoding> errors;
    if (FAILED(result->GetErrorBuffer(&errors)) ||
        !errors || errors->GetBufferSize() == 0U) {
        return {};
    }
    return std::string(
        static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
}
#endif

} // namespace

core::Result<device::ShaderBinary> compile_hlsl(const ShaderSource& source) {
    if (source.debug_name.empty() || source.entry_point.empty() || source.source.empty()) {
        return core::Result<device::ShaderBinary>::failure(invalid(
            "HLSL compilation requires a name, entry point, and source"));
    }
    if (source.source.size() > kMaxHlslSourceBytes) {
        return core::Result<device::ShaderBinary>::failure(invalid(
            "HLSL source exceeds the Cartographer compilation limit of 16 MiB"));
    }
    const auto stage_bits = static_cast<std::uint32_t>(source.stage);
    if (stage_bits != static_cast<std::uint32_t>(gpu::ShaderStage::vertex) &&
        stage_bits != static_cast<std::uint32_t>(gpu::ShaderStage::fragment) &&
        stage_bits != static_cast<std::uint32_t>(gpu::ShaderStage::compute)) {
        return core::Result<device::ShaderBinary>::failure(invalid(
            "HLSL compilation requires one vertex, fragment, or compute stage"));
    }
#if !CARTO_D3D12_HAS_DXC
    return core::Result<device::ShaderBinary>::failure(unsupported(
        "Cartographer was built without the Windows SDK DXC headers"));
#else
    DxcModuleGuard module_guard{load_dxc_module()};
    HMODULE module = module_guard.module;
    if (module == nullptr) {
        return core::Result<device::ShaderBinary>::failure(Diagnostic(
            ErrorCode::not_found,
            "dxcompiler.dll was not found; set CARTO_DXCOMPILER_PATH to an approved DXC runtime"));
    }
    const auto create = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(module, "DxcCreateInstance"));
    if (create == nullptr) {
        return core::Result<device::ShaderBinary>::failure(Diagnostic(
            ErrorCode::invalid_state, "dxcompiler.dll does not export DxcCreateInstance"));
    }

    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> compiler;
    HRESULT status = create(
        CLSID_DxcUtils, kIidDxcUtils, reinterpret_cast<void**>(utils.GetAddressOf()));
    if (FAILED(status)) {
        return core::Result<device::ShaderBinary>::failure(
            native_failure("DxcCreateInstance(CLSID_DxcUtils)", status));
    }
    status = create(
        CLSID_DxcCompiler, kIidDxcCompiler3,
        reinterpret_cast<void**>(compiler.GetAddressOf()));
    if (FAILED(status)) {
        return core::Result<device::ShaderBinary>::failure(
            native_failure("DxcCreateInstance(CLSID_DxcCompiler)", status));
    }

    const std::wstring source_name = utf8_to_wide(source.source_name);
    const std::wstring entry_point = utf8_to_wide(source.entry_point);
    const wchar_t* target_profile = nullptr;
    if (source.stage == gpu::ShaderStage::vertex) target_profile = L"vs_6_0";
    else if (source.stage == gpu::ShaderStage::fragment) target_profile = L"ps_6_0";
    else target_profile = L"cs_6_0";
    const wchar_t* arguments[] = {
        L"-E", entry_point.c_str(),
        L"-T", target_profile,
        L"-HV", L"2021",
        L"-Ges",
        source.optimize ? L"-O3" : L"-O0",
        source.debug_info ? L"-Zi" : L"-Qstrip_debug",
    };
    DxcBuffer buffer{
        source.source.data(), source.source.size(), DXC_CP_UTF8};
    Microsoft::WRL::ComPtr<IDxcIncludeHandler> include_handler;
    status = utils->CreateDefaultIncludeHandler(&include_handler);
    if (FAILED(status)) {
        return core::Result<device::ShaderBinary>::failure(
            native_failure("IDxcUtils::CreateDefaultIncludeHandler", status));
    }
    Microsoft::WRL::ComPtr<IDxcResult> result;
    status = compiler->Compile(
        &buffer,
        const_cast<LPCWSTR*>(arguments),
        static_cast<UINT32>(std::size(arguments)),
        include_handler.Get(),
        kIidDxcResult,
        reinterpret_cast<void**>(result.GetAddressOf()));
    if (FAILED(status)) {
        return core::Result<device::ShaderBinary>::failure(
            native_failure("IDxcCompiler3::Compile", status));
    }
    HRESULT compile_status = E_FAIL;
    status = result->GetStatus(&compile_status);
    const std::string diagnostics = dxc_error_text(result.Get());
    if (FAILED(status) || FAILED(compile_status)) {
        std::string message = "DXC shader compilation failed";
        if (!diagnostics.empty()) message += ": " + diagnostics;
        return core::Result<device::ShaderBinary>::failure(
            Diagnostic(ErrorCode::validation_failed, std::move(message)));
    }

    Microsoft::WRL::ComPtr<IDxcBlob> object;
    status = result->GetResult(&object);
    if (FAILED(status) || !object || object->GetBufferSize() == 0U) {
        return core::Result<device::ShaderBinary>::failure(
            native_failure("IDxcOperationResult::GetResult",
                           FAILED(status) ? status : E_FAIL));
    }
    const auto* bytes = static_cast<const std::uint8_t*>(object->GetBufferPointer());
    std::vector<std::uint8_t> bytecode(bytes, bytes + object->GetBufferSize());
    const auto* source_bytes = reinterpret_cast<const std::uint8_t*>(source.source.data());
    const std::string source_digest = assets::sha256(
        std::span<const std::uint8_t>{source_bytes, source.source.size()}).hex();
    const std::string binary_digest = assets::sha256(bytecode).hex();
    const std::string compiler_recipe =
        "dxcompiler-runtime|profile=" + wide_name(target_profile) +
        "|entry=" + source.entry_point +
        "|optimization=" + (source.optimize ? "O3" : "O0") +
        "|debug=" + (source.debug_info ? "Zi" : "Qstrip_debug");
    const std::string compiler_digest = assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(compiler_recipe.data()),
            compiler_recipe.size()}).hex();
    return core::Result<device::ShaderBinary>::success(device::ShaderBinary{
        source.debug_name,
        source.stage,
        source.entry_point,
        device::BinaryFormat::dxil,
        source_digest,
        compiler_digest,
        std::move(bytecode),
        binary_digest,
    });
#endif
}

struct D3D12Device::Impl {
    struct DescriptorLease {
        std::vector<UINT>* free_indices = nullptr;
        UINT index = 0U;

        DescriptorLease(std::vector<UINT>* free_list, UINT descriptor_index)
            : free_indices(free_list), index(descriptor_index) {}

        DescriptorLease(const DescriptorLease&) = delete;
        DescriptorLease& operator=(const DescriptorLease&) = delete;

        ~DescriptorLease() {
            if (free_indices != nullptr) free_indices->push_back(index);
        }
    };

    struct BufferResource {
        gpu::BufferDesc descriptor;
        ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES native_state = D3D12_RESOURCE_STATE_COMMON;
    };

    struct TextureResource {
        gpu::TextureDesc descriptor;
        ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES native_state = D3D12_RESOURCE_STATE_COMMON;
        D3D12_CPU_DESCRIPTOR_HANDLE srv{};
        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE uav{};
        D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu{};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        std::shared_ptr<DescriptorLease> srv_lease;
        std::shared_ptr<DescriptorLease> uav_lease;
        std::shared_ptr<DescriptorLease> rtv_lease;
        std::shared_ptr<DescriptorLease> dsv_lease;
        bool has_srv = false;
        bool has_uav = false;
        bool has_rtv = false;
        bool has_dsv = false;
    };

    struct ShaderResource {
        device::ShaderBinary binary;
        gpu::ShaderDesc descriptor;
    };

    struct PipelineResource {
        gpu::PipelineDesc descriptor;
        ComPtr<ID3D12RootSignature> root_signature;
        ComPtr<ID3D12PipelineState> pipeline_state;
        std::optional<UINT> push_constant_root_index;
        std::optional<UINT> sampled_texture_root_index;
        std::vector<UINT> sampled_texture_root_indices;
        std::optional<UINT> storage_texture_root_index;
        std::optional<UINT> sampler_root_index;
        bool compute = false;
    };

    struct SamplerResource {
        gpu::SamplerDesc descriptor;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        std::shared_ptr<DescriptorLease> lease;
        bool has_descriptor = false;
    };

    struct QueueResource {
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command_list;
        ComPtr<ID3D12Fence> fence;
        HANDLE event = nullptr;
        gpu::SubmissionSerial last_submitted = 0U;

        QueueResource() = default;
        ~QueueResource() {
            if (event != nullptr) CloseHandle(event);
        }

        QueueResource(const QueueResource&) = delete;
        QueueResource& operator=(const QueueResource&) = delete;
    };

    D3D12DeviceOptions options;
    device::DeviceIdentity identity;
    gpu::DeviceCapabilities capabilities;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12InfoQueue> info_queue;
    std::map<gpu::QueueType, QueueResource> queues;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12DescriptorHeap> sampler_heap;
    UINT rtv_increment = 0U;
    UINT dsv_increment = 0U;
    UINT srv_increment = 0U;
    UINT sampler_increment = 0U;
    UINT next_rtv = 0U;
    UINT next_dsv = 0U;
    UINT next_srv = 0U;
    UINT next_sampler = 0U;
    static constexpr UINT descriptor_capacity = 4096U;
    // D3D12 caps shader-visible sampler heaps at 2048 descriptors, while the
    // CBV/SRV/UAV heap has a separate, larger limit. Keep the capacities
    // distinct so device creation does not request an invalid native heap.
    static constexpr UINT sampler_descriptor_capacity = 2048U;
    std::vector<UINT> free_rtv;
    std::vector<UINT> free_dsv;
    std::vector<UINT> free_srv;
    std::vector<UINT> free_sampler;
    gpu::Registry<gpu::BufferTag, BufferResource> buffers;
    gpu::Registry<gpu::TextureTag, TextureResource> textures;
    gpu::Registry<gpu::SamplerTag, SamplerResource> samplers;
    gpu::Registry<gpu::ShaderTag, ShaderResource> shaders;
    gpu::Registry<gpu::PipelineTag, PipelineResource> pipelines;
    std::map<device_ir::ResourceHandle, device_ir::ResourceState> resource_states;
    gpu::SubmissionSerial next_serial = 0U;
    mutable gpu::SubmissionSerial completed = 0U;
    mutable std::map<gpu::SubmissionSerial, gpu::QueueType> pending;

    mutable gpu::DeferredDestructionQueue deferred_destruction;
    mutable std::vector<std::string> debug_receipts;
    mutable bool device_lost = false;

    [[nodiscard]] core::Result<std::shared_ptr<DescriptorLease>> acquire_descriptor(
        std::vector<UINT>& free_indices,
        UINT& next_index,
        UINT capacity,
        const char* label) {
        UINT index = 0U;
        if (!free_indices.empty()) {
            index = free_indices.back();
            free_indices.pop_back();
        } else {
            if (next_index >= capacity) {
                return core::Result<std::shared_ptr<DescriptorLease>>::failure(invalid(
                    std::string("D3D12 ") + label + " descriptor heap is exhausted"));
            }
            index = next_index++;
        }
        return core::Result<std::shared_ptr<DescriptorLease>>::success(
            std::make_shared<DescriptorLease>(&free_indices, index));
    }

    void capture_debug_messages() const {
        if (!info_queue) return;
        const UINT64 count = info_queue->GetNumStoredMessages();
        constexpr UINT64 maximum_receipts = 256U;
        const UINT64 first_index = count > maximum_receipts ? count - maximum_receipts : 0U;
        for (UINT64 index = first_index; index < count; ++index) {
            SIZE_T bytes = 0U;
            if (FAILED(info_queue->GetMessage(index, nullptr, &bytes)) || bytes == 0U) continue;
            if (bytes > kMaxDebugMessageBytes) continue;
            std::vector<std::uint8_t> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(info_queue->GetMessage(index, message, &bytes)) ||
                message->pDescription == nullptr) {
                continue;
            }
            debug_receipts.emplace_back(message->pDescription);
        }
        if (count != 0U) info_queue->ClearStoredMessages();
        if (debug_receipts.size() > maximum_receipts) {
            debug_receipts.erase(
                debug_receipts.begin(),
                debug_receipts.end() - static_cast<std::ptrdiff_t>(maximum_receipts));
        }
    }

    void record_device_lost(const char* operation, HRESULT reason) const {
        device_lost = true;
        std::ostringstream receipt;
        receipt << operation << " device removed: HRESULT 0x" << std::hex
                << static_cast<unsigned long>(reason);
        debug_receipts.push_back(receipt.str());
        capture_debug_messages();
    }

    [[nodiscard]] gpu::SubmissionSerial retirement_serial() const noexcept {
        return pending.empty() ? completed : pending.rbegin()->first;
    }
};

core::Result<std::unique_ptr<D3D12Device>> D3D12Device::create(D3D12DeviceOptions options) {
    auto impl = std::make_unique<Impl>();
    impl->options = std::move(options);

    bool debug_layer_enabled = false;
    if (impl->options.enable_debug_layer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug_layer_enabled = true;
        }
    }

    const UINT factory_flags = debug_layer_enabled ? DXGI_CREATE_FACTORY_DEBUG : 0U;
    HRESULT status = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&impl->factory));
    if (FAILED(status)) {
        return core::Result<std::unique_ptr<D3D12Device>>::failure(
            native_failure("CreateDXGIFactory2", status));
    }

    for (UINT index = 0U;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        status = impl->factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
        if (status == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(status)) {
            return core::Result<std::unique_ptr<D3D12Device>>::failure(
                native_failure("EnumAdapterByGpuPreference", status));
        }

        DXGI_ADAPTER_DESC1 descriptor{};
        if (FAILED(candidate->GetDesc1(&descriptor))) continue;
        if ((descriptor.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U &&
            !impl->options.allow_software_adapter) {
            continue;
        }
        if (!impl->options.adapter_id.empty() && adapter_id(descriptor) != impl->options.adapter_id) {
            continue;
        }
        if (FAILED(D3D12CreateDevice(
                candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&impl->device)))) {
            continue;
        }
        impl->adapter = std::move(candidate);
        impl->identity.adapter_name = wide_name(descriptor.Description);
        impl->identity.vendor_id = descriptor.VendorId;
        impl->identity.device_id = descriptor.DeviceId;
        impl->identity.stable_adapter_id = adapter_id(descriptor);
        impl->identity.local_memory_budget = descriptor.DedicatedVideoMemory;
        impl->identity.feature_level = "D3D_FEATURE_LEVEL_11_0";
        impl->identity.driver = debug_layer_enabled ? "d3d12-debug-layer" : "d3d12";
        break;
    }
    if (!impl->device) {
        return core::Result<std::unique_ptr<D3D12Device>>::failure(Diagnostic(
            ErrorCode::not_found,
            "no D3D12 adapter matched the explicit selection and software policy"));
    }
    static_cast<void>(impl->device.As(&impl->info_queue));

    const std::array<std::pair<gpu::QueueType, D3D12_COMMAND_LIST_TYPE>, 3U> queue_types = {{
        {gpu::QueueType::graphics, D3D12_COMMAND_LIST_TYPE_DIRECT},
        {gpu::QueueType::compute, D3D12_COMMAND_LIST_TYPE_COMPUTE},
        {gpu::QueueType::copy, D3D12_COMMAND_LIST_TYPE_COPY},
    }};
    for (const auto [queue_type, native_type] : queue_types) {
        D3D12_COMMAND_QUEUE_DESC descriptor{};
        descriptor.Type = native_type;
        descriptor.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        descriptor.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        status = impl->device->CreateCommandQueue(
            &descriptor, IID_PPV_ARGS(&impl->queues[queue_type].queue));
        if (FAILED(status)) {
            return core::Result<std::unique_ptr<D3D12Device>>::failure(
                native_failure("ID3D12Device::CreateCommandQueue", status));
        }
        auto& queue = impl->queues.at(queue_type);
        status = impl->device->CreateCommandAllocator(
            native_type, IID_PPV_ARGS(&queue.allocator));
        if (FAILED(status)) {
            return core::Result<std::unique_ptr<D3D12Device>>::failure(
                native_failure("ID3D12Device::CreateCommandAllocator", status));
        }
        status = impl->device->CreateCommandList(
            0U,
            native_type,
            queue.allocator.Get(),
            nullptr,
            IID_PPV_ARGS(&queue.command_list));
        if (FAILED(status) || FAILED(queue.command_list->Close())) {
            return core::Result<std::unique_ptr<D3D12Device>>::failure(
                native_failure("ID3D12Device::CreateCommandList", FAILED(status) ? status : E_FAIL));
        }
        status = impl->device->CreateFence(
            0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue.fence));
        if (FAILED(status)) {
            return core::Result<std::unique_ptr<D3D12Device>>::failure(
                native_failure("ID3D12Device::CreateFence", status));
        }
        queue.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (queue.event == nullptr) {
            return core::Result<std::unique_ptr<D3D12Device>>::failure(
                Diagnostic(ErrorCode::invalid_state, "CreateEventW failed for D3D12 fence"));
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_descriptor{};
    rtv_heap_descriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_descriptor.NumDescriptors = 4096U;
    status = impl->device->CreateDescriptorHeap(
        &rtv_heap_descriptor, IID_PPV_ARGS(&impl->rtv_heap));
    if (FAILED(status)) {
        return core::Result<std::unique_ptr<D3D12Device>>::failure(
            native_failure("CreateDescriptorHeap(RTV)", status));
    }
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_descriptor{};
    dsv_heap_descriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_descriptor.NumDescriptors = 4096U;
    status = impl->device->CreateDescriptorHeap(
        &dsv_heap_descriptor, IID_PPV_ARGS(&impl->dsv_heap));
    if (FAILED(status)) {
        return core::Result<std::unique_ptr<D3D12Device>>::failure(
            native_failure("CreateDescriptorHeap(DSV)", status));
    }
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_descriptor{};
    srv_heap_descriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_descriptor.NumDescriptors = Impl::descriptor_capacity;
    srv_heap_descriptor.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    status = impl->device->CreateDescriptorHeap(
        &srv_heap_descriptor, IID_PPV_ARGS(&impl->srv_heap));
    if (FAILED(status)) {
        return core::Result<std::unique_ptr<D3D12Device>>::failure(
            native_failure("CreateDescriptorHeap(CBV_SRV_UAV)", status));
    }
    D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_descriptor{};
    sampler_heap_descriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_heap_descriptor.NumDescriptors = Impl::sampler_descriptor_capacity;
    sampler_heap_descriptor.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    status = impl->device->CreateDescriptorHeap(
        &sampler_heap_descriptor, IID_PPV_ARGS(&impl->sampler_heap));
    if (FAILED(status)) {
        return core::Result<std::unique_ptr<D3D12Device>>::failure(
            native_failure("CreateDescriptorHeap(SAMPLER)", status));
    }
    impl->free_rtv.reserve(Impl::descriptor_capacity);
    impl->free_dsv.reserve(Impl::descriptor_capacity);
    impl->free_srv.reserve(Impl::descriptor_capacity);
    impl->free_sampler.reserve(Impl::sampler_descriptor_capacity);
    impl->rtv_increment = impl->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    impl->dsv_increment = impl->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    impl->srv_increment = impl->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    impl->sampler_increment = impl->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    impl->identity.backend = device::BackendKind::d3d12;
    ComPtr<IDXGIAdapter3> adapter3;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory_info{};
    if (SUCCEEDED(impl->adapter.As(&adapter3)) &&
        SUCCEEDED(adapter3->QueryVideoMemoryInfo(
            0U, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory_info))) {
        impl->identity.local_memory_budget = memory_info.Budget;
    }
    impl->capabilities.graphics_queue = true;
    impl->capabilities.compute_queue = true;
    impl->capabilities.copy_queue = true;
    impl->capabilities.presentation = true;
    impl->capabilities.anisotropy = true;
    impl->capabilities.sampler_compare = true;
    impl->capabilities.max_texture_dimension_2d = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    impl->capabilities.max_sampler_anisotropy =
        static_cast<float>(D3D12_MAX_MAXANISOTROPY);
    impl->capabilities.max_color_attachments = 8U;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fp16_support{};
    fp16_support.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (SUCCEEDED(impl->device->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT,
            &fp16_support,
            sizeof(fp16_support)))) {
        impl->capabilities.fp16_color_attachment =
            (static_cast<std::uint32_t>(fp16_support.Support1) &
             static_cast<std::uint32_t>(D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) != 0U;
        impl->capabilities.fp16_storage =
            (static_cast<std::uint32_t>(fp16_support.Support2) &
             static_cast<std::uint32_t>(D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) != 0U;
    }
    if (auto result = gpu::validate(impl->capabilities); !result) {
        return core::Result<std::unique_ptr<D3D12Device>>::failure(result.error());
    }
    return core::Result<std::unique_ptr<D3D12Device>>::success(
        std::unique_ptr<D3D12Device>(new D3D12Device(std::move(impl))));
}

D3D12Device::D3D12Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
D3D12Device::~D3D12Device() {
    // A normal shutdown must not release resources while a queue can still
    // reference them. Device removal is already terminal, so there is no
    // meaningful fence wait in that case.
    if (impl_ != nullptr && !impl_->device_lost) {
        static_cast<void>(wait_idle());
    }
}

device::BackendKind D3D12Device::backend() const noexcept { return device::BackendKind::d3d12; }
const device::DeviceIdentity& D3D12Device::identity() const noexcept { return impl_->identity; }
const gpu::DeviceCapabilities& D3D12Device::capabilities() const noexcept {
    return impl_->capabilities;
}

core::Result<gpu::BufferHandle> D3D12Device::create_buffer(const gpu::BufferDesc& descriptor) {
    if (impl_->device_lost) {
        return core::Result<gpu::BufferHandle>::failure(device_lost_failure());
    }
    if (auto result = gpu::validate(descriptor); !result) {
        return core::Result<gpu::BufferHandle>::failure(result.error());
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heap_type(descriptor.memory);
    heap.CreationNodeMask = 1U;
    heap.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = descriptor.bytes;
    resource.Height = 1U;
    resource.DepthOrArraySize = 1U;
    resource.MipLevels = 1U;
    resource.Format = DXGI_FORMAT_UNKNOWN;
    resource.SampleDesc.Count = 1U;
    resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Impl::BufferResource value{};
    value.descriptor = descriptor;
    value.native_state = initial_state(descriptor.memory);
    const HRESULT status = impl_->device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &resource,
        initial_state(descriptor.memory),
        nullptr,
        IID_PPV_ARGS(&value.resource));
    if (FAILED(status)) {
        return core::Result<gpu::BufferHandle>::failure(
            native_failure("CreateCommittedResource(buffer)", status));
    }
    auto handle = impl_->buffers.insert(std::move(value));
    if (handle) impl_->resource_states.emplace(
        device_ir::ResourceHandle{handle.value()}, device_ir::ResourceState::undefined);
    return handle;
}

core::Result<gpu::TextureHandle> D3D12Device::create_texture(const gpu::TextureDesc& descriptor) {
    if (impl_->device_lost) {
        return core::Result<gpu::TextureHandle>::failure(device_lost_failure());
    }
    if (auto result = gpu::validate(descriptor); !result) {
        return core::Result<gpu::TextureHandle>::failure(result.error());
    }
    if (descriptor.dimension != gpu::TextureDimension::texture_2d || descriptor.depth != 1U) {
        return core::Result<gpu::TextureHandle>::failure(unsupported(
            "D3D12 reference tranche supports only 2D textures"));
    }
    if (descriptor.layers > std::numeric_limits<UINT16>::max() ||
        descriptor.mip_levels > std::numeric_limits<UINT16>::max()) {
        return core::Result<gpu::TextureHandle>::failure(invalid(
            "D3D12 texture array and mip counts exceed the native descriptor bounds"));
    }
    const DXGI_FORMAT native_format = format(descriptor.format);
    if (native_format == DXGI_FORMAT_UNKNOWN) {
        return core::Result<gpu::TextureHandle>::failure(invalid(
            "D3D12 texture format is not mapped"));
    }
    std::shared_ptr<Impl::DescriptorLease> srv_lease;
    std::shared_ptr<Impl::DescriptorLease> rtv_lease;
    std::shared_ptr<Impl::DescriptorLease> dsv_lease;
    if (gpu::has_usage(descriptor.usage, gpu::TextureUsage::sampled)) {
        if (gpu::is_depth_format(descriptor.format)) {
            return core::Result<gpu::TextureHandle>::failure(unsupported(
                "D3D12 sampled textures currently require a color format"));
        }
        auto lease = impl_->acquire_descriptor(
            impl_->free_srv, impl_->next_srv, Impl::descriptor_capacity, "SRV");
        if (!lease) return core::Result<gpu::TextureHandle>::failure(lease.error());
        srv_lease = std::move(lease.value());
    }
    std::shared_ptr<Impl::DescriptorLease> uav_lease;
    if (gpu::has_usage(descriptor.usage, gpu::TextureUsage::storage)) {
        if (gpu::is_depth_format(descriptor.format)) {
            return core::Result<gpu::TextureHandle>::failure(unsupported(
                "D3D12 storage textures currently require a color format"));
        }
        auto lease = impl_->acquire_descriptor(
            impl_->free_srv, impl_->next_srv, Impl::descriptor_capacity, "UAV");
        if (!lease) return core::Result<gpu::TextureHandle>::failure(lease.error());
        uav_lease = std::move(lease.value());
    }
    if (gpu::has_usage(descriptor.usage, gpu::TextureUsage::color_attachment)) {
        auto lease = impl_->acquire_descriptor(
            impl_->free_rtv, impl_->next_rtv, Impl::descriptor_capacity, "RTV");
        if (!lease) return core::Result<gpu::TextureHandle>::failure(lease.error());
        rtv_lease = std::move(lease.value());
    }
    if (gpu::has_usage(descriptor.usage, gpu::TextureUsage::depth_attachment)) {
        auto lease = impl_->acquire_descriptor(
            impl_->free_dsv, impl_->next_dsv, Impl::descriptor_capacity, "DSV");
        if (!lease) return core::Result<gpu::TextureHandle>::failure(lease.error());
        dsv_lease = std::move(lease.value());
    }
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (gpu::has_usage(descriptor.usage, gpu::TextureUsage::color_attachment)) {
        flags = flags | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (gpu::has_usage(descriptor.usage, gpu::TextureUsage::depth_attachment)) {
        flags = flags | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    if (gpu::has_usage(descriptor.usage, gpu::TextureUsage::storage)) {
        flags = flags | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1U;
    heap.VisibleNodeMask = 1U;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = descriptor.width;
    resource.Height = descriptor.height;
    resource.DepthOrArraySize = static_cast<UINT16>(descriptor.layers);
    resource.MipLevels = static_cast<UINT16>(descriptor.mip_levels);
    resource.Format = native_format;
    resource.SampleDesc.Count = 1U;
    resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource.Flags = flags;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = native_format;
    if (gpu::is_depth_format(descriptor.format)) {
        clear.DepthStencil.Depth = descriptor.clear_depth;
    } else {
        std::copy(
            descriptor.clear_color.begin(),
            descriptor.clear_color.end(),
            clear.Color);
    }
    Impl::TextureResource value{};
    value.descriptor = descriptor;
    value.native_state = D3D12_RESOURCE_STATE_COMMON;
    value.srv_lease = std::move(srv_lease);
    value.uav_lease = std::move(uav_lease);
    value.rtv_lease = std::move(rtv_lease);
    value.dsv_lease = std::move(dsv_lease);
    const bool has_clear_value =
        gpu::has_usage(descriptor.usage, gpu::TextureUsage::color_attachment) ||
        gpu::has_usage(descriptor.usage, gpu::TextureUsage::depth_attachment);
    const HRESULT status = impl_->device->CreateCommittedResource(
        &heap,
        D3D12_HEAP_FLAG_NONE,
        &resource,
        D3D12_RESOURCE_STATE_COMMON,
        has_clear_value ? &clear : nullptr,
        IID_PPV_ARGS(&value.resource));
    if (FAILED(status)) {
        return core::Result<gpu::TextureHandle>::failure(
            native_failure("CreateCommittedResource(texture)", status));
    }
    if (gpu::has_usage(descriptor.usage, gpu::TextureUsage::sampled)) {
        const auto cpu_start = impl_->srv_heap->GetCPUDescriptorHandleForHeapStart();
        const auto gpu_start = impl_->srv_heap->GetGPUDescriptorHandleForHeapStart();
        value.srv.ptr = cpu_start.ptr +
            static_cast<SIZE_T>(value.srv_lease->index) * impl_->srv_increment;
        value.srv_gpu.ptr = gpu_start.ptr +
            static_cast<UINT64>(value.srv_lease->index) * impl_->srv_increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = native_format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MostDetailedMip = 0U;
        view.Texture2D.MipLevels = descriptor.mip_levels;
        impl_->device->CreateShaderResourceView(value.resource.Get(), &view, value.srv);
        value.has_srv = true;
    }
    if (gpu::has_usage(descriptor.usage, gpu::TextureUsage::storage)) {
        const auto cpu_start = impl_->srv_heap->GetCPUDescriptorHandleForHeapStart();
        const auto gpu_start = impl_->srv_heap->GetGPUDescriptorHandleForHeapStart();
        value.uav.ptr = cpu_start.ptr +
            static_cast<SIZE_T>(value.uav_lease->index) * impl_->srv_increment;
        value.uav_gpu.ptr = gpu_start.ptr +
            static_cast<UINT64>(value.uav_lease->index) * impl_->srv_increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = native_format;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = 0U;
        impl_->device->CreateUnorderedAccessView(
            value.resource.Get(), nullptr, &view, value.uav);
        value.has_uav = true;
    }
    if (gpu::has_usage(descriptor.usage, gpu::TextureUsage::color_attachment)) {
        const auto start = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
        value.rtv.ptr = start.ptr +
            static_cast<SIZE_T>(value.rtv_lease->index) * impl_->rtv_increment;
        impl_->device->CreateRenderTargetView(value.resource.Get(), nullptr, value.rtv);
        value.has_rtv = true;
    }
    if (gpu::has_usage(descriptor.usage, gpu::TextureUsage::depth_attachment)) {
        const auto start = impl_->dsv_heap->GetCPUDescriptorHandleForHeapStart();
        value.dsv.ptr = start.ptr +
            static_cast<SIZE_T>(value.dsv_lease->index) * impl_->dsv_increment;
        impl_->device->CreateDepthStencilView(value.resource.Get(), nullptr, value.dsv);
        value.has_dsv = true;
    }
    auto handle = impl_->textures.insert(std::move(value));
    if (handle) impl_->resource_states.emplace(
        device_ir::ResourceHandle{handle.value()}, device_ir::ResourceState::undefined);
    return handle;
}

core::Result<gpu::TextureHandle> D3D12Device::import_swapchain_texture(
    void* native_resource,
    const gpu::TextureDesc& descriptor) {
    if (impl_->device_lost) {
        return core::Result<gpu::TextureHandle>::failure(device_lost_failure());
    }
    if (native_resource == nullptr) {
        return core::Result<gpu::TextureHandle>::failure(invalid(
            "D3D12 swapchain texture import requires a native resource"));
    }
    if (auto result = gpu::validate(descriptor); !result) {
        return core::Result<gpu::TextureHandle>::failure(result.error());
    }
    if (descriptor.dimension != gpu::TextureDimension::texture_2d ||
        descriptor.depth != 1U || descriptor.layers != 1U || descriptor.mip_levels != 1U ||
        !gpu::has_usage(descriptor.usage, gpu::TextureUsage::color_attachment) ||
        gpu::is_depth_format(descriptor.format)) {
        return core::Result<gpu::TextureHandle>::failure(invalid(
            "D3D12 swapchain imports require a single-level 2D color attachment"));
    }
    auto* resource = reinterpret_cast<ID3D12Resource*>(native_resource);
    const D3D12_RESOURCE_DESC native_descriptor = resource->GetDesc();
    if (native_descriptor.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        native_descriptor.Width != descriptor.width ||
        native_descriptor.Height != descriptor.height ||
        native_descriptor.DepthOrArraySize != 1U || native_descriptor.MipLevels != 1U ||
        native_descriptor.SampleDesc.Count != 1U ||
        native_descriptor.Format != format(descriptor.format)) {
        return core::Result<gpu::TextureHandle>::failure(validation(
            "D3D12 swapchain resource does not match its Cartographer texture descriptor"));
    }
    auto rtv_lease = impl_->acquire_descriptor(
        impl_->free_rtv, impl_->next_rtv, Impl::descriptor_capacity, "RTV");
    if (!rtv_lease) {
        return core::Result<gpu::TextureHandle>::failure(rtv_lease.error());
    }

    Impl::TextureResource value{};
    value.descriptor = descriptor;
    value.resource = resource;
    value.native_state = D3D12_RESOURCE_STATE_PRESENT;
    value.rtv_lease = std::move(rtv_lease.value());
    const auto start = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    value.rtv.ptr = start.ptr +
        static_cast<SIZE_T>(value.rtv_lease->index) * impl_->rtv_increment;
    impl_->device->CreateRenderTargetView(value.resource.Get(), nullptr, value.rtv);
    value.has_rtv = true;

    auto handle = impl_->textures.insert(std::move(value));
    if (handle) {
        impl_->resource_states.emplace(
            device_ir::ResourceHandle{handle.value()}, device_ir::ResourceState::present);
    }
    return handle;
}

core::Result<gpu::SamplerHandle> D3D12Device::create_sampler(
    const gpu::SamplerDesc& descriptor) {
    if (impl_->device_lost) {
        return core::Result<gpu::SamplerHandle>::failure(device_lost_failure());
    }
    if (auto result = gpu::validate(descriptor); !result) {
        return core::Result<gpu::SamplerHandle>::failure(result.error());
    }
    if (descriptor.anisotropy &&
        (descriptor.max_anisotropy > impl_->capabilities.max_sampler_anisotropy ||
         std::floor(descriptor.max_anisotropy) != descriptor.max_anisotropy)) {
        return core::Result<gpu::SamplerHandle>::failure(invalid(
            "D3D12 sampler anisotropy must be an integer within the device limit"));
    }
    auto lease = impl_->acquire_descriptor(
        impl_->free_sampler,
        impl_->next_sampler,
        Impl::sampler_descriptor_capacity,
        "sampler");
    if (!lease) return core::Result<gpu::SamplerHandle>::failure(lease.error());
    Impl::SamplerResource resource{};
    resource.descriptor = descriptor;
    resource.lease = std::move(lease.value());
    const auto cpu_start = impl_->sampler_heap->GetCPUDescriptorHandleForHeapStart();
    const auto gpu_start = impl_->sampler_heap->GetGPUDescriptorHandleForHeapStart();
    resource.cpu.ptr = cpu_start.ptr +
        static_cast<SIZE_T>(resource.lease->index) * impl_->sampler_increment;
    resource.gpu.ptr = gpu_start.ptr +
        static_cast<UINT64>(resource.lease->index) * impl_->sampler_increment;
    D3D12_SAMPLER_DESC native{};
    native.Filter = native_filter(descriptor);
    native.AddressU = native_address_mode(descriptor.u);
    native.AddressV = native_address_mode(descriptor.v);
    native.AddressW = native_address_mode(descriptor.w);
    native.MipLODBias = 0.0F;
    native.MaxAnisotropy = descriptor.anisotropy
        ? static_cast<UINT>(descriptor.max_anisotropy) : 1U;
    // D3D12 ignores ComparisonFunc for non-comparison filters, but the debug
    // layer still reports ALWAYS as a likely configuration error. NEVER is
    // inert for ordinary sampling and keeps the native descriptor truthful.
    native.ComparisonFunc = descriptor.compare ?
        native_compare(descriptor.compare_op) : D3D12_COMPARISON_FUNC_NEVER;
    native.BorderColor[0] = 0.0F;
    native.BorderColor[1] = 0.0F;
    native.BorderColor[2] = 0.0F;
    native.BorderColor[3] = 0.0F;
    native.MinLOD = descriptor.min_lod;
    native.MaxLOD = descriptor.max_lod;
    impl_->device->CreateSampler(&native, resource.cpu);
    resource.has_descriptor = true;
    return impl_->samplers.insert(std::move(resource));
}

core::Result<gpu::ShaderHandle> D3D12Device::create_shader(
    const device::ShaderBinary& binary) {
    if (impl_->device_lost) {
        return core::Result<gpu::ShaderHandle>::failure(device_lost_failure());
    }
    if (auto result = device::validate(binary); !result) {
        return core::Result<gpu::ShaderHandle>::failure(result.error());
    }
    if (binary.format != device::BinaryFormat::dxil) {
        return core::Result<gpu::ShaderHandle>::failure(unsupported(
            "D3D12 backend requires DXIL shader binaries"));
    }
    gpu::ShaderDesc descriptor{
        binary.debug_name, binary.stage, binary.entry_point, binary.bytes};
    if (auto result = gpu::validate(descriptor); !result) {
        return core::Result<gpu::ShaderHandle>::failure(result.error());
    }
    return impl_->shaders.insert(Impl::ShaderResource{binary, std::move(descriptor)});
}

core::Result<gpu::PipelineHandle> D3D12Device::create_pipeline(
    const gpu::PipelineDesc& descriptor) {
    if (impl_->device_lost) {
        return core::Result<gpu::PipelineHandle>::failure(device_lost_failure());
    }
    if (auto result = gpu::validate(descriptor); !result) {
        return core::Result<gpu::PipelineHandle>::failure(result.error());
    }
    if (descriptor.sample_count != 1U) {
        return core::Result<gpu::PipelineHandle>::failure(unsupported(
            "D3D12 reference pipeline creation currently requires one sample"));
    }
    const bool has_multi_sampled_compute_bindings = descriptor.sampled_texture_count != 0U;
    if ((!has_multi_sampled_compute_bindings && descriptor.descriptor_bindings.size() > 1U) ||
        (has_multi_sampled_compute_bindings &&
         descriptor.descriptor_bindings.size() !=
             static_cast<std::size_t>(descriptor.sampled_texture_count) + 1U)) {
        return core::Result<gpu::PipelineHandle>::failure(unsupported(
            "D3D12 supports one texture binding, or up to three sampled compute bindings plus one storage binding"));
    }
    for (std::size_t binding_index = 0U;
         binding_index < descriptor.descriptor_bindings.size(); ++binding_index) {
        const auto& binding = descriptor.descriptor_bindings.at(binding_index);
        if (binding.set != 0U || binding.binding != binding_index ||
            (!gpu::has_stage(binding.stages, gpu::ShaderStage::fragment) &&
             !gpu::has_stage(binding.stages, gpu::ShaderStage::compute))) {
            return core::Result<gpu::PipelineHandle>::failure(unsupported(
                "D3D12 descriptor bindings must use consecutive set 0 registers"));
        }
    }
    if (descriptor.push_constant_bytes % sizeof(std::uint32_t) != 0U) {
        return core::Result<gpu::PipelineHandle>::failure(invalid(
            "D3D12 root constants must be aligned to four bytes"));
    }

    const Impl::ShaderResource* vertex_shader = nullptr;
    const Impl::ShaderResource* fragment_shader = nullptr;
    const Impl::ShaderResource* compute_shader = nullptr;
    for (const gpu::ShaderHandle shader_handle : descriptor.shaders) {
        auto shader = impl_->shaders.resolve(shader_handle);
        if (!shader) return core::Result<gpu::PipelineHandle>::failure(shader.error());
        if (shader.value()->descriptor.stage == gpu::ShaderStage::vertex) {
            if (vertex_shader != nullptr) {
                return core::Result<gpu::PipelineHandle>::failure(validation(
                    "D3D12 graphics pipelines cannot contain duplicate vertex shaders"));
            }
            vertex_shader = shader.value();
        } else if (shader.value()->descriptor.stage == gpu::ShaderStage::fragment) {
            if (fragment_shader != nullptr) {
                return core::Result<gpu::PipelineHandle>::failure(validation(
                    "D3D12 graphics pipelines cannot contain duplicate fragment shaders"));
            }
            fragment_shader = shader.value();
        } else if (shader.value()->descriptor.stage == gpu::ShaderStage::compute) {
            if (compute_shader != nullptr) {
                return core::Result<gpu::PipelineHandle>::failure(validation(
                    "D3D12 compute pipelines cannot contain duplicate compute shaders"));
            }
            compute_shader = shader.value();
        } else {
            return core::Result<gpu::PipelineHandle>::failure(unsupported(
                "D3D12 pipelines require supported DXIL stages"));
        }
    }

    if (compute_shader != nullptr) {
        if (vertex_shader != nullptr || fragment_shader != nullptr) {
            return core::Result<gpu::PipelineHandle>::failure(validation(
                "D3D12 compute pipelines cannot mix graphics shader stages"));
        }
        if (!descriptor.descriptor_bindings.empty() &&
            !gpu::has_stage(descriptor.descriptor_bindings.front().stages,
                            gpu::ShaderStage::compute)) {
            return core::Result<gpu::PipelineHandle>::failure(validation(
                "D3D12 compute resource bindings must be staged for compute"));
        }
        if (descriptor.sampled_texture_count > 3U) {
            return core::Result<gpu::PipelineHandle>::failure(unsupported(
                "D3D12 compute pipelines support at most three sampled textures"));
        }
        for (const auto& binding : descriptor.descriptor_bindings) {
            if (!gpu::has_stage(binding.stages, gpu::ShaderStage::compute)) {
                return core::Result<gpu::PipelineHandle>::failure(validation(
                    "D3D12 compute resource bindings must all be staged for compute"));
            }
        }

        Impl::PipelineResource value{};
        value.descriptor = descriptor;
        value.compute = true;
        std::array<D3D12_ROOT_PARAMETER, 5U> root_parameters{};
        std::array<D3D12_DESCRIPTOR_RANGE, 4U> descriptor_ranges{};
        UINT root_parameter_count = 0U;
        D3D12_ROOT_SIGNATURE_DESC root_descriptor{};
        if (descriptor.push_constant_bytes != 0U) {
            const UINT root_index = root_parameter_count++;
            value.push_constant_root_index = root_index;
            auto& root_parameter = root_parameters.at(root_index);
            root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            root_parameter.Constants.ShaderRegister = 0U;
            root_parameter.Constants.RegisterSpace = 0U;
            root_parameter.Constants.Num32BitValues =
                descriptor.push_constant_bytes / sizeof(std::uint32_t);
            root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        for (std::uint32_t sampled_index = 0U;
             sampled_index < descriptor.sampled_texture_count; ++sampled_index) {
            auto& sampled_range = descriptor_ranges.at(sampled_index);
            sampled_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            sampled_range.NumDescriptors = 1U;
            sampled_range.BaseShaderRegister = sampled_index;
            sampled_range.RegisterSpace = 0U;
            sampled_range.OffsetInDescriptorsFromTableStart = 0U;
            const UINT sampled_root_index = root_parameter_count++;
            value.sampled_texture_root_indices.push_back(sampled_root_index);
            auto& sampled_parameter = root_parameters.at(sampled_root_index);
            sampled_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            sampled_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            sampled_parameter.DescriptorTable.pDescriptorRanges = &sampled_range;
            sampled_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        if (descriptor.descriptor_bindings.size() > descriptor.sampled_texture_count) {
            auto& storage_range = descriptor_ranges.at(descriptor.sampled_texture_count);
            storage_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            storage_range.NumDescriptors = 1U;
            storage_range.BaseShaderRegister = 0U;
            storage_range.RegisterSpace = 0U;
            storage_range.OffsetInDescriptorsFromTableStart = 0U;
            const UINT storage_root_index = root_parameter_count++;
            value.storage_texture_root_index = storage_root_index;
            auto& storage_parameter = root_parameters.at(storage_root_index);
            storage_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            storage_parameter.DescriptorTable.NumDescriptorRanges = 1U;
            storage_parameter.DescriptorTable.pDescriptorRanges = &storage_range;
            storage_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        root_descriptor.NumParameters = root_parameter_count;
        root_descriptor.pParameters = root_parameter_count == 0U
            ? nullptr : root_parameters.data();
        root_descriptor.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        Microsoft::WRL::ComPtr<ID3DBlob> serialized_root;
        Microsoft::WRL::ComPtr<ID3DBlob> root_errors;
        HRESULT status = D3D12SerializeRootSignature(
            &root_descriptor,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized_root,
            &root_errors);
        if (FAILED(status) || !serialized_root) {
            std::string message = "D3D12 compute root-signature serialization failed";
            if (root_errors && root_errors->GetBufferPointer() != nullptr) {
                message += ": ";
                message.append(
                    static_cast<const char*>(root_errors->GetBufferPointer()),
                    root_errors->GetBufferSize());
            }
            return core::Result<gpu::PipelineHandle>::failure(
                Diagnostic(ErrorCode::validation_failed, std::move(message)));
        }
        status = impl_->device->CreateRootSignature(
            0U,
            serialized_root->GetBufferPointer(),
            serialized_root->GetBufferSize(),
            IID_PPV_ARGS(&value.root_signature));
        if (FAILED(status)) {
            return core::Result<gpu::PipelineHandle>::failure(
                native_failure("ID3D12Device::CreateRootSignature(compute)", status));
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = value.root_signature.Get();
        pipeline.CS = {
            compute_shader->binary.bytes.data(), compute_shader->binary.bytes.size()};
        pipeline.NodeMask = 0U;
        pipeline.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        status = impl_->device->CreateComputePipelineState(
            &pipeline, IID_PPV_ARGS(&value.pipeline_state));
        if (FAILED(status)) {
            impl_->capture_debug_messages();
            return core::Result<gpu::PipelineHandle>::failure(
                native_failure("ID3D12Device::CreateComputePipelineState", status));
        }
        return impl_->pipelines.insert(std::move(value));
    }

    if (vertex_shader == nullptr || fragment_shader == nullptr) {
        return core::Result<gpu::PipelineHandle>::failure(validation(
            "D3D12 graphics pipelines require one vertex and one fragment shader"));
    }
    if (descriptor.sampled_texture_count != 0U) {
        return core::Result<gpu::PipelineHandle>::failure(unsupported(
            "D3D12 multi-texture bindings are limited to compute pipelines"));
    }

    Impl::PipelineResource value{};
    value.descriptor = descriptor;
    std::array<D3D12_ROOT_PARAMETER, 3U> root_parameters{};
    std::array<D3D12_DESCRIPTOR_RANGE, 2U> descriptor_ranges{};
    UINT root_parameter_count = 0U;
    D3D12_ROOT_SIGNATURE_DESC root_descriptor{};
    if (descriptor.push_constant_bytes != 0U) {
        const UINT root_index = root_parameter_count++;
        value.push_constant_root_index = root_index;
        auto& root_parameter = root_parameters.at(root_index);
        root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_parameter.Constants.ShaderRegister = 0U;
        root_parameter.Constants.RegisterSpace = 0U;
        root_parameter.Constants.Num32BitValues =
            descriptor.push_constant_bytes / sizeof(std::uint32_t);
        root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    if (!descriptor.descriptor_bindings.empty()) {
        auto& texture_range = descriptor_ranges.at(0U);
        texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        texture_range.NumDescriptors = 1U;
        texture_range.BaseShaderRegister = 0U;
        texture_range.RegisterSpace = 0U;
        texture_range.OffsetInDescriptorsFromTableStart = 0U;
        const UINT texture_root_index = root_parameter_count++;
        value.sampled_texture_root_index = texture_root_index;
        auto& texture_parameter = root_parameters.at(texture_root_index);
        texture_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        texture_parameter.DescriptorTable.NumDescriptorRanges = 1U;
        texture_parameter.DescriptorTable.pDescriptorRanges = &texture_range;
        texture_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        auto& sampler_range = descriptor_ranges.at(1U);
        sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        sampler_range.NumDescriptors = 1U;
        sampler_range.BaseShaderRegister = 0U;
        sampler_range.RegisterSpace = 0U;
        sampler_range.OffsetInDescriptorsFromTableStart = 0U;
        const UINT sampler_root_index = root_parameter_count++;
        value.sampler_root_index = sampler_root_index;
        auto& sampler_parameter = root_parameters.at(sampler_root_index);
        sampler_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        sampler_parameter.DescriptorTable.NumDescriptorRanges = 1U;
        sampler_parameter.DescriptorTable.pDescriptorRanges = &sampler_range;
        sampler_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    root_descriptor.NumParameters = root_parameter_count;
    root_descriptor.pParameters = root_parameter_count == 0U ? nullptr : root_parameters.data();
    root_descriptor.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized_root;
    Microsoft::WRL::ComPtr<ID3DBlob> root_errors;
    HRESULT status = D3D12SerializeRootSignature(
        &root_descriptor,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized_root,
        &root_errors);
    if (FAILED(status) || !serialized_root) {
        std::string message = "D3D12 root-signature serialization failed";
        if (root_errors && root_errors->GetBufferPointer() != nullptr) {
            message += ": ";
            message.append(
                static_cast<const char*>(root_errors->GetBufferPointer()),
                root_errors->GetBufferSize());
        }
        return core::Result<gpu::PipelineHandle>::failure(
            Diagnostic(ErrorCode::validation_failed, std::move(message)));
    }

    status = impl_->device->CreateRootSignature(
        0U,
        serialized_root->GetBufferPointer(),
        serialized_root->GetBufferSize(),
        IID_PPV_ARGS(&value.root_signature));
    if (FAILED(status)) {
        return core::Result<gpu::PipelineHandle>::failure(
            native_failure("ID3D12Device::CreateRootSignature", status));
    }

    std::vector<std::string> semantic_names;
    std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
    if (descriptor.vertex_attributes.empty()) {
        semantic_names.emplace_back("POSITION");
        input_elements.push_back(D3D12_INPUT_ELEMENT_DESC{
            semantic_names.back().c_str(), 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 0U,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U});
    } else {
        semantic_names.reserve(descriptor.vertex_attributes.size());
        input_elements.reserve(descriptor.vertex_attributes.size());
        for (const auto& attribute : descriptor.vertex_attributes) {
            semantic_names.push_back(attribute.semantic);
            input_elements.push_back(D3D12_INPUT_ELEMENT_DESC{
                semantic_names.back().c_str(),
                attribute.semantic_index,
                vertex_format(attribute.format),
                attribute.input_slot,
                attribute.offset_bytes,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                0U});
        }
    }
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = descriptor.polygon_mode == gpu::PolygonMode::fill
        ? D3D12_FILL_MODE_SOLID : D3D12_FILL_MODE_WIREFRAME;
    switch (descriptor.cull_mode) {
    case gpu::CullMode::none: rasterizer.CullMode = D3D12_CULL_MODE_NONE; break;
    case gpu::CullMode::front: rasterizer.CullMode = D3D12_CULL_MODE_FRONT; break;
    case gpu::CullMode::back: rasterizer.CullMode = D3D12_CULL_MODE_BACK; break;
    case gpu::CullMode::front_and_back:
        return core::Result<gpu::PipelineHandle>::failure(unsupported(
            "D3D12 rasterization rejects front-and-back culling"));
    }
    rasterizer.FrontCounterClockwise =
        descriptor.front_face == gpu::FrontFace::counter_clockwise ? TRUE : FALSE;
    rasterizer.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizer.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizer.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.MultisampleEnable = FALSE;
    rasterizer.AntialiasedLineEnable = FALSE;
    rasterizer.ForcedSampleCount = 0U;
    rasterizer.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    const auto blend_factor = [](gpu::BlendFactor factor) {
        switch (factor) {
        case gpu::BlendFactor::zero: return D3D12_BLEND_ZERO;
        case gpu::BlendFactor::one: return D3D12_BLEND_ONE;
        case gpu::BlendFactor::source_alpha: return D3D12_BLEND_SRC_ALPHA;
        case gpu::BlendFactor::one_minus_source_alpha: return D3D12_BLEND_INV_SRC_ALPHA;
        }
        return D3D12_BLEND_ONE;
    };
    const auto blend_op = [](gpu::BlendOp operation) {
        switch (operation) {
        case gpu::BlendOp::add: return D3D12_BLEND_OP_ADD;
        case gpu::BlendOp::subtract: return D3D12_BLEND_OP_SUBTRACT;
        case gpu::BlendOp::reverse_subtract: return D3D12_BLEND_OP_REV_SUBTRACT;
        case gpu::BlendOp::minimum: return D3D12_BLEND_OP_MIN;
        case gpu::BlendOp::maximum: return D3D12_BLEND_OP_MAX;
        }
        return D3D12_BLEND_OP_ADD;
    };
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = descriptor.blend ? TRUE : FALSE;
    blend.RenderTarget[0].SrcBlend = blend_factor(descriptor.source_color);
    blend.RenderTarget[0].DestBlend = blend_factor(descriptor.destination_color);
    blend.RenderTarget[0].BlendOp = blend_op(descriptor.color_blend);
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    const auto compare = [](gpu::CompareOp operation) {
        switch (operation) {
        case gpu::CompareOp::never: return D3D12_COMPARISON_FUNC_NEVER;
        case gpu::CompareOp::less: return D3D12_COMPARISON_FUNC_LESS;
        case gpu::CompareOp::equal: return D3D12_COMPARISON_FUNC_EQUAL;
        case gpu::CompareOp::less_or_equal: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case gpu::CompareOp::greater: return D3D12_COMPARISON_FUNC_GREATER;
        case gpu::CompareOp::not_equal: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case gpu::CompareOp::greater_or_equal: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case gpu::CompareOp::always: return D3D12_COMPARISON_FUNC_ALWAYS;
        }
        return D3D12_COMPARISON_FUNC_ALWAYS;
    };
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = descriptor.depth_test ? TRUE : FALSE;
    depth.DepthWriteMask = descriptor.depth_write
        ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = compare(descriptor.depth_compare);
    depth.StencilEnable = FALSE;
    depth.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depth.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    depth.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.BackFace = depth.FrontFace;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = value.root_signature.Get();
    pipeline.VS = {vertex_shader->binary.bytes.data(), vertex_shader->binary.bytes.size()};
    pipeline.PS = {fragment_shader->binary.bytes.data(), fragment_shader->binary.bytes.size()};
    pipeline.BlendState = blend;
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState = rasterizer;
    pipeline.DepthStencilState = depth;
    pipeline.InputLayout = {
        input_elements.data(), static_cast<UINT>(input_elements.size())};
    pipeline.PrimitiveTopologyType = descriptor.topology == gpu::PrimitiveTopology::line_list
        ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1U;
    pipeline.RTVFormats[0] = format(descriptor.color_format);
    pipeline.DSVFormat = descriptor.depth_test ? format(descriptor.depth_format) : DXGI_FORMAT_UNKNOWN;
    pipeline.SampleDesc.Count = 1U;
    status = impl_->device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&value.pipeline_state));
    if (FAILED(status)) {
        impl_->capture_debug_messages();
        return core::Result<gpu::PipelineHandle>::failure(
            native_failure("ID3D12Device::CreateGraphicsPipelineState", status));
    }
    return impl_->pipelines.insert(std::move(value));
}

core::Result<void> D3D12Device::destroy_buffer(gpu::BufferHandle handle) {
    static_cast<void>(completed_serial());
    auto value = impl_->buffers.resolve(handle);
    if (!value) return core::Result<void>::failure(value.error());
    if (!impl_->pending.empty()) {
        if (auto retired = impl_->deferred_destruction.retire(
                impl_->retirement_serial(),
                std::make_shared<Impl::BufferResource>(*value.value())); !retired) {
            return retired;
        }
    }
    const auto result = impl_->buffers.remove(handle);
    if (result) impl_->resource_states.erase(device_ir::ResourceHandle{handle});
    return result;
}

core::Result<void> D3D12Device::destroy_texture(gpu::TextureHandle handle) {
    static_cast<void>(completed_serial());
    auto value = impl_->textures.resolve(handle);
    if (!value) return core::Result<void>::failure(value.error());
    if (!impl_->pending.empty()) {
        if (auto retired = impl_->deferred_destruction.retire(
                impl_->retirement_serial(),
                std::make_shared<Impl::TextureResource>(*value.value())); !retired) {
            return retired;
        }
    }
    const auto result = impl_->textures.remove(handle);
    if (result) impl_->resource_states.erase(device_ir::ResourceHandle{handle});
    return result;
}

core::Result<void> D3D12Device::destroy_sampler(gpu::SamplerHandle handle) {
    static_cast<void>(completed_serial());
    auto value = impl_->samplers.resolve(handle);
    if (!value) return core::Result<void>::failure(value.error());
    if (!impl_->pending.empty()) {
        if (auto retired = impl_->deferred_destruction.retire(
                impl_->retirement_serial(),
                std::make_shared<Impl::SamplerResource>(*value.value())); !retired) {
            return retired;
        }
    }
    return impl_->samplers.remove(handle);
}

core::Result<void> D3D12Device::destroy_shader(gpu::ShaderHandle handle) {
    static_cast<void>(completed_serial());
    auto value = impl_->shaders.resolve(handle);
    if (!value) return core::Result<void>::failure(value.error());
    if (!impl_->pending.empty()) {
        if (auto retired = impl_->deferred_destruction.retire(
                impl_->retirement_serial(),
                std::make_shared<Impl::ShaderResource>(*value.value())); !retired) {
            return retired;
        }
    }
    return impl_->shaders.remove(handle);
}

core::Result<void> D3D12Device::destroy_pipeline(gpu::PipelineHandle handle) {
    static_cast<void>(completed_serial());
    auto value = impl_->pipelines.resolve(handle);
    if (!value) return core::Result<void>::failure(value.error());
    if (!impl_->pending.empty()) {
        if (auto retired = impl_->deferred_destruction.retire(
                impl_->retirement_serial(),
                std::make_shared<Impl::PipelineResource>(*value.value())); !retired) {
            return retired;
        }
    }
    return impl_->pipelines.remove(handle);
}

core::Result<void> D3D12Device::write_buffer(
    gpu::BufferHandle handle,
    std::uint64_t offset,
    std::span<const std::uint8_t> bytes) {
    if (impl_->device_lost) return core::Result<void>::failure(device_lost_failure());
    auto result = impl_->buffers.resolve(handle);
    if (!result) return core::Result<void>::failure(result.error());
    if (result.value()->descriptor.memory != gpu::MemoryClass::upload) {
        return core::Result<void>::failure(unsupported(
            "D3D12 direct buffer writes require an upload resource in this tranche"));
    }
    if (offset > result.value()->descriptor.bytes ||
        bytes.size() > result.value()->descriptor.bytes - offset) {
        return core::Result<void>::failure(invalid("D3D12 buffer write exceeds the resource"));
    }
    if (bytes.empty()) return core::Result<void>::success();
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0U, 0U};
    const HRESULT status = result.value()->resource->Map(0U, &read_range, &mapped);
    if (FAILED(status)) return core::Result<void>::failure(native_failure("Map(upload)", status));
    std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, bytes.data(), bytes.size());
    result.value()->resource->Unmap(0U, nullptr);
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> D3D12Device::read_buffer(
    gpu::BufferHandle handle,
    std::uint64_t offset,
    std::uint64_t bytes) const {
    if (impl_->device_lost) {
        return core::Result<std::vector<std::uint8_t>>::failure(device_lost_failure());
    }
    auto result = impl_->buffers.resolve(handle);
    if (!result) return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    if (result.value()->descriptor.memory != gpu::MemoryClass::readback) {
        return core::Result<std::vector<std::uint8_t>>::failure(unsupported(
            "D3D12 direct buffer reads require a readback resource in this tranche"));
    }
    if (offset > result.value()->descriptor.bytes ||
        bytes > result.value()->descriptor.bytes - offset ||
        bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return core::Result<std::vector<std::uint8_t>>::failure(invalid(
            "D3D12 buffer read exceeds the resource"));
    }
    void* mapped = nullptr;
    const D3D12_RANGE read_range{offset, offset + bytes};
    const HRESULT status = result.value()->resource->Map(0U, &read_range, &mapped);
    if (FAILED(status)) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            native_failure("Map(readback)", status));
    }
    std::vector<std::uint8_t> output(static_cast<std::size_t>(bytes));
    std::memcpy(output.data(), static_cast<const std::uint8_t*>(mapped) + offset, output.size());
    // This is a CPU read from a READBACK heap; an empty written range tells
    // D3D12 that the CPU did not modify the resource while it was mapped.
    result.value()->resource->Unmap(0U, nullptr);
    return core::Result<std::vector<std::uint8_t>>::success(std::move(output));
}

core::Result<std::vector<float>> D3D12Device::read_texture_float32(
    gpu::TextureHandle handle,
    gpu::Format expected_format,
    std::uint32_t channel_count) const {
    if (impl_->device_lost) {
        return core::Result<std::vector<float>>::failure(device_lost_failure());
    }
    auto texture = impl_->textures.resolve(handle);
    if (!texture) {
        return core::Result<std::vector<float>>::failure(texture.error());
    }
    if (texture.value()->descriptor.format != expected_format) {
        return core::Result<std::vector<float>>::failure(unsupported(
            "D3D12 float readback format does not match the requested source texture"));
    }
    const auto current = impl_->resource_states.find(device_ir::ResourceHandle{handle});
    if (current == impl_->resource_states.end()) {
        return core::Result<std::vector<float>>::failure(Diagnostic(
            ErrorCode::invalid_state, "D3D12 texture readback has no semantic resource state"));
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count = 0U;
    UINT64 row_size = 0U;
    UINT64 total_bytes = 0U;
    const D3D12_RESOURCE_DESC native_descriptor = texture.value()->resource->GetDesc();
    impl_->device->GetCopyableFootprints(
        &native_descriptor, 0U, 1U, 0U, &footprint, &row_count, &row_size, &total_bytes);
    if (total_bytes == 0U || total_bytes > std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<std::vector<float>>::failure(invalid(
            "D3D12 texture readback footprint is empty or overflows"));
    }
    auto* mutable_device = const_cast<D3D12Device*>(this);
    const auto readback = mutable_device->create_buffer(gpu::BufferDesc{
        total_bytes,
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    if (!readback) return core::Result<std::vector<float>>::failure(readback.error());

    device_ir::DeviceCommandStream stream;
    if (current->second != device_ir::ResourceState::copy_source) {
        stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{handle},
            current->second,
            device_ir::ResourceState::copy_source,
        });
    }
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{readback.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    stream.append(device_ir::CmdCopyTextureToBuffer{handle, readback.value(), 0U});
    if (current->second != device_ir::ResourceState::copy_source) {
        stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{handle},
            device_ir::ResourceState::copy_source,
            current->second,
        });
    }
    // A texture may need to return from copy_source to its prior render-target
    // state. Record both transitions on the graphics queue; a copy queue cannot
    // legally own that graphics-state restoration barrier on all D3D12 drivers.
    const auto serial = mutable_device->submit(stream, gpu::QueueType::graphics);
    if (!serial) {
        static_cast<void>(mutable_device->destroy_buffer(readback.value()));
        return core::Result<std::vector<float>>::failure(serial.error());
    }
    if (auto waited = mutable_device->wait(serial.value()); !waited) {
        static_cast<void>(mutable_device->destroy_buffer(readback.value()));
        return core::Result<std::vector<float>>::failure(waited.error());
    }
    const auto bytes = mutable_device->read_buffer(readback.value(), 0U, total_bytes);
    static_cast<void>(mutable_device->destroy_buffer(readback.value()));
    if (!bytes) return core::Result<std::vector<float>>::failure(bytes.error());
    const std::uint64_t expected_values =
        static_cast<std::uint64_t>(texture.value()->descriptor.width) *
        static_cast<std::uint64_t>(texture.value()->descriptor.height) * channel_count;
    if (expected_values > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return core::Result<std::vector<float>>::failure(out_of_memory(
            "D3D12 texture readback result exceeds host addressable memory"));
    }
    std::vector<float> output(static_cast<std::size_t>(expected_values), 0.0F);
    const std::size_t row_bytes = static_cast<std::size_t>(row_size);
    const std::size_t destination_row_bytes =
        static_cast<std::size_t>(texture.value()->descriptor.width) * channel_count *
        sizeof(float);
    for (std::uint32_t row = 0U; row < texture.value()->descriptor.height; ++row) {
        const std::size_t source_offset = static_cast<std::size_t>(footprint.Offset) +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(footprint.Footprint.RowPitch);
        const std::size_t destination_offset = static_cast<std::size_t>(row) * destination_row_bytes;
        if (source_offset > bytes.value().size() ||
            row_bytes > bytes.value().size() - source_offset ||
            destination_offset > output.size() * sizeof(float) ||
            destination_row_bytes > output.size() * sizeof(float) - destination_offset) {
            return core::Result<std::vector<float>>::failure(invalid(
                "D3D12 texture readback footprint exceeds the mapped buffer"));
        }
        std::memcpy(
            reinterpret_cast<std::uint8_t*>(output.data()) + destination_offset,
            bytes.value().data() + source_offset,
            destination_row_bytes);
    }
    return core::Result<std::vector<float>>::success(std::move(output));
}

core::Result<std::vector<float>> D3D12Device::read_texture_rgba32f(
    gpu::TextureHandle handle) const {
    return read_texture_float32(handle, gpu::Format::rgba32_float, 4U);
}

core::Result<std::vector<float>> D3D12Device::read_texture_depth32f(
    gpu::TextureHandle handle) const {
    return read_texture_float32(handle, gpu::Format::depth32_float, 1U);
}

core::Result<void> D3D12Device::wait_idle() {
    if (impl_->device_lost) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state,
            "D3D12 device is lost; cannot wait for queue idle"));
    }
    while (!impl_->pending.empty()) {
        const gpu::SubmissionSerial serial = impl_->pending.begin()->first;
        if (auto result = wait(serial); !result) return result;
    }
    static_cast<void>(completed_serial());
    return core::Result<void>::success();
}

core::Result<gpu::SubmissionSerial> D3D12Device::submit(
    const device_ir::DeviceCommandStream& stream,
    gpu::QueueType queue) {
    if (impl_->device_lost) {
        return core::Result<gpu::SubmissionSerial>::failure(Diagnostic(
            ErrorCode::invalid_state,
            "D3D12 device is lost; call recover() to create a fresh device"));
    }
    if (auto result = stream.validate(); !result) {
        return core::Result<gpu::SubmissionSerial>::failure(result.error());
    }
    const auto queue_it = impl_->queues.find(queue);
    if (queue_it == impl_->queues.end()) {
        return core::Result<gpu::SubmissionSerial>::failure(unsupported(
            "D3D12 device does not expose the requested queue"));
    }
    if (impl_->next_serial == std::numeric_limits<gpu::SubmissionSerial>::max()) {
        return core::Result<gpu::SubmissionSerial>::failure(Diagnostic(
            ErrorCode::invalid_state, "D3D12 submission serial exhausted"));
    }

    auto& queue_resource = queue_it->second;
    if (queue_resource.last_submitted != 0U &&
        queue_resource.fence->GetCompletedValue() < queue_resource.last_submitted) {
        if (FAILED(queue_resource.fence->SetEventOnCompletion(
                queue_resource.last_submitted, queue_resource.event))) {
            return core::Result<gpu::SubmissionSerial>::failure(Diagnostic(
                ErrorCode::invalid_state, "D3D12 fence wait registration failed"));
        }
        WaitForSingleObject(queue_resource.event, INFINITE);
    }
    if (FAILED(queue_resource.allocator->Reset()) ||
        FAILED(queue_resource.command_list->Reset(queue_resource.allocator.Get(), nullptr))) {
        return core::Result<gpu::SubmissionSerial>::failure(Diagnostic(
            ErrorCode::invalid_state, "D3D12 command allocator/list reset failed"));
    }

    const auto semantic_state_snapshot = impl_->resource_states;
    std::map<device_ir::ResourceHandle, D3D12_RESOURCE_STATES> native_state_snapshot;
    auto remember_native_state = [this, &native_state_snapshot](
        const device_ir::ResourceHandle& resource) -> core::Result<void> {
        if (native_state_snapshot.contains(resource)) return core::Result<void>::success();
        return std::visit(
            [this, &native_state_snapshot, &resource](const auto handle) -> core::Result<void> {
                using Handle = std::decay_t<decltype(handle)>;
                if constexpr (std::is_same_v<Handle, gpu::BufferHandle>) {
                    auto value = impl_->buffers.resolve(handle);
                    if (!value) return core::Result<void>::failure(value.error());
                    native_state_snapshot.emplace(resource, value.value()->native_state);
                } else {
                    auto value = impl_->textures.resolve(handle);
                    if (!value) return core::Result<void>::failure(value.error());
                    native_state_snapshot.emplace(resource, value.value()->native_state);
                }
                return core::Result<void>::success();
            },
            resource);
    };
    auto rollback_preexecution_state = [this, &semantic_state_snapshot, &native_state_snapshot]() {
        impl_->resource_states = semantic_state_snapshot;
        for (const auto& [resource, native_state_value] : native_state_snapshot) {
            std::visit(
                [this, native_state_value](const auto handle) {
                    using Handle = std::decay_t<decltype(handle)>;
                    if constexpr (std::is_same_v<Handle, gpu::BufferHandle>) {
                        if (auto value = impl_->buffers.resolve(handle); value) {
                            value.value()->native_state = native_state_value;
                        }
                    } else {
                        if (auto value = impl_->textures.resolve(handle); value) {
                            value.value()->native_state = native_state_value;
                        }
                    }
                },
                resource);
        }
    };

    auto require_state = [this](
        const device_ir::ResourceHandle& resource,
        device_ir::ResourceState expected) -> core::Result<void> {
        const auto current = impl_->resource_states.find(resource);
        if (current == impl_->resource_states.end() || current->second != expected) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::validation_failed,
                "D3D12 command uses a resource in the wrong semantic state"));
        }
        return core::Result<void>::success();
    };
    auto require_graphics = [queue]() -> core::Result<void> {
        if (queue != gpu::QueueType::graphics) {
            return core::Result<void>::failure(unsupported(
                "D3D12 render-target commands require the graphics queue"));
        }
        return core::Result<void>::success();
    };
    auto require_compute = [queue]() -> core::Result<void> {
        if (queue != gpu::QueueType::compute) {
            return core::Result<void>::failure(unsupported(
                "D3D12 compute commands require the compute queue"));
        }
        return core::Result<void>::success();
    };
    auto transition = [this, &queue_resource, &remember_native_state, queue](
        const device_ir::CmdTransitionResource& command) -> core::Result<void> {
        const auto current = impl_->resource_states.find(command.resource);
        if (current == impl_->resource_states.end() || current->second != command.before) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::validation_failed,
                "D3D12 resource transition does not match the semantic state"));
        }
        if (auto result = remember_native_state(command.resource); !result) return result;
        const D3D12_RESOURCE_STATES after = native_state(command.after);
        const auto record = [&](ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES& before) -> core::Result<void> {
            if (before != after) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = resource;
                barrier.Transition.StateBefore = before;
                barrier.Transition.StateAfter = after;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                queue_resource.command_list->ResourceBarrier(1U, &barrier);
            }
            before = after;
            return core::Result<void>::success();
        };
        const bool copy_queue = queue == gpu::QueueType::copy;
        const auto result = std::visit(
            [&record, this, &command, copy_queue](const auto handle) -> core::Result<void> {
                using Handle = std::decay_t<decltype(handle)>;
                if constexpr (std::is_same_v<Handle, gpu::BufferHandle>) {
                    auto resource = impl_->buffers.resolve(handle);
                    if (!resource) return core::Result<void>::failure(resource.error());
                    if (resource.value()->descriptor.memory == gpu::MemoryClass::upload) {
                        const bool read_only =
                            command.after == device_ir::ResourceState::copy_source ||
                            command.after == device_ir::ResourceState::vertex_read ||
                            command.after == device_ir::ResourceState::index_read ||
                            command.after == device_ir::ResourceState::constant_read ||
                            command.after == device_ir::ResourceState::shader_read;
                        if (!read_only) {
                            return core::Result<void>::failure(unsupported(
                                "D3D12 upload resources cannot enter a write state"));
                        }
                        impl_->resource_states[command.resource] = command.after;
                        return core::Result<void>::success();
                    }
                    if (resource.value()->descriptor.memory == gpu::MemoryClass::readback) {
                        if (command.after != device_ir::ResourceState::copy_destination) {
                            return core::Result<void>::failure(unsupported(
                                "D3D12 readback resources can only enter copy-destination state"));
                        }
                        impl_->resource_states[command.resource] = command.after;
                        return core::Result<void>::success();
                    }
                    return record(resource.value()->resource.Get(), resource.value()->native_state);
                } else {
                    auto resource = impl_->textures.resolve(handle);
                    if (!resource) return core::Result<void>::failure(resource.error());
                    if (copy_queue &&
                        (command.after == device_ir::ResourceState::copy_source ||
                         command.after == device_ir::ResourceState::copy_destination)) {
                        // Copy queues use COMMON as the native ownership state and
                        // rely on D3D12's copy-state promotion/decay. Keep the
                        // semantic state explicit while leaving the native state
                        // at COMMON for the copy command list.
                        if (resource.value()->native_state != D3D12_RESOURCE_STATE_COMMON) {
                            return core::Result<void>::failure(unsupported(
                                "D3D12 copy-queue texture use requires native COMMON ownership"));
                        }
                        impl_->resource_states[command.resource] = command.after;
                        return core::Result<void>::success();
                    }
                    return record(resource.value()->resource.Get(), resource.value()->native_state);
                }
            },
            command.resource);
        if (result) impl_->resource_states[command.resource] = command.after;
        return result;
    };

    std::optional<gpu::PipelineHandle> bound_pipeline;
    std::optional<device_ir::ResourceBinding> bound_sampled_texture;
    std::optional<device_ir::ResourceBinding> bound_storage_texture;
    bool vertex_buffer_bound = false;
    bool index_buffer_bound = false;
    for (const auto& command : stream.commands()) {
        const auto result = std::visit(
            [this, &queue_resource, &require_state, &require_graphics, &require_compute,
             &transition, &bound_pipeline, &bound_sampled_texture,
             &bound_storage_texture, &vertex_buffer_bound, &index_buffer_bound](
                const auto& value) -> core::Result<void> {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, device_ir::CmdBeginLabel> ||
                              std::is_same_v<T, device_ir::CmdEndLabel> ||
                              std::is_same_v<T, device_ir::CmdSignalTimeline>) {
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdTransitionResource>) {
                    return transition(value);
                } else if constexpr (std::is_same_v<T, device_ir::CmdUavBarrier>) {
                    D3D12_RESOURCE_BARRIER barrier{};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    const auto record = std::visit(
                        [&barrier, this](const auto handle) -> core::Result<void> {
                            using Handle = std::decay_t<decltype(handle)>;
                            if constexpr (std::is_same_v<Handle, gpu::BufferHandle>) {
                                auto resource = impl_->buffers.resolve(handle);
                                if (!resource) return core::Result<void>::failure(resource.error());
                                barrier.UAV.pResource = resource.value()->resource.Get();
                            } else {
                                auto resource = impl_->textures.resolve(handle);
                                if (!resource) return core::Result<void>::failure(resource.error());
                                barrier.UAV.pResource = resource.value()->resource.Get();
                            }
                            return core::Result<void>::success();
                        },
                        value.resource);
                    if (!record) return record;
                    queue_resource.command_list->ResourceBarrier(1U, &barrier);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyBuffer>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.source},
                            device_ir::ResourceState::copy_source); !result) return result;
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.destination},
                            device_ir::ResourceState::copy_destination); !result) return result;
                    auto source = impl_->buffers.resolve(value.source);
                    auto destination = impl_->buffers.resolve(value.destination);
                    if (!source) return core::Result<void>::failure(source.error());
                    if (!destination) return core::Result<void>::failure(destination.error());
                    if (value.source_offset > source.value()->descriptor.bytes ||
                        value.destination_offset > destination.value()->descriptor.bytes ||
                        value.bytes > source.value()->descriptor.bytes - value.source_offset ||
                        value.bytes > destination.value()->descriptor.bytes - value.destination_offset) {
                        return core::Result<void>::failure(invalid(
                            "D3D12 buffer copy exceeds a resource"));
                    }
                    queue_resource.command_list->CopyBufferRegion(
                        destination.value()->resource.Get(),
                        value.destination_offset,
                        source.value()->resource.Get(),
                        value.source_offset,
                        value.bytes);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyBufferToTexture>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.source},
                            device_ir::ResourceState::copy_source); !result) return result;
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.destination},
                            device_ir::ResourceState::copy_destination); !result) return result;
                    auto source = impl_->buffers.resolve(value.source);
                    auto destination = impl_->textures.resolve(value.destination);
                    if (!source) return core::Result<void>::failure(source.error());
                    if (!destination) return core::Result<void>::failure(destination.error());
                    const D3D12_RESOURCE_DESC descriptor = destination.value()->resource->GetDesc();
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
                    UINT row_count = 0U;
                    UINT64 row_size = 0U;
                    UINT64 total_bytes = 0U;
                    impl_->device->GetCopyableFootprints(
                        &descriptor, 0U, 1U, value.source_offset,
                        &footprint, &row_count, &row_size, &total_bytes);
                    static_cast<void>(row_count);
                    static_cast<void>(row_size);
                    if (value.source_offset > source.value()->descriptor.bytes ||
                        total_bytes > source.value()->descriptor.bytes - value.source_offset) {
                        return core::Result<void>::failure(invalid(
                            "D3D12 buffer-to-texture copy exceeds the source buffer"));
                    }
                    D3D12_TEXTURE_COPY_LOCATION source_location{};
                    source_location.pResource = source.value()->resource.Get();
                    source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    source_location.PlacedFootprint = footprint;
                    D3D12_TEXTURE_COPY_LOCATION destination_location{};
                    destination_location.pResource = destination.value()->resource.Get();
                    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    destination_location.SubresourceIndex = 0U;
                    queue_resource.command_list->CopyTextureRegion(
                        &destination_location, 0U, 0U, 0U, &source_location, nullptr);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyTextureToBuffer>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.source},
                            device_ir::ResourceState::copy_source); !result) return result;
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.destination},
                            device_ir::ResourceState::copy_destination); !result) return result;
                    auto source = impl_->textures.resolve(value.source);
                    auto destination = impl_->buffers.resolve(value.destination);
                    if (!source) return core::Result<void>::failure(source.error());
                    if (!destination) return core::Result<void>::failure(destination.error());
                    const D3D12_RESOURCE_DESC descriptor = source.value()->resource->GetDesc();
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
                    UINT row_count = 0U;
                    UINT64 row_size = 0U;
                    UINT64 total_bytes = 0U;
                    impl_->device->GetCopyableFootprints(
                        &descriptor, 0U, 1U, value.destination_offset,
                        &footprint, &row_count, &row_size, &total_bytes);
                    static_cast<void>(row_count);
                    static_cast<void>(row_size);
                    if (value.destination_offset > destination.value()->descriptor.bytes ||
                        total_bytes > destination.value()->descriptor.bytes - value.destination_offset) {
                        return core::Result<void>::failure(invalid(
                            "D3D12 texture-to-buffer copy exceeds the destination buffer"));
                    }
                    D3D12_TEXTURE_COPY_LOCATION source_location{};
                    source_location.pResource = source.value()->resource.Get();
                    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    source_location.SubresourceIndex = 0U;
                    D3D12_TEXTURE_COPY_LOCATION destination_location{};
                    destination_location.pResource = destination.value()->resource.Get();
                    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    destination_location.PlacedFootprint = footprint;
                    queue_resource.command_list->CopyTextureRegion(
                        &destination_location, 0U, 0U, 0U, &source_location, nullptr);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyTexture>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.source},
                            device_ir::ResourceState::copy_source); !result) return result;
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.destination},
                            device_ir::ResourceState::copy_destination); !result) return result;
                    auto source = impl_->textures.resolve(value.source);
                    auto destination = impl_->textures.resolve(value.destination);
                    if (!source) return core::Result<void>::failure(source.error());
                    if (!destination) return core::Result<void>::failure(destination.error());
                    if (source.value()->descriptor.width != destination.value()->descriptor.width ||
                        source.value()->descriptor.height != destination.value()->descriptor.height ||
                        source.value()->descriptor.format != destination.value()->descriptor.format) {
                        return core::Result<void>::failure(validation(
                            "D3D12 texture copies require matching extents and formats"));
                    }
                    D3D12_TEXTURE_COPY_LOCATION source_location{};
                    source_location.pResource = source.value()->resource.Get();
                    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    source_location.SubresourceIndex = 0U;
                    D3D12_TEXTURE_COPY_LOCATION destination_location{};
                    destination_location.pResource = destination.value()->resource.Get();
                    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    destination_location.SubresourceIndex = 0U;
                    queue_resource.command_list->CopyTextureRegion(
                        &destination_location, 0U, 0U, 0U, &source_location, nullptr);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdClearColor>) {
                    if (auto result = require_graphics(); !result) return result;
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.target},
                            device_ir::ResourceState::render_target); !result) return result;
                    auto target = impl_->textures.resolve(value.target);
                    if (!target) return core::Result<void>::failure(target.error());
                    if (!target.value()->has_rtv) {
                        return core::Result<void>::failure(Diagnostic(
                            ErrorCode::validation_failed,
                            "D3D12 color clear target has no RTV descriptor"));
                    }
                    queue_resource.command_list->ClearRenderTargetView(
                        target.value()->rtv, value.color.data(), 0U, nullptr);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdClearDepth>) {
                    if (auto result = require_graphics(); !result) return result;
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.target},
                            device_ir::ResourceState::depth_write); !result) return result;
                    auto target = impl_->textures.resolve(value.target);
                    if (!target) return core::Result<void>::failure(target.error());
                    if (!target.value()->has_dsv) {
                        return core::Result<void>::failure(Diagnostic(
                            ErrorCode::validation_failed,
                            "D3D12 depth clear target has no DSV descriptor"));
                    }
                    queue_resource.command_list->ClearDepthStencilView(
                        target.value()->dsv, D3D12_CLEAR_FLAG_DEPTH, value.depth, 0U, 0U, nullptr);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBeginRendering>) {
                    if (auto result = require_graphics(); !result) return result;
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.color_target},
                            device_ir::ResourceState::render_target); !result) return result;
                    auto color = impl_->textures.resolve(value.color_target);
                    if (!color) return core::Result<void>::failure(color.error());
                    if (!color.value()->has_rtv) {
                        return core::Result<void>::failure(Diagnostic(
                            ErrorCode::validation_failed,
                            "D3D12 rendering target has no RTV descriptor"));
                    }
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = color.value()->rtv;
                    if (value.depth_target.has_value()) {
                        const auto depth_state = impl_->resource_states.find(
                            device_ir::ResourceHandle{*value.depth_target});
                        if (depth_state == impl_->resource_states.end() ||
                            (depth_state->second != device_ir::ResourceState::depth_write &&
                             depth_state->second != device_ir::ResourceState::depth_read)) {
                            return core::Result<void>::failure(Diagnostic(
                                ErrorCode::validation_failed,
                                "D3D12 rendering depth target has the wrong semantic state"));
                        }
                        auto depth = impl_->textures.resolve(*value.depth_target);
                        if (!depth) return core::Result<void>::failure(depth.error());
                        if (!depth.value()->has_dsv) {
                            return core::Result<void>::failure(Diagnostic(
                                ErrorCode::validation_failed,
                                "D3D12 rendering depth target has no DSV descriptor"));
                        }
                        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth.value()->dsv;
                        queue_resource.command_list->OMSetRenderTargets(1U, &rtv, FALSE, &dsv);
                    } else {
                        queue_resource.command_list->OMSetRenderTargets(1U, &rtv, FALSE, nullptr);
                    }
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdEndRendering>) {
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdSetViewport>) {
                    if (auto result = require_graphics(); !result) return result;
                    const D3D12_VIEWPORT viewport{
                        value.x, value.y, value.width, value.height,
                        value.min_depth, value.max_depth};
                    queue_resource.command_list->RSSetViewports(1U, &viewport);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdSetScissor>) {
                    if (auto result = require_graphics(); !result) return result;
                    const RECT rectangle{
                        value.x,
                        value.y,
                        value.x + static_cast<LONG>(value.width),
                        value.y + static_cast<LONG>(value.height)};
                    queue_resource.command_list->RSSetScissorRects(1U, &rectangle);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindPipeline>) {
                    auto pipeline = impl_->pipelines.resolve(value.pipeline);
                    if (!pipeline) return core::Result<void>::failure(pipeline.error());
                    if (pipeline.value()->compute) {
                        if (auto result = require_compute(); !result) return result;
                        queue_resource.command_list->SetPipelineState(
                            pipeline.value()->pipeline_state.Get());
                        queue_resource.command_list->SetComputeRootSignature(
                            pipeline.value()->root_signature.Get());
                        bound_pipeline = value.pipeline;
                        return core::Result<void>::success();
                    }
                    if (auto result = require_graphics(); !result) return result;
                    queue_resource.command_list->SetPipelineState(
                        pipeline.value()->pipeline_state.Get());
                    queue_resource.command_list->SetGraphicsRootSignature(
                        pipeline.value()->root_signature.Get());
                    switch (pipeline.value()->descriptor.topology) {
                    case gpu::PrimitiveTopology::triangle_list:
                        queue_resource.command_list->IASetPrimitiveTopology(
                            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        break;
                    case gpu::PrimitiveTopology::line_list:
                        queue_resource.command_list->IASetPrimitiveTopology(
                            D3D_PRIMITIVE_TOPOLOGY_LINELIST);
                        break;
                    case gpu::PrimitiveTopology::triangle_strip:
                        return core::Result<void>::failure(unsupported(
                            "D3D12 reference pipeline does not execute triangle strips"));
                    }
                    bound_pipeline = value.pipeline;
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindVertexBuffer>) {
                    if (auto result = require_graphics(); !result) return result;
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.buffer},
                            device_ir::ResourceState::vertex_read); !result) return result;
                    auto buffer = impl_->buffers.resolve(value.buffer);
                    if (!buffer) return core::Result<void>::failure(buffer.error());
                    if (value.offset > buffer.value()->descriptor.bytes ||
                        buffer.value()->descriptor.bytes - value.offset >
                            std::numeric_limits<UINT>::max()) {
                        return core::Result<void>::failure(invalid(
                            "D3D12 vertex binding exceeds native view bounds"));
                    }
                    if (bound_pipeline.has_value()) {
                        auto pipeline = impl_->pipelines.resolve(*bound_pipeline);
                        if (!pipeline) return core::Result<void>::failure(pipeline.error());
                        if (pipeline.value()->descriptor.vertex_stride_bytes != 0U &&
                            value.stride_bytes !=
                                pipeline.value()->descriptor.vertex_stride_bytes) {
                            return core::Result<void>::failure(validation(
                                "D3D12 vertex binding stride does not match the pipeline layout"));
                        }
                    }
                    const D3D12_VERTEX_BUFFER_VIEW view{
                        buffer.value()->resource->GetGPUVirtualAddress() + value.offset,
                        static_cast<UINT>(buffer.value()->descriptor.bytes - value.offset),
                        value.stride_bytes};
                    queue_resource.command_list->IASetVertexBuffers(0U, 1U, &view);
                    vertex_buffer_bound = true;
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindIndexBuffer>) {
                    if (auto result = require_graphics(); !result) return result;
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.buffer},
                            device_ir::ResourceState::index_read); !result) return result;
                    auto buffer = impl_->buffers.resolve(value.buffer);
                    if (!buffer) return core::Result<void>::failure(buffer.error());
                    if (value.offset > buffer.value()->descriptor.bytes ||
                        buffer.value()->descriptor.bytes - value.offset >
                            std::numeric_limits<UINT>::max()) {
                        return core::Result<void>::failure(invalid(
                            "D3D12 index binding exceeds native view bounds"));
                    }
                    const DXGI_FORMAT index_format = value.format == device_ir::IndexFormat::uint16
                        ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
                    const D3D12_INDEX_BUFFER_VIEW view{
                        buffer.value()->resource->GetGPUVirtualAddress() + value.offset,
                        static_cast<UINT>(buffer.value()->descriptor.bytes - value.offset),
                        index_format};
                    queue_resource.command_list->IASetIndexBuffer(&view);
                    index_buffer_bound = true;
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindResources>) {
                    bound_sampled_texture.reset();
                    bound_storage_texture.reset();
                    if (value.bindings.empty()) return core::Result<void>::success();
                    if (!bound_pipeline.has_value() ||
                        !std::holds_alternative<gpu::TextureHandle>(
                        value.bindings.front().resource)) {
                        return core::Result<void>::failure(unsupported(
                            "D3D12 resource bindings require a bound texture pipeline"));
                    }
                    auto pipeline = impl_->pipelines.resolve(*bound_pipeline);
                    if (!pipeline) return core::Result<void>::failure(pipeline.error());
                    if (pipeline.value()->compute &&
                        pipeline.value()->descriptor.sampled_texture_count != 0U) {
                        const std::size_t sampled_count = static_cast<std::size_t>(
                            pipeline.value()->descriptor.sampled_texture_count);
                        if (!pipeline.value()->storage_texture_root_index.has_value() ||
                            value.bindings.size() != sampled_count + 1U) {
                            return core::Result<void>::failure(validation(
                                "D3D12 multi-texture compute bindings require sampled textures followed by one storage texture"));
                        }
                        if (auto result = require_compute(); !result) return result;
                        ID3D12DescriptorHeap* heaps[] = {impl_->srv_heap.Get()};
                        queue_resource.command_list->SetDescriptorHeaps(1U, heaps);
                        for (std::size_t sampled_index = 0U;
                             sampled_index < sampled_count; ++sampled_index) {
                            const auto& binding = value.bindings.at(sampled_index);
                            if (binding.set != 0U || binding.binding != sampled_index ||
                                binding.sampler.has_value() ||
                                !std::holds_alternative<gpu::TextureHandle>(binding.resource)) {
                                return core::Result<void>::failure(validation(
                                    "D3D12 sampled compute bindings must be consecutive texture SRVs without samplers"));
                            }
                            const auto texture_handle = std::get<gpu::TextureHandle>(
                                binding.resource);
                            if (auto result = require_state(
                                    device_ir::ResourceHandle{texture_handle},
                                    device_ir::ResourceState::shader_read); !result) {
                                return result;
                            }
                            auto texture = impl_->textures.resolve(texture_handle);
                            if (!texture) return core::Result<void>::failure(texture.error());
                            if (!texture.value()->has_srv) {
                                return core::Result<void>::failure(unsupported(
                                    "D3D12 sampled compute texture has no SRV descriptor"));
                            }
                            queue_resource.command_list->SetComputeRootDescriptorTable(
                                pipeline.value()->sampled_texture_root_indices.at(sampled_index),
                                texture.value()->srv_gpu);
                        }
                        const auto& storage_binding = value.bindings.at(sampled_count);
                        if (storage_binding.set != 0U ||
                            storage_binding.binding != sampled_count ||
                            storage_binding.sampler.has_value() ||
                            !std::holds_alternative<gpu::TextureHandle>(storage_binding.resource)) {
                            return core::Result<void>::failure(validation(
                                "D3D12 storage compute binding must follow sampled texture bindings"));
                        }
                        const auto storage_handle = std::get<gpu::TextureHandle>(
                            storage_binding.resource);
                        if (auto result = require_state(
                                device_ir::ResourceHandle{storage_handle},
                                device_ir::ResourceState::shader_write); !result) {
                            return result;
                        }
                        auto storage = impl_->textures.resolve(storage_handle);
                        if (!storage) return core::Result<void>::failure(storage.error());
                        if (!storage.value()->has_uav) {
                            return core::Result<void>::failure(unsupported(
                                "D3D12 compute storage texture has no UAV descriptor"));
                        }
                        queue_resource.command_list->SetComputeRootDescriptorTable(
                            *pipeline.value()->storage_texture_root_index,
                            storage.value()->uav_gpu);
                        bound_sampled_texture = value.bindings.front();
                        bound_storage_texture = storage_binding;
                        return core::Result<void>::success();
                    }
                    if (value.bindings.size() != 1U) {
                        return core::Result<void>::failure(unsupported(
                            "D3D12 graphics and single-storage compute pipelines accept one texture binding"));
                    }
                    const auto texture_handle = std::get<gpu::TextureHandle>(
                        value.bindings.front().resource);
                    if (pipeline.value()->compute) {
                        if (auto result = require_compute(); !result) return result;
                        if (!pipeline.value()->storage_texture_root_index.has_value()) {
                            return core::Result<void>::failure(validation(
                                "D3D12 compute resource binding is not declared by the bound pipeline"));
                        }
                        if (value.bindings.front().sampler.has_value()) {
                            return core::Result<void>::failure(validation(
                                "D3D12 compute storage bindings do not accept a sampler"));
                        }
                        if (auto result = require_state(
                                device_ir::ResourceHandle{texture_handle},
                                device_ir::ResourceState::shader_write); !result) {
                            return result;
                        }
                        auto texture = impl_->textures.resolve(texture_handle);
                        if (!texture) return core::Result<void>::failure(texture.error());
                        if (!texture.value()->has_uav) {
                            return core::Result<void>::failure(unsupported(
                                "D3D12 compute storage texture has no UAV descriptor"));
                        }
                        ID3D12DescriptorHeap* heaps[] = {impl_->srv_heap.Get()};
                        queue_resource.command_list->SetDescriptorHeaps(1U, heaps);
                        queue_resource.command_list->SetComputeRootDescriptorTable(
                            *pipeline.value()->storage_texture_root_index,
                            texture.value()->uav_gpu);
                        bound_storage_texture = value.bindings.front();
                        return core::Result<void>::success();
                    }
                    if (!pipeline.value()->sampled_texture_root_index.has_value() ||
                        !pipeline.value()->sampler_root_index.has_value()) {
                        return core::Result<void>::failure(validation(
                            "D3D12 resource binding is not declared by the bound pipeline"));
                    }
                    if (auto result = require_state(
                            device_ir::ResourceHandle{texture_handle},
                            device_ir::ResourceState::shader_read); !result) {
                        return result;
                    }
                    auto texture = impl_->textures.resolve(texture_handle);
                    if (!texture) return core::Result<void>::failure(texture.error());
                    if (!texture.value()->has_srv) {
                        return core::Result<void>::failure(unsupported(
                            "D3D12 sampled texture has no SRV descriptor"));
                    }
                    if (!value.bindings.front().sampler.has_value()) {
                        return core::Result<void>::failure(validation(
                            "D3D12 sampled texture bindings require an explicit sampler"));
                    }
                    auto sampler = impl_->samplers.resolve(
                        *value.bindings.front().sampler);
                    if (!sampler) return core::Result<void>::failure(sampler.error());
                    if (!sampler.value()->has_descriptor) {
                        return core::Result<void>::failure(validation(
                            "D3D12 sampler has no native descriptor"));
                    }
                    ID3D12DescriptorHeap* heaps[] = {
                        impl_->srv_heap.Get(), impl_->sampler_heap.Get()};
                    queue_resource.command_list->SetDescriptorHeaps(2U, heaps);
                    queue_resource.command_list->SetGraphicsRootDescriptorTable(
                        *pipeline.value()->sampled_texture_root_index,
                        texture.value()->srv_gpu);
                    queue_resource.command_list->SetGraphicsRootDescriptorTable(
                        *pipeline.value()->sampler_root_index,
                        sampler.value()->gpu);
                    bound_sampled_texture = value.bindings.front();
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdPushConstants>) {
                    if (!bound_pipeline.has_value()) {
                        return core::Result<void>::failure(validation(
                            "D3D12 push constants require a bound pipeline"));
                    }
                    auto pipeline = impl_->pipelines.resolve(*bound_pipeline);
                    if (!pipeline) return core::Result<void>::failure(pipeline.error());
                    if (pipeline.value()->compute) {
                        if (auto result = require_compute(); !result) return result;
                    } else {
                        if (auto result = require_graphics(); !result) return result;
                    }
                    if (!pipeline.value()->push_constant_root_index.has_value()) {
                        return core::Result<void>::failure(validation(
                            "D3D12 push constants are not declared by the bound pipeline"));
                    }
                    if (value.bytes.size() % sizeof(std::uint32_t) != 0U ||
                        value.bytes.size() > pipeline.value()->descriptor.push_constant_bytes) {
                        return core::Result<void>::failure(invalid(
                            "D3D12 push constants exceed the bound root-constant range"));
                    }
                    if (!value.bytes.empty()) {
                        const UINT count = static_cast<UINT>(
                            value.bytes.size() / sizeof(std::uint32_t));
                        if (pipeline.value()->compute) {
                            queue_resource.command_list->SetComputeRoot32BitConstants(
                                *pipeline.value()->push_constant_root_index,
                                count,
                                value.bytes.data(),
                                0U);
                        } else {
                            queue_resource.command_list->SetGraphicsRoot32BitConstants(
                                *pipeline.value()->push_constant_root_index,
                                count,
                                value.bytes.data(),
                                0U);
                        }
                    }
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdDraw>) {
                    if (auto result = require_graphics(); !result) return result;
                    if (!bound_pipeline.has_value() || !vertex_buffer_bound) {
                        return core::Result<void>::failure(validation(
                            "D3D12 draw requires a bound pipeline and vertex buffer"));
                    }
                    auto pipeline = impl_->pipelines.resolve(*bound_pipeline);
                    if (!pipeline) return core::Result<void>::failure(pipeline.error());
                    if (pipeline.value()->compute) {
                        return core::Result<void>::failure(validation(
                            "D3D12 draw cannot use a compute pipeline"));
                    }
                    if (pipeline.value()->sampled_texture_root_index.has_value() &&
                        !bound_sampled_texture.has_value()) {
                        return core::Result<void>::failure(validation(
                            "D3D12 sampled draw requires a bound texture"));
                    }
                    queue_resource.command_list->DrawInstanced(
                        value.vertex_count,
                        value.instance_count,
                        value.first_vertex,
                        value.first_instance);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdDrawIndexed>) {
                    if (auto result = require_graphics(); !result) return result;
                    if (!bound_pipeline.has_value() || !vertex_buffer_bound || !index_buffer_bound) {
                        return core::Result<void>::failure(validation(
                            "D3D12 indexed draw requires a bound pipeline, vertex buffer, and index buffer"));
                    }
                    auto pipeline = impl_->pipelines.resolve(*bound_pipeline);
                    if (!pipeline) return core::Result<void>::failure(pipeline.error());
                    if (pipeline.value()->compute) {
                        return core::Result<void>::failure(validation(
                            "D3D12 indexed draw cannot use a compute pipeline"));
                    }
                    if (pipeline.value()->sampled_texture_root_index.has_value() &&
                        !bound_sampled_texture.has_value()) {
                        return core::Result<void>::failure(validation(
                            "D3D12 sampled indexed draw requires a bound texture"));
                    }
                    queue_resource.command_list->DrawIndexedInstanced(
                        value.index_count,
                        value.instance_count,
                        value.first_index,
                        value.vertex_offset,
                        value.first_instance);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdDispatch>) {
                    if (auto result = require_compute(); !result) return result;
                    if (!bound_pipeline.has_value()) {
                        return core::Result<void>::failure(validation(
                            "D3D12 dispatch requires a bound compute pipeline"));
                    }
                    auto pipeline = impl_->pipelines.resolve(*bound_pipeline);
                    if (!pipeline) return core::Result<void>::failure(pipeline.error());
                    if (!pipeline.value()->compute) {
                        return core::Result<void>::failure(validation(
                            "D3D12 dispatch requires a compute pipeline"));
                    }
                    if (pipeline.value()->storage_texture_root_index.has_value() &&
                        !bound_storage_texture.has_value()) {
                        return core::Result<void>::failure(validation(
                            "D3D12 storage dispatch requires a bound texture"));
                    }
                    if (pipeline.value()->descriptor.sampled_texture_count != 0U &&
                        !bound_sampled_texture.has_value()) {
                        return core::Result<void>::failure(validation(
                            "D3D12 sampled compute dispatch requires bound textures"));
                    }
                    queue_resource.command_list->Dispatch(
                        value.group_count_x, value.group_count_y, value.group_count_z);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBlitTexture> ||
                                     std::is_same_v<T, device_ir::CmdWriteTimestamp> ||
                                     std::is_same_v<T, device_ir::CmdResolveTimestamps>) {
                    return core::Result<void>::failure(unsupported(
                        "D3D12 command is reserved for a later parity tranche"));
                } else if constexpr (std::is_same_v<T, device_ir::CmdWaitTimeline>) {
                    if (value.serial > completed_serial()) {
                        return core::Result<void>::failure(Diagnostic(
                            ErrorCode::invalid_state,
                            "D3D12 timeline wait is ahead of completion"));
                    }
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdPresent>) {
                    return core::Result<void>::failure(unsupported(
                        "D3D12 presentation awaits the swapchain tranche"));
                }
                return core::Result<void>::success();
            },
            command);
        if (!result) {
            rollback_preexecution_state();
            queue_resource.command_list->Close();
            return core::Result<gpu::SubmissionSerial>::failure(result.error());
        }
    }

    const HRESULT close_status = queue_resource.command_list->Close();
    if (FAILED(close_status)) {
        rollback_preexecution_state();
        return core::Result<gpu::SubmissionSerial>::failure(
            native_failure("ID3D12GraphicsCommandList::Close", close_status));
    }
    ID3D12CommandList* command_lists[] = {queue_resource.command_list.Get()};
    queue_resource.queue->ExecuteCommandLists(1U, command_lists);
    const HRESULT removed_reason = impl_->device->GetDeviceRemovedReason();
    if (FAILED(removed_reason) && is_device_removed_hresult(removed_reason)) {
        impl_->record_device_lost("ID3D12Device::GetDeviceRemovedReason", removed_reason);
        return core::Result<gpu::SubmissionSerial>::failure(
            native_failure("ID3D12Device::GetDeviceRemovedReason", removed_reason));
    }
    const gpu::SubmissionSerial serial = ++impl_->next_serial;
    const HRESULT signal_status = queue_resource.queue->Signal(queue_resource.fence.Get(), serial);
    if (FAILED(signal_status)) {
        if (is_device_removed_hresult(signal_status)) {
            impl_->record_device_lost("ID3D12CommandQueue::Signal", signal_status);
        }
        return core::Result<gpu::SubmissionSerial>::failure(
            native_failure("ID3D12CommandQueue::Signal", signal_status));
    }
    impl_->capture_debug_messages();
    queue_resource.last_submitted = serial;
    impl_->pending.emplace(serial, queue);
    return core::Result<gpu::SubmissionSerial>::success(serial);
}

gpu::SubmissionSerial D3D12Device::completed_serial() const noexcept {
    auto current = impl_->pending.begin();
    while (current != impl_->pending.end()) {
        const auto& queue = impl_->queues.at(current->second);
        const UINT64 fence_value = queue.fence->GetCompletedValue();
        if (fence_value == UINT64_MAX) {
            const HRESULT reason = impl_->device->GetDeviceRemovedReason();
            if (FAILED(reason) && is_device_removed_hresult(reason)) {
                impl_->record_device_lost("ID3D12Device::GetDeviceRemovedReason", reason);
            }
            break;
        }
        if (fence_value < current->first) break;
        impl_->completed = current->first;
        current = impl_->pending.erase(current);
    }
    static_cast<void>(impl_->deferred_destruction.collect(impl_->completed));
    impl_->capture_debug_messages();
    return impl_->completed;
}

core::Result<void> D3D12Device::wait(gpu::SubmissionSerial serial) {
    if (impl_->device_lost) return core::Result<void>::failure(device_lost_failure());
    if (serial == 0U || serial > impl_->next_serial) {
        return core::Result<void>::failure(Diagnostic(
            ErrorCode::invalid_state, "D3D12 wait requested an unknown submission serial"));
    }
    while (completed_serial() < serial) {
        if (impl_->pending.empty()) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state, "D3D12 submission completion state is inconsistent"));
        }
        const auto pending = impl_->pending.begin();
        auto& queue = impl_->queues.at(pending->second);
        if (FAILED(queue.fence->SetEventOnCompletion(pending->first, queue.event))) {
            const HRESULT reason = impl_->device->GetDeviceRemovedReason();
            if (FAILED(reason) && is_device_removed_hresult(reason)) {
                impl_->record_device_lost("ID3D12Device::GetDeviceRemovedReason", reason);
            }
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state, "D3D12 wait registration failed"));
        }
        const DWORD wait_status = WaitForSingleObject(queue.event, INFINITE);
        if (wait_status != WAIT_OBJECT_0) {
            return core::Result<void>::failure(Diagnostic(
                ErrorCode::invalid_state, "D3D12 wait event did not signal"));
        }
    }
    return core::Result<void>::success();
}

const std::vector<std::string>& D3D12Device::debug_receipts() const noexcept {
    return impl_->debug_receipts;
}

bool D3D12Device::device_lost() const noexcept {
    return impl_->device_lost;
}

void D3D12Device::record_external_device_loss(
    const char* operation,
    std::uint32_t native_status) {
    const HRESULT status = static_cast<HRESULT>(native_status);
    if (is_device_removed_hresult(status)) {
        impl_->record_device_lost(operation, status);
    }
}

void* D3D12Device::native_device_handle() const noexcept {
    return impl_->device.Get();
}

void* D3D12Device::native_graphics_queue_handle() const noexcept {
    const auto queue = impl_->queues.find(gpu::QueueType::graphics);
    return queue == impl_->queues.end() ? nullptr : queue->second.queue.Get();
}

core::Result<std::unique_ptr<D3D12Device>> D3D12Device::recover() const {
    if (!impl_->device_lost) {
        return core::Result<std::unique_ptr<D3D12Device>>::failure(Diagnostic(
            ErrorCode::invalid_state,
            "D3D12 recovery is only valid after a recorded device-removal event"));
    }
    return create(impl_->options);
}

} // namespace carto::d3d12
