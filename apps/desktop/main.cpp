#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#define IMGUI_DEFINE_MATH_OPERATORS

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <vulkan/vulkan.h>

#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "branding.hpp"

// Dear ImGui intentionally keeps the Win32 message handler declaration behind
// a disabled block in its public backend header to avoid pulling in windows.h.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param);

#include <carto/application/application.hpp>
#include <carto/condition/wave_import.hpp>
#include <carto/editor/snap_service.hpp>
#include <carto/editor/viewport_interaction.hpp>
#include <carto/io/builtin_provider.hpp>
#include <carto/io/gltf_export.hpp>
#include <carto/io/obj.hpp>
#include <carto/io/ply.hpp>
#include <carto/io/stl.hpp>
#include <carto/production/preparation_job.hpp>
#include <carto/production/preparation_operation.hpp>
#include <carto/production/preparation_scope.hpp>
#include <carto/providers/registry.hpp>
#include <carto/terrain/terrain_tile.hpp>
#include <carto/ui/ui.hpp>
#include <carto/vulkan/runtime.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using carto::application::ApplicationSession;
using carto::application::ApplicationSnapshot;
using carto::application::SelectEdgeAction;
using carto::application::SelectFaceAction;
using carto::application::SelectObjectAction;
using carto::application::SelectVertexAction;
using carto::application::SetObjectTransformAction;
using carto::application::SetSelectionModeAction;
using carto::application::UndoAction;
using carto::application::RedoAction;
using carto::core::Vec3d;
using carto::ui::UiSnapshot;

[[nodiscard]] std::optional<carto::project::MaterialGraphValueType>
material_graph_input_type(
    const carto::project::MaterialGraphNode& node,
    std::size_t input_index) noexcept {
    using carto::project::MaterialGraphNodeKind;
    using carto::project::MaterialGraphValueType;
    switch (node.kind) {
    case MaterialGraphNodeKind::texture_sample:
        if (input_index < 2U) return MaterialGraphValueType::scalar;
        break;
    case MaterialGraphNodeKind::add:
    case MaterialGraphNodeKind::multiply:
        if (input_index < 2U) return node.value_type;
        break;
    case MaterialGraphNodeKind::mix:
        if (input_index < 2U) return node.value_type;
        if (input_index == 2U) return MaterialGraphValueType::scalar;
        break;
    case MaterialGraphNodeKind::constant_scalar:
    case MaterialGraphNodeKind::constant_color:
    case MaterialGraphNodeKind::parameter_scalar:
    case MaterialGraphNodeKind::parameter_color:
    case MaterialGraphNodeKind::surface_input:
    case MaterialGraphNodeKind::unsupported:
        break;
    }
    return std::nullopt;
}

[[nodiscard]] const char* material_graph_value_type_name(
    carto::project::MaterialGraphValueType type) noexcept {
    switch (type) {
    case carto::project::MaterialGraphValueType::scalar: return "scalar";
    case carto::project::MaterialGraphValueType::color: return "color";
    case carto::project::MaterialGraphValueType::normal: return "normal";
    }
    return "unknown";
}

[[nodiscard]] std::wstring wide(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

[[nodiscard]] std::optional<std::string> system_font_path(const wchar_t* file_name) {
    std::array<wchar_t, MAX_PATH> windows_directory{};
    const UINT directory_length = GetWindowsDirectoryW(
        windows_directory.data(), static_cast<UINT>(windows_directory.size()));
    if (directory_length == 0U || directory_length >= windows_directory.size()) return std::nullopt;

    std::wstring path(windows_directory.data(), directory_length);
    if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
    path += L"Fonts\\";
    path += file_name;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        return std::nullopt;
    }

    const int utf8_length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), static_cast<int>(path.size()),
        nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0) return std::nullopt;
    std::string utf8_path(static_cast<std::size_t>(utf8_length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), static_cast<int>(path.size()),
            utf8_path.data(), utf8_length, nullptr, nullptr) != utf8_length) {
        return std::nullopt;
    }
    return utf8_path;
}

[[nodiscard]] std::optional<std::string> preferred_system_font_path(
    std::initializer_list<const wchar_t*> file_names) {
    for (const wchar_t* file_name : file_names) {
        if (const auto path = system_font_path(file_name); path.has_value()) return path;
    }
    return std::nullopt;
}

void check_vk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with Vulkan result " +
                                 std::to_string(static_cast<int>(result)));
    }
}

class VulkanShell final {
public:
    ~VulkanShell() { shutdown(); }

    void initialize(HWND window) {
        window_ = window;
        create_instance();
        const auto surface_create_info = surface_info();
        check_vk(vkCreateWin32SurfaceKHR(instance_, &surface_create_info, nullptr, &surface_),
                 "vkCreateWin32SurfaceKHR");
        choose_device();
        create_device();
        create_command_objects();
        create_descriptor_pool();
        create_swapchain();

        ImGui_ImplVulkan_InitInfo init_info{};
        init_info.ApiVersion = VK_API_VERSION_1_1;
        init_info.Instance = instance_;
        init_info.PhysicalDevice = physical_device_;
        init_info.Device = device_;
        init_info.QueueFamily = queue_family_;
        init_info.Queue = queue_;
        init_info.DescriptorPool = descriptor_pool_;
        init_info.MinImageCount = image_count_;
        init_info.ImageCount = image_count_;
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.PipelineInfoMain.RenderPass = render_pass_;
        if (!ImGui_ImplVulkan_Init(&init_info)) {
            throw std::runtime_error("Dear ImGui Vulkan backend initialization failed");
        }
        imgui_initialized_ = true;
    }

    void request_resize() noexcept { resize_pending_ = true; }

    void render(ImDrawData* draw_data) {
        if (resize_pending_) {
            recreate_swapchain();
        }
        if (client_width() == 0 || client_height() == 0) return;

        FrameContext& frame = frame_contexts_.at(current_frame_);
        check_vk(vkWaitForFences(device_, 1, &frame.fence, VK_TRUE, UINT64_MAX),
                 "vkWaitForFences frame context");

        std::uint32_t image_index = 0;
        const VkResult acquired = vkAcquireNextImageKHR(
            device_, swapchain_, UINT64_MAX, frame.image_available, VK_NULL_HANDLE, &image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            resize_pending_ = true;
            return;
        }
        if (acquired == VK_SUBOPTIMAL_KHR) resize_pending_ = true;
        check_vk(acquired, "vkAcquireNextImageKHR");
        if (image_fences_.at(image_index) != VK_NULL_HANDLE &&
            image_fences_.at(image_index) != frame.fence) {
            check_vk(vkWaitForFences(
                         device_, 1, &image_fences_.at(image_index), VK_TRUE, UINT64_MAX),
                     "vkWaitForFences swapchain image");
        }
        image_fences_.at(image_index) = frame.fence;
        check_vk(vkResetFences(device_, 1, &frame.fence), "vkResetFences");

        check_vk(vkResetCommandBuffer(frame.command_buffer, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        check_vk(vkBeginCommandBuffer(frame.command_buffer, &begin), "vkBeginCommandBuffer");

        VkClearValue clear_value{};
        clear_value.color.float32[0] = 0.035F;
        clear_value.color.float32[1] = 0.045F;
        clear_value.color.float32[2] = 0.060F;
        clear_value.color.float32[3] = 1.0F;
        const std::array<VkClearValue, 1> clear{clear_value};
        VkRenderPassBeginInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = render_pass_;
        pass.framebuffer = framebuffers_.at(image_index);
        pass.renderArea.offset = {0, 0};
        pass.renderArea.extent = {swapchain_width_, swapchain_height_};
        pass.clearValueCount = static_cast<std::uint32_t>(clear.size());
        pass.pClearValues = clear.data();
        vkCmdBeginRenderPass(frame.command_buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(draw_data, frame.command_buffer);
        vkCmdEndRenderPass(frame.command_buffer);
        check_vk(vkEndCommandBuffer(frame.command_buffer), "vkEndCommandBuffer");

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.image_available;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &frame.command_buffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &frame.render_finished;
        check_vk(vkQueueSubmit(queue_, 1, &submit, frame.fence), "vkQueueSubmit");

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &frame.render_finished;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_index;
        const VkResult presented = vkQueuePresentKHR(queue_, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            resize_pending_ = true;
        } else {
            check_vk(presented, "vkQueuePresentKHR");
        }
        frame.frame_id = next_frame_id_++;
        current_frame_ = (current_frame_ + 1U) % frame_contexts_.size();
    }

private:
    struct FrameContext {
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore image_available = VK_NULL_HANDLE;
        VkSemaphore render_finished = VK_NULL_HANDLE;
        std::uint64_t frame_id = 0U;
    };

    static constexpr std::size_t kFrameContextCount = 3U;

    enum class PresentPolicy {
        performance,
        vsync,
        immediate,
    };

    [[nodiscard]] static PresentPolicy present_policy_from_environment() noexcept {
        const char* requested = std::getenv("CARTO_PRESENT_MODE");
        if (requested == nullptr) return PresentPolicy::performance;
        const std::string_view value(requested);
        if (value == "fifo" || value == "vsync") return PresentPolicy::vsync;
        if (value == "immediate" || value == "debug") return PresentPolicy::immediate;
        return PresentPolicy::performance;
    }

    [[nodiscard]] VkPresentModeKHR select_present_mode(
        const std::vector<VkPresentModeKHR>& supported) const {
        const auto has = [&supported](VkPresentModeKHR mode) {
            return std::find(supported.begin(), supported.end(), mode) != supported.end();
        };
        switch (present_policy_) {
        case PresentPolicy::performance:
            if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
            return VK_PRESENT_MODE_FIFO_KHR;
        case PresentPolicy::vsync:
            return VK_PRESENT_MODE_FIFO_KHR;
        case PresentPolicy::immediate:
            if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
            return VK_PRESENT_MODE_FIFO_KHR;
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    [[nodiscard]] VkWin32SurfaceCreateInfoKHR surface_info() const noexcept {
        VkWin32SurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hinstance = GetModuleHandleW(nullptr);
        info.hwnd = window_;
        return info;
    }

    [[nodiscard]] std::uint32_t client_width() const noexcept {
        RECT rect{};
        GetClientRect(window_, &rect);
        return static_cast<std::uint32_t>(std::max<LONG>(0, rect.right - rect.left));
    }

    [[nodiscard]] std::uint32_t client_height() const noexcept {
        RECT rect{};
        GetClientRect(window_, &rect);
        return static_cast<std::uint32_t>(std::max<LONG>(0, rect.bottom - rect.top));
    }

    void create_instance() {
        const std::array<const char*, 2> extensions{
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Cartographer";
        app.applicationVersion = 1;
        app.pEngineName = "Cartographer";
        app.engineVersion = 1;
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        check_vk(vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
    }

    void choose_device() {
        std::uint32_t count = 0;
        check_vk(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
        if (count == 0U) throw std::runtime_error("Vulkan reported no physical devices");
        std::vector<VkPhysicalDevice> devices(count);
        check_vk(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices");
        for (VkPhysicalDevice device : devices) {
            std::uint32_t extension_count = 0U;
            check_vk(vkEnumerateDeviceExtensionProperties(
                         device, nullptr, &extension_count, nullptr),
                     "vkEnumerateDeviceExtensionProperties");
            std::vector<VkExtensionProperties> extensions(extension_count);
            check_vk(vkEnumerateDeviceExtensionProperties(
                         device, nullptr, &extension_count, extensions.data()),
                     "vkEnumerateDeviceExtensionProperties");
            const bool supports_swapchain = std::any_of(
                extensions.begin(), extensions.end(), [](const VkExtensionProperties& extension) {
                    return std::string_view(extension.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME;
                });
            if (!supports_swapchain) continue;

            std::uint32_t queue_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queue_count);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, queues.data());
            for (std::uint32_t index = 0; index < queue_count; ++index) {
                VkBool32 present = VK_FALSE;
                check_vk(vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface_, &present),
                         "vkGetPhysicalDeviceSurfaceSupportKHR");
                if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U && present == VK_TRUE) {
                    physical_device_ = device;
                    queue_family_ = index;
                    return;
                }
            }
        }
        throw std::runtime_error("no Vulkan device exposes a graphics/present queue");
    }

    void create_device() {
        const float priority = 1.0F;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        const std::array<const char*, 1> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue_info;
        info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        check_vk(vkCreateDevice(physical_device_, &info, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    }

    void create_render_pass() {
        VkAttachmentDescription color{};
        color.format = swapchain_format_;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference reference{};
        reference.attachment = 0;
        reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments = &color;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        check_vk(vkCreateRenderPass(device_, &info, nullptr, &render_pass_), "vkCreateRenderPass");
        render_pass_format_ = swapchain_format_;
    }

    void create_command_objects() {
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queue_family_;
        check_vk(vkCreateCommandPool(device_, &pool, nullptr, &command_pool_), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = command_pool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = static_cast<std::uint32_t>(kFrameContextCount);
        std::array<VkCommandBuffer, kFrameContextCount> command_buffers{};
        check_vk(vkAllocateCommandBuffers(device_, &allocate, command_buffers.data()),
                 "vkAllocateCommandBuffers");

        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        frame_contexts_.resize(kFrameContextCount);
        for (std::size_t index = 0U; index < frame_contexts_.size(); ++index) {
            auto& frame = frame_contexts_[index];
            frame.command_buffer = command_buffers[index];
            check_vk(vkCreateSemaphore(device_, &semaphore, nullptr, &frame.image_available),
                     "vkCreateSemaphore image_available");
            check_vk(vkCreateSemaphore(device_, &semaphore, nullptr, &frame.render_finished),
                     "vkCreateSemaphore render_finished");
            check_vk(vkCreateFence(device_, &fence, nullptr, &frame.fence),
                     "vkCreateFence frame context");
        }
    }

    void create_descriptor_pool() {
        const std::array<VkDescriptorPoolSize, 11> sizes{{
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
        }};
        VkDescriptorPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = 1000U * static_cast<std::uint32_t>(sizes.size());
        info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        info.pPoolSizes = sizes.data();
        check_vk(vkCreateDescriptorPool(device_, &info, nullptr, &descriptor_pool_), "vkCreateDescriptorPool");
    }

    void select_swapchain_configuration() {
        VkSurfaceCapabilitiesKHR capabilities{};
        check_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities),
                 "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        std::uint32_t format_count = 0;
        check_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr),
                 "vkGetPhysicalDeviceSurfaceFormatsKHR");
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        check_vk(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data()),
                 "vkGetPhysicalDeviceSurfaceFormatsKHR");
        if (formats.empty()) throw std::runtime_error("Vulkan surface reported no swapchain formats");
        std::uint32_t present_mode_count = 0U;
        check_vk(vkGetPhysicalDeviceSurfacePresentModesKHR(
                     physical_device_, surface_, &present_mode_count, nullptr),
                 "vkGetPhysicalDeviceSurfacePresentModesKHR");
        std::vector<VkPresentModeKHR> present_modes(present_mode_count);
        check_vk(vkGetPhysicalDeviceSurfacePresentModesKHR(
                     physical_device_, surface_, &present_mode_count, present_modes.data()),
                 "vkGetPhysicalDeviceSurfacePresentModesKHR");
        swapchain_present_mode_ = select_present_mode(present_modes);
        swapchain_pre_transform_ = capabilities.currentTransform;
        VkSurfaceFormatKHR selected_format{};
        constexpr std::array<VkFormat, 4> preferred_formats{
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_B8G8R8A8_SRGB,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R8G8B8A8_SRGB,
        };
        for (const VkFormat preferred : preferred_formats) {
            const auto format = std::find_if(
                formats.begin(), formats.end(), [preferred](const VkSurfaceFormatKHR& candidate) {
                    return candidate.format == preferred;
                });
            if (format != formats.end()) {
                selected_format = *format;
                break;
            }
        }
        if (selected_format.format == VK_FORMAT_UNDEFINED) {
            selected_format = formats.front();
        }
        swapchain_format_ = selected_format.format;
        swapchain_color_space_ = selected_format.colorSpace;
        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
            extent = {client_width(), client_height()};
        }
        extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        swapchain_width_ = extent.width;
        swapchain_height_ = extent.height;
        image_count_ = capabilities.minImageCount + 1U;
        if (capabilities.maxImageCount != 0U) image_count_ = std::min(image_count_, capabilities.maxImageCount);
        if (image_count_ < 2U) {
            throw std::runtime_error("desktop shell requires at least two swapchain images");
        }
    }

    void create_swapchain() {
        select_swapchain_configuration();
        if (render_pass_ == VK_NULL_HANDLE || render_pass_format_ != swapchain_format_) {
            if (render_pass_ != VK_NULL_HANDLE) {
                vkDestroyRenderPass(device_, render_pass_, nullptr);
                render_pass_ = VK_NULL_HANDLE;
            }
            create_render_pass();
        }

        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface_;
        info.minImageCount = image_count_;
        info.imageFormat = swapchain_format_;
        info.imageColorSpace = swapchain_color_space_;
        info.imageExtent = {swapchain_width_, swapchain_height_};
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = swapchain_pre_transform_;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = swapchain_present_mode_;
        info.clipped = VK_TRUE;
        check_vk(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR");

        std::uint32_t actual_count = 0;
        check_vk(vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, nullptr), "vkGetSwapchainImagesKHR");
        images_.resize(actual_count);
        check_vk(vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, images_.data()), "vkGetSwapchainImagesKHR");
        image_views_.resize(images_.size());
        framebuffers_.resize(images_.size());
        image_fences_.assign(images_.size(), VK_NULL_HANDLE);
        for (std::size_t index = 0; index < images_.size(); ++index) {
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = images_[index];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = swapchain_format_;
            view.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = 1;
            view.subresourceRange.layerCount = 1;
            check_vk(vkCreateImageView(device_, &view, nullptr, &image_views_[index]), "vkCreateImageView");
            VkFramebufferCreateInfo framebuffer{};
            framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer.renderPass = render_pass_;
            framebuffer.attachmentCount = 1;
            framebuffer.pAttachments = &image_views_[index];
            framebuffer.width = swapchain_width_;
            framebuffer.height = swapchain_height_;
            framebuffer.layers = 1;
            check_vk(vkCreateFramebuffer(device_, &framebuffer, nullptr, &framebuffers_[index]), "vkCreateFramebuffer");
        }
        resize_pending_ = false;
    }

    void recreate_swapchain() {
        if (client_width() == 0 || client_height() == 0) return;
        check_vk(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle resize");
        for (VkFramebuffer framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
        for (VkImageView view : image_views_) vkDestroyImageView(device_, view, nullptr);
        framebuffers_.clear();
        image_views_.clear();
        images_.clear();
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
        create_swapchain();
        ImGui_ImplVulkan_SetMinImageCount(image_count_);
    }

public:
    void shutdown() noexcept {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            if (imgui_initialized_) {
                ImGui_ImplVulkan_Shutdown();
                imgui_initialized_ = false;
            }
            for (VkFramebuffer framebuffer : framebuffers_) vkDestroyFramebuffer(device_, framebuffer, nullptr);
            for (VkImageView view : image_views_) vkDestroyImageView(device_, view, nullptr);
            if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            if (descriptor_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
            for (auto& frame : frame_contexts_) {
                if (frame.fence != VK_NULL_HANDLE) vkDestroyFence(device_, frame.fence, nullptr);
                if (frame.image_available != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device_, frame.image_available, nullptr);
                }
                if (frame.render_finished != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device_, frame.render_finished, nullptr);
                }
            }
            if (command_pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, command_pool_, nullptr);
            if (render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, render_pass_, nullptr);
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
        frame_contexts_.clear();
        image_fences_.clear();
    }

private:
    HWND window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR swapchain_color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR swapchain_present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkSurfaceTransformFlagBitsKHR swapchain_pre_transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::uint32_t swapchain_width_ = 0;
    std::uint32_t swapchain_height_ = 0;
    std::uint32_t image_count_ = 2;
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkFormat render_pass_format_ = VK_FORMAT_UNDEFINED;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<FrameContext> frame_contexts_;
    std::vector<VkFence> image_fences_;
    std::size_t current_frame_ = 0U;
    std::uint64_t next_frame_id_ = 0U;
    PresentPolicy present_policy_ = present_policy_from_environment();
    bool imgui_initialized_ = false;
    bool resize_pending_ = false;
};

struct DesktopState;
DesktopState* g_state = nullptr;

enum class ShellSurface {
    workspace,
    project,
    market,
    diagnostics,
    settings,
};

enum class WorkbenchFlow {
    focus,
    author,
    inspect,
    draft,
    custom,
};

[[nodiscard]] const char* workbench_flow_name(WorkbenchFlow flow) noexcept {
    switch (flow) {
    case WorkbenchFlow::focus: return "Focus";
    case WorkbenchFlow::author: return "Author";
    case WorkbenchFlow::inspect: return "Inspect";
    case WorkbenchFlow::draft: return "Draft";
    case WorkbenchFlow::custom: return "Custom";
    }
    return "Custom";
}

[[nodiscard]] std::optional<std::size_t> workbench_flow_memory_index(
    WorkbenchFlow flow) noexcept {
    switch (flow) {
    case WorkbenchFlow::focus: return 0U;
    case WorkbenchFlow::author: return 1U;
    case WorkbenchFlow::inspect: return 2U;
    case WorkbenchFlow::draft: return 3U;
    case WorkbenchFlow::custom: return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] WorkbenchFlow workbench_flow_from_memory_index(std::int32_t index) noexcept {
    switch (index) {
    case 0: return WorkbenchFlow::focus;
    case 1: return WorkbenchFlow::author;
    case 2: return WorkbenchFlow::inspect;
    case 3: return WorkbenchFlow::draft;
    default: return WorkbenchFlow::custom;
    }
}

[[nodiscard]] const char* workbench_flow_shortcut(WorkbenchFlow flow) noexcept {
    switch (flow) {
    case WorkbenchFlow::focus: return "Ctrl+Shift+1";
    case WorkbenchFlow::author: return "Ctrl+Shift+2";
    case WorkbenchFlow::inspect: return "Ctrl+Shift+3";
    case WorkbenchFlow::draft: return "Ctrl+Shift+4";
    case WorkbenchFlow::custom: return "";
    }
    return "";
}

struct WorkbenchState {
    bool scene = false;
    bool assets = false;
    bool layers = false;
    bool tools = false;
    bool references = false;
    bool draft = false;
    bool inspector = false;
    bool material = false;
    bool constraint = false;
    bool modify = false;
    bool measure = false;
    bool transform = false;
    bool extrude = false;
    bool ledger_collapsed = true;
    carto::ui::WorkbenchPreferences persisted;
    std::array<bool, carto::ui::kWorkbenchInstrumentCount> seeded{};
};

[[nodiscard]] std::size_t workbench_index(carto::ui::WorkbenchInstrument instrument) noexcept {
    return static_cast<std::size_t>(instrument);
}

[[nodiscard]] bool& workbench_visible(
    WorkbenchState& state, carto::ui::WorkbenchInstrument instrument) noexcept {
    switch (instrument) {
    case carto::ui::WorkbenchInstrument::scene: return state.scene;
    case carto::ui::WorkbenchInstrument::assets: return state.assets;
    case carto::ui::WorkbenchInstrument::layers: return state.layers;
    case carto::ui::WorkbenchInstrument::tools: return state.tools;
    case carto::ui::WorkbenchInstrument::references: return state.references;
    case carto::ui::WorkbenchInstrument::draft: return state.draft;
    case carto::ui::WorkbenchInstrument::inspector: return state.inspector;
    case carto::ui::WorkbenchInstrument::material: return state.material;
    case carto::ui::WorkbenchInstrument::constraint: return state.constraint;
    case carto::ui::WorkbenchInstrument::modify: return state.modify;
    case carto::ui::WorkbenchInstrument::measure: return state.measure;
    case carto::ui::WorkbenchInstrument::transform: return state.transform;
    case carto::ui::WorkbenchInstrument::extrude: return state.extrude;
    }
    return state.scene;
}

[[nodiscard]] bool point_in_triangle(ImVec2 point, ImVec2 a, ImVec2 b, ImVec2 c) {
    const float ab = (point.x - b.x) * (a.y - b.y) - (point.y - b.y) * (a.x - b.x);
    const float bc = (point.x - c.x) * (b.y - c.y) - (point.y - c.y) * (b.x - c.x);
    const float ca = (point.x - a.x) * (c.y - a.y) - (point.y - a.y) * (c.x - a.x);
    return (ab >= 0.0F && bc >= 0.0F && ca >= 0.0F) ||
           (ab <= 0.0F && bc <= 0.0F && ca <= 0.0F);
}

[[nodiscard]] float point_segment_distance_squared(ImVec2 point, ImVec2 a, ImVec2 b) {
    const ImVec2 delta = b - a;
    const float length_squared = delta.x * delta.x + delta.y * delta.y;
    if (length_squared <= 1e-6F) {
        const ImVec2 offset = point - a;
        return offset.x * offset.x + offset.y * offset.y;
    }
    const ImVec2 offset = point - a;
    const float projection = std::clamp(
        (offset.x * delta.x + offset.y * delta.y) / length_squared, 0.0F, 1.0F);
    const ImVec2 closest = a + delta * projection;
    const ImVec2 distance = point - closest;
    return distance.x * distance.x + distance.y * distance.y;
}

struct ViewportFaceHit {
    carto::scene::ObjectId object;
    carto::geometry::FaceId face;
    std::array<ImVec2, 3> points;
};

struct ViewportVertexHit {
    carto::scene::ObjectId object;
    carto::geometry::VertexId vertex;
    ImVec2 point;
};

struct ViewportEdgeHit {
    carto::scene::ObjectId object;
    carto::geometry::EdgeId edge;
    ImVec2 first;
    ImVec2 second;
};

struct NativeTranslateGizmo {
    carto::scene::ObjectId object;
    ImVec2 origin;
    ImVec2 x_tip;
    ImVec2 z_tip;
    double pixels_per_world_x = 0.0;
    double pixels_per_world_z = 0.0;
    carto::core::Transform transform = carto::core::Transform::identity();
    carto::core::Revision project_revision;
};

struct NativeTransformDragCapture {
    carto::editor::ViewportTransformSession session;
    carto::editor::SnapSession geometry_snap_session;
    std::optional<carto::editor::SnapCandidate> geometry_snap_candidate;
    carto::editor::ViewportTranslateAxis axis =
        carto::editor::ViewportTranslateAxis::world_x;
    ImRect viewport;
    carto::core::Transform initial_transform = carto::core::Transform::identity();
    carto::core::Transform preview_transform = carto::core::Transform::identity();
    bool agent_ghost = false;
    std::string agent_task_id;
    carto::core::Revision agent_branch_revision;
};

enum class NativeConditionGizmoKind {
    audio_minimum_distance,
    audio_maximum_distance,
    acoustic_sphere_radius,
    acoustic_box_corner,
};

struct NativeConditionGizmoHandle {
    NativeConditionGizmoKind kind = NativeConditionGizmoKind::audio_maximum_distance;
    std::uint64_t record_id = 0U;
    ImVec2 center{};
    ImVec2 handle{};
    carto::core::Transform owner_world = carto::core::Transform::identity();
    carto::core::Transform local_transform = carto::core::Transform::identity();
    double minimum_distance = 0.0;
    double maximum_distance = 0.0;
    double radius = 0.0;
    carto::core::Vec3d half_extents{};
    carto::core::Revision authoring_revision;
};

struct NativeConditionGizmoDrag {
    NativeConditionGizmoHandle handle;
    carto::core::Revision project_revision;
    ImRect viewport;
    double preview_minimum = 0.0;
    double preview_maximum = 0.0;
    double preview_radius = 0.0;
    carto::core::Vec3d preview_half_extents{};
};

constexpr std::size_t kMaxNativeSculptTriangles = 65'536U;
constexpr std::size_t kMaxNativeSculptCursorPoints = 8'192U;

struct NativeViewportHitCache {
    carto::core::Revision project_revision;
    ImVec2 screen_origin{};
    ImVec2 extent{};
    bool valid = false;
    bool interactive = false;
    bool sculpt_triangles_truncated = false;
    std::vector<ViewportFaceHit> faces;
    std::vector<carto::ui::SculptViewportTriangle> sculpt_triangles;
    std::vector<ViewportVertexHit> vertices;
    std::vector<ViewportEdgeHit> edges;
    std::vector<std::pair<carto::scene::ObjectId, ImRect>> objects;
    std::optional<NativeTranslateGizmo> translate_gizmo;
    std::vector<NativeConditionGizmoHandle> condition_gizmos;

    void reset() {
        project_revision = {};
        screen_origin = {};
        extent = {};
        valid = false;
        interactive = false;
        sculpt_triangles_truncated = false;
        faces.clear();
        sculpt_triangles.clear();
        vertices.clear();
        edges.clear();
        objects.clear();
        translate_gizmo.reset();
        condition_gizmos.clear();
    }
};

struct NativeSculptStrokeCapture {
    carto::core::Revision project_revision;
    carto::scene::ObjectId object;
    std::uint64_t mesh_asset = 0U;
    carto::production::SculptStrokeMode mode = carto::production::SculptStrokeMode::add;
    double radius = 0.25;
    double strength = 0.05;
    ImRect viewport;
    std::vector<carto::ui::SculptViewportTriangle> triangles;
    std::vector<carto::core::Vec2d> screen_path;
};

[[nodiscard]] Vec3d world_point(const carto::core::Transform& transform, Vec3d point) {
    return transform.translation +
           transform.rotation.rotate(carto::core::componentwise_multiply(transform.scale, point));
}

enum class FileDialogKind {
    project,
    agent_checkpoint,
    agent_demonstration,
    agent_demonstration_trace,
    agent_demonstration_recipe,
    engineering,
    engineering_authoring,
    condition,
    condition_authoring,
    audio_clip,
    market_package,
    mesh_import,
    gltf,
    obj,
    ply,
    stl,
};

enum class StandardMeshExport {
    obj,
    ply,
    stl,
};

[[nodiscard]] std::optional<std::filesystem::path> choose_file(
    bool save, FileDialogKind kind = FileDialogKind::project) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    switch (kind) {
    case FileDialogKind::market_package:
        dialog.lpstrFilter =
            L"Cartographer Market package (*.cartomarket;*.cartoplug;*.zip)\0"
            L"*.cartomarket;*.cartoplug;*.zip\0"
            L"Cartographer Market directory manifest (manifest.json)\0manifest.json\0"
            L"All Files\0*.*\0";
        break;
    case FileDialogKind::mesh_import:
        dialog.lpstrFilter =
            L"Supported Meshes (*.obj;*.ply;*.stl)\0*.obj;*.ply;*.stl\0"
            L"Wavefront OBJ (*.obj)\0*.obj\0"
            L"PLY (*.ply)\0*.ply\0"
            L"STL (*.stl)\0*.stl\0"
            L"All Files\0*.*\0";
        break;
    case FileDialogKind::gltf:
        dialog.lpstrFilter = L"glTF export (*.gltf)\0*.gltf\0All Files\0*.*\0";
        dialog.lpstrDefExt = L"gltf";
        break;
    case FileDialogKind::obj:
        dialog.lpstrFilter = L"Wavefront OBJ (*.obj)\0*.obj\0All Files\0*.*\0";
        dialog.lpstrDefExt = L"obj";
        break;
    case FileDialogKind::ply:
        dialog.lpstrFilter = L"ASCII PLY (*.ply)\0*.ply\0All Files\0*.*\0";
        dialog.lpstrDefExt = L"ply";
        break;
    case FileDialogKind::stl:
        dialog.lpstrFilter = L"Binary STL (*.stl)\0*.stl\0All Files\0*.*\0";
        dialog.lpstrDefExt = L"stl";
        break;
    case FileDialogKind::project:
        dialog.lpstrFilter = L"Cartographer Project (*.carto)\0*.carto\0All Files\0*.*\0";
        dialog.lpstrDefExt = L"carto";
        break;
    case FileDialogKind::agent_checkpoint:
        dialog.lpstrFilter =
            L"Cartographer Agent Task (*.carto-agent-task)\0*.carto-agent-task\0"
            L"All Files\0*.*\0";
        dialog.lpstrDefExt = L"carto-agent-task";
        break;
    case FileDialogKind::agent_demonstration:
        dialog.lpstrFilter =
            L"Cartographer Agent Demonstration (*.carto-agent-demo)\0*.carto-agent-demo\0"
            L"All Files\0*.*\0";
        dialog.lpstrDefExt = L"carto-agent-demo";
        break;
    case FileDialogKind::agent_demonstration_trace:
        dialog.lpstrFilter =
            L"Cartographer Agent Demonstration Trace (*.carto-agent-trace)\0"
            L"*.carto-agent-trace\0All Files\0*.*\0";
        dialog.lpstrDefExt = L"carto-agent-trace";
        break;
    case FileDialogKind::agent_demonstration_recipe:
        dialog.lpstrFilter =
            L"Cartographer Demonstration Recipe (*.carto-agent-recipe)\0"
            L"*.carto-agent-recipe\0All Files\0*.*\0";
        dialog.lpstrDefExt = L"carto-agent-recipe";
        break;
    case FileDialogKind::engineering:
        dialog.lpstrFilter =
            L"Cartographer Inspection Package (*.carto-inspection)\0*.carto-inspection\0"
            L"All Files\0*.*\0";
        dialog.lpstrDefExt = L"carto-inspection";
        break;
    case FileDialogKind::engineering_authoring:
        dialog.lpstrFilter =
            L"Cartographer Engineering Authoring (*.carto-engineering-authoring)\0"
            L"*.carto-engineering-authoring\0All Files\0*.*\0";
        dialog.lpstrDefExt = L"carto-engineering-authoring";
        break;
    case FileDialogKind::condition:
        dialog.lpstrFilter =
            L"Cartographer CONDITION Package (*.carto-condition)\0*.carto-condition\0"
            L"All Files\0*.*\0";
        dialog.lpstrDefExt = L"carto-condition";
        break;
    case FileDialogKind::condition_authoring:
        dialog.lpstrFilter =
            L"Cartographer CONDITION Authoring (*.carto-condition-authoring)\0"
            L"*.carto-condition-authoring\0All Files\0*.*\0";
        dialog.lpstrDefExt = L"carto-condition-authoring";
        break;
    case FileDialogKind::audio_clip:
        dialog.lpstrFilter = L"WAVE audio clip (*.wav;*.wave)\0*.wav;*.wave\0All Files\0*.*\0";
        break;
    }
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_PATHMUSTEXIST | (save
        ? ((kind == FileDialogKind::agent_checkpoint || kind == FileDialogKind::agent_demonstration) ? 0U : OFN_OVERWRITEPROMPT)
        : OFN_FILEMUSTEXIST);
    const BOOL selected = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    if (!selected) return std::nullopt;
    return std::filesystem::path(path);
}

[[nodiscard]] std::optional<std::filesystem::path> user_preference_path(bool create_parent) {
    std::array<wchar_t, 32768> local_app_data{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data.data(), static_cast<DWORD>(local_app_data.size()));
    if (length == 0U || length >= local_app_data.size()) return std::nullopt;
    std::filesystem::path directory(std::wstring(local_app_data.data(), length));
    directory /= L"Cartographer";
    if (create_parent) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return std::nullopt;
    }
    return directory / L"workspace.prefs";
}

[[nodiscard]] std::optional<std::filesystem::path> workbench_preference_path(bool create_parent) {
    const auto workspace_path = user_preference_path(create_parent);
    if (!workspace_path.has_value()) return std::nullopt;
    auto path = *workspace_path;
    path.replace_filename(L"workbench.prefs");
    return path;
}

[[nodiscard]] std::optional<std::filesystem::path> market_library_path(bool create_parent) {
    const auto workspace_path = user_preference_path(create_parent);
    if (!workspace_path.has_value()) return std::nullopt;
    auto directory = workspace_path->parent_path() / L"market-library";
    if (create_parent) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return std::nullopt;
    }
    return directory;
}

struct DesktopState {
    ApplicationSession session;
    carto::ui::UiController ui;
    VulkanShell renderer;
    ~DesktopState() { stop_condition_audio_audition(); }
    HWND window = nullptr;
    ShellSurface surface = ShellSurface::workspace;
    WorkbenchFlow workbench_flow = WorkbenchFlow::focus;
    WorkbenchState workbench;
    bool renderer_ready = false;
    std::optional<carto::ui::Theme> applied_window_theme;
    ImFont* body_font = nullptr;
    ImFont* heading_font = nullptr;
    ImFont* mono_font = nullptr;
    ImVec2 workbench_origin{};
    ImVec2 workbench_extent{};
    bool workbench_bounds_valid = false;
    NativeViewportHitCache native_viewport_hits;
    carto::editor::WorkSurfaceCamera work_surface_camera;
    bool work_surface_camera_framed = false;
    bool work_surface_camera_panning = false;
    ImVec2 work_surface_camera_last_pointer{};
    std::optional<NativeSculptStrokeCapture> native_sculpt_stroke;
    std::optional<NativeTransformDragCapture> native_transform_drag;
    std::optional<NativeConditionGizmoDrag> native_condition_gizmo_drag;
    carto::scene::ObjectId inspector_object{};
    carto::core::Transform inspector_transform = carto::core::Transform::identity();
    carto::geometry::VertexId inspector_vertex{};
    Vec3d inspector_vertex_position{};
    carto::render::MaterialId selected_material_id = 0U;
    std::uint32_t material_region_id = 1U;
    carto::core::Revision material_draft_revision{};
    carto::render::StandardMaterial material_draft;
    bool material_draft_loaded = false;
    bool material_draft_dirty = false;
    std::array<char, 257> material_graph_identity_buffer{};
    std::string selected_material_graph_identity;
    carto::project::MaterialGraph material_graph_draft;
    carto::core::Revision material_graph_draft_project_revision{};
    std::optional<carto::assets::Sha256Digest> material_graph_draft_digest;
    std::optional<carto::project::MaterialGraphCompileReport> material_graph_compile_report;
    std::uint64_t selected_material_graph_node = 0U;
    bool material_graph_draft_loaded = false;
    bool material_graph_draft_dirty = false;
    int material_graph_new_node_kind = 0;
    int material_graph_new_node_type = 0;
    int material_graph_new_surface_input = 0;
    std::array<char, 129> material_graph_new_parameter_name{
        'n', 'e', 'w', '.', 'p', 'a', 'r', 'a', 'm', 'e', 't', 'e', 'r', '\0'};
    std::array<char, 129> material_graph_parameter_draft{};
    std::uint64_t material_graph_parameter_node = 0U;
    std::string material_graph_parameter_identity;
    bool material_graph_parameter_draft_loaded = false;
    std::string material_graph_edit_diagnostic;
    std::array<char, 96> material_name_buffer{
        'N', 'e', 'w', ' ', 'M', 'a', 't', 'e', 'r', 'i', 'a', 'l', '\0'};
    double extrude_distance = 0.25;
    double inset_distance = 0.15;
    int sculpt_mode_index = 0;
    double sculpt_radius = 0.25;
    double sculpt_strength = 0.05;
    double edge_split_factor = 0.5;
    double bevel_width = 0.1;
    int bevel_segments = 1;
    double vertex_slide_factor = 0.5;
    bool remove_orphaned_vertices = true;
    int surface_target_triangles = 6;
    carto::surface::RepresentationMode surface_mode =
        carto::surface::RepresentationMode::fidelity_closure;
    std::optional<carto::application::ApplicationAction> pending_discard_action;
    std::uint64_t inspector_generation = std::numeric_limits<std::uint64_t>::max();
    bool pending_exit = false;
    bool discard_prompt_pending = false;
    bool close_requested = false;
    std::string ai_intent;
    std::optional<carto::ai::Proposal> ai_proposal;
    std::optional<carto::editor::AuthoringPreview> ai_preview;
    std::string ai_feedback;
    std::string agent_goal;
    bool agent_goal_focus_requested = false;
    std::string agent_step;
    bool agent_allow_root_translation = false;
    double agent_translation_distance_meters = 1.0;
    std::string agent_question;
    std::string agent_answer;
    std::string agent_repair_step_id;
    std::string agent_repair_intent;
    std::string agent_repair_justification;
    std::string agent_ghost_justification;
    carto::scene::ObjectId agent_ghost_selected_object{};
    bool agent_ghost_edit_mode = false;
    int agent_clarification_impact = 0;
    int agent_step_dependency = 0;
    int agent_repair_step_index = 0;
    std::string agent_feedback;
    int agent_preview_mode = 0;
    int agent_trace_start_index = 0;
    int agent_trace_end_index = 0;
    std::optional<carto::ui::Workspace> previous_workspace_kind;
    std::string action_feedback;
    bool action_feedback_error = false;
    std::optional<std::uint64_t> engineering_source_update_id;
    std::array<char, 129> engineering_source_revision{};
    std::array<char, 65> engineering_source_digest{};
    std::optional<std::uint64_t> condition_source_update_id;
    std::array<char, 129> condition_source_revision{};
    std::array<char, 65> condition_source_digest{};
    int condition_audio_asset_index = 0;
    int condition_audio_clip_index = 0;
    int condition_audio_bus_index = 1;
    int condition_audio_occlusion_index = 1;
    double condition_audio_gain = 1.0;
    double condition_audio_pitch = 1.0;
    double condition_audio_x = 0.0;
    double condition_audio_y = 0.0;
    double condition_audio_z = 0.0;
    double condition_audio_minimum_distance = 1.0;
    double condition_audio_maximum_distance = 12.0;
    double condition_audio_reverb_send = 0.0;
    int condition_audio_priority = 128;
    bool condition_audio_loop = false;
    bool condition_audio_spatial = true;
    bool condition_audio_enabled = true;
    std::array<char, 129> condition_audio_state{'a','c','t','i','v','e'};
    std::array<char, 129> condition_audio_familiarity_role{};
    std::vector<std::uint8_t> condition_audio_preview_buffer;
    int condition_zone_asset_index = 0;
    int condition_zone_shape_index = 0;
    double condition_zone_center_x = 0.0;
    double condition_zone_center_y = 0.0;
    double condition_zone_center_z = 0.0;
    double condition_zone_half_x = 2.0;
    double condition_zone_half_y = 1.5;
    double condition_zone_half_z = 2.0;
    double condition_zone_radius = 3.0;
    double condition_zone_gain = 1.0;
    int condition_zone_priority = 0;
    std::array<char, 129> condition_zone_class{'i','n','t','e','r','i','o','r'};
    std::array<char, 129> condition_zone_reverb{};
    std::array<char, 257> condition_zone_semantic{};
    int condition_effect_asset_index = 0;
    int condition_effect_shape_index = 0;
    int condition_effect_intent_index = 0;
    int condition_effect_source_index = 0;
    int condition_influence_field_index = 0;
    int condition_influence_kind_index = 0;
    int condition_effect_particle_bound = 128;
    int condition_effect_seed = 1;
    double condition_effect_x = 0.0;
    double condition_effect_y = 0.0;
    double condition_effect_z = 0.0;
    double condition_effect_half_x = 0.5;
    double condition_effect_half_y = 0.5;
    double condition_effect_half_z = 0.5;
    double condition_effect_radius = 0.5;
    double condition_emitter_rate = 10.0;
    double condition_emitter_lifetime = 2.0;
    double condition_emitter_duration = 4.0;
    double condition_emitter_velocity_y = 0.5;
    double condition_influence_strength = 0.25;
    double condition_preview_elapsed = 1.0;
    std::array<char, 257> condition_effect_owner{'c','o','n','d','i','t','i','o','n','.','e','f','f','e','c','t'};
    std::array<char, 129> condition_effect_normal_state{'a','c','t','i','v','e'};
    std::array<char, 129> condition_effect_disabled_state{'d','i','s','a','b','l','e','d'};
    std::optional<carto::condition::EffectPreviewResult> condition_effect_preview_result;
    std::array<int, 5> condition_vegetation_field_indices{};
    int condition_vegetation_asset_index = 0;
    double condition_vegetation_spacing = 2.0;
    double condition_vegetation_density = 0.05;
    double condition_vegetation_threshold = 0.2;
    double condition_vegetation_min_scale = 0.8;
    double condition_vegetation_max_scale = 1.2;
    int condition_vegetation_seed = 1;
    std::optional<carto::condition::VegetationPreviewResult>
        condition_vegetation_preview_result;
    std::array<char, 129> condition_state_branch_name{
        'm','a','i','n','t','e','n','a','n','c','e','-','a','l','t','e','r','n','a','t','e'};
    std::array<char, 257> condition_state_branch_applicability{};
    int condition_state_target_kind_index = 0;
    int condition_safe_place_profile_index = 0;
    int condition_safe_place_property_index = 0;
    int condition_surface_state_index = 0;
    int condition_surface_state_property_index = 0;
    int condition_surface_enum_choice = 0;
    std::uint64_t condition_surface_enum_record_id = 0U;
    int condition_surface_enum_property_index = -1;
    std::string condition_surface_enum_source_value;
    double condition_surface_numeric_source_value = -1.0;
    int condition_state_parent_index = 0;
    int condition_state_compare_left_index = 0;
    int condition_state_compare_right_index = 1;
    double condition_state_branch_value = 0.5;
    std::optional<carto::condition::StateComparisonResult>
        condition_state_comparison_result;
    std::array<char, 129> spatial_field_identity{'f','i','e','l','d','.','m','o','i','s','t','u','r','e'};
    std::string selected_spatial_field;
    std::string selected_wind_u_field;
    std::string selected_wind_v_field;
    std::optional<carto::spatial_field::VectorFieldPreview> condition_vector_preview;
    std::optional<carto::terrain::TerrainTileProduct> condition_terrain_preview;
    std::optional<carto::application::RoadSurfacePreview> condition_road_surface_preview;
    std::optional<carto::geometry::SweptMeshProduct> condition_sweep_preview;
    std::uint64_t condition_sweep_preview_generation = 0U;
    double spatial_field_origin_u = 0.0;
    double spatial_field_origin_v = 0.0;
    double spatial_field_cell_size = 1.0;
    double spatial_field_minimum = 0.0;
    double spatial_field_maximum = 1.0;
    double spatial_field_initial = 0.0;
    double spatial_brush_u = 0.0;
    double spatial_brush_v = 0.0;
    double spatial_brush_radius = 2.0;
    double spatial_brush_value = 0.75;
    double spatial_brush_strength = 0.5;
    int spatial_field_semantic = 1;
    int spatial_field_tile_x = 0;
    int spatial_field_tile_z = 0;
    int spatial_brush_mode = 0;
    std::array<char, 129> road_centerline_identity{
        'r','o','a','d','.','p','i','n','e','-','r','i','d','g','e','.','c','e','n','t','e','r','l','i','n','e'};
    std::array<char, 129> road_source_identity{
        'r','o','a','d','.','p','i','n','e','-','r','i','d','g','e','.','a','p','p','r','o','a','c','h'};
    std::string selected_curve_path;
    std::string selected_road_source;
    std::string selected_sweep_source;
    std::string loaded_sweep_source_identity;
    std::vector<std::array<double, 12>> road_segment_control_points{
        std::array<double, 12>{
            0.0, 0.0, 0.0, 3.0, 0.0, 0.0,
            7.0, 0.0, 2.0, 10.0, 0.0, 2.0}};
    int road_selected_segment = 0;
    double road_width_meters = 6.0;
    double road_shoulder_meters = 1.0;
    double road_maximum_grade = 0.12;
    double road_minimum_curve_radius = 20.0;
    std::array<char, 129> sweep_source_identity{
        's','w','e','e','p','.','p','r','o','f','i','l','e'};
    int sweep_profile_sides = 12;
    int sweep_samples_per_segment = 8;
    int sweep_cap_policy = static_cast<int>(carto::geometry::SweepCapPolicy::both);
    double sweep_profile_radius = 0.25;
    double sweep_start_scale = 1.0;
    double sweep_end_scale = 1.0;
    int road_surface_class = 0;
    int road_function_class = 0;
    int road_access_state = 0;
    int road_drainage_intent = 0;
    std::optional<std::uint64_t> selected_operation_id;
    std::unique_ptr<carto::production::PreparationJob> preparation_job;

    static constexpr float kWorkbenchRackWidth = 148.0F;
    static constexpr float kWorkbenchSnapDistance = 18.0F;
    static constexpr float kWorkbenchPlacementMargin = 12.0F;

    DesktopState() : ui(session) {}

    void load_fonts() {
        ImGuiIO& io = ImGui::GetIO();
        const ImWchar* glyph_ranges = io.Fonts->GetGlyphRangesDefault();
        ImFontConfig font_config{};
        font_config.OversampleH = 2;
        font_config.OversampleV = 1;
        font_config.PixelSnapH = false;

        if (const auto path = preferred_system_font_path(
                {L"Aptos.ttf", L"segoeui.ttf"}); path.has_value()) {
            body_font = io.Fonts->AddFontFromFileTTF(
                path->c_str(), 14.5F, &font_config, glyph_ranges);
        }
        if (const auto path = preferred_system_font_path(
                {L"AptosDisplay-Bold.ttf", L"segoeuib.ttf"}); path.has_value()) {
            heading_font = io.Fonts->AddFontFromFileTTF(
                path->c_str(), 18.0F, &font_config, glyph_ranges);
        }
        if (const auto path = preferred_system_font_path(
                {L"CascadiaMono.ttf", L"consola.ttf"}); path.has_value()) {
            mono_font = io.Fonts->AddFontFromFileTTF(
                path->c_str(), 13.0F, &font_config, glyph_ranges);
        }
        if (body_font != nullptr) io.FontDefault = body_font;
    }

    void heading_text(const char* text) const {
        if (heading_font != nullptr) ImGui::PushFont(heading_font);
        ImGui::TextUnformatted(text);
        if (heading_font != nullptr) ImGui::PopFont();
    }

    void heading_text_colored(carto::ui::UiColor accent, const char* text) const {
        if (heading_font != nullptr) ImGui::PushFont(heading_font);
        ImGui::TextColored(color(accent), "%s", text);
        if (heading_font != nullptr) ImGui::PopFont();
    }

    void draw_brand_mark(const UiSnapshot& snapshot, float size) const {
        ImGui::Dummy(ImVec2(size, size));
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 center{min.x + size * 0.5F, min.y + size * 0.5F};
        const auto brand_color = [](carto::ui::UiColor value) {
            return ImGui::ColorConvertFloat4ToU32(
                ImVec4(value.red, value.green, value.blue, value.alpha));
        };
        carto::desktop_branding::draw_mark(
            ImGui::GetWindowDrawList(), center, size,
            brand_color(snapshot.colors.border_soft),
            brand_color(snapshot.colors.text_secondary),
            brand_color(snapshot.colors.text_primary),
            brand_color(snapshot.colors.accent_primary),
            brand_color(snapshot.colors.accent_secondary));
    }

    void draw_brand_lockup(const UiSnapshot& snapshot) {
        ImGui::BeginGroup();
        draw_brand_mark(snapshot, 42.0F);
        ImGui::SameLine(0.0F, 10.0F);
        ImGui::BeginGroup();
        heading_text_colored(snapshot.colors.text_primary, "CARTOGRAPHER");
        ImGui::TextColored(
            color(snapshot.colors.accent_secondary),
            "S P A T I A L   I N T E L L I G E N C E   B Y   D E S I G N");
        ImGui::EndGroup();
        ImGui::EndGroup();
    }

    void mono_text_disabled(const char* text) const {
        if (mono_font != nullptr) ImGui::PushFont(mono_font);
        ImGui::TextDisabled("%s", text);
        if (mono_font != nullptr) ImGui::PopFont();
    }

    void load_preferences() {
        const auto path = user_preference_path(false);
        if (!path.has_value()) return;
        const auto result = ui.load_preferences(*path);
        if (!result && result.error().code != carto::core::ErrorCode::not_found) {
            // Malformed preference files are recorded by UiController. Other
            // read failures remain visible through the native error surface.
            MessageBoxW(window, wide(result.error().message).c_str(),
                        L"Cartographer preferences unavailable", MB_OK | MB_ICONWARNING);
        }
    }

    void save_preferences() {
        const auto path = user_preference_path(true);
        if (!path.has_value()) {
            MessageBoxW(window, L"Cartographer could not prepare its local preference directory.",
                        L"Cartographer preferences unavailable", MB_OK | MB_ICONWARNING);
            return;
        }
        const auto result = ui.save_preferences(*path);
        if (!result) {
            MessageBoxW(window, wide(result.error().message).c_str(),
                        L"Cartographer preferences unavailable", MB_OK | MB_ICONWARNING);
        }
    }

    void load_workbench_preferences() {
        const auto path = workbench_preference_path(false);
        if (!path.has_value()) return;
        const auto result = carto::ui::load_workbench_preferences(*path);
        if (!result) {
            if (result.error().code == carto::core::ErrorCode::not_found) return;
            MessageBoxW(window, wide(result.error().message).c_str(),
                        L"Cartographer workbench layout unavailable", MB_OK | MB_ICONWARNING);
            return;
        }
        workbench.persisted = result.value();
        workbench.ledger_collapsed = result.value().ledger_collapsed;
        workbench_flow = WorkbenchFlow::custom;
        for (std::size_t index = 0U; index < carto::ui::kWorkbenchInstrumentCount; ++index) {
            const auto instrument = static_cast<carto::ui::WorkbenchInstrument>(index);
            workbench_visible(workbench, instrument) = workbench.persisted.instruments[index].visible;
        }
    }

    void load_market_library() {
        const auto path = market_library_path(true);
        if (!path.has_value()) {
            action_feedback = "Cartographer could not prepare its local market library.";
            action_feedback_error = true;
            return;
        }
        const auto result = ui.configure_market_library(*path);
        if (!result) {
            action_feedback = result.error().message;
            action_feedback_error = true;
        }
    }

    void remember_workbench_flow(WorkbenchFlow flow) noexcept {
        const auto index = workbench_flow_memory_index(flow);
        if (!index.has_value()) return;
        auto& uses = workbench.persisted.flow_use_counts.at(*index);
        if (uses < carto::ui::kMaxWorkbenchFlowUses) ++uses;
        workbench.persisted.last_flow_index = static_cast<std::int32_t>(*index);
    }

    [[nodiscard]] std::optional<WorkbenchFlow> remembered_workbench_flow() const noexcept {
        const auto& persisted = workbench.persisted;
        if (persisted.last_flow_index < 0 ||
            persisted.last_flow_index >= static_cast<std::int32_t>(
                carto::ui::kWorkbenchFlowMemoryCount)) {
            return std::nullopt;
        }
        const WorkbenchFlow flow = workbench_flow_from_memory_index(persisted.last_flow_index);
        return flow == WorkbenchFlow::custom ? std::nullopt : std::optional(flow);
    }

    [[nodiscard]] std::uint32_t workbench_flow_uses(WorkbenchFlow flow) const noexcept {
        const auto index = workbench_flow_memory_index(flow);
        return index.has_value() ? workbench.persisted.flow_use_counts.at(*index) : 0U;
    }

    [[nodiscard]] std::array<WorkbenchFlow, carto::ui::kWorkbenchFlowMemoryCount>
    workbench_flow_order() const {
        std::array<WorkbenchFlow, carto::ui::kWorkbenchFlowMemoryCount> order{
            WorkbenchFlow::focus, WorkbenchFlow::author,
            WorkbenchFlow::inspect, WorkbenchFlow::draft};
        const auto remembered = remembered_workbench_flow();
        std::stable_sort(order.begin(), order.end(), [this, remembered](
            WorkbenchFlow left, WorkbenchFlow right) {
            const std::uint32_t left_uses = workbench_flow_uses(left);
            const std::uint32_t right_uses = workbench_flow_uses(right);
            if (left_uses != right_uses) return left_uses > right_uses;
            if (remembered.has_value()) {
                if (left == *remembered && right != *remembered) return true;
                if (right == *remembered && left != *remembered) return false;
            }
            return static_cast<std::size_t>(*workbench_flow_memory_index(left)) <
                static_cast<std::size_t>(*workbench_flow_memory_index(right));
        });
        return order;
    }

    void forget_workbench_flow_memory() noexcept {
        workbench.persisted.flow_use_counts.fill(0U);
        workbench.persisted.last_flow_index = -1;
    }

    void mark_custom_workbench() noexcept { workbench_flow = WorkbenchFlow::custom; }

    void save_workbench_preferences() {
        const auto path = workbench_preference_path(true);
        if (!path.has_value()) {
            MessageBoxW(window, L"Cartographer could not prepare its local workbench layout directory.",
                        L"Cartographer workbench layout unavailable", MB_OK | MB_ICONWARNING);
            return;
        }
        workbench.persisted.ledger_collapsed = workbench.ledger_collapsed;
        for (std::size_t index = 0U; index < carto::ui::kWorkbenchInstrumentCount; ++index) {
            const auto instrument = static_cast<carto::ui::WorkbenchInstrument>(index);
            workbench.persisted.instruments[index].visible = workbench_visible(workbench, instrument);
        }
        const auto result = carto::ui::save_workbench_preferences(*path, workbench.persisted);
        if (!result) {
            MessageBoxW(window, wide(result.error().message).c_str(),
                        L"Cartographer workbench layout unavailable", MB_OK | MB_ICONWARNING);
        }
    }

    void compose_workbench_flow(WorkbenchFlow flow) {
        if (flow == WorkbenchFlow::custom) return;
        remember_workbench_flow(flow);
        workbench_flow = flow;
        const bool scene = flow == WorkbenchFlow::author || flow == WorkbenchFlow::inspect;
        const bool tools = flow == WorkbenchFlow::author;
        const bool inspector = flow == WorkbenchFlow::author ||
            flow == WorkbenchFlow::inspect || flow == WorkbenchFlow::draft;
        const bool transform = flow == WorkbenchFlow::inspect;
        const bool measure = flow == WorkbenchFlow::inspect;
        const bool draft = flow == WorkbenchFlow::draft;
        workbench.scene = scene;
        workbench.tools = tools;
        workbench.inspector = inspector;
        workbench.transform = transform;
        workbench.measure = measure;
        workbench.draft = draft;
        workbench.assets = false;
        workbench.layers = false;
        workbench.references = false;
        workbench.material = false;
        workbench.constraint = false;
        workbench.modify = false;
        workbench.extrude = false;
        workbench.ledger_collapsed = flow != WorkbenchFlow::inspect;
        for (std::size_t index = 0U; index < carto::ui::kWorkbenchInstrumentCount; ++index) {
            const auto instrument = static_cast<carto::ui::WorkbenchInstrument>(index);
            auto& layout = workbench.persisted.instruments[index];
            layout.visible = workbench_visible(workbench, instrument);
            layout.positioned = false;
            layout.x = 0;
            layout.y = 0;
            layout.width = 0;
            layout.height = 0;
        }
        workbench.persisted.ledger_collapsed = workbench.ledger_collapsed;
        workbench.seeded.fill(false);
    }

    void reset_workbench() {
        const auto flow_use_counts = workbench.persisted.flow_use_counts;
        const std::int32_t last_flow_index = workbench.persisted.last_flow_index;
        workbench_flow = WorkbenchFlow::focus;
        workbench = WorkbenchState{};
        workbench.persisted.flow_use_counts = flow_use_counts;
        workbench.persisted.last_flow_index = last_flow_index;
    }

    void draw_action_feedback(const UiSnapshot& snapshot, bool compact = false) const {
        if (action_feedback.empty()) return;
        const ImVec4 feedback_color = action_feedback_error
            ? color(snapshot.colors.semantic_error)
            : color(snapshot.colors.semantic_success);
        if (compact) {
            ImGui::TextColored(
                feedback_color, "%s", action_feedback_error ? "Action failed" : "Action complete");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0F);
                ImGui::TextUnformatted(action_feedback.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, feedback_color);
        ImGui::TextWrapped("%s", action_feedback.c_str());
        ImGui::PopStyleColor();
    }

    bool dispatch(const carto::application::ApplicationAction& action) {
        const auto result = ui.dispatch(action);
        if (!result) {
            action_feedback = result.error().message;
            action_feedback_error = true;
            return false;
        }
        action_feedback = result.value().action + " accepted at revision " +
            std::to_string(result.value().revision_after.value());
        action_feedback_error = false;
        return true;
    }

    bool open_agent_workspace(bool focus_goal) {
        const auto opened = ui.set_workspace(carto::ui::Workspace::ai);
        if (!opened) {
            action_feedback = opened.error().message;
            action_feedback_error = true;
            return false;
        }
        surface = ShellSurface::workspace;
        workbench.draft = true;
        if (focus_goal) agent_goal_focus_requested = true;
        return true;
    }

    bool open_agent_review_workspace() {
        const auto opened = ui.set_workspace(carto::ui::Workspace::review);
        if (!opened) {
            action_feedback = opened.error().message;
            action_feedback_error = true;
            return false;
        }
        surface = ShellSurface::workspace;
        workbench.draft = true;
        agent_preview_mode = 2;
        return true;
    }

    bool save_project(const UiSnapshot& snapshot, bool save_as = false) {
        if (!save_as && snapshot.project_path.has_value()) {
            return dispatch(carto::application::SaveProjectAction{std::nullopt});
        }
        const auto path = choose_file(true, FileDialogKind::project);
        if (!path.has_value()) return false;
        return dispatch(carto::application::SaveProjectAction{*path});
    }

    void duplicate_selected_object(const UiSnapshot& snapshot) {
        if (snapshot.selection.mode != carto::editor::SelectionMode::object ||
            snapshot.selection.objects.size() != 1U) {
            action_feedback = "Select exactly one object before duplicating it.";
            action_feedback_error = true;
            return;
        }
        const auto source = snapshot.selection.objects.front();
        const auto object = std::find_if(
            snapshot.objects.begin(), snapshot.objects.end(),
            [source](const auto& candidate) { return candidate.object.id == source; });
        if (object == snapshot.objects.end()) {
            action_feedback = "The selected object is no longer present.";
            action_feedback_error = true;
            return;
        }
        static_cast<void>(dispatch(carto::application::DuplicateMeshInstanceAction{
            source,
            object->object.name + " Copy",
            {0.25, 0.0, 0.25},
        }));
    }

    void import_mesh() {
        const auto path = choose_file(false, FileDialogKind::mesh_import);
        if (!path.has_value()) return;
        std::string extension = path->extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        std::optional<carto::ui::MeshImportFormat> format;
        if (extension == ".obj") format = carto::ui::MeshImportFormat::obj;
        else if (extension == ".ply") format = carto::ui::MeshImportFormat::ply;
        else if (extension == ".stl") format = carto::ui::MeshImportFormat::stl;
        if (!format.has_value()) {
            action_feedback = "Choose an OBJ, PLY, or STL mesh file.";
            action_feedback_error = true;
            MessageBoxW(window, wide(action_feedback).c_str(),
                        L"Cartographer mesh import", MB_OK | MB_ICONWARNING);
            return;
        }

        auto imported = ui.import_mesh(*path, *format);
        if (!imported) {
            action_feedback = imported.error().message;
            action_feedback_error = true;
            MessageBoxW(window, wide(action_feedback).c_str(),
                        L"Cartographer mesh import failed", MB_OK | MB_ICONERROR);
            return;
        }
        const auto& receipt = imported.value();
        action_feedback = "Imported " + receipt.object_name + " (" +
            std::to_string(receipt.vertices) + " vertices, " +
            std::to_string(receipt.triangles) + " triangles) through " +
            carto::ui::mesh_import_format_name(receipt.format) + ".";
        if (!receipt.warnings.empty()) {
            action_feedback += " " + std::to_string(receipt.warnings.size()) +
                " interchange warning(s).";
            std::string warning_message = action_feedback;
            for (const auto& warning : receipt.warnings) {
                warning_message += "\n\n- " + warning;
                if (warning_message.size() > 6'000U) break;
            }
            MessageBoxW(window, wide(warning_message).c_str(),
                        L"Cartographer mesh import warnings", MB_OK | MB_ICONWARNING);
        }
        action_feedback_error = false;
        workbench.assets = true;
        mark_custom_workbench();
    }

    void install_market_package() {
        const auto package = choose_file(false, FileDialogKind::market_package);
        if (!package.has_value()) return;
        const auto installed = package->filename().wstring() == L"manifest.json"
            ? ui.install_market_directory(package->parent_path())
            : ui.install_market_archive(*package);
        if (!installed) {
            action_feedback = installed.error().message;
            action_feedback_error = true;
            return;
        }
        action_feedback = "Installed " + installed.value().package_id + "@" +
            installed.value().listing_version_id + " into the local data-only library.";
        action_feedback_error = false;
        surface = ShellSurface::market;
    }

    void publish_engineering_package() {
        const auto path = choose_file(true, FileDialogKind::engineering);
        if (!path.has_value()) return;
        const auto published = ui.publish_engineering_package(*path);
        if (!published) {
            action_feedback = published.error().message;
            action_feedback_error = true;
            return;
        }
        const auto package = ui.build_engineering_package();
        action_feedback = "Published immutable inspection package " +
            (package ? package.value().publication_digest.hex() : std::string{"unknown"}) +
            " to " + path->filename().string() + ".";
        action_feedback_error = false;
    }

    void import_engineering_authoring() {
        const auto path = choose_file(false, FileDialogKind::engineering_authoring);
        if (!path.has_value()) return;
        const auto imported = ui.import_engineering_authoring(*path);
        if (!imported) {
            action_feedback = imported.error().message;
            action_feedback_error = true;
            return;
        }
        engineering_source_update_id.reset();
        engineering_source_revision.fill('\0');
        engineering_source_digest.fill('\0');
        const auto snapshot = ui.snapshot();
        action_feedback = "Imported validated engineering authoring from " +
            path->filename().string() + " (" +
            std::to_string(snapshot.engineering.source_count) + " sources).";
        action_feedback_error = false;
    }

    void export_engineering_authoring() {
        const auto path = choose_file(true, FileDialogKind::engineering_authoring);
        if (!path.has_value()) return;
        const auto exported = ui.export_engineering_authoring(*path);
        if (!exported) {
            action_feedback = exported.error().message;
            action_feedback_error = true;
            return;
        }
        action_feedback = "Exported the validated engineering authoring document to " +
            path->filename().string() + ".";
        action_feedback_error = false;
    }

    void record_engineering_source_change() {
        if (!engineering_source_update_id.has_value()) return;
        const std::string revision(engineering_source_revision.data());
        const auto digest = carto::assets::Sha256Digest::from_hex(
            std::string_view(engineering_source_digest.data()));
        if (revision.empty() || !digest) {
            action_feedback = revision.empty()
                ? "Enter the source revision before recording drift."
                : digest.error().message;
            action_feedback_error = true;
            return;
        }
        const auto updated = ui.invalidate_engineering_source(
            *engineering_source_update_id, revision, digest.value());
        if (!updated) {
            action_feedback = updated.error().message;
            action_feedback_error = true;
            return;
        }
        action_feedback = "Recorded the new engineering source evidence. Dependent authored "
            "records are stale until reconciled in an updated authoring document.";
        action_feedback_error = false;
        engineering_source_update_id.reset();
        engineering_source_revision.fill('\0');
        engineering_source_digest.fill('\0');
    }

    void publish_condition_package() {
        const auto path = choose_file(true, FileDialogKind::condition);
        if (!path.has_value()) return;
        const auto published = ui.publish_condition_package(*path);
        if (!published) {
            action_feedback = published.error().message;
            action_feedback_error = true;
            return;
        }
        const auto package = ui.build_condition_package();
        action_feedback = "Published admitted CONDITION source package " +
            (package ? package.value().publication_digest.hex() : std::string{"unknown"}) +
            " to " + path->filename().string() + ".";
        action_feedback_error = false;
    }

    void import_condition_authoring() {
        const auto path = choose_file(false, FileDialogKind::condition_authoring);
        if (!path.has_value()) return;
        const auto imported = ui.import_condition_authoring(*path);
        if (!imported) {
            action_feedback = imported.error().message;
            action_feedback_error = true;
            return;
        }
        condition_source_update_id.reset();
        condition_source_revision.fill('\0');
        condition_source_digest.fill('\0');
        const auto snapshot = ui.snapshot();
        action_feedback = "Imported validated CONDITION authoring from " +
            path->filename().string() + " (" +
            std::to_string(snapshot.condition.source_count) + " sources).";
        action_feedback_error = false;
    }

    void import_condition_audio_clip() {
        const auto path = choose_file(false, FileDialogKind::audio_clip);
        if (!path.has_value()) return;
        const auto imported = ui.import_condition_audio_clip(*path);
        if (!imported) {
            action_feedback = imported.error().message;
            action_feedback_error = true;
            return;
        }
        const auto snapshot = ui.snapshot();
        action_feedback = "Inspected WAVE clip metadata as unreviewed source evidence; " +
            std::to_string(snapshot.condition.source_count) + " CONDITION sources now present. "
            "Audio samples were not embedded.";
        action_feedback_error = false;
    }

    void stop_condition_audio_audition() noexcept {
        (void)PlaySoundA(nullptr, nullptr, 0U);
        condition_audio_preview_buffer.clear();
    }

    void audition_condition_audio_clip(std::uint64_t source_id) {
        const auto snapshot = ui.snapshot();
        const auto source = std::find_if(snapshot.condition.sources.begin(),
            snapshot.condition.sources.end(), [source_id](const auto& candidate) {
                return candidate.id == source_id && candidate.kind == "imported-asset";
            });
        if (source == snapshot.condition.sources.end() || !source->current) {
            action_feedback = "Audition requires current imported WAVE source evidence.";
            action_feedback_error = true;
            return;
        }
        const auto path = choose_file(false, FileDialogKind::audio_clip);
        if (!path.has_value()) return;
        std::error_code file_error;
        const auto file_size = std::filesystem::file_size(*path, file_error);
        if (file_error || file_size == 0U ||
            file_size > carto::condition::kMaxWaveClipBytes) {
            action_feedback = "Selected WAVE file is empty, unavailable, or exceeds the import bound.";
            action_feedback_error = true;
            return;
        }
        std::ifstream stream(*path, std::ios::binary);
        std::vector<std::uint8_t> source_bytes(static_cast<std::size_t>(file_size));
        stream.read(reinterpret_cast<char*>(source_bytes.data()),
            static_cast<std::streamsize>(source_bytes.size()));
        if (!stream) {
            action_feedback = "Unable to read the selected WAVE source for audition.";
            action_feedback_error = true;
            return;
        }
        const auto source_digest = carto::assets::sha256(
            std::span<const std::uint8_t>{source_bytes});
        if (source_digest != source->digest) {
            action_feedback = "Selected audio does not match the recorded source digest.";
            action_feedback_error = true;
            return;
        }
        auto excerpt = carto::condition::make_wave_audition_excerpt(source_bytes);
        if (!excerpt) {
            action_feedback = excerpt.error().message;
            action_feedback_error = true;
            return;
        }
        stop_condition_audio_audition();
        condition_audio_preview_buffer = std::move(excerpt.value());
        if (condition_audio_preview_buffer.empty() ||
            PlaySoundA(reinterpret_cast<LPCSTR>(condition_audio_preview_buffer.data()),
                nullptr, SND_ASYNC | SND_MEMORY | SND_NODEFAULT) == FALSE) {
            condition_audio_preview_buffer.clear();
            action_feedback = "Windows audio audition could not start this WAVE excerpt.";
            action_feedback_error = true;
            return;
        }
        action_feedback = "Auditioning a digest-verified, transient three-second WAVE excerpt.";
        action_feedback_error = false;
    }

    void export_condition_authoring() {
        const auto path = choose_file(true, FileDialogKind::condition_authoring);
        if (!path.has_value()) return;
        const auto exported = ui.export_condition_authoring(*path);
        if (!exported) {
            action_feedback = exported.error().message;
            action_feedback_error = true;
            return;
        }
        action_feedback = "Exported the validated CONDITION authoring document to " +
            path->filename().string() + ".";
        action_feedback_error = false;
    }

    void record_condition_source_change() {
        if (!condition_source_update_id.has_value()) return;
        const std::string revision(condition_source_revision.data());
        const auto digest = carto::assets::Sha256Digest::from_hex(
            std::string_view(condition_source_digest.data()));
        if (revision.empty() || !digest) {
            action_feedback = revision.empty()
                ? "Enter the source revision before recording drift."
                : digest.error().message;
            action_feedback_error = true;
            return;
        }
        const auto updated = ui.invalidate_condition_source(
            *condition_source_update_id, revision, digest.value());
        if (!updated) {
            action_feedback = updated.error().message;
            action_feedback_error = true;
            return;
        }
        action_feedback = "Recorded the new CONDITION source evidence. Dependent authored "
            "records are stale until reconciled in an updated authoring document.";
        action_feedback_error = false;
        condition_source_update_id.reset();
        condition_source_revision.fill('\0');
        condition_source_digest.fill('\0');
    }

    bool route_native_key(const carto::ui::NativeKeyEvent& event) {
        if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantTextInput) {
            return false;
        }
        const auto shortcut = carto::ui::shortcut_for_native_key(event);
        if (!shortcut.has_value()) return false;
        if (!event.repeat) {
            if (*shortcut == carto::ui::Shortcut::save) {
                static_cast<void>(save_project(ui.snapshot()));
            } else {
                const auto routed = ui.handle_shortcut(*shortcut);
                if (!routed) {
                    action_feedback = routed.error().message;
                    action_feedback_error = true;
                }
            }
        }
        return true;
    }

    bool native_pointer_screen_position(
        const carto::ui::NativePointerEvent& event,
        carto::core::Vec2d& position) const {
        if (window == nullptr) return false;
        POINT client_point{static_cast<LONG>(event.x), static_cast<LONG>(event.y)};
        if (ClientToScreen(window, &client_point) == FALSE) return false;
        position = {static_cast<double>(client_point.x), static_cast<double>(client_point.y)};
        return position.finite();
    }

    bool condition_gizmo_local_delta(
        const NativeConditionGizmoHandle& handle,
        carto::core::Vec2d screen_position,
        carto::core::Vec3d& local_delta) const {
        const auto world = work_surface_camera.screen_to_world(screen_position,
            {static_cast<double>(native_viewport_hits.screen_origin.x),
                static_cast<double>(native_viewport_hits.screen_origin.y)},
            {static_cast<double>(native_viewport_hits.extent.x),
                static_cast<double>(native_viewport_hits.extent.y)});
        if (!world) return false;
        const auto inverse_rotate = [](carto::core::Quaternion rotation,
                                       carto::core::Vec3d value) {
            const auto normalized = rotation.normalized();
            return carto::core::Quaternion{
                -normalized.x, -normalized.y, -normalized.z, normalized.w}.rotate(value);
        };
        const auto divide_scale = [](carto::core::Vec3d value,
                                     carto::core::Vec3d scale) -> std::optional<carto::core::Vec3d> {
            constexpr double kMinScale = 1.0e-9;
            if (!scale.finite() || std::abs(scale.x) < kMinScale ||
                std::abs(scale.y) < kMinScale || std::abs(scale.z) < kMinScale) {
                return std::nullopt;
            }
            return carto::core::Vec3d{
                value.x / scale.x, value.y / scale.y, value.z / scale.z};
        };
        const auto center = world_point(handle.owner_world,
            handle.local_transform.translation);
        const carto::core::Vec3d world_delta{
            world.value().x - center.x, 0.0, world.value().y - center.z};
        const auto parent_delta = divide_scale(
            inverse_rotate(handle.owner_world.rotation, world_delta),
            handle.owner_world.scale);
        if (!parent_delta) return false;
        const auto local_unscaled = inverse_rotate(handle.local_transform.rotation, *parent_delta);
        const auto local = divide_scale(local_unscaled, handle.local_transform.scale);
        if (!local || !local->finite()) return false;
        local_delta = *local;
        return true;
    }

    void cancel_native_condition_gizmo(
        std::string reason, bool release_capture = true) {
        if (!native_condition_gizmo_drag.has_value()) return;
        native_condition_gizmo_drag.reset();
        if (release_capture && window != nullptr && GetCapture() == window) {
            ReleaseCapture();
        }
        action_feedback = std::move(reason);
        action_feedback_error = true;
    }

    bool begin_native_condition_gizmo_drag(
        const carto::ui::NativePointerEvent& event,
        const UiSnapshot& snapshot,
        ImVec2 pointer) {
        if (!event.pressed || event.button != carto::ui::NativePointerButton::primary ||
            snapshot.workspace_kind != carto::ui::Workspace::condition ||
            carto::ui::selection_operation_for_native_pointer(event) !=
                std::optional<carto::editor::SelectionOperation>{
                    carto::editor::SelectionOperation::replace}) {
            return false;
        }
        constexpr float kHandleRadiusSquared = 12.0F * 12.0F;
        const NativeConditionGizmoHandle* hit = nullptr;
        float best = kHandleRadiusSquared;
        for (const auto& candidate : native_viewport_hits.condition_gizmos) {
            const float dx = candidate.handle.x - pointer.x;
            const float dy = candidate.handle.y - pointer.y;
            const float distance = dx * dx + dy * dy;
            if (distance < best) {
                best = distance;
                hit = &candidate;
            }
        }
        if (hit == nullptr) return false;
        const auto authoring = session.condition_authoring();
        if (!authoring || authoring.value().project_revision != hit->authoring_revision ||
            snapshot.project_revision != native_viewport_hits.project_revision) {
            action_feedback = authoring
                ? "Acoustic gizmo input is stale; rebuild the viewport and retry."
                : authoring.error().message;
            action_feedback_error = true;
            return true;
        }
        NativeConditionGizmoDrag capture;
        capture.handle = *hit;
        capture.project_revision = snapshot.project_revision;
        capture.viewport = ImRect(native_viewport_hits.screen_origin,
            native_viewport_hits.screen_origin + native_viewport_hits.extent);
        capture.preview_minimum = hit->minimum_distance;
        capture.preview_maximum = hit->maximum_distance;
        capture.preview_radius = hit->radius;
        capture.preview_half_extents = hit->half_extents;
        native_condition_gizmo_drag = std::move(capture);
        if (window != nullptr) SetCapture(window);
        action_feedback = "Drag the acoustic handle; release to commit a revision-bound edit.";
        action_feedback_error = false;
        return true;
    }

    bool route_native_condition_gizmo_drag(const carto::ui::NativePointerEvent& event) {
        if (!native_condition_gizmo_drag.has_value()) return false;
        const auto snapshot = ui.snapshot();
        if (snapshot.workspace_kind != carto::ui::Workspace::condition ||
            snapshot.project_revision != native_condition_gizmo_drag->project_revision) {
            cancel_native_condition_gizmo(
                "Acoustic gizmo cancelled because its project snapshot changed.");
            return true;
        }
        carto::core::Vec2d pointer;
        if (!native_pointer_screen_position(event, pointer)) {
            cancel_native_condition_gizmo(
                "Acoustic gizmo cancelled because pointer position is unavailable.");
            return true;
        }
        if (!native_condition_gizmo_drag->viewport.Contains(ImVec2(
                static_cast<float>(pointer.x), static_cast<float>(pointer.y)))) {
            cancel_native_condition_gizmo(
                "Acoustic gizmo cancelled after the pointer left the viewport.");
            return true;
        }
        carto::core::Vec3d local_delta;
        if (!condition_gizmo_local_delta(
                native_condition_gizmo_drag->handle, pointer, local_delta)) {
            cancel_native_condition_gizmo("Acoustic gizmo could not resolve local coordinates.");
            return true;
        }
        auto& capture = *native_condition_gizmo_drag;
        const double radius = std::hypot(local_delta.x, local_delta.z);
        switch (capture.handle.kind) {
        case NativeConditionGizmoKind::audio_minimum_distance:
            capture.preview_minimum = std::clamp(radius, 0.0,
                std::max(0.0, capture.handle.maximum_distance - 0.01));
            break;
        case NativeConditionGizmoKind::audio_maximum_distance:
            capture.preview_maximum = std::clamp(radius,
                capture.handle.minimum_distance + 0.01, 1.0e6);
            break;
        case NativeConditionGizmoKind::acoustic_sphere_radius:
            capture.preview_radius = std::clamp(radius, 0.01, 1.0e6);
            break;
        case NativeConditionGizmoKind::acoustic_box_corner:
            capture.preview_half_extents = {
                std::clamp(std::abs(local_delta.x), 0.01, 1.0e6),
                capture.handle.half_extents.y,
                std::clamp(std::abs(local_delta.z), 0.01, 1.0e6)};
            break;
        }
        if (event.pressed) return true;

        const auto kind = capture.handle.kind;
        const auto record_id = capture.handle.record_id;
        const auto expected_revision = capture.handle.authoring_revision;
        const auto minimum_distance = capture.preview_minimum;
        const auto maximum_distance = capture.preview_maximum;
        const auto preview_radius = capture.preview_radius;
        const auto half_extents = capture.preview_half_extents;
        const auto initial_minimum = capture.handle.minimum_distance;
        const auto initial_maximum = capture.handle.maximum_distance;
        const auto initial_radius = capture.handle.radius;
        const auto initial_half_extents = capture.handle.half_extents;
        native_condition_gizmo_drag.reset();
        if (window != nullptr && GetCapture() == window) ReleaseCapture();

        if (kind == NativeConditionGizmoKind::audio_minimum_distance ||
            kind == NativeConditionGizmoKind::audio_maximum_distance) {
            if (minimum_distance != initial_minimum || maximum_distance != initial_maximum) {
                const auto updated = ui.update_condition_audio_source_distances(
                    record_id, expected_revision, minimum_distance, maximum_distance);
                action_feedback = updated
                    ? "Committed acoustic source distance through project command history."
                    : updated.error().message;
                action_feedback_error = !updated;
            } else {
                action_feedback = "Acoustic source distance is unchanged.";
                action_feedback_error = false;
            }
        } else if ((kind == NativeConditionGizmoKind::acoustic_sphere_radius &&
                    preview_radius != initial_radius) ||
                   (kind == NativeConditionGizmoKind::acoustic_box_corner &&
                    (half_extents.x != initial_half_extents.x ||
                     half_extents.z != initial_half_extents.z))) {
            const auto updated = ui.update_condition_acoustic_zone_volume(
                record_id, expected_revision, half_extents, preview_radius);
            action_feedback = updated
                ? "Committed acoustic-zone volume through project command history."
                : updated.error().message;
            action_feedback_error = !updated;
        } else {
            action_feedback = "Acoustic zone volume is unchanged.";
            action_feedback_error = false;
        }
        return true;
    }

    void cancel_native_transform_drag(std::string reason, bool release_capture = true) {
        if (!native_transform_drag.has_value()) return;
        native_transform_drag.reset();
        if (release_capture && window != nullptr && GetCapture() == window) {
            ReleaseCapture();
        }
        action_feedback = std::move(reason);
        action_feedback_error = true;
    }

    bool begin_native_transform_drag(
        const carto::ui::NativePointerEvent& event,
        const UiSnapshot& snapshot,
        ImVec2 screen_point) {
        const bool ghost_edit = agent_ghost_edit_mode &&
            agent_preview_mode == 2 && snapshot.agent_task.has_value() &&
            snapshot.project_revision == snapshot.agent_task->base_revision &&
            snapshot.agent_task->status != carto::agent::TaskStatus::accepted &&
            snapshot.agent_task->status != carto::agent::TaskStatus::rejected;
        if (!event.pressed || event.button != carto::ui::NativePointerButton::primary ||
            surface != ShellSurface::workspace ||
            (!ghost_edit && snapshot.workspace_kind != carto::ui::Workspace::model) ||
            (!ghost_edit && snapshot.selection.mode != carto::editor::SelectionMode::object) ||
            (!ghost_edit && snapshot.selection.objects.size() != 1U) ||
            !native_viewport_hits.translate_gizmo.has_value() ||
            native_transform_drag.has_value()) {
            return false;
        }
        const auto& gizmo = *native_viewport_hits.translate_gizmo;
        if (snapshot.project_revision != gizmo.project_revision ||
            snapshot.project_revision != native_viewport_hits.project_revision ||
            (ghost_edit && agent_ghost_selected_object != gizmo.object) ||
            (!ghost_edit && snapshot.selection.objects.front() != gizmo.object)) {
            return false;
        }
        const float x_distance = point_segment_distance_squared(
            screen_point, gizmo.origin + ImVec2(8.0F, 0.0F), gizmo.x_tip);
        const float z_distance = point_segment_distance_squared(
            screen_point, gizmo.origin + ImVec2(0.0F, -8.0F), gizmo.z_tip);
        constexpr float kHandleRadiusSquared = 9.0F * 9.0F;
        if (x_distance > kHandleRadiusSquared && z_distance > kHandleRadiusSquared) {
            return false;
        }
        const auto object = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
            [&gizmo](const auto& value) { return value.object.id == gizmo.object; });
        if (object == snapshot.objects.end() || object->object.parent.has_value() ||
            object->object.locked) {
            return false;
        }
        carto::core::Transform initial_transform = object->object.local_transform;
        if (ghost_edit) {
            const auto branch_instances = snapshot.agent_task->after_scene.instances();
            const auto branch_instance = std::find_if(
                branch_instances.begin(), branch_instances.end(),
                [&gizmo](const auto& instance) { return instance.object == gizmo.object; });
            if (branch_instance == branch_instances.end() ||
                std::find(snapshot.agent_task->scope_objects.begin(),
                    snapshot.agent_task->scope_objects.end(), gizmo.object) ==
                    snapshot.agent_task->scope_objects.end() ||
                std::find(snapshot.agent_task->protected_objects.begin(),
                    snapshot.agent_task->protected_objects.end(), gizmo.object) !=
                    snapshot.agent_task->protected_objects.end()) {
                return false;
            }
            initial_transform = branch_instance->world_transform;
        }
        const auto axis = x_distance <= z_distance
            ? carto::editor::ViewportTranslateAxis::world_x
            : carto::editor::ViewportTranslateAxis::world_z;
        const double pixels_per_world_unit = axis ==
                carto::editor::ViewportTranslateAxis::world_x
            ? gizmo.pixels_per_world_x : gizmo.pixels_per_world_z;
        if (ghost_edit && agent_ghost_justification.empty()) {
            action_feedback = "Enter a correction justification before editing the ghost.";
            action_feedback_error = true;
            return true;
        }
        carto::core::Vec2d pointer{
            static_cast<double>(screen_point.x), static_cast<double>(screen_point.y)};
        NativeTransformDragCapture capture;
        if (auto result = capture.session.begin(gizmo.object, initial_transform,
                snapshot.project_revision, axis, pointer, pixels_per_world_unit); !result) {
            action_feedback = result.error().message;
            action_feedback_error = true;
            return true;
        }
        capture.axis = axis;
        capture.viewport = ImRect(
            native_viewport_hits.screen_origin,
            native_viewport_hits.screen_origin + native_viewport_hits.extent);
        capture.initial_transform = initial_transform;
        capture.preview_transform = capture.initial_transform;
        capture.agent_ghost = ghost_edit;
        if (ghost_edit) {
            capture.agent_task_id = snapshot.agent_task->task_id;
            capture.agent_branch_revision = snapshot.agent_task->speculative_revision;
        }
        native_transform_drag = std::move(capture);
        if (window != nullptr) SetCapture(window);
        return true;
    }

    bool route_native_transform_drag(const carto::ui::NativePointerEvent& event) {
        if (!native_transform_drag.has_value()) return false;
        const auto snapshot = ui.snapshot();
        if (snapshot.project_revision != native_transform_drag->session.source_revision()) {
            cancel_native_transform_drag(
                "Transform cancelled because the project revision changed.");
            return true;
        }
        if (native_transform_drag->agent_ghost &&
            (!snapshot.agent_task.has_value() ||
             snapshot.agent_task->task_id != native_transform_drag->agent_task_id ||
             snapshot.agent_task->speculative_revision !=
                native_transform_drag->agent_branch_revision ||
             snapshot.project_revision != snapshot.agent_task->base_revision)) {
            cancel_native_transform_drag(
                "Ghost transform cancelled because the task branch changed.");
            return true;
        }
        carto::core::Vec2d pointer;
        if (!native_pointer_screen_position(event, pointer)) {
            cancel_native_transform_drag("Transform cancelled because pointer position is unavailable.");
            return true;
        }
        if (!native_transform_drag->viewport.Contains(ImVec2(
                static_cast<float>(pointer.x), static_cast<float>(pointer.y)))) {
            cancel_native_transform_drag("Transform cancelled after the pointer left the viewport.");
            return true;
        }
        auto preview = native_transform_drag->session.preview(
            pointer, snapshot.project_revision);
        if (!preview) {
            cancel_native_transform_drag(preview.error().message);
            return true;
        }
        const bool grid_snap =
            (event.modifiers & carto::ui::kNativeModifierControl) != 0U;
        const bool geometry_snap = !grid_snap &&
            (event.modifiers & carto::ui::kNativeModifierShift) != 0U;
        const auto snapped_preview = carto::editor::snap_viewport_transform_preview(
            preview.value(), native_transform_drag->axis, snapshot.viewport.scene,
            snapshot.project_revision, grid_snap, geometry_snap,
            native_transform_drag->geometry_snap_session,
            std::optional<carto::scene::ObjectId>{
                native_transform_drag->session.object()});
        if (!snapped_preview) {
            cancel_native_transform_drag(snapped_preview.error().message);
            return true;
        }
        native_transform_drag->geometry_snap_candidate =
            snapped_preview.value().candidate;
        native_transform_drag->preview_transform = snapped_preview.value().transform;
        if (event.pressed) {
            if (grid_snap) {
                action_feedback = "Translate preview · 0.25 m grid snap";
            } else if (geometry_snap &&
                native_transform_drag->geometry_snap_candidate.has_value()) {
                const auto& candidate =
                    *native_transform_drag->geometry_snap_candidate;
                const char* kind = candidate.kind == carto::editor::SnapKind::vertex
                    ? "vertex" : candidate.kind == carto::editor::SnapKind::edge_midpoint
                        ? "edge midpoint" : "face center";
                action_feedback = std::string("Translate preview · snapped to ") +
                    kind + " on object " + std::to_string(candidate.object->value);
            } else if (geometry_snap) {
                action_feedback = "Translate preview · no aligned geometry target within 0.25 m";
            } else {
                action_feedback = "Translate preview · X/Z axis constrained";
            }
            action_feedback_error = false;
            return true;
        }

        const auto object = native_transform_drag->session.object();
        const auto source_revision = native_transform_drag->session.source_revision();
        const auto initial = native_transform_drag->initial_transform;
        auto capture = std::move(*native_transform_drag);
        const auto committed_transform = capture.session.complete(
            pointer, snapshot.project_revision);
        native_transform_drag.reset();
        if (window != nullptr && GetCapture() == window) ReleaseCapture();
        if (!committed_transform) {
            action_feedback = committed_transform.error().message;
            action_feedback_error = true;
            return true;
        }
        const auto final_preview = carto::editor::snap_viewport_transform_preview(
            committed_transform.value(), capture.axis, snapshot.viewport.scene,
            snapshot.project_revision, grid_snap, geometry_snap,
            capture.geometry_snap_session, std::optional<carto::scene::ObjectId>{object});
        if (!final_preview) {
            action_feedback = final_preview.error().message;
            action_feedback_error = true;
            return true;
        }
        auto final_transform = final_preview.value().transform;
        if (final_preview.value().candidate.has_value()) {
            const auto& candidate = *final_preview.value().candidate;
            const auto* target = candidate.object.has_value()
                ? snapshot.viewport.scene.find(*candidate.object) : nullptr;
            if (candidate.source_revision != source_revision ||
                !candidate.object.has_value() || !candidate.mesh_asset.has_value() ||
                target == nullptr || target->mesh == nullptr ||
                target->mesh_asset != candidate.mesh_asset ||
                target->mesh->source_revision != candidate.mesh_revision) {
                action_feedback =
                    "Transform cancelled because its geometry snap receipt is stale or incomplete.";
                action_feedback_error = true;
                return true;
            }
        }
        const bool changed = capture.axis == carto::editor::ViewportTranslateAxis::world_x
            ? final_transform.translation.x != initial.translation.x
            : final_transform.translation.z != initial.translation.z;
        if (changed) {
            if (capture.agent_ghost) {
                const auto corrected = ui.correct_agent_ghost_transform(
                    object, final_transform, agent_ghost_justification);
                if (corrected) {
                    agent_ghost_justification.clear();
                    action_feedback = "Human correction locked into the speculative branch; gates rechecked.";
                    action_feedback_error = false;
                } else {
                    action_feedback = corrected.error().message;
                    action_feedback_error = true;
                }
            } else {
                static_cast<void>(dispatch(SetObjectTransformAction{
                    object, final_transform, source_revision}));
            }
        }
        return true;
    }

    void cancel_native_sculpt_stroke(std::string reason, bool release_capture = true) {
        if (!native_sculpt_stroke.has_value()) return;
        native_sculpt_stroke.reset();
        if (release_capture && window != nullptr && GetCapture() == window) {
            ReleaseCapture();
        }
        action_feedback = std::move(reason);
        action_feedback_error = true;
    }

    bool begin_native_sculpt_stroke(const carto::ui::NativePointerEvent& event) {
        if (!event.pressed || event.button != carto::ui::NativePointerButton::primary ||
            surface != ShellSurface::workspace || !native_viewport_hits.valid ||
            !native_viewport_hits.interactive ||
            carto::ui::selection_operation_for_native_pointer(event) !=
                std::optional<carto::editor::SelectionOperation>{
                    carto::editor::SelectionOperation::replace}) {
            return false;
        }
        const auto snapshot = ui.snapshot();
        if (snapshot.workspace_kind != carto::ui::Workspace::sculpt ||
            snapshot.project_revision != native_viewport_hits.project_revision) {
            return false;
        }
        std::optional<carto::scene::ObjectId> target;
        if (snapshot.selection.component_object.has_value()) {
            target = snapshot.selection.component_object;
        } else if (snapshot.selection.objects.size() == 1U) {
            target = snapshot.selection.objects.front();
        }
        if (!target.has_value()) return false;
        const auto object = std::find_if(
            snapshot.objects.begin(), snapshot.objects.end(), [target](const auto& value) {
                return value.object.id == *target;
            });
        if (object == snapshot.objects.end() || !object->object.mesh_asset.has_value() ||
            object->object.locked || !object->object.visible) {
            return false;
        }

        carto::core::Vec2d screen_position;
        if (!native_pointer_screen_position(event, screen_position)) return false;
        const ImVec2 point{
            static_cast<float>(screen_position.x), static_cast<float>(screen_position.y)};
        const ImRect viewport(
            native_viewport_hits.screen_origin,
            native_viewport_hits.screen_origin + native_viewport_hits.extent);
        if (!viewport.Contains(point)) return false;
        if (native_viewport_hits.sculpt_triangles_truncated) {
            action_feedback = "This selected mesh exceeds the viewport stroke sampling bound.";
            action_feedback_error = true;
            return true;
        }

        NativeSculptStrokeCapture capture;
        capture.project_revision = snapshot.project_revision;
        capture.object = *target;
        capture.mesh_asset = *object->object.mesh_asset;
        capture.mode = static_cast<carto::production::SculptStrokeMode>(
            std::clamp(sculpt_mode_index, 0, 11));
        capture.radius = sculpt_radius;
        capture.strength = sculpt_strength;
        capture.viewport = viewport;
        capture.screen_path.reserve(128U);
        capture.screen_path.push_back(screen_position);
        for (const auto& triangle : native_viewport_hits.sculpt_triangles) {
            if (triangle.object == *target) capture.triangles.push_back(triangle);
        }
        if (capture.triangles.empty()) return false;
        const auto initial_hit = carto::ui::sample_sculpt_viewport_surface(
            capture.triangles, capture.object, screen_position);
        if (!initial_hit) return false;
        if (!std::isfinite(capture.radius) || capture.radius <= 0.0 ||
            !std::isfinite(capture.strength) || capture.strength <= 0.0) {
            action_feedback = "Sculpt radius and strength must be finite and positive.";
            action_feedback_error = true;
            return true;
        }
        native_sculpt_stroke = std::move(capture);
        SetCapture(window);
        if (GetCapture() != window) {
            native_sculpt_stroke.reset();
            action_feedback = "The desktop could not capture the sculpt pointer.";
            action_feedback_error = true;
            return true;
        }
        return true;
    }

    bool continue_native_sculpt_stroke(const carto::ui::NativePointerEvent& event) {
        if (!native_sculpt_stroke.has_value()) return false;
        carto::core::Vec2d position;
        if (!native_pointer_screen_position(event, position)) {
            cancel_native_sculpt_stroke("Sculpt stroke cancelled because the cursor position was unavailable.");
            return true;
        }
        const auto snapshot = ui.snapshot();
        if (snapshot.project_revision != native_sculpt_stroke->project_revision ||
            snapshot.workspace_kind != carto::ui::Workspace::sculpt) {
            cancel_native_sculpt_stroke("Sculpt stroke cancelled because its project snapshot changed.");
            return true;
        }
        const ImVec2 point{static_cast<float>(position.x), static_cast<float>(position.y)};
        if (!native_sculpt_stroke->viewport.Contains(point)) {
            cancel_native_sculpt_stroke("Sculpt stroke cancelled when the cursor left the viewport.");
            return true;
        }
        if (native_sculpt_stroke->screen_path.size() >= kMaxNativeSculptCursorPoints) {
            cancel_native_sculpt_stroke("Sculpt stroke exceeded its bounded cursor path.");
            return true;
        }
        const auto& last = native_sculpt_stroke->screen_path.back();
        if (position.x != last.x || position.y != last.y) {
            native_sculpt_stroke->screen_path.push_back(position);
        }
        return true;
    }

    bool finish_native_sculpt_stroke(const carto::ui::NativePointerEvent& event) {
        if (!native_sculpt_stroke.has_value()) return false;
        carto::core::Vec2d position;
        if (!native_pointer_screen_position(event, position)) {
            cancel_native_sculpt_stroke("Sculpt stroke cancelled because the cursor position was unavailable.");
            return true;
        }
        const ImVec2 point{static_cast<float>(position.x), static_cast<float>(position.y)};
        if (!native_sculpt_stroke->viewport.Contains(point)) {
            cancel_native_sculpt_stroke("Sculpt stroke cancelled when the cursor left the viewport.");
            return true;
        }
        if (native_sculpt_stroke->screen_path.size() >= kMaxNativeSculptCursorPoints) {
            cancel_native_sculpt_stroke("Sculpt stroke exceeded its bounded cursor path.");
            return true;
        }
        if (position.x != native_sculpt_stroke->screen_path.back().x ||
            position.y != native_sculpt_stroke->screen_path.back().y) {
            native_sculpt_stroke->screen_path.push_back(position);
        }
        NativeSculptStrokeCapture capture = std::move(*native_sculpt_stroke);
        native_sculpt_stroke.reset();
        if (window != nullptr && GetCapture() == window) ReleaseCapture();

        const auto snapshot = ui.snapshot();
        const auto selected = snapshot.selection.component_object.has_value()
            ? snapshot.selection.component_object
            : (snapshot.selection.objects.size() == 1U
                ? std::optional<carto::scene::ObjectId>{snapshot.selection.objects.front()}
                : std::nullopt);
        if (snapshot.project_revision != capture.project_revision ||
            snapshot.workspace_kind != carto::ui::Workspace::sculpt ||
            selected != std::optional<carto::scene::ObjectId>{capture.object}) {
            action_feedback = "Sculpt stroke cancelled because its selected source changed.";
            action_feedback_error = true;
            return true;
        }
        auto samples = carto::ui::sample_sculpt_viewport_stroke(
            capture.triangles, capture.object, capture.screen_path, 4.0);
        if (!samples) {
            action_feedback = samples.error().message;
            action_feedback_error = true;
            return true;
        }
        static_cast<void>(dispatch(carto::application::AppendSculptSurfaceStrokeAction{
            capture.project_revision,
            capture.object,
            capture.mesh_asset,
            capture.mode,
            capture.radius,
            capture.strength,
            std::move(samples.value())}));
        return true;
    }

    bool route_native_pointer(const carto::ui::NativePointerEvent& event) {
        const auto selection_operation =
            carto::ui::selection_operation_for_native_pointer(event);
        if (!selection_operation.has_value() || surface != ShellSurface::workspace ||
            !native_viewport_hits.valid || !native_viewport_hits.interactive) {
            return false;
        }
        const auto snapshot = ui.snapshot();
        if (snapshot.project_revision != native_viewport_hits.project_revision) return false;

        POINT client_point{
            static_cast<LONG>(event.x),
            static_cast<LONG>(event.y),
        };
        if (window == nullptr || ClientToScreen(window, &client_point) == FALSE) return false;
        const ImVec2 point{
            static_cast<float>(client_point.x),
            static_cast<float>(client_point.y),
        };
        const ImRect viewport(
            native_viewport_hits.screen_origin,
            native_viewport_hits.screen_origin + native_viewport_hits.extent);
        if (!viewport.Contains(point)) return false;

        if (begin_native_condition_gizmo_drag(event, snapshot, point)) return true;
        if (begin_native_transform_drag(event, snapshot, point)) return true;

        if (agent_ghost_edit_mode && agent_preview_mode == 2 &&
            snapshot.agent_task.has_value() &&
            snapshot.project_revision == snapshot.agent_task->base_revision) {
            agent_ghost_selected_object = {};
            for (auto iterator = native_viewport_hits.objects.rbegin();
                 iterator != native_viewport_hits.objects.rend(); ++iterator) {
                if (!iterator->second.Contains(point)) continue;
                const auto& task = *snapshot.agent_task;
                const bool in_scope = std::find(task.scope_objects.begin(),
                    task.scope_objects.end(), iterator->first) != task.scope_objects.end();
                const bool protected_object = std::find(task.protected_objects.begin(),
                    task.protected_objects.end(), iterator->first) !=
                    task.protected_objects.end();
                if (in_scope && !protected_object) {
                    agent_ghost_selected_object = iterator->first;
                }
                return true;
            }
            return true;
        }

        if (snapshot.selection.mode == carto::editor::SelectionMode::edge) {
            float best = 12.0F * 12.0F;
            const ViewportEdgeHit* hit = nullptr;
            for (const auto& candidate : native_viewport_hits.edges) {
                const float distance = point_segment_distance_squared(
                    point, candidate.first, candidate.second);
                if (distance < best) {
                    best = distance;
                    hit = &candidate;
                }
            }
            if (hit != nullptr) {
                static_cast<void>(dispatch(SelectEdgeAction{
                    hit->object, hit->edge, *selection_operation}));
            }
        } else if (snapshot.selection.mode == carto::editor::SelectionMode::face) {
            for (auto iterator = native_viewport_hits.faces.rbegin();
                 iterator != native_viewport_hits.faces.rend(); ++iterator) {
                if (point_in_triangle(
                        point, iterator->points[0], iterator->points[1], iterator->points[2])) {
                    static_cast<void>(dispatch(SelectFaceAction{
                        iterator->object, iterator->face, *selection_operation}));
                    return true;
                }
            }
        } else if (snapshot.selection.mode == carto::editor::SelectionMode::vertex) {
            float best = 12.0F * 12.0F;
            const ViewportVertexHit* hit = nullptr;
            for (const auto& candidate : native_viewport_hits.vertices) {
                const float dx = candidate.point.x - point.x;
                const float dy = candidate.point.y - point.y;
                const float distance = dx * dx + dy * dy;
                if (distance < best) {
                    best = distance;
                    hit = &candidate;
                }
            }
            if (hit != nullptr) {
                static_cast<void>(dispatch(SelectVertexAction{
                    hit->object, hit->vertex, *selection_operation}));
            }
        } else {
            for (auto iterator = native_viewport_hits.objects.rbegin();
                 iterator != native_viewport_hits.objects.rend(); ++iterator) {
                if (iterator->second.Contains(point)) {
                    static_cast<void>(dispatch(SelectObjectAction{
                        iterator->first, *selection_operation}));
                    return true;
                }
            }
        }
        return true;
    }

    void export_gltf(const UiSnapshot& snapshot) {
        if (!snapshot.project_path.has_value()) {
            MessageBoxW(window, L"Save the .carto project before exporting a glTF asset.",
                        L"Cartographer export unavailable", MB_OK | MB_ICONWARNING);
            return;
        }
        if (snapshot.dirty) {
            MessageBoxW(window, L"Save the project before exporting so the derived asset has a durable source revision.",
                        L"Cartographer export unavailable", MB_OK | MB_ICONWARNING);
            return;
        }
        const auto path = choose_file(true, FileDialogKind::gltf);
        if (!path.has_value()) return;
        carto::providers::Registry providers;
        if (auto result = carto::io::register_builtin_gltf_providers(providers); !result) {
            MessageBoxW(window, wide(result.error().message).c_str(),
                        L"Cartographer exporter unavailable", MB_OK | MB_ICONERROR);
            return;
        }
        if (auto result = providers.resolve("geometry.export.gltf"); !result) {
            MessageBoxW(window, wide(result.error().message).c_str(),
                        L"Cartographer exporter unavailable", MB_OK | MB_ICONERROR);
            return;
        }
        const auto document = carto::project::ProjectDocument::load(*snapshot.project_path);
        if (!document) {
            MessageBoxW(window, wide(document.error().message).c_str(),
                        L"Cartographer export failed", MB_OK | MB_ICONERROR);
            return;
        }
        const auto report = carto::io::export_gltf(document.value(), *path);
        if (!report) {
            MessageBoxW(window, wide(report.error().message).c_str(),
                        L"Cartographer export failed", MB_OK | MB_ICONERROR);
            return;
        }
        const std::string message = "Exported " + path->string() + "\\n" +
            std::to_string(report.value().meshes) + " mesh(es), " +
            std::to_string(report.value().triangles) + " triangle(s).";
        MessageBoxW(window, wide(message).c_str(),
                    L"Cartographer glTF export", MB_OK | MB_ICONINFORMATION);
    }

    void prepare_content(const UiSnapshot& snapshot, bool selected_only) {
        if (preparation_job != nullptr &&
            !carto::production::preparation_job_state_is_terminal(
                preparation_job->snapshot().state)) {
            MessageBoxW(window,
                        L"A preparation operation is already running. Cancel it or wait for its receipt before starting another.",
                        L"Cartographer preparation active", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (!snapshot.project_path.has_value()) {
            MessageBoxW(window, L"Save the .carto project before preparing content.",
                        L"Cartographer preparation unavailable", MB_OK | MB_ICONWARNING);
            return;
        }
        if (snapshot.dirty) {
            MessageBoxW(window,
                        L"Save the project before preparation so every product is bound to one durable source revision.",
                        L"Cartographer preparation unavailable", MB_OK | MB_ICONWARNING);
            return;
        }
        const auto document = carto::project::ProjectDocument::load(
            snapshot.project_path.value());
        if (!document) {
            MessageBoxW(window, wide(document.error().message).c_str(),
                        L"Cartographer preparation failed", MB_OK | MB_ICONERROR);
            return;
        }

        carto::production::PreparationScopeRequest scope;
        scope.expected_source_revision = document.value().revision();
        if (selected_only) {
            scope.kind = carto::production::PreparationScopeKind::selection;
            scope.node_ids.reserve(snapshot.selection.objects.size() + 1U);
            for (const auto object : snapshot.selection.objects) {
                scope.node_ids.push_back(object.value);
            }
            if (snapshot.selection.component_object.has_value()) {
                scope.node_ids.push_back(snapshot.selection.component_object->value);
            }
            std::sort(scope.node_ids.begin(), scope.node_ids.end());
            scope.node_ids.erase(
                std::unique(scope.node_ids.begin(), scope.node_ids.end()),
                scope.node_ids.end());
            if (scope.node_ids.empty()) {
                MessageBoxW(window, L"Select at least one scene object to prepare a selection.",
                            L"Cartographer preparation scope", MB_OK | MB_ICONWARNING);
                return;
            }
        } else {
            scope.kind = carto::production::PreparationScopeKind::scene;
        }

        const std::filesystem::path output_root{
            snapshot.project_path->string() + ".prepared"};
        const auto current = carto::production::read_selected_prepared_scene(output_root);
        if (!current) {
            MessageBoxW(window, wide(current.error().message).c_str(),
                        L"Cartographer preparation baseline invalid", MB_OK | MB_ICONERROR);
            return;
        }
        std::optional<carto::assets::Sha256Digest> expected;
        if (current.value().has_value()) {
            expected = current.value()->prepared_scene_digest;
        }
        auto source_root = snapshot.project_path->parent_path();
        if (source_root.empty()) source_root = std::filesystem::current_path();
        const carto::production::PreparationOperationRequest request{
            .scope = std::move(scope),
            .profile = carto::production::portable_preparation_profile(),
            .source_root = std::move(source_root),
            .output_root = output_root,
            .expected_selected_digest = expected,
            .stop_token = {},
        };
        auto started = carto::production::PreparationJob::start(
            document.value(), request);
        if (!started) {
            MessageBoxW(window, wide(started.error().message).c_str(),
                        L"Cartographer preparation failed", MB_OK | MB_ICONERROR);
            return;
        }
        preparation_job = std::move(started.value());
    }

    void poll_preparation_job() {
        if (preparation_job == nullptr) return;
        const auto status = preparation_job->snapshot();
        if (!carto::production::preparation_job_state_is_terminal(status.state)) return;

        preparation_job->wait();
        const auto report = preparation_job->report();
        preparation_job.reset();
        if (!report.has_value()) {
            MessageBoxW(window, L"Preparation ended without a terminal receipt.",
                        L"Cartographer preparation failed", MB_OK | MB_ICONERROR);
            return;
        }
        if (!report->succeeded()) {
            std::string message = "Preparation ended as " +
                std::string(carto::production::preparation_job_state_name(status.state)) + ".";
            for (const auto& diagnostic : report->diagnostics) {
                message += "\n\n" + diagnostic.code + "\n" + diagnostic.message;
                if (diagnostic.source_primary.has_value()) {
                    message += "\nSource " +
                        std::to_string(diagnostic.source_primary.value());
                }
                if (message.size() > 6'000U) break;
            }
            const UINT icon = status.state ==
                    carto::production::PreparationJobState::cancelled
                ? MB_ICONINFORMATION
                : MB_ICONERROR;
            MessageBoxW(window, wide(message).c_str(),
                        L"Cartographer preparation", MB_OK | icon);
            return;
        }
        std::string message =
            "Prepared " + std::to_string(report->resolved_scope->asset_ids.size()) +
            " asset(s), " + std::to_string(report->resolved_scope->node_ids.size()) +
            " node(s), and " + std::to_string(report->envelope->products.size()) +
            " verified product(s).\n\nRevision " +
            report->publication->prepared_scene_digest.hex() + "\n" +
            report->publication->revision_path.string();
        if (!report->diagnostics.empty()) {
            message += "\n\nCompleted with " +
                std::to_string(report->diagnostics.size()) + " warning(s).";
        }
        MessageBoxW(window, wide(message).c_str(),
                    L"Cartographer preparation complete", MB_OK | MB_ICONINFORMATION);
    }

    void export_standard_mesh(const UiSnapshot& snapshot, StandardMeshExport format) {
        if (!snapshot.project_path.has_value()) {
            MessageBoxW(window, L"Save the .carto project before exporting a mesh asset.",
                        L"Cartographer export unavailable", MB_OK | MB_ICONWARNING);
            return;
        }
        if (snapshot.dirty) {
            MessageBoxW(window, L"Save the project before exporting so the derived asset has a durable source revision.",
                        L"Cartographer export unavailable", MB_OK | MB_ICONWARNING);
            return;
        }
        const FileDialogKind dialog_kind = format == StandardMeshExport::obj
            ? FileDialogKind::obj
            : format == StandardMeshExport::ply ? FileDialogKind::ply : FileDialogKind::stl;
        const auto path = choose_file(true, dialog_kind);
        if (!path.has_value()) return;

        carto::providers::Registry providers;
        const auto register_provider = [&]() -> carto::core::Result<void> {
            if (format == StandardMeshExport::obj) return carto::io::register_builtin_obj_providers(providers);
            if (format == StandardMeshExport::ply) return carto::io::register_builtin_ply_providers(providers);
            return carto::io::register_builtin_stl_providers(providers);
        };
        const char* capability = format == StandardMeshExport::obj
            ? "geometry.export.obj"
            : format == StandardMeshExport::ply ? "geometry.export.ply" : "geometry.export.stl";
        if (auto result = register_provider(); !result) {
            MessageBoxW(window, wide(result.error().message).c_str(),
                        L"Cartographer exporter unavailable", MB_OK | MB_ICONERROR);
            return;
        }
        if (auto result = providers.resolve(capability); !result) {
            MessageBoxW(window, wide(result.error().message).c_str(),
                        L"Cartographer exporter unavailable", MB_OK | MB_ICONERROR);
            return;
        }
        const auto document = carto::project::ProjectDocument::load(*snapshot.project_path);
        if (!document) {
            MessageBoxW(window, wide(document.error().message).c_str(),
                        L"Cartographer export failed", MB_OK | MB_ICONERROR);
            return;
        }
        std::vector<std::uint64_t> selected_nodes;
        selected_nodes.reserve(snapshot.selection.objects.size() + 1U);
        for (const auto object : snapshot.selection.objects) {
            selected_nodes.push_back(object.value);
        }
        if (snapshot.selection.component_object.has_value()) {
            selected_nodes.push_back(snapshot.selection.component_object->value);
        }
        std::sort(selected_nodes.begin(), selected_nodes.end());
        selected_nodes.erase(
            std::unique(selected_nodes.begin(), selected_nodes.end()),
            selected_nodes.end());

        carto::core::Result<std::uint64_t> resolved_mesh =
            carto::production::resolve_single_mesh_export(document.value(), std::nullopt);
        if (!selected_nodes.empty()) {
            const auto scope = carto::production::resolve_preparation_scope(
                document.value(), carto::production::PreparationScopeRequest{
                    carto::production::PreparationScopeKind::selection,
                    snapshot.project_revision,
                    {},
                    selected_nodes,
                });
            if (!scope) {
                MessageBoxW(window, wide(scope.error().message).c_str(),
                            L"Cartographer export scope invalid", MB_OK | MB_ICONWARNING);
                return;
            }
            if (scope.value().asset_ids.size() != 1U) {
                MessageBoxW(window,
                            L"Select objects that resolve to exactly one reusable mesh asset.",
                            L"Cartographer export scope ambiguous", MB_OK | MB_ICONWARNING);
                return;
            }
            resolved_mesh = carto::core::Result<std::uint64_t>::success(
                scope.value().asset_ids.front());
        }
        if (!resolved_mesh) {
            MessageBoxW(window, wide(resolved_mesh.error().message).c_str(),
                        L"Cartographer export scope required", MB_OK | MB_ICONWARNING);
            return;
        }
        const auto export_mesh = [&]() -> carto::core::Result<carto::io::IoReport> {
            const auto& mesh = document.value().meshes().at(resolved_mesh.value());
            if (format == StandardMeshExport::obj) return carto::io::export_obj(mesh, *path);
            if (format == StandardMeshExport::ply) return carto::io::export_ply(mesh, *path);
            return carto::io::export_stl(mesh, *path);
        };
        const auto report = export_mesh();
        if (!report) {
            MessageBoxW(window, wide(report.error().message).c_str(),
                        L"Cartographer export failed", MB_OK | MB_ICONERROR);
            return;
        }
        const std::string message = "Exported " + path->string() + "\n" +
            "Mesh asset " + std::to_string(resolved_mesh.value()) + ", " +
            std::to_string(report.value().vertices) + " vertex/vertices, " +
            std::to_string(report.value().triangles) + " triangle(s).";
        MessageBoxW(window, wide(message).c_str(),
                    L"Cartographer mesh export", MB_OK | MB_ICONINFORMATION);
    }

    void recover_latest_snapshot(const UiSnapshot& snapshot) {
        std::optional<std::filesystem::path> path = snapshot.project_path;
        if (!path.has_value()) path = choose_file(false, FileDialogKind::project);
        if (!path.has_value()) return;

        const auto inspection = ApplicationSession::inspect_recovery(*path);
        if (!inspection) {
            MessageBoxW(window, wide(inspection.error().message).c_str(),
                        L"Cartographer recovery unavailable", MB_OK | MB_ICONERROR);
            return;
        }
        if (inspection.value().state ==
            carto::application::RecoveryInspectionState::blocked) {
            const std::string message = inspection.value().diagnostic.has_value()
                ? inspection.value().diagnostic->message
                : "durable recovery evidence is blocked";
            MessageBoxW(window, wide(message).c_str(),
                        L"Cartographer recovery blocked", MB_OK | MB_ICONERROR);
            return;
        }
        if (inspection.value().state !=
            carto::application::RecoveryInspectionState::pending) {
            MessageBoxW(window,
                        L"No pending journal snapshot is available for this project.",
                        L"Cartographer recovery", MB_OK | MB_ICONINFORMATION);
            return;
        }

        carto::application::RecoverProjectAction action{*path};
        if (snapshot.dirty) {
            action.discard_dirty = true;
            request_discard(std::move(action));
        } else {
            static_cast<void>(dispatch(action));
        }
    }

    void request_discard(carto::application::ApplicationAction action) {
        pending_discard_action = std::move(action);
        pending_exit = false;
        discard_prompt_pending = true;
    }

    void request_close() noexcept { close_requested = true; }

    void approve_close() noexcept {
        PostQuitMessage(0);
    }

    void process_close_request() {
        if (!close_requested) return;
        close_requested = false;
        if (ui.can_close()) {
            approve_close();
            return;
        }
        pending_discard_action.reset();
        pending_exit = true;
        discard_prompt_pending = true;
    }

    void draw_discard_prompt() {
        if (discard_prompt_pending) {
            ImGui::OpenPopup("Discard Unsaved Changes");
            discard_prompt_pending = false;
        }
        if (!ImGui::BeginPopupModal(
                "Discard Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            return;
        }
        ImGui::TextUnformatted("This project has unsaved changes.");
        ImGui::TextUnformatted("Discard them and continue?");
        if (ImGui::Button("Discard", ImVec2(120, 0))) {
            if (pending_exit) {
                pending_exit = false;
                pending_discard_action.reset();
                ImGui::CloseCurrentPopup();
                approve_close();
            } else if (pending_discard_action.has_value()) {
                auto action = std::move(*pending_discard_action);
                pending_discard_action.reset();
                ImGui::CloseCurrentPopup();
                static_cast<void>(dispatch(action));
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep Editing", ImVec2(120, 0))) {
            pending_exit = false;
            pending_discard_action.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    [[nodiscard]] bool has_pane(const ApplicationSnapshot& snapshot, carto::application::Pane pane) const {
        return std::find(
            snapshot.workspace.visible_panes.begin(),
            snapshot.workspace.visible_panes.end(),
            pane) != snapshot.workspace.visible_panes.end();
    }

    [[nodiscard]] static std::optional<carto::ui::Panel> ui_panel(
        carto::application::Pane pane) noexcept {
        switch (pane) {
        case carto::application::Pane::outliner: return carto::ui::Panel::scene;
        case carto::application::Pane::viewport: return carto::ui::Panel::viewport;
        case carto::application::Pane::inspector: return carto::ui::Panel::inspector;
        case carto::application::Pane::problems: return carto::ui::Panel::problems;
        case carto::application::Pane::history: return carto::ui::Panel::operations;
        }
        return std::nullopt;
    }

    void toggle_pane(const UiSnapshot& snapshot, carto::application::Pane pane) {
        if (pane == carto::application::Pane::viewport) return;
        const auto panel = ui_panel(pane);
        if (!panel.has_value()) return;
        const bool visible = std::find(
            snapshot.visible_panels.begin(), snapshot.visible_panels.end(), *panel) !=
            snapshot.visible_panels.end();
        static_cast<void>(ui.set_panel_visible(*panel, !visible));
    }

    [[nodiscard]] static ImVec4 color(carto::ui::UiColor value) noexcept {
        return ImVec4(value.red, value.green, value.blue, value.alpha);
    }

    void apply_theme(const UiSnapshot& snapshot) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.Colors[ImGuiCol_WindowBg] = color(snapshot.colors.canvas_0);
        style.Colors[ImGuiCol_ChildBg] = color(snapshot.colors.panel_0);
        style.Colors[ImGuiCol_PopupBg] = color(snapshot.colors.panel_1);
        style.Colors[ImGuiCol_MenuBarBg] = color(snapshot.colors.panel_0);
        style.Colors[ImGuiCol_TitleBg] = color(snapshot.colors.canvas_0);
        style.Colors[ImGuiCol_TitleBgActive] = color(snapshot.colors.panel_0);
        style.Colors[ImGuiCol_TitleBgCollapsed] = color(snapshot.colors.canvas_0);
        style.Colors[ImGuiCol_Border] = color(snapshot.colors.border_soft);
        style.Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        ImVec4 separator = color(snapshot.colors.border_soft);
        separator.w = 0.45F;
        style.Colors[ImGuiCol_Separator] = separator;
        style.Colors[ImGuiCol_SeparatorHovered] = color(snapshot.colors.border_focus);
        style.Colors[ImGuiCol_SeparatorActive] = color(snapshot.colors.accent_primary);
        style.Colors[ImGuiCol_FrameBg] = color(snapshot.colors.panel_1);
        style.Colors[ImGuiCol_FrameBgHovered] = color(snapshot.colors.panel_hover);
        style.Colors[ImGuiCol_FrameBgActive] = color(snapshot.colors.panel_selected);
        style.Colors[ImGuiCol_Button] = color(snapshot.colors.panel_1);
        style.Colors[ImGuiCol_ButtonHovered] = color(snapshot.colors.panel_hover);
        style.Colors[ImGuiCol_ButtonActive] = color(snapshot.colors.panel_selected);
        style.Colors[ImGuiCol_Header] = color(snapshot.colors.panel_1);
        style.Colors[ImGuiCol_HeaderHovered] = color(snapshot.colors.panel_hover);
        style.Colors[ImGuiCol_HeaderActive] = color(snapshot.colors.panel_selected);
        style.Colors[ImGuiCol_Text] = color(snapshot.colors.text_primary);
        style.Colors[ImGuiCol_TextDisabled] = color(snapshot.colors.text_disabled);
        style.Colors[ImGuiCol_TextSelectedBg] = color(snapshot.colors.panel_selected);
        style.Colors[ImGuiCol_CheckMark] = color(snapshot.colors.accent_primary);
        style.Colors[ImGuiCol_SliderGrab] = color(snapshot.colors.accent_primary);
        style.Colors[ImGuiCol_SliderGrabActive] = color(snapshot.colors.accent_secondary);
        style.Colors[ImGuiCol_ScrollbarBg] = color(snapshot.colors.canvas_0);
        style.Colors[ImGuiCol_ScrollbarGrab] = color(snapshot.colors.border_soft);
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = color(snapshot.colors.border_focus);
        style.Colors[ImGuiCol_ScrollbarGrabActive] = color(snapshot.colors.accent_primary);
        style.Colors[ImGuiCol_NavHighlight] = color(snapshot.colors.accent_primary);
        style.Colors[ImGuiCol_Tab] = color(snapshot.colors.panel_0);
        style.Colors[ImGuiCol_TabHovered] = color(snapshot.colors.panel_hover);
        style.Colors[ImGuiCol_TabActive] = color(snapshot.colors.panel_selected);
        // The workbench is equipment, not a dashboard. Keep the HAVEN-derived
        // palette, but use a restrained instrument geometry so rails, cards,
        // and floating tools read as parts of one machine.
        style.WindowRounding = 6.0F;
        style.ChildRounding = 5.0F;
        style.FrameRounding = 4.0F;
        style.PopupRounding = 5.0F;
        style.ScrollbarRounding = 3.0F;
        style.WindowBorderSize = 1.0F;
        style.ChildBorderSize = 1.0F;
        style.FrameBorderSize = 1.0F;
        style.ScrollbarSize = 9.0F;
        style.GrabMinSize = 14.0F;
        style.ItemSpacing = ImVec2(8.0F, 7.0F);
        style.ItemInnerSpacing = ImVec2(6.0F, 5.0F);
        style.SelectableTextAlign = ImVec2(0.0F, 0.5F);
        style.WindowPadding = snapshot.density == carto::ui::Density::compact
            ? ImVec2(10, 8) : snapshot.density == carto::ui::Density::touch
            ? ImVec2(14, 12) : ImVec2(13, 10);
        style.FramePadding = snapshot.density == carto::ui::Density::compact
            ? ImVec2(8, 5) : snapshot.density == carto::ui::Density::touch
            ? ImVec2(11, 8) : ImVec2(9, 6);
    }

    void apply_window_chrome(const UiSnapshot& snapshot) {
        if (window == nullptr || applied_window_theme == snapshot.theme) return;
        const BOOL dark_mode = snapshot.theme != carto::ui::Theme::light ? TRUE : FALSE;
        const COLORREF caption = dark_mode == TRUE ? RGB(7, 11, 18) : RGB(244, 246, 249);
        const COLORREF text = dark_mode == TRUE ? RGB(230, 236, 250) : RGB(20, 26, 36);
        // These attributes are supported by current Windows 10/11 builds. The
        // calls are intentionally best-effort so older systems keep the native
        // title bar instead of making startup fail.
        static_cast<void>(DwmSetWindowAttribute(
            window, static_cast<DWMWINDOWATTRIBUTE>(20), &dark_mode, sizeof(dark_mode)));
        static_cast<void>(DwmSetWindowAttribute(
            window, static_cast<DWMWINDOWATTRIBUTE>(35), &caption, sizeof(caption)));
        static_cast<void>(DwmSetWindowAttribute(
            window, static_cast<DWMWINDOWATTRIBUTE>(36), &text, sizeof(text)));
        applied_window_theme = snapshot.theme;
    }

    [[nodiscard]] ImVec4 status_color(const UiSnapshot& snapshot) const noexcept {
        switch (snapshot.project_status) {
        case carto::ui::ProjectStatus::unsaved:
            return color(snapshot.colors.semantic_warning);
        case carto::ui::ProjectStatus::saved:
            return color(snapshot.colors.semantic_success);
        case carto::ui::ProjectStatus::modified:
            return color(snapshot.colors.modified);
        case carto::ui::ProjectStatus::render_error:
            return color(snapshot.colors.semantic_warning);
        case carto::ui::ProjectStatus::ui_error:
            return color(snapshot.colors.semantic_error);
        }
        return color(snapshot.colors.text_secondary);
    }

    [[nodiscard]] const char* surface_title() const noexcept {
        switch (surface) {
        case ShellSurface::workspace: return "Workspace";
        case ShellSurface::project: return "Project";
        case ShellSurface::market: return "Market";
        case ShellSurface::diagnostics: return "Diagnostics";
        case ShellSurface::settings: return "Settings";
        }
        return "Workspace";
    }

    [[nodiscard]] std::string project_location(const UiSnapshot& snapshot) const {
        if (!snapshot.project_path.has_value()) return "Local workspace · not saved to disk";
        return snapshot.project_path->string();
    }

    void draw_shell_header(const UiSnapshot& snapshot) {
        const bool compact_header = ImGui::GetWindowWidth() < 900.0F;
        ImGui::BeginChild("ShellHeader", ImVec2(0, compact_header ? 124.0F : 96.0F),
                          true, ImGuiWindowFlags_NoScrollbar);
        ImGui::BeginGroup();
        draw_brand_lockup(snapshot);
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
        heading_text(surface_title());
        ImGui::Text("%s", snapshot.project_name.c_str());
        mono_text_disabled(project_location(snapshot).c_str());
        ImGui::EndGroup();

        if (compact_header) ImGui::Dummy(ImVec2(0, 4));
        else ImGui::SameLine(std::max(240.0F, ImGui::GetWindowWidth() - 555.0F));
        ImGui::BeginGroup();
        const ImVec4 status = status_color(snapshot);
        ImVec4 status_surface = status;
        status_surface.w = 0.16F;
        ImGui::PushStyleColor(ImGuiCol_Button, status_surface);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, status_surface);
        ImGui::PushStyleColor(ImGuiCol_Text, status);
        const std::string status_label = snapshot.project_status_text + "##header-status";
        ImGui::SmallButton(status_label.c_str());
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::TextDisabled("Appearance");
        ImGui::SameLine();
        if (snapshot.theme == carto::ui::Theme::dark) ImGui::PushStyleColor(
            ImGuiCol_Button, color(snapshot.colors.panel_selected));
        if (ImGui::SmallButton("Dark##header-theme")) {
            static_cast<void>(ui.set_theme(carto::ui::Theme::dark));
        }
        if (snapshot.theme == carto::ui::Theme::dark) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (snapshot.theme == carto::ui::Theme::light) ImGui::PushStyleColor(
            ImGuiCol_Button, color(snapshot.colors.panel_selected));
        if (ImGui::SmallButton("Light##header-theme")) {
            static_cast<void>(ui.set_theme(carto::ui::Theme::light));
        }
        if (snapshot.theme == carto::ui::Theme::light) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(compact_header ? "Commands" : "Command palette  Ctrl+K")) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::toggle_command_palette));
        }
        ImGui::SameLine();
        if (ImGui::Button(compact_header ? "Agent" : "Ask Cartographer…")) {
            static_cast<void>(open_agent_workspace(true));
        }
        ImGui::SameLine();
        if (ImGui::Button(compact_header ? "Flow" : "Compose")) {
            ImGui::OpenPopup("Compose workbench");
        }
        if (ImGui::BeginPopup("Compose workbench")) {
            ImGui::TextDisabled("Assemble the work surface for your flow.");
            ImGui::TextDisabled("Learns from your choices only; stays local.");
            ImGui::Separator();
            const auto remembered = remembered_workbench_flow();
            if (remembered.has_value()) {
                const std::string resume_label = "Resume " +
                    std::string(workbench_flow_name(*remembered));
                if (ImGui::MenuItem(resume_label.c_str(), "Ctrl+Shift+R")) {
                    compose_workbench_flow(*remembered);
                }
                ImGui::Separator();
            }
            for (const WorkbenchFlow flow : workbench_flow_order()) {
                std::string label = workbench_flow_name(flow);
                if (remembered.has_value() && flow == *remembered &&
                    workbench_flow_uses(flow) > 0U) {
                    label += "  · Recommended";
                }
                if (ImGui::MenuItem(label.c_str(), workbench_flow_shortcut(flow),
                                    workbench_flow == flow)) {
                    compose_workbench_flow(flow);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Forget learned order")) {
                forget_workbench_flow_memory();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", workbench_flow_name(workbench_flow));
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            static_cast<void>(save_project(snapshot));
        }
        ImGui::EndGroup();
        ImGui::EndChild();
    }

    void draw_navigation_rail(const UiSnapshot& snapshot) {
        const float available_width = ImGui::GetContentRegionAvail().x;
        const bool narrow = available_width < 760.0F;
        const float rail_width = narrow ? 72.0F : available_width < 980.0F ? 184.0F : 232.0F;
        ImGui::BeginChild("ProductRail", ImVec2(rail_width, 0), true);
        if (narrow) {
            draw_brand_mark(snapshot, 34.0F);
            ImGui::TextDisabled("Carto");
        } else {
            draw_brand_mark(snapshot, 42.0F);
            ImGui::SameLine();
            ImGui::BeginGroup();
            heading_text("Spatial authoring");
            ImGui::TextDisabled("Geometry, history, and evidence.");
            ImGui::EndGroup();
        }
        ImGui::Dummy(ImVec2(0, 6));

        const auto nav_item = [this, &snapshot, narrow](const char* label, const char* compact_label,
                                                        const char* detail, ShellSurface target) {
            const bool selected = surface == target;
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Header, color(snapshot.colors.panel_selected));
                ImGui::PushStyleColor(ImGuiCol_Text, color(snapshot.colors.accent_primary));
            }
            const char* shown_label = narrow ? compact_label : label;
            if (ImGui::Selectable(shown_label, selected, 0, ImVec2(0, 32))) surface = target;
            if (selected) ImGui::PopStyleColor(2);
            if (narrow && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(label);
                ImGui::TextDisabled("%s", detail);
                ImGui::EndTooltip();
            } else if (!narrow) {
                ImGui::TextDisabled("  %s", detail);
            }
        };
        nav_item("Workspace", "W", "Author geometry", ShellSurface::workspace);
        nav_item("Project", "P", "Persistence and revision", ShellSurface::project);
        nav_item("Market", "M", "Local data packages", ShellSurface::market);
        nav_item("Diagnostics", "D", "Problems and capability", ShellSurface::diagnostics);
        nav_item("Settings", "S", "Density, theme, operator", ShellSurface::settings);

        ImGui::Spacing();
        if (ImGui::Button(narrow ? "+" : "+  New Project", ImVec2(-1, 34))) {
            carto::application::NewProjectAction action{"Untitled"};
            if (snapshot.dirty) {
                action.discard_dirty = true;
                request_discard(std::move(action));
            } else {
                static_cast<void>(dispatch(action));
            }
        }
        if (ImGui::Button(narrow ? "Open" : "Open Project", ImVec2(-1, 30))) {
            if (const auto path = choose_file(false); path.has_value()) {
                carto::application::OpenProjectAction action{*path};
                if (snapshot.dirty) {
                    action.discard_dirty = true;
                    request_discard(std::move(action));
                } else {
                    static_cast<void>(dispatch(action));
                }
            }
        }

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::TextDisabled(narrow ? "State" : "Session");
        if (narrow) {
            ImGui::Text("R%llu", static_cast<unsigned long long>(snapshot.project_revision.value()));
            ImGui::Text("%zu obj", snapshot.objects.size());
            ImGui::TextColored(status_color(snapshot), "%s", snapshot.dirty ? "●" : "○");
            ImGui::Spacing();
            ImGui::TextColored(renderer_ready ? color(snapshot.colors.semantic_success)
                                              : color(snapshot.colors.semantic_error),
                              renderer_ready ? "●" : "×");
        } else {
            ImGui::Text("Revision %llu", static_cast<unsigned long long>(snapshot.project_revision.value()));
            ImGui::Text("%zu object%s", snapshot.objects.size(), snapshot.objects.size() == 1U ? "" : "s");
            ImGui::TextColored(status_color(snapshot), "%s", snapshot.project_status_text.c_str());
            if (snapshot.dirty) ImGui::TextColored(color(snapshot.colors.modified), "Unsaved changes");
            ImGui::Spacing();
            ImGui::TextDisabled("Native shell");
            ImGui::TextColored(renderer_ready ? color(snapshot.colors.semantic_success)
                                              : color(snapshot.colors.semantic_error),
                              renderer_ready ? "Active" : "Unavailable");
            ImGui::TextDisabled("Mutations remain behind the application session.");
        }
        ImGui::EndChild();
    }

    void rack_button(const UiSnapshot& snapshot, const char* label, bool& open) {
        if (open) {
            ImGui::PushStyleColor(ImGuiCol_Button, color(snapshot.colors.panel_selected));
            ImGui::PushStyleColor(ImGuiCol_Text, color(snapshot.colors.accent_primary));
        }
        if (ImGui::Button(label, ImVec2(-1, 34))) {
            open = !open;
            mark_custom_workbench();
        }
        if (open) ImGui::PopStyleColor(2);
    }

    void draw_tool_rack(const UiSnapshot& snapshot) {
        ImGui::BeginChild("ToolRack", ImVec2(kWorkbenchRackWidth, 0), true);
        heading_text_colored(snapshot.colors.accent_primary, "Tool rack");
        ImGui::Dummy(ImVec2(0, 4));
        rack_button(snapshot, "Scene", workbench.scene);
        rack_button(snapshot, "Assets", workbench.assets);
        rack_button(snapshot, "Layers", workbench.layers);
        rack_button(snapshot, "Tools", workbench.tools);
        rack_button(snapshot, "Refs", workbench.references);
        rack_button(snapshot, "Draft", workbench.draft);
        ImGui::Spacing();
        ImGui::TextDisabled("Drag an instrument\nfrom here.");
        ImGui::EndChild();
    }

    void draw_bay_rack(const UiSnapshot& snapshot) {
        ImGui::BeginChild("InstrumentBay", ImVec2(kWorkbenchRackWidth, 0), true);
        heading_text_colored(snapshot.colors.accent_secondary, "Instrument bay");
        ImGui::Dummy(ImVec2(0, 4));
        rack_button(snapshot, "Inspect", workbench.inspector);
        rack_button(snapshot, "Transform", workbench.transform);
        rack_button(snapshot, "Material", workbench.material);
        rack_button(snapshot, "Constraint", workbench.constraint);
        rack_button(snapshot, "Modify", workbench.modify);
        rack_button(snapshot, "Fidelity", workbench.measure);
        rack_button(snapshot, "Extrude", workbench.extrude);
        ImGui::Spacing();
        ImGui::TextDisabled("Stored instruments\nlive here.");
        ImGui::EndChild();
    }

    [[nodiscard]] ImVec2 instrument_origin(ImVec2 offset, ImVec2 size) const noexcept {
        if (workbench_bounds_valid) {
            if (offset.x < 0.0F) offset.x = workbench_extent.x + offset.x - size.x;
            if (offset.y < 0.0F) offset.y = workbench_extent.y + offset.y - size.y;
            return workbench_origin + offset;
        }
        return ImGui::GetMainViewport()->WorkPos + offset;
    }

    bool begin_instrument(const UiSnapshot& snapshot,
                          carto::ui::WorkbenchInstrument instrument,
                          const char* title, bool& open,
                          ImVec2 offset, ImVec2 size, carto::ui::UiColor accent) {
        const std::size_t index = workbench_index(instrument);
        auto& layout = workbench.persisted.instruments.at(index);
        if (!workbench.seeded.at(index)) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const ImVec2 requested_position = layout.positioned
                ? viewport->WorkPos + ImVec2(static_cast<float>(layout.x), static_cast<float>(layout.y))
                : instrument_origin(offset, size);
            const ImVec2 position = clamp_instrument_position(requested_position,
                                                              layout.positioned
                                                                  ? ImVec2(static_cast<float>(layout.width),
                                                                           static_cast<float>(layout.height))
                                                                  : size);
            const ImVec2 window_size = layout.positioned
                ? ImVec2(static_cast<float>(layout.width), static_cast<float>(layout.height))
                : size;
            ImGui::SetNextWindowPos(position, ImGuiCond_Always);
            ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
            workbench.seeded.at(index) = true;
        }
        ImVec4 title_color = color(accent);
        title_color.w = 0.22F;
        ImVec4 active_title = color(accent);
        active_title.w = 0.32F;
        ImGui::PushStyleColor(ImGuiCol_TitleBg, title_color);
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, active_title);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, color(snapshot.colors.panel_0));
        const bool visible = ImGui::Begin(title, &open,
                                          ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleColor(3);
        if (!visible) {
            ImGui::End();
            return false;
        }

        ImGui::BeginChild("##instrument_grip", ImVec2(-1, 7), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 grip_origin = ImGui::GetCursorScreenPos();
        const ImVec2 grip_extent = ImGui::GetContentRegionAvail();
        ImVec4 grip_color = color(accent);
        grip_color.w = 0.70F;
        ImGui::GetWindowDrawList()->AddRectFilled(
            grip_origin + ImVec2(0.0F, 1.0F), grip_origin + ImVec2(grip_extent.x, 5.0F),
            ImGui::ColorConvertFloat4ToU32(grip_color), 2.0F);
        ImGui::EndChild();
        ImGui::TextDisabled("Drag to reposition · resize · snap to a rack");
        return true;
    }

    void draw_instrument_separator(const UiSnapshot& snapshot, const char* label) {
        ImGui::Separator();
        ImGui::TextColored(color(snapshot.colors.accent_primary), "%s", label);
    }

    void capture_instrument_layout(carto::ui::WorkbenchInstrument instrument) {
        auto& layout = workbench.persisted.instruments.at(workbench_index(instrument));
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 position = ImGui::GetWindowPos() - viewport->WorkPos;
        const ImVec2 size = ImGui::GetWindowSize();
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(size.x) || !std::isfinite(size.y)) return;
        layout.positioned = true;
        layout.x = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            static_cast<std::int64_t>(std::lround(position.x)), -65536, 65536));
        layout.y = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            static_cast<std::int64_t>(std::lround(position.y)), -65536, 65536));
        layout.width = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            static_cast<std::int64_t>(std::lround(size.x)), 120, 4096));
        layout.height = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            static_cast<std::int64_t>(std::lround(size.y)), 80, 4096));
    }

    [[nodiscard]] ImVec2 clamp_instrument_position(ImVec2 position, ImVec2 size) const noexcept {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 minimum = viewport->WorkPos + ImVec2(
            kWorkbenchPlacementMargin, kWorkbenchPlacementMargin);
        const ImVec2 maximum(
            std::max(minimum.x, viewport->WorkPos.x + viewport->WorkSize.x - size.x -
                kWorkbenchPlacementMargin),
            std::max(minimum.y, viewport->WorkPos.y + viewport->WorkSize.y - size.y -
                kWorkbenchPlacementMargin));
        return ImVec2(
            std::clamp(position.x, minimum.x, maximum.x),
            std::clamp(position.y, minimum.y, maximum.y));
    }

    void magnetize_instrument(carto::ui::WorkbenchInstrument instrument,
                              ImVec2 position, ImVec2 size) {
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        std::optional<ImVec2> best_position;
        float best_distance = std::numeric_limits<float>::max();
        const auto consider = [&](ImVec2 candidate) {
            const float dx = std::abs(position.x - candidate.x);
            const float dy = std::abs(position.y - candidate.y);
            if (dx > kWorkbenchSnapDistance || dy > kWorkbenchSnapDistance) return;
            const float distance = dx * dx + dy * dy;
            if (distance < best_distance) {
                best_distance = distance;
                best_position = candidate;
            }
        };

        // Keep a panel recoverable after a display resize or a stale preference
        // file, then make the workbench edges behave like a physical frame.
        const float margin = kWorkbenchPlacementMargin;
        consider(ImVec2(viewport->WorkPos.x + margin, position.y));
        consider(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - size.x - margin, position.y));
        consider(ImVec2(position.x, viewport->WorkPos.y + margin));
        consider(ImVec2(position.x, viewport->WorkPos.y + viewport->WorkSize.y - size.y - margin));
        if (workbench_bounds_valid) {
            consider(ImVec2(workbench_origin.x + gap, position.y));
            consider(ImVec2(workbench_origin.x + workbench_extent.x - size.x - gap, position.y));
            consider(ImVec2(position.x, workbench_origin.y + gap));
            consider(ImVec2(position.x, workbench_origin.y + workbench_extent.y - size.y - gap));
        }

        for (std::size_t index = 0U; index < carto::ui::kWorkbenchInstrumentCount; ++index) {
            const auto other = static_cast<carto::ui::WorkbenchInstrument>(index);
            if (other == instrument || !workbench_visible(workbench, other)) continue;
            const auto& layout = workbench.persisted.instruments[index];
            if (!layout.positioned) continue;
            const ImVec2 other_position = viewport->WorkPos + ImVec2(
                static_cast<float>(layout.x), static_cast<float>(layout.y));
            const ImVec2 other_size(static_cast<float>(layout.width),
                                    static_cast<float>(layout.height));
            consider(ImVec2(other_position.x + other_size.x + gap, other_position.y));
            consider(ImVec2(other_position.x - size.x - gap, other_position.y));
            consider(ImVec2(other_position.x, other_position.y + other_size.y + gap));
            consider(ImVec2(other_position.x, other_position.y - size.y - gap));
            // Also align shared edges without forcing the panels to touch.
            consider(ImVec2(other_position.x, position.y));
            consider(ImVec2(other_position.x + other_size.x - size.x, position.y));
            consider(ImVec2(position.x, other_position.y));
            consider(ImVec2(position.x, other_position.y + other_size.y - size.y));
        }
        const ImVec2 settled = clamp_instrument_position(
            best_position.value_or(position), size);
        if (std::abs(settled.x - position.x) > 0.5F ||
            std::abs(settled.y - position.y) > 0.5F) {
            ImGui::SetWindowPos(settled);
        }
    }

    void settle_instrument(carto::ui::WorkbenchInstrument instrument, bool& open) {
        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
            !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
        const ImVec2 position = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const ImVec2 rack_gap(ImGui::GetStyle().ItemSpacing.x, ImGui::GetStyle().ItemSpacing.y);
        const ImRect left_rack(
            ImVec2(workbench_origin.x - kWorkbenchRackWidth - rack_gap.x, workbench_origin.y),
            ImVec2(workbench_origin.x - rack_gap.x,
                   workbench_origin.y + workbench_extent.y));
        const ImRect right_rack(
            ImVec2(workbench_origin.x + workbench_extent.x + rack_gap.x, workbench_origin.y),
            ImVec2(workbench_origin.x + workbench_extent.x + rack_gap.x + kWorkbenchRackWidth,
                   workbench_origin.y + workbench_extent.y));
        const ImVec2 pointer = ImGui::GetIO().MousePos;
        const bool dropped_on_rack = workbench_bounds_valid &&
            (left_rack.Contains(pointer) || right_rack.Contains(pointer));
        if (dropped_on_rack) {
            open = false;
            auto& layout = workbench.persisted.instruments.at(workbench_index(instrument));
            layout.positioned = false;
            layout.x = 0;
            layout.y = 0;
            layout.width = 0;
            layout.height = 0;
            workbench.seeded.at(workbench_index(instrument)) = false;
            return;
        }
        magnetize_instrument(instrument, position, size);
    }

    void end_instrument(carto::ui::WorkbenchInstrument instrument, bool& open) {
        settle_instrument(instrument, open);
        if (open) capture_instrument_layout(instrument);
        ImGui::End();
    }

    void draw_placeholder_instrument(const UiSnapshot& snapshot,
                                      carto::ui::WorkbenchInstrument instrument,
                                      const char* title,
                                      bool& open, ImVec2 offset, ImVec2 size,
                                      const char* category, const char* description) {
        if (!begin_instrument(snapshot, instrument, title, open, offset, size,
                              snapshot.colors.accent_secondary)) {
            return;
        }
        ImGui::TextDisabled("%s", category);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", description);
        ImGui::Spacing();
        ImGui::TextColored(color(snapshot.colors.semantic_warning), "Stored · not connected");
        ImGui::TextDisabled("This instrument is an explicit capability boundary, not fabricated live state.");
        end_instrument(instrument, open);
    }

    void route_shortcuts() {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) return;
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_1)) {
            compose_workbench_flow(WorkbenchFlow::focus);
        } else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_2)) {
            compose_workbench_flow(WorkbenchFlow::author);
        } else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_3)) {
            compose_workbench_flow(WorkbenchFlow::inspect);
        } else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_4)) {
            compose_workbench_flow(WorkbenchFlow::draft);
        } else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R)) {
            if (const auto remembered = remembered_workbench_flow(); remembered.has_value()) {
                compose_workbench_flow(*remembered);
            }
        } else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_L)) {
            workbench.ledger_collapsed = !workbench.ledger_collapsed;
            mark_custom_workbench();
        } else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_I)) {
            workbench.inspector = !workbench.inspector;
            mark_custom_workbench();
        } else if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_T)) {
            workbench.transform = !workbench.transform;
            mark_custom_workbench();
        } else if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
                   ImGui::IsKeyPressed(ImGuiKey_I)) {
            import_mesh();
        } else if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
                   ImGui::IsKeyPressed(ImGuiKey_D)) {
            duplicate_selected_object(ui.snapshot());
        } else if (io.KeyCtrl && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_B)) {
            prepare_content(ui.snapshot(), io.KeyShift);
        } else if (io.KeyCtrl && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_O)) {
            export_standard_mesh(ui.snapshot(), StandardMeshExport::obj);
        } else if (io.KeyCtrl && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_P)) {
            export_standard_mesh(ui.snapshot(), StandardMeshExport::ply);
        } else if (io.KeyCtrl && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_S)) {
            export_standard_mesh(ui.snapshot(), StandardMeshExport::stl);
        } else if (io.KeyCtrl && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_E)) {
            export_gltf(ui.snapshot());
        }
    }

    void draw_workspace_bar(const UiSnapshot& snapshot) {
        ImGui::BeginChild("WorkspaceBar", ImVec2(0, 52), true);
        for (const auto& definition : carto::ui::workspace_registry()) {
            const auto workspace = definition.workspace;
            const char* label = definition.label.c_str();
            const bool available = definition.available;
            if (available && workspace == snapshot.workspace_kind) {
                ImGui::PushStyleColor(ImGuiCol_Button, color(snapshot.colors.panel_selected));
            }
            if (available) {
                if (ImGui::Button(label)) {
                    if (workspace == carto::ui::Workspace::ai) {
                        static_cast<void>(open_agent_workspace(false));
                    } else if (workspace == carto::ui::Workspace::review) {
                        static_cast<void>(open_agent_review_workspace());
                    } else {
                        static_cast<void>(ui.set_workspace(workspace));
                    }
                }
            } else {
                ImGui::TextDisabled("%s (future)", label);
            }
            if (available && workspace == snapshot.workspace_kind) ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::TextDisabled("Operator:");
        ImGui::SameLine();
        if (ImGui::Button(snapshot.operator_mode == carto::ui::OperatorMode::person_first
                              ? "Person first"
                              : "AI first")) {
            static_cast<void>(ui.set_operator_mode(
                snapshot.operator_mode == carto::ui::OperatorMode::person_first
                    ? carto::ui::OperatorMode::ai_first
                    : carto::ui::OperatorMode::person_first));
        }
        ImGui::EndChild();
    }

    void draw_menu(const UiSnapshot& snapshot) {
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New")) {
                carto::application::NewProjectAction action{"Untitled"};
                if (snapshot.dirty) {
                    action.discard_dirty = true;
                    request_discard(std::move(action));
                }
                else static_cast<void>(dispatch(action));
            }
            if (ImGui::MenuItem("Open")) {
                if (const auto path = choose_file(false); path.has_value()) {
                    carto::application::OpenProjectAction action{*path};
                    if (snapshot.dirty) {
                        action.discard_dirty = true;
                        request_discard(std::move(action));
                    }
                    else static_cast<void>(dispatch(action));
                }
            }
            if (ImGui::MenuItem("Import Mesh...", "Ctrl+I")) {
                import_mesh();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                static_cast<void>(save_project(snapshot));
            }
            if (ImGui::MenuItem("Save As...")) {
                static_cast<void>(save_project(snapshot, true));
            }
            if (ImGui::MenuItem("Recover latest journal snapshot")) {
                recover_latest_snapshot(snapshot);
            }
            if (ImGui::MenuItem("Install data-only market package...")) {
                install_market_package();
            }
            if (ImGui::MenuItem("Refresh local market library")) {
                const auto refreshed = ui.refresh_market_library();
                if (!refreshed) {
                    action_feedback = refreshed.error().message;
                    action_feedback_error = true;
                } else {
                    action_feedback = "Local market library refreshed.";
                    action_feedback_error = false;
                }

            }
            const bool preparation_active = preparation_job != nullptr &&
                !carto::production::preparation_job_state_is_terminal(
                    preparation_job->snapshot().state);
            const bool can_export = snapshot.project_path.has_value() && !snapshot.dirty;
            const bool has_preparation_selection =
                !snapshot.selection.objects.empty() ||
                snapshot.selection.component_object.has_value();
            if (ImGui::MenuItem("Prepare scene", "Ctrl+Alt+B", false,
                                can_export && !preparation_active)) {
                prepare_content(snapshot, false);
            }
            if (ImGui::MenuItem("Prepare selection", "Ctrl+Alt+Shift+B", false,
                                can_export && has_preparation_selection &&
                                    !preparation_active)) {
                prepare_content(snapshot, true);
            }
            if (ImGui::MenuItem("Cancel preparation", nullptr, false,
                                preparation_active)) {
                static_cast<void>(preparation_job->request_cancel());
            }
            if (ImGui::MenuItem("Export glTF", "Ctrl+Alt+E", false, can_export)) {
                export_gltf(snapshot);
            }
            if (ImGui::MenuItem("Export OBJ", "Ctrl+Alt+O", false, can_export)) {
                export_standard_mesh(snapshot, StandardMeshExport::obj);
            }
            if (ImGui::MenuItem("Export PLY", "Ctrl+Alt+P", false, can_export)) {
                export_standard_mesh(snapshot, StandardMeshExport::ply);
            }
            if (ImGui::MenuItem("Export STL", "Ctrl+Alt+S", false, can_export)) {
                export_standard_mesh(snapshot, StandardMeshExport::stl);
            }
            if (!can_export) {
                ImGui::TextDisabled("Export requires a saved, unmodified .carto source");
            }
            if (ImGui::MenuItem("Exit")) request_close();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) dispatch(carto::application::UndoAction{});
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) dispatch(carto::application::RedoAction{});
            ImGui::Separator();
            const bool can_duplicate = snapshot.selection.mode ==
                    carto::editor::SelectionMode::object &&
                snapshot.selection.objects.size() == 1U;
            if (ImGui::MenuItem("Duplicate Selected", "Ctrl+D", false, can_duplicate)) {
                duplicate_selected_object(snapshot);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Scene instrument", nullptr, workbench.scene)) {
                workbench.scene = !workbench.scene;
                mark_custom_workbench();
            }
            if (ImGui::MenuItem("Assets instrument", nullptr, workbench.assets)) {
                workbench.assets = !workbench.assets;
                mark_custom_workbench();
            }
            if (ImGui::MenuItem("Tools instrument", nullptr, workbench.tools)) {
                workbench.tools = !workbench.tools;
                mark_custom_workbench();
            }
            if (ImGui::MenuItem("Inspector instrument", nullptr, workbench.inspector)) {
                workbench.inspector = !workbench.inspector;
                mark_custom_workbench();
            }
            if (ImGui::MenuItem("Transform instrument", nullptr, workbench.transform)) {
                workbench.transform = !workbench.transform;
                mark_custom_workbench();
            }
            if (ImGui::MenuItem("Modify instrument", nullptr, workbench.modify)) {
                workbench.modify = !workbench.modify;
                mark_custom_workbench();
            }
            if (ImGui::MenuItem("Draft instrument", nullptr, workbench.draft)) {
                workbench.draft = !workbench.draft;
                mark_custom_workbench();
            }
            if (ImGui::MenuItem("Fidelity instrument", nullptr, workbench.measure)) {
                workbench.measure = !workbench.measure;
                mark_custom_workbench();
            }
            if (ImGui::MenuItem("Operation ledger", nullptr, !workbench.ledger_collapsed)) {
                workbench.ledger_collapsed = !workbench.ledger_collapsed;
                mark_custom_workbench();
            }
            ImGui::Separator();
            ImGui::TextDisabled("The work surface is fixed; instruments are movable.");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Compact Density", nullptr,
                               snapshot.density == carto::ui::Density::compact)) {
                static_cast<void>(ui.set_density(carto::ui::Density::compact));
            }
            if (ImGui::MenuItem("Standard Density", nullptr,
                               snapshot.density == carto::ui::Density::standard)) {
                static_cast<void>(ui.set_density(carto::ui::Density::standard));
            }
            if (ImGui::MenuItem("Touch Density", nullptr,
                               snapshot.density == carto::ui::Density::touch)) {
                static_cast<void>(ui.set_density(carto::ui::Density::touch));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Dark Theme", nullptr, snapshot.theme == carto::ui::Theme::dark)) {
                static_cast<void>(ui.set_theme(carto::ui::Theme::dark));
            }
            if (ImGui::MenuItem("Light Theme", nullptr, snapshot.theme == carto::ui::Theme::light)) {
                static_cast<void>(ui.set_theme(carto::ui::Theme::light));
            }
            if (ImGui::MenuItem("High Contrast", nullptr,
                               snapshot.theme == carto::ui::Theme::high_contrast)) {
                static_cast<void>(ui.set_theme(carto::ui::Theme::high_contrast));
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    void draw_toolbar(const UiSnapshot& snapshot) {
        ImGui::BeginChild("Toolbar", ImVec2(0, 52), true);
        heading_text("Work surface");
        ImGui::SameLine();
        if (ImGui::Button("Undo")) dispatch(carto::application::UndoAction{});
        ImGui::SameLine();
        if (ImGui::Button("Redo")) dispatch(carto::application::RedoAction{});
        ImGui::SameLine();
        int mode = snapshot.selection.mode == carto::editor::SelectionMode::object ? 0
            : snapshot.selection.mode == carto::editor::SelectionMode::vertex ? 1
            : snapshot.selection.mode == carto::editor::SelectionMode::edge ? 2 : 3;
        const char* modes[] = {"Object", "Vertex", "Edge", "Face"};
        if (ImGui::Combo("Mode", &mode, modes, 4)) {
            if (mode == 0) static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::object_mode));
            else if (mode == 1) static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::vertex_mode));
            else if (mode == 2) static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::edge_mode));
            else static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::face_mode));
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(snapshot.project_status_text.c_str());
        if (!action_feedback.empty()) {
            ImGui::SameLine();
            draw_action_feedback(snapshot, true);
        }
        if (preparation_job != nullptr) {
            const auto preparation = preparation_job->snapshot();
            ImGui::SameLine();
            ImGui::TextDisabled("Prepare: %s",
                carto::production::preparation_job_state_name(preparation.state));
            if (!carto::production::preparation_job_state_is_terminal(
                    preparation.state)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel##preparation")) {
                    static_cast<void>(preparation_job->request_cancel());
                }
            }
        }
        if (snapshot.viewport.error.has_value()) {
            ImGui::SameLine();
            ImGui::TextColored(color(snapshot.colors.semantic_error), "Render unavailable");
        }
        ImGui::EndChild();
    }

    void draw_command_palette(const UiSnapshot& snapshot) {
        if (!snapshot.command_palette_open) return;
        bool open = true;
        if (!ImGui::Begin("Command Palette", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            return;
        }
        std::array<char, 1025> query{};
        const std::size_t copy_bytes = std::min(snapshot.command_palette_query.size(), query.size() - 1U);
        std::copy_n(snapshot.command_palette_query.data(), copy_bytes, query.data());
        if (ImGui::InputText("Search", query.data(), query.size())) {
            static_cast<void>(ui.set_command_palette_query(std::string(query.data())));
        }
        for (const auto& command : snapshot.commands) {
            if (!snapshot.command_palette_query.empty() &&
                command.label.find(snapshot.command_palette_query) == std::string::npos &&
                command.id.find(snapshot.command_palette_query) == std::string::npos) {
                continue;
            }
            if (!command.supported) {
                ImGui::TextDisabled("%s — unavailable: %s", command.label.c_str(),
                                   command.unavailable_reason.c_str());
                continue;
            }
            if (ImGui::Selectable((command.label +
                                  (command.shortcut.empty() ? "" : "    " + command.shortcut)).c_str())) {
                if (command.id == "project.save") static_cast<void>(save_project(ui.snapshot()));
                else if (command.id == "file.import-mesh") import_mesh();
                else if (command.id == "edit.duplicate") duplicate_selected_object(ui.snapshot());
                else if (command.id == "edit.undo") static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::undo));
                else if (command.id == "edit.redo") static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::redo));
                else if (command.id == "edit.repeat-last") static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::repeat_last_tool));
                else if (command.id == "mode.object") static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::object_mode));
                else if (command.id == "mode.vertex") static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::vertex_mode));
                else if (command.id == "mode.edge") static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::edge_mode));
                else if (command.id == "mode.face") static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::face_mode));
                else if (command.id == "operator.person") static_cast<void>(ui.set_operator_mode(carto::ui::OperatorMode::person_first));
                else if (command.id == "operator.ai") static_cast<void>(ui.set_operator_mode(carto::ui::OperatorMode::ai_first));
                else if (command.id == "workspace.reset") {
                    static_cast<void>(ui.reset_preferences());
                    reset_workbench();
                }
                else if (command.id == "market.refresh") {
                    const auto refreshed = ui.refresh_market_library();
                    if (!refreshed) {
                        action_feedback = refreshed.error().message;
                        action_feedback_error = true;
                    } else {
                        action_feedback = "Local market library refreshed.";
                        action_feedback_error = false;
                    }
                }
                else if (command.id == "create.box") dispatch(carto::application::CreateBoxAction{"Box", {2.0, 2.0, 2.0}});
                else if (command.id == "create.plane") dispatch(carto::application::CreatePlaneAction{"Plane", 2.0, 2.0});
                static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::close_overlay));
                break;
            }
        }
        if (!open) static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::close_overlay));
        ImGui::End();
    }

    void draw_outliner(const ApplicationSnapshot& snapshot) {
        heading_text("Scene");
        ImGui::TextDisabled("Objects currently on the work surface");
        ImGui::Dummy(ImVec2(0, 4));
        for (const auto& object : snapshot.objects) {
            const bool selected = std::find(
                snapshot.selection.objects.begin(), snapshot.selection.objects.end(), object.object.id) !=
                snapshot.selection.objects.end();
            const std::string object_id = std::to_string(object.object.id.value);
            ImGui::PushID(object_id.c_str());
            if (ImGui::Selectable(object.object.name.c_str(), selected)) {
                dispatch(SelectObjectAction{object.object.id});
            }
            if (ImGui::BeginPopupContextItem("outliner-object-context")) {
                ImGui::TextDisabled("Object %llu",
                    static_cast<unsigned long long>(object.object.id.value));
                if (ImGui::MenuItem("Ask Cartographer about this object")) {
                    if (dispatch(SelectObjectAction{object.object.id})) {
                        static_cast<void>(open_agent_workspace(true));
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("#%llu", static_cast<unsigned long long>(object.object.id.value));
            ImGui::PopID();
        }
        if (snapshot.objects.empty()) ImGui::TextDisabled("No project objects");
    }

    void draw_viewport(const UiSnapshot& snapshot, ImVec2 size = ImVec2(0, 0)) {
        native_viewport_hits.reset();
        const bool showing_agent_preview = snapshot.agent_task.has_value() &&
            agent_preview_mode != 0;
        const bool showing_agent_after = showing_agent_preview && agent_preview_mode == 2;
        const carto::terrain::TerrainTileProduct* terrain_preview = nullptr;
        const carto::spatial_field::VectorFieldPreview* vector_preview = nullptr;
        const carto::application::RoadSurfacePreview* road_preview = nullptr;
        const carto::geometry::SweptMeshProduct* sweep_preview = nullptr;
        bool terrain_preview_stale = false;
        bool vector_preview_stale = false;
        bool road_preview_stale = false;
        bool sweep_preview_stale = false;
        if (!showing_agent_preview &&
            snapshot.workspace_kind == carto::ui::Workspace::condition &&
            condition_terrain_preview.has_value()) {
            const auto source = std::find_if(snapshot.spatial_fields.begin(),
                snapshot.spatial_fields.end(), [this](const auto& field) {
                    return field.identity == condition_terrain_preview->source_identity();
                });
            if (source != snapshot.spatial_fields.end() &&
                source->source_revision == condition_terrain_preview->source_revision() &&
                source->digest == condition_terrain_preview->source_digest() &&
                condition_terrain_preview->validate()) {
                terrain_preview = &*condition_terrain_preview;
            } else {
                terrain_preview_stale = true;
            }
        }
        if (!showing_agent_preview &&
            snapshot.workspace_kind == carto::ui::Workspace::condition &&
            condition_vector_preview.has_value()) {
            const auto& product = *condition_vector_preview;
            const auto u_source = std::find_if(snapshot.spatial_fields.begin(),
                snapshot.spatial_fields.end(), [&product](const auto& field) {
                    return field.identity == product.u_component.identity;
                });
            const auto v_source = std::find_if(snapshot.spatial_fields.begin(),
                snapshot.spatial_fields.end(), [&product](const auto& field) {
                    return field.identity == product.v_component.identity;
                });
            if (u_source != snapshot.spatial_fields.end() &&
                v_source != snapshot.spatial_fields.end() &&
                u_source->semantic == carto::spatial_field::FieldSemantic::wind_u &&
                v_source->semantic == carto::spatial_field::FieldSemantic::wind_v &&
                u_source->source_revision == product.u_component.source_revision &&
                v_source->source_revision == product.v_component.source_revision &&
                u_source->digest == product.u_component.digest &&
                v_source->digest == product.v_component.digest && product.validate()) {
                vector_preview = &product;
            } else {
                vector_preview_stale = true;
            }
        }
        if (!showing_agent_preview &&
            snapshot.workspace_kind == carto::ui::Workspace::condition &&
            condition_road_surface_preview.has_value()) {
            const auto& product = *condition_road_surface_preview;
            const auto road = std::find_if(snapshot.road_sources.begin(),
                snapshot.road_sources.end(), [&product](const auto& source) {
                    return source.identity == product.road_identity;
                });
            const auto centerline = std::find_if(snapshot.curve_paths.begin(),
                snapshot.curve_paths.end(), [&product](const auto& source) {
                    return source.identity == product.mesh.centerline_identity;
                });
            if (road != snapshot.road_sources.end() &&
                centerline != snapshot.curve_paths.end() &&
                product.matches(snapshot.project_generation, *road, *centerline)) {
                road_preview = &product;
            } else {
                road_preview_stale = true;
            }
        }
        if (!showing_agent_preview &&
            snapshot.workspace_kind == carto::ui::Workspace::condition &&
            condition_sweep_preview.has_value()) {
            const auto& product = *condition_sweep_preview;
            const auto source = std::find_if(snapshot.sweep_sources.begin(),
                snapshot.sweep_sources.end(), [&product](const auto& item) {
                    return item.source.source_identity == product.source_identity;
                });
            const auto centerline = std::find_if(snapshot.curve_paths.begin(),
                snapshot.curve_paths.end(), [&product](const auto& item) {
                    return item.identity == product.centerline_identity;
                });
            if (condition_sweep_preview_generation == snapshot.project_generation &&
                source != snapshot.sweep_sources.end() &&
                centerline != snapshot.curve_paths.end() &&
                source->source.source_revision == product.source_revision &&
                source->source.centerline_identity == product.centerline_identity &&
                source->source.centerline_revision == product.centerline_revision &&
                centerline->source_revision == product.centerline_revision &&
                product.mesh.validate()) {
                sweep_preview = &product;
            } else {
                sweep_preview_stale = true;
            }
        }
        const auto& rendered_scene = !showing_agent_preview
            ? snapshot.viewport.scene
            : (showing_agent_after ? snapshot.agent_task->after_scene
                                   : snapshot.agent_task->before_scene);
        const auto& rendered_error = !showing_agent_preview
            ? snapshot.viewport.error
            : (showing_agent_after ? snapshot.agent_task->after_viewport_error
                                   : snapshot.agent_task->before_viewport_error);
        ImGui::BeginChild("WorkSurfaceViewport", size, true, ImGuiWindowFlags_NoScrollbar);
        heading_text(showing_agent_preview
            ? (showing_agent_after
                ? (agent_ghost_edit_mode
                    ? "Speculative after · human ghost correction"
                    : "Speculative after · review only")
                                   : "Task base · review only")
            : "Work surface · compiled snapshot");
        ImGui::SameLine();
        const bool request_camera_frame = ImGui::SmallButton("Frame all");
        ImGui::SameLine();
        ImGui::TextDisabled("View %.3f m", work_surface_camera.view_height());
        ImGui::Dummy(ImVec2(0, 4));
        if (rendered_error.has_value()) {
            ImGui::TextColored(
                color(snapshot.colors.semantic_error),
                "Render unavailable: %s",
                rendered_error->message.c_str());
            ImGui::EndChild();
            return;
        }
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 extent = ImGui::GetContentRegionAvail();
        native_viewport_hits.project_revision = snapshot.project_revision;
        native_viewport_hits.screen_origin = origin;
        native_viewport_hits.extent = extent;
        native_viewport_hits.valid = extent.x > 0.0F && extent.y > 0.0F;
        const bool ghost_edit_available = showing_agent_after && agent_ghost_edit_mode &&
            snapshot.agent_task.has_value() &&
            snapshot.project_revision == snapshot.agent_task->base_revision &&
            snapshot.agent_task->status != carto::agent::TaskStatus::accepted &&
            snapshot.agent_task->status != carto::agent::TaskStatus::rejected;
        native_viewport_hits.interactive =
            (!showing_agent_preview || ghost_edit_available) && ImGui::IsWindowHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const auto rgba = [](carto::ui::UiColor token, float alpha) {
            token.alpha *= alpha;
            return ImGui::ColorConvertFloat4ToU32(color(token));
        };
        draw->AddRectFilled(origin, origin + extent, rgba(snapshot.colors.canvas_0, 1.0F));
        const float grid_step = snapshot.density == carto::ui::Density::compact ? 28.0F : 36.0F;
        const ImU32 grid_color = rgba(snapshot.colors.border_soft, 0.22F);
        for (float x = origin.x + grid_step; x < origin.x + extent.x; x += grid_step) {
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + extent.y), grid_color, 1.0F);
        }
        for (float y = origin.y + grid_step; y < origin.y + extent.y; y += grid_step) {
            draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + extent.x, y), grid_color, 1.0F);
        }
        draw->AddLine(ImVec2(origin.x + extent.x * 0.5F, origin.y),
                      ImVec2(origin.x + extent.x * 0.5F, origin.y + extent.y),
                      rgba(snapshot.colors.accent_primary, 0.12F), 1.0F);
        draw->AddLine(ImVec2(origin.x, origin.y + extent.y * 0.5F),
                      ImVec2(origin.x + extent.x, origin.y + extent.y * 0.5F),
                      rgba(snapshot.colors.accent_primary, 0.12F), 1.0F);
        auto instances = rendered_scene.instances();
        bool invalid_render_snapshot = false;
        Vec3d minimum{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::infinity()};
        Vec3d maximum{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                      -std::numeric_limits<double>::infinity()};
        for (const auto& instance : instances) {
            if (instance.mesh == nullptr || !instance.mesh->valid()) {
                invalid_render_snapshot = true;
                continue;
            }
            for (const auto point : instance.mesh->positions) {
                const auto world = world_point(instance.world_transform, point);
                minimum.x = std::min(minimum.x, world.x);
                minimum.y = std::min(minimum.y, world.y);
                minimum.z = std::min(minimum.z, world.z);
                maximum.x = std::max(maximum.x, world.x);
                maximum.y = std::max(maximum.y, world.y);
                maximum.z = std::max(maximum.z, world.z);
            }
        }
        if (terrain_preview != nullptr) {
            for (const auto& vertex : terrain_preview->vertices()) {
                minimum.x = std::min(minimum.x, vertex.position.x);
                minimum.y = std::min(minimum.y, vertex.position.y);
                minimum.z = std::min(minimum.z, vertex.position.z);
                maximum.x = std::max(maximum.x, vertex.position.x);
                maximum.y = std::max(maximum.y, vertex.position.y);
                maximum.z = std::max(maximum.z, vertex.position.z);
            }
        }
        if (vector_preview != nullptr) {
            const auto& source = vector_preview->u_component;
            const double max_u = source.origin_uv.x +
                source.cell_size * static_cast<double>(source.width - 1U);
            const double max_v = source.origin_uv.y +
                source.cell_size * static_cast<double>(source.height - 1U);
            minimum.x = std::min(minimum.x, source.origin_uv.x);
            minimum.z = std::min(minimum.z, source.origin_uv.y);
            maximum.x = std::max(maximum.x, max_u);
            maximum.z = std::max(maximum.z, max_v);
        }
        if (road_preview != nullptr) {
            for (const auto& station : road_preview->mesh.stations) {
                for (const auto point : {station.shoulder_left, station.shoulder_right}) {
                    minimum.x = std::min(minimum.x, point.x);
                    minimum.y = std::min(minimum.y, point.y);
                    minimum.z = std::min(minimum.z, point.z);
                    maximum.x = std::max(maximum.x, point.x);
                    maximum.y = std::max(maximum.y, point.y);
                    maximum.z = std::max(maximum.z, point.z);
                }
            }
        }
        if (sweep_preview != nullptr &&
            (!work_surface_camera_framed || request_camera_frame)) {
            for (const auto& vertex : sweep_preview->mesh.vertices_sorted()) {
                minimum.x = std::min(minimum.x, vertex.position.x);
                minimum.y = std::min(minimum.y, vertex.position.y);
                minimum.z = std::min(minimum.z, vertex.position.z);
                maximum.x = std::max(maximum.x, vertex.position.x);
                maximum.y = std::max(maximum.y, vertex.position.y);
                maximum.z = std::max(maximum.z, vertex.position.z);
            }
        }
        const double measured_width = maximum.x - minimum.x;
        const double measured_height = maximum.z - minimum.z;
        const double width = std::isfinite(measured_width)
            ? std::max(1e-6, measured_width) : 1.0;
        const double height = std::isfinite(measured_height)
            ? std::max(1e-6, measured_height) : 1.0;
        const carto::core::Vec2d viewport_origin{origin.x, origin.y};
        const carto::core::Vec2d viewport_extent{
            std::max(1.0, static_cast<double>(extent.x)),
            std::max(1.0, static_cast<double>(extent.y))};
        bool camera_frame_failed = false;
        const bool should_frame_camera = !work_surface_camera_framed || request_camera_frame;
        if (should_frame_camera && (!instances.empty() || terrain_preview != nullptr ||
                vector_preview != nullptr || road_preview != nullptr || sweep_preview != nullptr) &&
            !invalid_render_snapshot &&
            std::isfinite(minimum.x) && std::isfinite(maximum.x) &&
            std::isfinite(minimum.z) && std::isfinite(maximum.z)) {
            const carto::core::Vec2d centre{
                minimum.x + (maximum.x - minimum.x) * 0.5,
                minimum.z + (maximum.z - minimum.z) * 0.5};
            if (work_surface_camera.frame(centre, {width, height}, viewport_extent)) {
                work_surface_camera_framed = true;
            } else {
                camera_frame_failed = true;
            }
        }

        const bool canvas_hovered = ImGui::IsMouseHoveringRect(
            origin, origin + extent, true);
        const bool camera_input_enabled = canvas_hovered && !showing_agent_preview &&
            !native_transform_drag.has_value() && !native_sculpt_stroke.has_value();
        const ImGuiIO& camera_io = ImGui::GetIO();
        const ImVec2 mouse_position = ImGui::GetMousePos();
        if (camera_input_enabled && camera_io.MouseWheel != 0.0F) {
            if (work_surface_camera.zoom_at(
                    camera_io.MouseWheel,
                    {mouse_position.x, mouse_position.y},
                    viewport_origin,
                    viewport_extent)) {
                work_surface_camera_framed = true;
            }
        }
        if (camera_input_enabled && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            work_surface_camera_panning = true;
            work_surface_camera_last_pointer = mouse_position;
        }
        if (work_surface_camera_panning) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                const carto::core::Vec2d delta{
                    static_cast<double>(mouse_position.x - work_surface_camera_last_pointer.x),
                    static_cast<double>(mouse_position.y - work_surface_camera_last_pointer.y)};
                if (work_surface_camera.pan_pixels(delta, viewport_extent.y)) {
                    work_surface_camera_framed = true;
                    work_surface_camera_last_pointer = mouse_position;
                }
            } else {
                work_surface_camera_panning = false;
            }
        }
        const auto project = [&](Vec3d point) {
            const auto world = world_point(carto::core::Transform::identity(), point);
            const auto projected = work_surface_camera.world_to_screen(
                {world.x, world.z}, viewport_origin, viewport_extent);
            if (!projected) {
                invalid_render_snapshot = true;
                native_viewport_hits.interactive = false;
                return origin;
            }
            return ImVec2(
                static_cast<float>(projected.value().x),
                static_cast<float>(projected.value().y));
        };

        if (terrain_preview != nullptr) {
            double minimum_elevation = std::numeric_limits<double>::infinity();
            double maximum_elevation = -std::numeric_limits<double>::infinity();
            for (const auto& vertex : terrain_preview->vertices()) {
                minimum_elevation = std::min(minimum_elevation, vertex.position.y);
                maximum_elevation = std::max(maximum_elevation, vertex.position.y);
            }
            const double elevation_span = maximum_elevation - minimum_elevation;
            const Vec3d light_direction = Vec3d{0.35, 0.84, 0.41}.normalized();
            const auto blend = [](ImVec4 first, ImVec4 second, float amount) {
                return ImVec4(
                    first.x + (second.x - first.x) * amount,
                    first.y + (second.y - first.y) * amount,
                    first.z + (second.z - first.z) * amount,
                    0.92F);
            };
            const auto& vertices = terrain_preview->vertices();
            const auto& indices = terrain_preview->indices();
            for (std::size_t index = 0U; index + 2U < indices.size(); index += 3U) {
                const auto& first = vertices[indices[index]];
                const auto& second = vertices[indices[index + 1U]];
                const auto& third = vertices[indices[index + 2U]];
                const double elevation = (first.position.y + second.position.y +
                    third.position.y) / 3.0;
                const float normalized_elevation = static_cast<float>(elevation_span > 0.0
                    ? std::clamp((elevation - minimum_elevation) / elevation_span, 0.0, 1.0)
                    : 0.0);
                const ImVec4 low{0.035F, 0.16F, 0.22F, 0.92F};
                const ImVec4 middle{0.13F, 0.32F, 0.25F, 0.92F};
                const ImVec4 high{0.48F, 0.40F, 0.23F, 0.92F};
                const ImVec4 summit{0.76F, 0.81F, 0.82F, 0.92F};
                ImVec4 terrain_color = normalized_elevation < 0.4F
                    ? blend(low, middle, normalized_elevation / 0.4F)
                    : normalized_elevation < 0.8F
                        ? blend(middle, high, (normalized_elevation - 0.4F) / 0.4F)
                        : blend(high, summit, (normalized_elevation - 0.8F) / 0.2F);
                const Vec3d average_normal = (first.normal + second.normal +
                    third.normal).normalized();
                const float illumination = static_cast<float>(0.52 + 0.48 * std::max(
                    0.0, carto::core::dot(average_normal, light_direction)));
                terrain_color.x *= illumination;
                terrain_color.y *= illumination;
                terrain_color.z *= illumination;
                const auto first_screen = project(first.position);
                const auto second_screen = project(second.position);
                const auto third_screen = project(third.position);
                draw->AddTriangleFilled(first_screen, second_screen, third_screen,
                    ImGui::ColorConvertFloat4ToU32(terrain_color));
                draw->AddTriangle(first_screen, second_screen, third_screen,
                    rgba(snapshot.colors.border_soft, 0.22F), 0.75F);
            }
            const std::string label = terrain_preview->source_identity() + " · r" +
                std::to_string(terrain_preview->source_revision().value()) +
                " · derived mesh preview · not project geometry";
            draw->AddText(origin + ImVec2(8.0F, 8.0F),
                rgba(snapshot.colors.accent_secondary, 0.96F), label.c_str());
        } else if (terrain_preview_stale) {
            draw->AddText(origin + ImVec2(8.0F, 8.0F),
                rgba(snapshot.colors.semantic_warning, 0.96F),
                "Terrain preview stale · re-analyze the current source");
        }

        if (vector_preview != nullptr) {
            double maximum_magnitude = 0.0;
            for (const auto& sample : vector_preview->samples) {
                maximum_magnitude = std::max(maximum_magnitude,
                    std::hypot(sample.vector_uv.x, sample.vector_uv.y));
            }
            const ImU32 arrow_color = rgba(snapshot.colors.accent_primary, 0.96F);
            for (const auto& sample : vector_preview->samples) {
                const double magnitude = std::hypot(
                    sample.vector_uv.x, sample.vector_uv.y);
                const ImVec2 start = project({sample.position_uv.x, 0.0,
                                              sample.position_uv.y});
                if (magnitude <= 1.0e-12 || maximum_magnitude <= 1.0e-12) {
                    draw->AddCircleFilled(start, 1.7F, arrow_color, 8);
                    continue;
                }
                const double length = vector_preview->u_component.cell_size *
                    (0.18 + 0.38 * magnitude / maximum_magnitude);
                const Vec3d end_world{
                    sample.position_uv.x + sample.vector_uv.x / magnitude * length,
                    0.0,
                    sample.position_uv.y + sample.vector_uv.y / magnitude * length};
                const ImVec2 end = project(end_world);
                draw->AddLine(start, end, arrow_color, 1.8F);
                const float dx = end.x - start.x;
                const float dy = end.y - start.y;
                const float screen_length = std::hypot(dx, dy);
                if (screen_length > 1.0F) {
                    const float inverse_length = 1.0F / screen_length;
                    const ImVec2 direction{dx * inverse_length, dy * inverse_length};
                    const ImVec2 perpendicular{-direction.y, direction.x};
                    const ImVec2 base{
                        end.x - direction.x * 4.5F,
                        end.y - direction.y * 4.5F};
                    draw->AddTriangleFilled(
                        end,
                        ImVec2(base.x + perpendicular.x * 2.4F,
                               base.y + perpendicular.y * 2.4F),
                        ImVec2(base.x - perpendicular.x * 2.4F,
                               base.y - perpendicular.y * 2.4F),
                        arrow_color);
                }
            }
            const std::string label = vector_preview->u_component.identity + " + " +
                vector_preview->v_component.identity + " · derived U/V vectors only";
            draw->AddText(origin + ImVec2(8.0F, terrain_preview != nullptr ? 26.0F : 8.0F),
                arrow_color, label.c_str());
        } else if (vector_preview_stale) {
            draw->AddText(origin + ImVec2(8.0F, terrain_preview != nullptr ? 26.0F : 8.0F),
                rgba(snapshot.colors.semantic_warning, 0.96F),
                "Wind vector preview stale · rebuild from current sources");
        }

        if (road_preview != nullptr) {
            ImVec4 road_color{};
            switch (road_preview->surface_class) {
            case carto::project::RoadSurfaceClass::asphalt:
                road_color = ImVec4(0.20F, 0.25F, 0.31F, 0.94F);
                break;
            case carto::project::RoadSurfaceClass::concrete:
                road_color = ImVec4(0.55F, 0.60F, 0.63F, 0.94F);
                break;
            case carto::project::RoadSurfaceClass::gravel:
                road_color = ImVec4(0.56F, 0.43F, 0.25F, 0.94F);
                break;
            case carto::project::RoadSurfaceClass::dirt:
                road_color = ImVec4(0.43F, 0.27F, 0.18F, 0.94F);
                break;
            case carto::project::RoadSurfaceClass::trail:
                road_color = ImVec4(0.20F, 0.39F, 0.29F, 0.94F);
                break;
            }
            const ImU32 road_fill = ImGui::ColorConvertFloat4ToU32(road_color);
            const ImU32 shoulder_fill = ImGui::ColorConvertFloat4ToU32(
                ImVec4(0.66F, 0.48F, 0.23F, 0.58F));
            const ImU32 edge_color = rgba(snapshot.colors.text_primary, 0.82F);
            const auto& stations = road_preview->mesh.stations;
            for (std::size_t index = 0U; index + 1U < stations.size(); ++index) {
                const auto& first = stations[index];
                const auto& second = stations[index + 1U];
                const ImVec2 shoulder_left_a = project(first.shoulder_left);
                const ImVec2 road_left_a = project(first.road_left);
                const ImVec2 road_right_a = project(first.road_right);
                const ImVec2 shoulder_right_a = project(first.shoulder_right);
                const ImVec2 shoulder_left_b = project(second.shoulder_left);
                const ImVec2 road_left_b = project(second.road_left);
                const ImVec2 road_right_b = project(second.road_right);
                const ImVec2 shoulder_right_b = project(second.shoulder_right);
                draw->AddQuadFilled(shoulder_left_a, road_left_a,
                    road_left_b, shoulder_left_b, shoulder_fill);
                draw->AddQuadFilled(road_right_a, shoulder_right_a,
                    shoulder_right_b, road_right_b, shoulder_fill);
                draw->AddQuadFilled(road_left_a, road_right_a,
                    road_right_b, road_left_b, road_fill);
                draw->AddLine(road_left_a, road_left_b, edge_color, 1.1F);
                draw->AddLine(road_right_a, road_right_b, edge_color, 1.1F);
            }
            const std::string label = road_preview->road_identity + " · r" +
                std::to_string(road_preview->road_revision.value()) +
                " · derived road ribbon · sampled checks · preview only";
            const float label_y = 8.0F + (terrain_preview != nullptr ? 20.0F : 0.0F) +
                (vector_preview != nullptr ? 20.0F : 0.0F);
            draw->AddText(origin + ImVec2(8.0F, label_y), road_fill, label.c_str());
        } else if (road_preview_stale) {
            const float label_y = 8.0F + (terrain_preview != nullptr ? 20.0F : 0.0F) +
                (vector_preview != nullptr ? 20.0F : 0.0F);
            draw->AddText(origin + ImVec2(8.0F, label_y),
                rgba(snapshot.colors.semantic_warning, 0.96F),
                "Road preview stale · rebuild from current road and centerline sources");
        }

        if (sweep_preview != nullptr) {
            constexpr std::size_t kMaxSweepPreviewEdges = 16'000U;
            const auto edges = sweep_preview->mesh.edges_sorted(kMaxSweepPreviewEdges);
            const ImU32 sweep_color = rgba(snapshot.colors.accent_secondary, 0.96F);
            for (const auto& edge : edges) {
                const auto* first = sweep_preview->mesh.find_vertex(edge.first);
                const auto* second = sweep_preview->mesh.find_vertex(edge.second);
                if (first == nullptr || second == nullptr) continue;
                draw->AddLine(project(first->position), project(second->position),
                    sweep_color, 1.25F);
            }
            std::string label = sweep_preview->source_identity + " · r" +
                std::to_string(sweep_preview->source_revision.value()) +
                " · derived sweep wireframe · preview only";
            if (edges.size() < sweep_preview->mesh.edge_count()) {
                label += " · display capped at 16k edges";
            }
            const float label_y = 8.0F + (terrain_preview != nullptr ? 20.0F : 0.0F) +
                (vector_preview != nullptr ? 20.0F : 0.0F) +
                (road_preview != nullptr ? 20.0F : 0.0F);
            draw->AddText(origin + ImVec2(8.0F, label_y), sweep_color, label.c_str());
        } else if (sweep_preview_stale) {
            const float label_y = 8.0F + (terrain_preview != nullptr ? 20.0F : 0.0F) +
                (vector_preview != nullptr ? 20.0F : 0.0F) +
                (road_preview != nullptr ? 20.0F : 0.0F);
            draw->AddText(origin + ImVec2(8.0F, label_y),
                rgba(snapshot.colors.semantic_warning, 0.96F),
                "Sweep preview stale · rebuild from current source and centerline");
        }

        if (native_transform_drag.has_value()) {
            if (showing_agent_preview || snapshot.project_revision !=
                    native_transform_drag->session.source_revision()) {
                cancel_native_transform_drag(
                    "Transform preview cancelled because its source snapshot is no longer current.");
            } else {
                for (auto& instance : instances) {
                    if (instance.object == native_transform_drag->session.object()) {
                        instance.world_transform.translation.x =
                            native_transform_drag->preview_transform.translation.x;
                        instance.world_transform.translation.z =
                            native_transform_drag->preview_transform.translation.z;
                    }
                }
            }
        }

        auto& faces = native_viewport_hits.faces;
        auto& vertices = native_viewport_hits.vertices;
        auto& edges = native_viewport_hits.edges;
        auto& objects = native_viewport_hits.objects;
        std::optional<carto::scene::ObjectId> sculpt_target;
        if (snapshot.workspace_kind == carto::ui::Workspace::sculpt) {
            if (snapshot.selection.component_object.has_value()) {
                sculpt_target = snapshot.selection.component_object;
            } else if (snapshot.selection.objects.size() == 1U) {
                sculpt_target = snapshot.selection.objects.front();
            }
        }
        for (const auto& instance : instances) {
            if (instance.mesh == nullptr || !instance.mesh->valid()) {
                invalid_render_snapshot = true;
                continue;
            }
            const bool highlighted = std::find(
                snapshot.selection.objects.begin(), snapshot.selection.objects.end(), instance.object) !=
                snapshot.selection.objects.end();
            ImVec2 object_min{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
            ImVec2 object_max{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
            std::vector<ImVec2> projected;
            projected.reserve(instance.mesh->positions.size());
            for (const auto point : instance.mesh->positions) {
                const ImVec2 screen = project(world_point(instance.world_transform, point));
                projected.push_back(screen);
                object_min.x = std::min(object_min.x, screen.x);
                object_min.y = std::min(object_min.y, screen.y);
                object_max.x = std::max(object_max.x, screen.x);
                object_max.y = std::max(object_max.y, screen.y);
                vertices.push_back({instance.object, instance.mesh->vertex_ids[projected.size() - 1U], screen});
            }
            objects.emplace_back(instance.object, ImRect(object_min - ImVec2(8, 8), object_max + ImVec2(8, 8)));
            for (std::size_t index = 0; index + 2U < instance.mesh->indices.size(); index += 3U) {
                const auto a = projected[instance.mesh->indices[index]];
                const auto b = projected[instance.mesh->indices[index + 1U]];
                const auto c = projected[instance.mesh->indices[index + 2U]];
                const bool face_selected = snapshot.selection.mode == carto::editor::SelectionMode::face &&
                    std::find(snapshot.selection.faces.begin(), snapshot.selection.faces.end(),
                              instance.mesh->triangle_faces[index / 3U]) != snapshot.selection.faces.end();
                const ImU32 fill = face_selected ? rgba(snapshot.colors.semantic_warning, 0.52F)
                                                  : (highlighted ? rgba(snapshot.colors.accent_primary, 0.30F)
                                                                 : rgba(snapshot.colors.panel_hover, 0.18F));
                draw->AddTriangleFilled(a, b, c, fill);
                draw->AddTriangle(a, b, c, rgba(snapshot.colors.border_soft, 0.82F), 1.0F);
                faces.push_back({instance.object, instance.mesh->triangle_faces[index / 3U], {a, b, c}});
                if (sculpt_target.has_value() && instance.object == *sculpt_target) {
                    if (native_viewport_hits.sculpt_triangles.size() <
                        kMaxNativeSculptTriangles) {
                        carto::ui::SculptViewportTriangle triangle;
                        triangle.object = instance.object;
                        triangle.face = instance.mesh->triangle_faces[index / 3U];
                        const std::array<std::uint32_t, 3> source_indices{
                            instance.mesh->indices[index],
                            instance.mesh->indices[index + 1U],
                            instance.mesh->indices[index + 2U]};
                        for (std::size_t corner = 0U; corner < source_indices.size(); ++corner) {
                            const std::uint32_t source_index = source_indices[corner];
                            triangle.vertices[corner] = instance.mesh->vertex_ids[source_index];
                            triangle.local_positions[corner] = instance.mesh->positions[source_index];
                            const auto world = world_point(
                                instance.world_transform, triangle.local_positions[corner]);
                            const ImVec2 screen = projected[source_index];
                            triangle.screen_positions[corner] = {
                                static_cast<double>(screen.x), static_cast<double>(screen.y)};
                            triangle.camera_depth[corner] = world.y;
                        }
                        native_viewport_hits.sculpt_triangles.push_back(std::move(triangle));
                    } else {
                        native_viewport_hits.sculpt_triangles_truncated = true;
                    }
                }
                const std::array<ImVec2, 3> points{a, b, c};
                const auto& triangle_edges = instance.mesh->triangle_edges[index / 3U];
                for (std::size_t edge_index = 0U; edge_index < triangle_edges.size(); ++edge_index) {
                    if (!triangle_edges[edge_index].has_value()) continue;
                    const ImVec2 first = points[edge_index];
                    const ImVec2 second = points[(edge_index + 1U) % points.size()];
                    const bool selected = snapshot.selection.mode == carto::editor::SelectionMode::edge &&
                        std::find(snapshot.selection.edges.begin(), snapshot.selection.edges.end(),
                                  *triangle_edges[edge_index]) != snapshot.selection.edges.end();
                    draw->AddLine(first, second,
                                  selected ? rgba(snapshot.colors.semantic_warning, 1.0F)
                                           : rgba(snapshot.colors.text_secondary, 0.88F),
                                  selected ? 3.0F : 1.0F);
                    edges.push_back({instance.object, *triangle_edges[edge_index], first, second});
                }
            }
        }
        if (!showing_agent_preview && snapshot.workspace_kind == carto::ui::Workspace::condition) {
            const auto owner_instance = [&](std::uint64_t asset_id) {
                const auto asset = std::find_if(snapshot.condition.assets.begin(),
                    snapshot.condition.assets.end(), [asset_id](const auto& value) {
                        return value.id == asset_id && value.authored_object_id != 0U;
                    });
                if (asset == snapshot.condition.assets.end()) return instances.end();
                return std::find_if(instances.begin(), instances.end(), [&](const auto& value) {
                    return value.object.value == asset->authored_object_id;
                });
            };
            const auto draw_ring = [&](const auto& owner, const carto::core::Transform& local,
                                       double radius, double start_angle, ImU32 ring_color)
                -> std::optional<ImVec2> {
                if (!std::isfinite(radius) || radius <= 0.0) return std::nullopt;
                constexpr std::size_t kRingSegments = 48U;
                std::array<ImVec2, kRingSegments + 1U> points{};
                constexpr double kTau = 6.2831853071795864769;
                for (std::size_t index = 0U; index <= kRingSegments; ++index) {
                    const double angle = start_angle + kTau * static_cast<double>(index) /
                        static_cast<double>(kRingSegments);
                    const Vec3d offset{std::cos(angle) * radius, 0.0,
                                       std::sin(angle) * radius};
                    const Vec3d asset_point = world_point(local, offset);
                    points[index] = project(world_point(owner.world_transform, asset_point));
                }
                draw->AddPolyline(points.data(), static_cast<int>(points.size()),
                    ring_color, 1.5F, ImDrawFlags_Closed);
                draw->AddCircleFilled(points.front(), 3.5F, ring_color);
                return points.front();
            };
            const ImU32 source_color = rgba(snapshot.colors.accent_primary, 0.76F);
            const ImU32 source_inner_color = rgba(snapshot.colors.accent_primary, 0.42F);
            const ImU32 zone_color = rgba(snapshot.colors.accent_secondary, 0.86F);
            for (const auto& source : snapshot.condition.audio_sources) {
                if (!source.enabled || !source.spatial) continue;
                const auto owner = owner_instance(source.asset_id);
                if (owner != instances.end()) {
                    double preview_minimum_distance = source.minimum_distance;
                    double preview_maximum_distance = source.maximum_distance;
                    if (native_condition_gizmo_drag.has_value() &&
                        native_condition_gizmo_drag->handle.record_id == source.id) {
                        if (native_condition_gizmo_drag->handle.kind ==
                                NativeConditionGizmoKind::audio_minimum_distance ||
                            native_condition_gizmo_drag->handle.kind ==
                                NativeConditionGizmoKind::audio_maximum_distance) {
                            preview_minimum_distance =
                                native_condition_gizmo_drag->preview_minimum;
                            preview_maximum_distance =
                                native_condition_gizmo_drag->preview_maximum;
                        }
                    }
                    const ImVec2 center = project(world_point(
                        owner->world_transform, source.local_transform.translation));
                    const auto minimum_handle = draw_ring(*owner, source.local_transform,
                        preview_minimum_distance, 1.5707963267948966, source_inner_color);
                    const auto maximum_handle = draw_ring(*owner, source.local_transform,
                        preview_maximum_distance, 0.0, source_color);
                    if (minimum_handle.has_value()) {
                        draw->AddText(*minimum_handle + ImVec2(5.0F, -13.0F),
                            source_inner_color, "MIN");
                        native_viewport_hits.condition_gizmos.push_back({
                            NativeConditionGizmoKind::audio_minimum_distance, source.id,
                            center, *minimum_handle, owner->world_transform,
                            source.local_transform, source.minimum_distance,
                            source.maximum_distance, 0.0, {},
                            snapshot.condition.authored_revision});
                    }
                    if (maximum_handle.has_value()) {
                        draw->AddText(*maximum_handle + ImVec2(5.0F, -13.0F),
                            source_color, "MAX");
                        native_viewport_hits.condition_gizmos.push_back({
                            NativeConditionGizmoKind::audio_maximum_distance, source.id,
                            center, *maximum_handle, owner->world_transform,
                            source.local_transform, source.minimum_distance,
                            source.maximum_distance, 0.0, {},
                            snapshot.condition.authored_revision});
                    }
                }
            }
            for (const auto& zone : snapshot.condition.acoustic_zones) {
                if (!zone.enabled) continue;
                const auto owner = owner_instance(zone.asset_id);
                if (owner == instances.end()) continue;
                auto half_extents = zone.half_extents;
                double radius = zone.radius;
                if (native_condition_gizmo_drag.has_value() &&
                    native_condition_gizmo_drag->handle.record_id == zone.id) {
                    if (native_condition_gizmo_drag->handle.kind ==
                            NativeConditionGizmoKind::acoustic_sphere_radius) {
                        radius = native_condition_gizmo_drag->preview_radius;
                    } else if (native_condition_gizmo_drag->handle.kind ==
                            NativeConditionGizmoKind::acoustic_box_corner) {
                        half_extents = native_condition_gizmo_drag->preview_half_extents;
                    }
                }
                const ImVec2 center = project(world_point(
                    owner->world_transform, zone.local_transform.translation));
                if (zone.shape == "sphere") {
                    const auto handle = draw_ring(*owner, zone.local_transform,
                        radius, 0.0, zone_color);
                    if (handle.has_value()) {
                        native_viewport_hits.condition_gizmos.push_back({
                            NativeConditionGizmoKind::acoustic_sphere_radius, zone.id,
                            center, *handle, owner->world_transform, zone.local_transform,
                            0.0, 0.0, zone.radius, zone.half_extents,
                            snapshot.condition.authored_revision});
                    }
                    const std::string priority_label = "P" + std::to_string(zone.priority);
                    draw->AddText(center + ImVec2(5.0F, 5.0F), zone_color,
                        priority_label.c_str());
                    continue;
                }
                const std::array<Vec3d, 5> local_corners{{
                    {-half_extents.x, 0.0, -half_extents.z},
                    { half_extents.x, 0.0, -half_extents.z},
                    { half_extents.x, 0.0,  half_extents.z},
                    {-half_extents.x, 0.0,  half_extents.z},
                    {-half_extents.x, 0.0, -half_extents.z}}};
                std::array<ImVec2, 5U> screen_corners{};
                for (std::size_t index = 0U; index < local_corners.size(); ++index) {
                    const Vec3d asset_point = world_point(zone.local_transform,
                        local_corners[index]);
                    screen_corners[index] = project(
                        world_point(owner->world_transform, asset_point));
                }
                draw->AddPolyline(screen_corners.data(),
                    static_cast<int>(screen_corners.size()), zone_color,
                    1.5F, ImDrawFlags_Closed);
                draw->AddCircleFilled(screen_corners[2], 4.0F, zone_color);
                const std::string priority_label = "P" + std::to_string(zone.priority);
                draw->AddText(center + ImVec2(5.0F, 5.0F), zone_color,
                    priority_label.c_str());
                native_viewport_hits.condition_gizmos.push_back({
                    NativeConditionGizmoKind::acoustic_box_corner, zone.id, center,
                    screen_corners[2], owner->world_transform, zone.local_transform,
                    0.0, 0.0, zone.radius, zone.half_extents,
                    snapshot.condition.authored_revision});
            }
        }
        const bool ghost_object_selected = ghost_edit_available &&
            static_cast<bool>(agent_ghost_selected_object) &&
            std::find(snapshot.agent_task->scope_objects.begin(),
                snapshot.agent_task->scope_objects.end(), agent_ghost_selected_object) !=
                snapshot.agent_task->scope_objects.end() &&
            std::find(snapshot.agent_task->protected_objects.begin(),
                snapshot.agent_task->protected_objects.end(), agent_ghost_selected_object) ==
                snapshot.agent_task->protected_objects.end();
        const bool source_object_selected = !showing_agent_preview &&
            snapshot.workspace_kind == carto::ui::Workspace::model &&
            snapshot.selection.mode == carto::editor::SelectionMode::object &&
            snapshot.selection.objects.size() == 1U;
        if (ghost_object_selected || source_object_selected) {
            const auto selected_id = ghost_object_selected
                ? agent_ghost_selected_object : snapshot.selection.objects.front();
            const auto selected = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
                [selected_id](const auto& object) { return object.object.id == selected_id; });
            const auto instance = std::find_if(instances.begin(), instances.end(),
                [selected_id](const auto& value) { return value.object == selected_id; });
            if (selected != snapshot.objects.end() && !selected->object.parent.has_value() &&
                !selected->object.locked && instance != instances.end()) {
                const ImVec2 center = project(instance->world_transform.translation);
                constexpr float kGizmoLength = 44.0F;
                const ImVec2 x_tip = center + ImVec2(kGizmoLength, 0.0F);
                const ImVec2 z_tip = center + ImVec2(0.0F, -kGizmoLength);
                const double pixels_per_world =
                    viewport_extent.y / work_surface_camera.view_height();
                const ImU32 x_color = rgba(snapshot.colors.accent_primary, 1.0F);
                const ImU32 z_color = rgba(snapshot.colors.accent_secondary, 1.0F);
                const auto gizmo_transform = ghost_object_selected
                    ? instance->world_transform : selected->object.local_transform;
                draw->AddCircleFilled(center, 4.0F, rgba(snapshot.colors.text_primary, 1.0F));
                draw->AddLine(center, x_tip, x_color, 3.0F);
                draw->AddLine(center, z_tip, z_color, 3.0F);
                draw->AddCircleFilled(x_tip, 4.0F, x_color);
                draw->AddCircleFilled(z_tip, 4.0F, z_color);
                draw->AddText(x_tip + ImVec2(6.0F, -7.0F), x_color, "X");
                draw->AddText(z_tip + ImVec2(-4.0F, -17.0F), z_color, "Z");
                native_viewport_hits.translate_gizmo = NativeTranslateGizmo{
                    selected_id, center, x_tip, z_tip,
                    pixels_per_world, pixels_per_world,
                    gizmo_transform, snapshot.project_revision};
            }
        }
        if (native_transform_drag.has_value() &&
            native_transform_drag->geometry_snap_candidate.has_value()) {
            const auto& candidate =
                *native_transform_drag->geometry_snap_candidate;
            if (candidate.source_revision == snapshot.project_revision &&
                candidate.object.has_value() && candidate.mesh_asset.has_value()) {
                const auto target = std::find_if(instances.begin(), instances.end(),
                    [&candidate](const auto& instance) {
                        return instance.object == *candidate.object &&
                            instance.mesh_asset == candidate.mesh_asset &&
                            instance.mesh != nullptr &&
                            instance.mesh->source_revision == candidate.mesh_revision;
                    });
                if (target != instances.end()) {
                    const ImVec2 marker = project(candidate.position);
                    if (ImRect(origin, origin + extent).Contains(marker)) {
                        const ImU32 marker_color =
                            rgba(snapshot.colors.accent_primary, 1.0F);
                        draw->AddCircleFilled(marker, 8.0F,
                            rgba(snapshot.colors.canvas_0, 0.96F), 16);
                        draw->AddCircle(marker, 7.0F, marker_color, 16, 2.0F);
                        draw->AddLine(marker + ImVec2(-11.0F, 0.0F),
                            marker + ImVec2(-4.0F, 0.0F), marker_color, 2.0F);
                        draw->AddLine(marker + ImVec2(4.0F, 0.0F),
                            marker + ImVec2(11.0F, 0.0F), marker_color, 2.0F);
                        draw->AddLine(marker + ImVec2(0.0F, -11.0F),
                            marker + ImVec2(0.0F, -4.0F), marker_color, 2.0F);
                        draw->AddLine(marker + ImVec2(0.0F, 4.0F),
                            marker + ImVec2(0.0F, 11.0F), marker_color, 2.0F);
                        const char* kind = candidate.kind ==
                                carto::editor::SnapKind::vertex
                            ? "VERTEX" : candidate.kind ==
                                carto::editor::SnapKind::edge_midpoint
                                ? "EDGE MIDPOINT" : "FACE CENTER";
                        const std::string label = std::string(kind) + " · object " +
                            std::to_string(candidate.object->value);
                        draw->AddText(marker + ImVec2(12.0F, -18.0F),
                            marker_color, label.c_str());
                    }
                }
            }
        }
        const auto& fidelity = snapshot.surface_fidelity;
        if ((fidelity.status == carto::ui::SurfaceFidelityStatus::ready ||
             fidelity.status == carto::ui::SurfaceFidelityStatus::rejected) &&
            fidelity.object.has_value() && fidelity.compiled != nullptr) {
            const auto source_instance = std::find_if(
                instances.begin(), instances.end(),
                [&fidelity](const auto& instance) {
                    return instance.object == *fidelity.object &&
                        instance.mesh != nullptr &&
                        instance.source_revision == fidelity.source_revision;
                });
            const auto& geometry = fidelity.compiled->geometry;
            const bool valid_preview = !geometry.positions.empty() &&
                geometry.normals.size() == geometry.positions.size() &&
                geometry.indices.size() % 3U == 0U;
            if (source_instance != instances.end() && valid_preview) {
                const ImU32 fill = fidelity.status == carto::ui::SurfaceFidelityStatus::ready
                    ? rgba(snapshot.colors.provisional, 0.24F)
                    : rgba(snapshot.colors.semantic_warning, 0.24F);
                const ImU32 outline = fidelity.status == carto::ui::SurfaceFidelityStatus::ready
                    ? rgba(snapshot.colors.provisional, 0.94F)
                    : rgba(snapshot.colors.semantic_warning, 0.94F);
                bool preview_indices_valid = true;
                for (const std::uint32_t index : geometry.indices) {
                    if (index >= geometry.positions.size()) {
                        preview_indices_valid = false;
                        break;
                    }
                }
                if (preview_indices_valid) {
                    for (std::size_t index = 0U; index + 2U < geometry.indices.size(); index += 3U) {
                        const ImVec2 a = project(world_point(
                            source_instance->world_transform, geometry.positions[geometry.indices[index]]));
                        const ImVec2 b = project(world_point(
                            source_instance->world_transform, geometry.positions[geometry.indices[index + 1U]]));
                        const ImVec2 c = project(world_point(
                            source_instance->world_transform, geometry.positions[geometry.indices[index + 2U]]));
                        draw->AddTriangleFilled(a, b, c, fill);
                        draw->AddTriangle(a, b, c, outline, 2.0F);
                    }
                    ImGui::TextColored(
                        fidelity.status == carto::ui::SurfaceFidelityStatus::ready
                            ? color(snapshot.colors.provisional)
                            : color(snapshot.colors.semantic_warning),
                        "%s overlay · %u triangles",
                        carto::ui::surface_fidelity_mode_name(fidelity.mode),
                        fidelity.target_triangles);
                }
            }
        }
        if (invalid_render_snapshot) {
            ImGui::TextColored(
                color(snapshot.colors.semantic_warning),
                "Render snapshot contains invalid geometry; selection disabled.");
        }
        if (camera_frame_failed) {
            ImGui::TextColored(
                color(snapshot.colors.semantic_warning),
                "Frame all could not fit this scene within the supported view range.");
        }
        if (instances.empty()) ImGui::TextDisabled("No compiled geometry");

        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetMousePos();
            std::uint8_t modifiers = 0U;
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl) modifiers |= carto::ui::kNativeModifierControl;
            if (io.KeyShift) modifiers |= carto::ui::kNativeModifierShift;
            if (io.KeyAlt) modifiers |= carto::ui::kNativeModifierAlt;
            const auto selection_operation =
                carto::ui::selection_operation_for_native_pointer({
                    carto::ui::NativePointerButton::primary,
                    0, 0, modifiers, true});
            if (selection_operation.has_value() &&
                snapshot.selection.mode == carto::editor::SelectionMode::edge) {
                float best = 12.0F * 12.0F;
                std::optional<ViewportEdgeHit> hit;
                for (const auto& candidate : edges) {
                    const float distance = point_segment_distance_squared(
                        mouse, candidate.first, candidate.second);
                    if (distance < best) {
                        best = distance;
                        hit = candidate;
                    }
                }
                if (hit.has_value()) dispatch(SelectEdgeAction{
                    hit->object, hit->edge, *selection_operation});
            } else if (selection_operation.has_value() &&
                       snapshot.selection.mode == carto::editor::SelectionMode::face) {
                for (auto iterator = faces.rbegin(); iterator != faces.rend(); ++iterator) {
                    if (point_in_triangle(mouse, iterator->points[0], iterator->points[1], iterator->points[2])) {
                        dispatch(SelectFaceAction{
                            iterator->object, iterator->face, *selection_operation});
                        break;
                    }
                }
            } else if (selection_operation.has_value() &&
                       snapshot.selection.mode == carto::editor::SelectionMode::vertex) {
                float best = 12.0F * 12.0F;
                std::optional<ViewportVertexHit> hit;
                for (const auto& candidate : vertices) {
                    const float dx = candidate.point.x - mouse.x;
                    const float dy = candidate.point.y - mouse.y;
                    const float distance = dx * dx + dy * dy;
                    if (distance < best) { best = distance; hit = candidate; }
                }
                if (hit.has_value()) dispatch(SelectVertexAction{
                    hit->object, hit->vertex, *selection_operation});
            } else if (selection_operation.has_value()) {
                for (auto iterator = objects.rbegin(); iterator != objects.rend(); ++iterator) {
                    if (iterator->second.Contains(mouse)) {
                        dispatch(SelectObjectAction{iterator->first, *selection_operation});
                        break;
                    }
                }
            }
        }
        ImGui::EndChild();
    }

    void draw_inspector(const ApplicationSnapshot& snapshot) {
        if (inspector_generation != snapshot.project_generation) {
            inspector_generation = snapshot.project_generation;
            inspector_object = {};
            inspector_transform = carto::core::Transform::identity();
            inspector_vertex = {};
            inspector_vertex_position = {};
        }
        heading_text("Inspector");
        ImGui::TextDisabled("Selected object and mesh-element instruments");
        ImGui::Separator();
        if (!snapshot.selection.component_object.has_value() && snapshot.selection.objects.empty()) {
            ImGui::TextDisabled("Select an object or mesh element");
            return;
        }
        const auto selected_id = snapshot.selection.component_object.value_or(
            snapshot.selection.objects.empty() ? carto::scene::ObjectId{} : snapshot.selection.objects.front());
        const auto object = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
                                         [selected_id](const auto& value) { return value.object.id == selected_id; });
        if (object != snapshot.objects.end()) {
            if (inspector_object != selected_id) {
                inspector_object = selected_id;
                inspector_transform = object->object.local_transform;
            }
            ImGui::Text("Object: %s", object->object.name.c_str());
            ImGui::TextDisabled("Transform is a detachable instrument.");
            if (ImGui::Button("Open Transform Instrument")) workbench.transform = true;
            ImGui::SameLine();
            if (ImGui::Button("Open Fidelity")) workbench.measure = true;
        }
        const auto instances = snapshot.viewport.scene.instances();
        const auto instance = std::find_if(instances.begin(), instances.end(),
                                           [selected_id](const auto& value) { return value.object == selected_id; });
        if (instance != instances.end() && snapshot.selection.mode == carto::editor::SelectionMode::vertex &&
            !snapshot.selection.vertices.empty()) {
            const auto selected_vertex = snapshot.selection.vertices.front();
            const auto vertex = std::find(instance->mesh->vertex_ids.begin(), instance->mesh->vertex_ids.end(), selected_vertex);
            if (vertex != instance->mesh->vertex_ids.end()) {
                const std::size_t index = static_cast<std::size_t>(vertex - instance->mesh->vertex_ids.begin());
                if (inspector_vertex != selected_vertex) {
                    inspector_vertex = selected_vertex;
                    inspector_vertex_position = instance->mesh->positions[index];
                }
                ImGui::Separator();
                ImGui::Text("Vertex %llu", static_cast<unsigned long long>(selected_vertex.value));
                ImGui::InputDouble("Position X", &inspector_vertex_position.x);
                ImGui::InputDouble("Position Y", &inspector_vertex_position.y);
                ImGui::InputDouble("Position Z", &inspector_vertex_position.z);
                if (ImGui::Button("Apply Vertex")) {
                    carto::editor::ToolArguments arguments;
                    arguments.position = inspector_vertex_position;
                    dispatch(carto::application::InvokeToolAction{"mesh.set-vertex-position", arguments});
                }
            }
        }
        if (snapshot.selection.mode == carto::editor::SelectionMode::edge &&
            !snapshot.selection.edges.empty()) {
            ImGui::Separator();
            ImGui::Text("Edge %llu", static_cast<unsigned long long>(snapshot.selection.edges.front().value));
            ImGui::InputDouble("Factor", &edge_split_factor);
            if (ImGui::Button("Split Edge")) {
                carto::editor::ToolArguments arguments;
                arguments.factor = edge_split_factor;
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.split-edge", arguments}));
            }
        }
        if (snapshot.selection.mode == carto::editor::SelectionMode::face && !snapshot.selection.faces.empty()) {
            ImGui::Separator();
            ImGui::Text("Face %llu", static_cast<unsigned long long>(snapshot.selection.faces.front().value));
            ImGui::TextDisabled("Face operation available as a movable instrument.");
            if (ImGui::Button("Open Extrude Instrument")) workbench.extrude = true;
            ImGui::SameLine();
            if (ImGui::Button("Inset Face")) {
                carto::editor::ToolArguments arguments;
                arguments.distance = inset_distance;
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.inset-face", arguments}));
            }
            ImGui::InputDouble("Inset Distance", &inset_distance);
            if (ImGui::Button("Delete Face")) {
                carto::editor::ToolArguments arguments;
                arguments.remove_orphaned_vertices = true;
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.remove-face", arguments}));
            }
        }
    }

    void draw_scene_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::scene,
                              "Scene", workbench.scene, ImVec2(118, 48),
                              ImVec2(286, 330), snapshot.colors.accent_primary)) return;
        draw_outliner(snapshot);
        end_instrument(carto::ui::WorkbenchInstrument::scene, workbench.scene);
    }

    void draw_assets_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::assets,
                              "Assets", workbench.assets, ImVec2(135, 100),
                              ImVec2(330, 360), snapshot.colors.accent_primary)) return;
        ImGui::TextDisabled("Project mesh assets · authoritative instances");
        if (ImGui::Button("Import OBJ / PLY / STL", ImVec2(-1, 30))) import_mesh();

        const bool can_duplicate = snapshot.selection.mode ==
                carto::editor::SelectionMode::object &&
            snapshot.selection.objects.size() == 1U;
        ImGui::BeginDisabled(!can_duplicate);
        if (ImGui::Button("Duplicate selected instance", ImVec2(-1, 30))) {
            duplicate_selected_object(snapshot);
        }
        ImGui::EndDisabled();
        ImGui::Separator();

        std::set<std::uint64_t> mesh_assets;
        for (const auto& object : snapshot.objects) {
            if (!object.object.mesh_asset.has_value()) continue;
            mesh_assets.insert(*object.object.mesh_asset);
            const bool selected = std::find(
                snapshot.selection.objects.begin(), snapshot.selection.objects.end(),
                object.object.id) != snapshot.selection.objects.end();
            const std::string label = object.object.name + "##asset_object_" +
                std::to_string(object.object.id.value);
            if (ImGui::Selectable(label.c_str(), selected)) {
                static_cast<void>(dispatch(SelectObjectAction{object.object.id}));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("mesh %llu", static_cast<unsigned long long>(
                *object.object.mesh_asset));
            const auto* instance = snapshot.viewport.scene.find(object.object.id);
            if (instance != nullptr && instance->mesh != nullptr) {
                ImGui::TextDisabled("  %zu vertices · %zu triangles · source rev %llu",
                    instance->mesh->positions.size(), instance->mesh->indices.size() / 3U,
                    static_cast<unsigned long long>(instance->source_revision.value()));
            } else {
                ImGui::TextDisabled("  not present in compiled viewport snapshot");
            }
        }
        if (mesh_assets.empty()) {
            ImGui::TextDisabled("No mesh assets. Import a file or create a primitive.");
        } else {
            ImGui::TextDisabled("%zu reusable mesh asset%s across %zu object%s",
                mesh_assets.size(), mesh_assets.size() == 1U ? "" : "s",
                snapshot.objects.size(), snapshot.objects.size() == 1U ? "" : "s");
        }
        ImGui::Separator();
        ImGui::Text("Source reimport registry · %zu", snapshot.source_assets.size());
        if (snapshot.source_assets.empty()) {
            ImGui::TextDisabled("No source-linked imports are recorded in this project.");
        } else {
            for (const auto& asset : snapshot.source_assets) {
                ImGui::PushID(asset.identity.c_str());
                ImGui::Text("%s  ·  gen %llu", asset.identity.c_str(),
                    static_cast<unsigned long long>(asset.source_generation.value()));
                ImGui::TextDisabled("%s", asset.relative_source_path.c_str());
                ImGui::TextDisabled("%s · %s · %s", asset.importer_identity.c_str(),
                    asset.artifact_identity.c_str(), asset.source_digest.hex().substr(0U, 12U).c_str());
                ImGui::PopID();
            }
        }
        if (snapshot.source_asset_tombstone_count > 0U) {
            ImGui::TextDisabled("%zu removed identity tombstone%s retained",
                snapshot.source_asset_tombstone_count,
                snapshot.source_asset_tombstone_count == 1U ? "" : "s");
        }
        if (!action_feedback.empty()) {
            ImGui::Separator();
            draw_action_feedback(snapshot);
        }
        end_instrument(carto::ui::WorkbenchInstrument::assets, workbench.assets);
    }

    void refresh_material_graph_report() {
        const auto report = carto::project::compile_material_graph(material_graph_draft);
        material_graph_compile_report = report
            ? std::optional<carto::project::MaterialGraphCompileReport>{report.value()}
            : std::nullopt;
        material_graph_edit_diagnostic = report ? std::string{} : report.error().message;
    }

    bool accept_material_graph_candidate(carto::project::MaterialGraph candidate) {
        const auto report = carto::project::compile_material_graph(candidate);
        if (!report) {
            material_graph_edit_diagnostic = report.error().message;
            return false;
        }
        material_graph_draft = std::move(candidate);
        material_graph_draft_dirty = true;
        material_graph_compile_report = report.value();
        material_graph_edit_diagnostic.clear();
        return true;
    }

    void draw_material_graph_topology_controls(const UiSnapshot& snapshot) {
        using carto::project::MaterialGraphNode;
        using carto::project::MaterialGraphNodeKind;
        using carto::project::MaterialGraphValueType;

        if (!ImGui::CollapsingHeader("Nodes, links & outputs")) return;
        ImGui::TextWrapped(
            "Add typed nodes, connect only matching value types, and route material outputs. "
            "Retired node identities remain reserved and can never be reused.");

        auto selected_node = std::find_if(
            material_graph_draft.nodes.begin(), material_graph_draft.nodes.end(),
            [this](const auto& node) { return node.id == selected_material_graph_node; });
        if (selected_node != material_graph_draft.nodes.end()) {
            const std::uint64_t selected_node_id = selected_node->id;
            const std::size_t input_count = selected_node->inputs.size();
            ImGui::TextDisabled("Links for %s",
                (selected_node->parameter_name.empty()
                    ? std::string("node #") + std::to_string(selected_node->id)
                    : selected_node->parameter_name).c_str());
            for (std::size_t input_index = 0U;
                 input_index < input_count; ++input_index) {
                const auto active_node = std::find_if(
                    material_graph_draft.nodes.begin(), material_graph_draft.nodes.end(),
                    [selected_node_id](const auto& node) {
                        return node.id == selected_node_id;
                    });
                if (active_node == material_graph_draft.nodes.end()) break;
                const auto expected = material_graph_input_type(*active_node, input_index);
                if (!expected.has_value()) continue;
                const std::uint64_t current_id = active_node->inputs[input_index];
                const auto current = std::find_if(
                    material_graph_draft.nodes.begin(), material_graph_draft.nodes.end(),
                    [current_id](const auto& node) { return node.id == current_id; });
                const std::string preview = current == material_graph_draft.nodes.end()
                    ? "Missing node" : "#" + std::to_string(current->id) + " · " +
                        material_graph_value_type_name(current->value_type);
                const std::string label = "Input " + std::to_string(input_index) +
                    " (" + material_graph_value_type_name(*expected) + ")";
                std::optional<std::uint64_t> replacement;
                ImGui::PushID(static_cast<int>(input_index));
                if (ImGui::BeginCombo(label.c_str(), preview.c_str())) {
                    for (const auto& candidate_node : material_graph_draft.nodes) {
                        if (candidate_node.id == selected_node_id ||
                            candidate_node.value_type != *expected) continue;
                        const std::string node_id = std::to_string(candidate_node.id);
                        ImGui::PushID(node_id.c_str());
                        const std::string candidate_label = "#" +
                            std::to_string(candidate_node.id) + " · " +
                            material_graph_value_type_name(candidate_node.value_type);
                        if (ImGui::Selectable(candidate_label.c_str(),
                                candidate_node.id == current_id)) {
                            replacement = candidate_node.id;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
                if (replacement.has_value()) {
                    auto edited = material_graph_draft;
                    const auto edited_node = std::find_if(
                        edited.nodes.begin(), edited.nodes.end(),
                        [selected_node_id](const auto& node) {
                            return node.id == selected_node_id;
                        });
                    edited_node->inputs[input_index] = *replacement;
                    static_cast<void>(accept_material_graph_candidate(std::move(edited)));
                }
            }
        }

        if (selected_material_graph_node != 0U) {
            const auto selected_for_retirement = std::find_if(
                material_graph_draft.nodes.begin(), material_graph_draft.nodes.end(),
                [this](const auto& node) { return node.id == selected_material_graph_node; });
            if (selected_for_retirement != material_graph_draft.nodes.end()) {
                const std::uint64_t retired_id = selected_for_retirement->id;
                const bool has_consumers = std::any_of(
                    material_graph_draft.nodes.begin(), material_graph_draft.nodes.end(),
                    [retired_id](const auto& node) {
                        return std::find(node.inputs.begin(), node.inputs.end(), retired_id) !=
                            node.inputs.end();
                    });
                const auto& outputs = material_graph_draft.outputs;
                const std::array<std::uint64_t, 15U> output_ids{
                    outputs.base_color, outputs.alpha, outputs.roughness,
                    outputs.metallic, outputs.normal, outputs.occlusion,
                    outputs.emissive, outputs.transmission, outputs.subsurface,
                    outputs.sheen, outputs.anisotropy, outputs.clearcoat,
                    outputs.clearcoat_roughness, outputs.opacity,
                    outputs.displacement};
                const bool routed_to_output = std::any_of(
                    output_ids.begin(), output_ids.end(),
                    [retired_id](std::uint64_t id) { return id == retired_id; });
                const bool unsupported = selected_for_retirement->kind ==
                    MaterialGraphNodeKind::unsupported;
                const bool lineage_full = material_graph_draft.retired_node_ids.size() >=
                    carto::project::MaterialGraph::kMaxRetiredNodeIds;
                ImGui::BeginDisabled(has_consumers || routed_to_output || unsupported || lineage_full);
                if (ImGui::Button("Retire selected node", ImVec2(-1, 24))) {
                    auto edited = material_graph_draft;
                    edited.nodes.erase(std::remove_if(
                        edited.nodes.begin(), edited.nodes.end(),
                        [retired_id](const auto& node) { return node.id == retired_id; }),
                        edited.nodes.end());
                    const auto insertion = std::lower_bound(
                        edited.retired_node_ids.begin(), edited.retired_node_ids.end(),
                        retired_id);
                    edited.retired_node_ids.insert(insertion, retired_id);
                    if (accept_material_graph_candidate(std::move(edited))) {
                        selected_material_graph_node = 0U;
                    }
                }
                ImGui::EndDisabled();
                if (has_consumers || routed_to_output) {
                    ImGui::TextDisabled(
                        "Disconnect dependents and material outputs before retiring this node.");
                } else if (unsupported) {
                    ImGui::TextDisabled(
                        "Unsupported donor payload is retained until its format is understood.");
                } else if (lineage_full) {
                    ImGui::TextDisabled("Retired-node lineage reached its safety limit.");
                }
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Material output routing");
        struct OutputBinding {
            const char* label;
            std::uint64_t carto::project::MaterialGraphOutputs::* member;
            MaterialGraphValueType type;
            bool required;
        };
        static constexpr std::array<OutputBinding, 15U> outputs{{
            {"Base color", &carto::project::MaterialGraphOutputs::base_color,
                MaterialGraphValueType::color, true},
            {"Alpha", &carto::project::MaterialGraphOutputs::alpha,
                MaterialGraphValueType::scalar, true},
            {"Roughness", &carto::project::MaterialGraphOutputs::roughness,
                MaterialGraphValueType::scalar, true},
            {"Metallic", &carto::project::MaterialGraphOutputs::metallic,
                MaterialGraphValueType::scalar, true},
            {"Normal", &carto::project::MaterialGraphOutputs::normal,
                MaterialGraphValueType::normal, false},
            {"Occlusion", &carto::project::MaterialGraphOutputs::occlusion,
                MaterialGraphValueType::scalar, false},
            {"Emissive", &carto::project::MaterialGraphOutputs::emissive,
                MaterialGraphValueType::color, false},
            {"Transmission", &carto::project::MaterialGraphOutputs::transmission,
                MaterialGraphValueType::scalar, false},
            {"Subsurface", &carto::project::MaterialGraphOutputs::subsurface,
                MaterialGraphValueType::scalar, false},
            {"Sheen", &carto::project::MaterialGraphOutputs::sheen,
                MaterialGraphValueType::scalar, false},
            {"Anisotropy", &carto::project::MaterialGraphOutputs::anisotropy,
                MaterialGraphValueType::scalar, false},
            {"Clearcoat", &carto::project::MaterialGraphOutputs::clearcoat,
                MaterialGraphValueType::scalar, false},
            {"Clearcoat roughness", &carto::project::MaterialGraphOutputs::clearcoat_roughness,
                MaterialGraphValueType::scalar, false},
            {"Opacity", &carto::project::MaterialGraphOutputs::opacity,
                MaterialGraphValueType::scalar, false},
            {"Displacement", &carto::project::MaterialGraphOutputs::displacement,
                MaterialGraphValueType::scalar, false},
        }};
        for (const auto& output : outputs) {
            auto& output_id = material_graph_draft.outputs.*(output.member);
            const auto current = output_id == 0U
                ? material_graph_draft.nodes.end()
                : std::find_if(material_graph_draft.nodes.begin(),
                    material_graph_draft.nodes.end(), [&output_id](const auto& node) {
                        return node.id == output_id;
                    });
            const std::string preview = output_id == 0U
                ? "Unbound" : current == material_graph_draft.nodes.end()
                    ? "Missing node" : "#" + std::to_string(current->id);
            bool output_changed = false;
            std::uint64_t replacement = output_id;
            ImGui::PushID(output.label);
            if (ImGui::BeginCombo(output.label, preview.c_str())) {
                if (!output.required && ImGui::Selectable("Unbound", output_id == 0U)) {
                    replacement = 0U;
                    output_changed = true;
                }
                for (const auto& candidate_node : material_graph_draft.nodes) {
                    if (candidate_node.value_type != output.type) continue;
                    const std::string node_id = std::to_string(candidate_node.id);
                    ImGui::PushID(node_id.c_str());
                    const std::string candidate_label = "#" +
                        std::to_string(candidate_node.id) + " · " +
                        material_graph_value_type_name(candidate_node.value_type);
                    if (ImGui::Selectable(candidate_label.c_str(),
                            candidate_node.id == output_id)) {
                        replacement = candidate_node.id;
                        output_changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
            if (output_changed) {
                auto edited = material_graph_draft;
                edited.outputs.*(output.member) = replacement;
                static_cast<void>(accept_material_graph_candidate(std::move(edited)));
            }
            if (output.type == MaterialGraphValueType::normal &&
                std::none_of(material_graph_draft.nodes.begin(),
                    material_graph_draft.nodes.end(), [](const auto& node) {
                        return node.value_type == MaterialGraphValueType::normal;
                    })) {
                ImGui::TextDisabled("No normal-valued node type is available yet.");
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Add a source node");
        static constexpr const char* node_kinds[]{
            "Constant scalar", "Constant color", "Scalar parameter", "Color parameter",
            "Surface input", "Add", "Multiply", "Mix"};
        material_graph_new_node_kind = std::clamp(material_graph_new_node_kind, 0, 7);
        ImGui::Combo("Node kind", &material_graph_new_node_kind, node_kinds, 8);
        const bool operator_node = material_graph_new_node_kind >= 5;
        if (operator_node) {
            static constexpr const char* value_types[]{"Scalar", "Color"};
            material_graph_new_node_type = std::clamp(material_graph_new_node_type, 0, 1);
            ImGui::Combo("Operator type", &material_graph_new_node_type, value_types, 2);
        } else if (material_graph_new_node_kind == 2 || material_graph_new_node_kind == 3) {
            ImGui::InputText("Parameter name", material_graph_new_parameter_name.data(),
                material_graph_new_parameter_name.size());
        } else if (material_graph_new_node_kind == 4) {
            static constexpr const char* surface_names[]{
                "Curvature", "Thickness", "Contact", "Weathering", "Edge process",
                "Surface domain", "Material stratigraphy", "Authored mask"};
            material_graph_new_surface_input = std::clamp(
                material_graph_new_surface_input, 0, 7);
            ImGui::Combo("Surface data", &material_graph_new_surface_input,
                surface_names, 8);
        }

        const auto count_type = [this](MaterialGraphValueType type) {
            return static_cast<std::size_t>(std::count_if(
                material_graph_draft.nodes.begin(), material_graph_draft.nodes.end(),
                [type](const auto& node) { return node.value_type == type; }));
        };
        const MaterialGraphValueType operator_type = material_graph_new_node_type == 0
            ? MaterialGraphValueType::scalar : MaterialGraphValueType::color;
        const bool enough_operator_inputs = !operator_node ||
            (count_type(operator_type) >= 2U &&
             (material_graph_new_node_kind != 7 || count_type(MaterialGraphValueType::scalar) > 0U));
        std::uint64_t maximum_node_id = 0U;
        for (const auto& node : material_graph_draft.nodes) {
            maximum_node_id = std::max(maximum_node_id, node.id);
        }
        for (const auto retired_id : material_graph_draft.retired_node_ids) {
            maximum_node_id = std::max(maximum_node_id, retired_id);
        }
        const bool node_identity_available = maximum_node_id !=
            std::numeric_limits<std::uint64_t>::max();
        ImGui::BeginDisabled(material_graph_draft.nodes.size() >=
                carto::project::MaterialGraph::kMaxNodes ||
            !node_identity_available || !enough_operator_inputs);
        if (ImGui::Button("Add node", ImVec2(-1, 25))) {
            MaterialGraphNode node;
            node.id = maximum_node_id + 1U;
            switch (material_graph_new_node_kind) {
            case 0:
                node.kind = MaterialGraphNodeKind::constant_scalar;
                node.value_type = MaterialGraphValueType::scalar;
                node.constants[0] = 0.5;
                break;
            case 1:
                node.kind = MaterialGraphNodeKind::constant_color;
                node.value_type = MaterialGraphValueType::color;
                node.constants = {0.5, 0.5, 0.5, 1.0};
                break;
            case 2:
                node.kind = MaterialGraphNodeKind::parameter_scalar;
                node.value_type = MaterialGraphValueType::scalar;
                node.parameter_name = material_graph_new_parameter_name.data();
                break;
            case 3:
                node.kind = MaterialGraphNodeKind::parameter_color;
                node.value_type = MaterialGraphValueType::color;
                node.parameter_name = material_graph_new_parameter_name.data();
                break;
            case 4:
                node.kind = MaterialGraphNodeKind::surface_input;
                node.value_type = MaterialGraphValueType::scalar;
                node.surface_input = static_cast<carto::project::MaterialSurfaceInput>(
                    material_graph_new_surface_input + 1);
                break;
            case 5:
            case 6:
            case 7: {
                node.kind = material_graph_new_node_kind == 5
                    ? MaterialGraphNodeKind::add
                    : material_graph_new_node_kind == 6
                        ? MaterialGraphNodeKind::multiply : MaterialGraphNodeKind::mix;
                node.value_type = operator_type;
                std::vector<std::uint64_t> matching;
                std::vector<std::uint64_t> scalar_inputs;
                for (const auto& source : material_graph_draft.nodes) {
                    if (source.value_type == operator_type) matching.push_back(source.id);
                    if (source.value_type == MaterialGraphValueType::scalar) {
                        scalar_inputs.push_back(source.id);
                    }
                }
                if (matching.size() >= 2U) {
                    node.inputs = {matching[0], matching[1]};
                    if (node.kind == MaterialGraphNodeKind::mix &&
                        !scalar_inputs.empty()) {
                        node.inputs.push_back(scalar_inputs.front());
                    }
                }
                break;
            }
            default:
                break;
            }
            auto edited = material_graph_draft;
            edited.nodes.push_back(std::move(node));
            if (accept_material_graph_candidate(std::move(edited))) {
                selected_material_graph_node = maximum_node_id + 1U;
            }
        }
        ImGui::EndDisabled();
        if (operator_node && !enough_operator_inputs) {
            ImGui::TextDisabled(
                "Add at least two matching source nodes first; Mix also needs a scalar weight.");
        }
        if (!material_graph_edit_diagnostic.empty()) {
            ImGui::TextColored(color(snapshot.colors.semantic_warning), "%s",
                material_graph_edit_diagnostic.c_str());
        }
    }

    void draw_material_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::material,
                              "Material", workbench.material, ImVec2(-8, 390),
                              ImVec2(340, 700), snapshot.colors.accent_secondary)) return;

        ImGui::TextDisabled("Project-owned PBR source · .carto persisted");
        ImGui::TextWrapped(
            "Edit durable source factors and typed material graphs. Structural graph checks "
            "do not compile GPU shaders or provide live viewport shading.");
        ImGui::Separator();

        if (snapshot.materials.empty()) {
            ImGui::TextDisabled("No material sources in this project.");
        } else {
            if (std::none_of(snapshot.materials.begin(), snapshot.materials.end(),
                    [this](const auto& value) { return value.id == selected_material_id; })) {
                selected_material_id = snapshot.materials.front().id;
                material_draft_loaded = false;
            }
            const auto selected = std::find_if(
                snapshot.materials.begin(), snapshot.materials.end(),
                [this](const auto& value) { return value.id == selected_material_id; });
            const std::string preview = selected == snapshot.materials.end()
                ? "Select material" : selected->name;
            ImGui::BeginDisabled(material_draft_dirty);
            if (ImGui::BeginCombo("Material source", preview.c_str())) {
                for (const auto& material : snapshot.materials) {
                    ImGui::PushID(static_cast<int>(material.id & 0x7fffffffU));
                    const std::string label = material.name + "  ·  rev " +
                        std::to_string(material.revision.value());
                    if (ImGui::Selectable(label.c_str(), material.id == selected_material_id)) {
                        selected_material_id = material.id;
                        material_draft_loaded = false;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();

            if (selected != snapshot.materials.end()) {
                if (!material_draft_loaded ||
                    (!material_draft_dirty && material_draft_revision != selected->revision)) {
                    material_draft = selected->definition;
                    material_draft_revision = selected->revision;
                    material_draft_loaded = true;
                    material_draft_dirty = false;
                }
                bool material_draft_stale = material_draft_dirty &&
                    material_draft_revision != selected->revision;
                ImGui::PushID(static_cast<int>(selected->id & 0x7fffffffU));
                ImGui::Text("%s", selected->name.c_str());
                ImGui::TextDisabled("Material %llu · source revision %llu",
                    static_cast<unsigned long long>(selected->id),
                    static_cast<unsigned long long>(selected->revision.value()));
                const auto graph_binding = std::find_if(
                    snapshot.material_graph_bindings.begin(),
                    snapshot.material_graph_bindings.end(),
                    [selected](const auto& value) { return value.material == selected->id; });
                std::string binding_preview =
                    graph_binding == snapshot.material_graph_bindings.end()
                    ? "Unbound" : graph_binding->graph_identity;
                if (graph_binding != snapshot.material_graph_bindings.end() &&
                    !graph_binding->source_current) {
                    binding_preview += " · stale source r" +
                        std::to_string(graph_binding->source_revision.value());
                }
                std::optional<std::optional<std::string>> requested_graph_binding;
                ImGui::BeginDisabled(material_draft_dirty);
                if (ImGui::BeginCombo("Source graph binding", binding_preview.c_str())) {
                    if (ImGui::Selectable("Unbound",
                            graph_binding == snapshot.material_graph_bindings.end()) &&
                        graph_binding != snapshot.material_graph_bindings.end()) {
                        requested_graph_binding = std::optional<std::string>{};
                    }
                    for (const auto& graph_view : snapshot.material_graphs) {
                        const bool is_bound = graph_binding !=
                                snapshot.material_graph_bindings.end() &&
                            graph_binding->source_current &&
                            graph_binding->graph_identity == graph_view.identity;
                        if (ImGui::Selectable(graph_view.identity.c_str(), is_bound) &&
                            !is_bound) {
                            requested_graph_binding = graph_view.identity;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
                if (requested_graph_binding.has_value()) {
                    std::optional<carto::core::Revision> expected_revision;
                    std::optional<carto::assets::Sha256Digest> expected_digest;
                    if (requested_graph_binding->has_value()) {
                        const auto graph_source = ui.material_graph_source(
                            requested_graph_binding->value());
                        if (graph_source) {
                            expected_revision = graph_source.value().revision;
                            const auto digest = graph_source.value().canonical_digest();
                            if (digest) expected_digest = digest.value();
                        }
                    }
                    static_cast<void>(dispatch(
                        carto::application::SetMaterialGraphBindingAction{
                            snapshot.project_revision, selected->id, selected->revision,
                            requested_graph_binding->value(), expected_revision,
                            expected_digest}));
                }
                ImGui::TextDisabled(
                    "Binding preserves source association; graph evaluation is not yet available.");
                if (material_draft_stale) {
                    ImGui::TextColored(color(snapshot.colors.semantic_warning),
                        "Source changed. Your local draft is retained; reload before applying.");
                    if (ImGui::Button("Discard stale draft and reload", ImVec2(-1, 24))) {
                        material_draft = selected->definition;
                        material_draft_revision = selected->revision;
                        material_draft_dirty = false;
                        material_draft_stale = false;
                    }
                }
                const auto& color_factor = material_draft.base_color_factor;
                const float swatch[4]{
                    static_cast<float>(color_factor.x), static_cast<float>(color_factor.y),
                    static_cast<float>(color_factor.z), static_cast<float>(color_factor.w)};
                ImGui::ColorButton("Current source color", ImVec4(
                    swatch[0], swatch[1], swatch[2], swatch[3]),
                    ImGuiColorEditFlags_NoTooltip, ImVec2(-1, 28));
                float editable_color[4]{swatch[0], swatch[1], swatch[2], swatch[3]};
                if (ImGui::ColorEdit4("Base color", editable_color,
                        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar)) {
                    material_draft.base_color_factor = {
                        editable_color[0], editable_color[1],
                        editable_color[2], editable_color[3]};
                    material_draft_dirty = true;
                }
                float metallic = static_cast<float>(material_draft.metallic);
                if (ImGui::SliderFloat("Metallic", &metallic, 0.0F, 1.0F, "%.3f")) {
                    material_draft.metallic = metallic;
                    material_draft_dirty = true;
                }
                float roughness = static_cast<float>(material_draft.roughness);
                if (ImGui::SliderFloat("Roughness", &roughness, 0.045F, 1.0F, "%.3f")) {
                    material_draft.roughness = roughness;
                    material_draft_dirty = true;
                }
                float clearcoat = static_cast<float>(material_draft.clearcoat);
                if (ImGui::SliderFloat("Clearcoat", &clearcoat, 0.0F, 1.0F, "%.3f")) {
                    material_draft.clearcoat = clearcoat;
                    material_draft_dirty = true;
                }
                ImGui::BeginDisabled(!material_draft_dirty || material_draft_stale);
                if (ImGui::Button("Apply source changes", ImVec2(-1, 28))) {
                    if (dispatch(carto::application::UpdateMaterialAction{
                            snapshot.project_revision, selected->id,
                            selected->revision, material_draft})) {
                        material_draft_dirty = false;
                    }
                }
                ImGui::EndDisabled();
                if (!material_draft_stale && material_draft_dirty &&
                    ImGui::Button("Discard draft", ImVec2(-1, 24))) {
                    material_draft = selected->definition;
                    material_draft_revision = selected->revision;
                    material_draft_dirty = false;
                }
                ImGui::PopID();
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Material graph source");
        ImGui::TextWrapped(
            "Create a starter graph from the selected PBR factors, then edit constant nodes. "
            "Topology and links are preserved; unsupported nodes remain explicit.");
        ImGui::InputText("New graph identity", material_graph_identity_buffer.data(),
                         material_graph_identity_buffer.size());
        const auto graph_material = std::find_if(
            snapshot.materials.begin(), snapshot.materials.end(),
            [this](const auto& value) { return value.id == selected_material_id; });
        ImGui::BeginDisabled(graph_material == snapshot.materials.end() ||
                             material_graph_identity_buffer.front() == '\0');
        if (ImGui::Button("Create factor graph", ImVec2(-1, 26))) {
            carto::project::MaterialGraph graph;
            graph.identity = material_graph_identity_buffer.data();
            graph.revision = carto::core::Revision{1U};
            const auto& definition = graph_material->definition;
            carto::project::MaterialGraphNode base_color;
            base_color.id = 1U;
            base_color.kind = carto::project::MaterialGraphNodeKind::constant_color;
            base_color.value_type = carto::project::MaterialGraphValueType::color;
            base_color.constants = {definition.base_color_factor.x,
                definition.base_color_factor.y, definition.base_color_factor.z,
                definition.base_color_factor.w};
            carto::project::MaterialGraphNode alpha;
            alpha.id = 2U;
            alpha.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
            alpha.value_type = carto::project::MaterialGraphValueType::scalar;
            alpha.constants[0] = definition.base_color_factor.w;
            carto::project::MaterialGraphNode roughness;
            roughness.id = 3U;
            roughness.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
            roughness.value_type = carto::project::MaterialGraphValueType::scalar;
            roughness.constants[0] = definition.roughness;
            carto::project::MaterialGraphNode metallic;
            metallic.id = 4U;
            metallic.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
            metallic.value_type = carto::project::MaterialGraphValueType::scalar;
            metallic.constants[0] = definition.metallic;
            graph.nodes = {base_color, alpha, roughness, metallic};
            graph.outputs.base_color = base_color.id;
            graph.outputs.alpha = alpha.id;
            graph.outputs.roughness = roughness.id;
            graph.outputs.metallic = metallic.id;
            const std::string identity = graph.identity;
            if (dispatch(carto::application::SetMaterialGraphAction{
                    snapshot.project_revision, std::nullopt, std::nullopt,
                    std::move(graph)})) {
                selected_material_graph_identity = identity;
                material_graph_draft_loaded = false;
                material_graph_draft_dirty = false;
                material_graph_identity_buffer.fill('\0');
            }
        }
        ImGui::EndDisabled();

        if (!snapshot.material_graphs.empty() &&
            std::none_of(snapshot.material_graphs.begin(), snapshot.material_graphs.end(),
                [this](const auto& graph) {
                    return graph.identity == selected_material_graph_identity;
                })) {
            selected_material_graph_identity = snapshot.material_graphs.front().identity;
            material_graph_draft_loaded = false;
        }
        if (snapshot.material_graphs.empty()) {
            ImGui::TextDisabled("No material graph sources in this project.");
        } else {
            auto selected_graph_view = std::find_if(
                snapshot.material_graphs.begin(), snapshot.material_graphs.end(),
                [this](const auto& graph) {
                    return graph.identity == selected_material_graph_identity;
                });
            const std::string graph_preview = selected_graph_view == snapshot.material_graphs.end()
                ? "Select graph" : selected_graph_view->identity;
            if (ImGui::BeginCombo("Graph source", graph_preview.c_str())) {
                for (const auto& graph : snapshot.material_graphs) {
                    ImGui::PushID(graph.identity.c_str());
                    const std::string label = graph.identity + " · rev " +
                        std::to_string(graph.source_revision.value()) + " · " +
                        std::to_string(graph.node_count) + " nodes";
                    if (ImGui::Selectable(label.c_str(),
                            graph.identity == selected_material_graph_identity)) {
                        selected_material_graph_identity = graph.identity;
                        material_graph_draft_loaded = false;
                        material_graph_draft_dirty = false;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            selected_graph_view = std::find_if(
                snapshot.material_graphs.begin(), snapshot.material_graphs.end(),
                [this](const auto& graph) {
                    return graph.identity == selected_material_graph_identity;
                });

            const bool graph_removed = selected_graph_view == snapshot.material_graphs.end();
            if (graph_removed) {
                ImGui::TextColored(color(snapshot.colors.semantic_warning),
                    "Selected graph source is no longer present.");
            } else {
                const bool source_changed = material_graph_draft_loaded &&
                    (material_graph_draft.revision != selected_graph_view->source_revision ||
                     material_graph_draft_project_revision != snapshot.project_revision);
                if (!material_graph_draft_loaded ||
                    (!material_graph_draft_dirty && source_changed)) {
                    const auto loaded = ui.material_graph_source(
                        selected_material_graph_identity);
                    if (loaded) {
                        material_graph_draft = loaded.value();
                        material_graph_draft_project_revision = snapshot.project_revision;
                        const auto digest = material_graph_draft.canonical_digest();
                        material_graph_draft_digest = digest
                            ? std::optional<carto::assets::Sha256Digest>{digest.value()}
                            : std::nullopt;
                        const auto report = carto::project::compile_material_graph(
                            material_graph_draft);
                        material_graph_compile_report = report
                            ? std::optional<carto::project::MaterialGraphCompileReport>{report.value()}
                            : std::nullopt;
                        selected_material_graph_node = material_graph_draft.nodes.empty()
                            ? 0U : material_graph_draft.nodes.front().id;
                        material_graph_parameter_draft_loaded = false;
                        material_graph_draft_loaded = static_cast<bool>(digest);
                        material_graph_draft_dirty = false;
                    } else {
                        material_graph_draft_loaded = false;
                        ImGui::TextColored(color(snapshot.colors.semantic_warning),
                            "Graph could not be loaded from the current project source.");
                    }
                }

                const bool graph_stale = material_graph_draft_dirty &&
                    (material_graph_draft_project_revision != snapshot.project_revision ||
                     material_graph_draft.revision != selected_graph_view->source_revision);
                ImGui::Text("Source rev %llu · %zu nodes",
                    static_cast<unsigned long long>(selected_graph_view->source_revision.value()),
                    selected_graph_view->node_count);
                if (graph_stale) {
                    ImGui::TextColored(color(snapshot.colors.semantic_warning),
                        "Project/source changed. Your graph draft is retained; reload before applying.");
                    if (ImGui::Button("Discard stale graph draft and reload", ImVec2(-1, 24))) {
                        material_graph_draft_loaded = false;
                        material_graph_draft_dirty = false;
                    }
                } else if (material_graph_draft_loaded && !material_graph_draft.nodes.empty()) {
                    if (selected_material_graph_node == 0U ||
                        std::none_of(material_graph_draft.nodes.begin(),
                            material_graph_draft.nodes.end(), [this](const auto& node) {
                                return node.id == selected_material_graph_node;
                            })) {
                        selected_material_graph_node = material_graph_draft.nodes.front().id;
                    }
                    const auto node_label = [](const carto::project::MaterialGraphNode& node) {
                        const char* kind = "Node";
                        switch (node.kind) {
                        case carto::project::MaterialGraphNodeKind::constant_scalar: kind = "Constant scalar"; break;
                        case carto::project::MaterialGraphNodeKind::constant_color: kind = "Constant color"; break;
                        case carto::project::MaterialGraphNodeKind::parameter_scalar: kind = "Scalar parameter"; break;
                        case carto::project::MaterialGraphNodeKind::parameter_color: kind = "Color parameter"; break;
                        case carto::project::MaterialGraphNodeKind::texture_sample: kind = "Texture sample"; break;
                        case carto::project::MaterialGraphNodeKind::surface_input: kind = "Surface input"; break;
                        case carto::project::MaterialGraphNodeKind::add: kind = "Add"; break;
                        case carto::project::MaterialGraphNodeKind::multiply: kind = "Multiply"; break;
                        case carto::project::MaterialGraphNodeKind::mix: kind = "Mix"; break;
                        case carto::project::MaterialGraphNodeKind::unsupported: kind = "Unsupported"; break;
                        }
                        return std::string(kind) + " · #" + std::to_string(node.id);
                    };
                    auto selected_node = std::find_if(
                        material_graph_draft.nodes.begin(), material_graph_draft.nodes.end(),
                        [this](const auto& node) { return node.id == selected_material_graph_node; });
                    const std::string node_preview = selected_node == material_graph_draft.nodes.end()
                        ? "Select node" : node_label(*selected_node);
                    if (ImGui::BeginCombo("Graph node", node_preview.c_str())) {
                        for (const auto& node : material_graph_draft.nodes) {
                            const std::string node_id = std::to_string(node.id);
                            ImGui::PushID(node_id.c_str());
                            const std::string label = node_label(node);
                            if (ImGui::Selectable(label.c_str(), node.id == selected_material_graph_node)) {
                                selected_material_graph_node = node.id;
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    selected_node = std::find_if(
                        material_graph_draft.nodes.begin(), material_graph_draft.nodes.end(),
                        [this](const auto& node) {
                            return node.id == selected_material_graph_node;
                        });
                    if (selected_node != material_graph_draft.nodes.end()) {
                        bool node_changed = false;
                        ImGui::TextDisabled("Node identity %llu · %s",
                            static_cast<unsigned long long>(selected_node->id),
                            selected_node->parameter_name.empty()
                                ? "typed source" : selected_node->parameter_name.c_str());
                        if (selected_node->kind ==
                            carto::project::MaterialGraphNodeKind::constant_scalar) {
                            double value = selected_node->constants[0];
                            if (ImGui::InputDouble("Scalar value", &value, 0.01, 0.1, "%.5f")) {
                                auto mutable_node = std::find_if(
                                    material_graph_draft.nodes.begin(), material_graph_draft.nodes.end(),
                                    [this](const auto& node) { return node.id == selected_material_graph_node; });
                                mutable_node->constants[0] = value;
                                node_changed = true;
                            }
                        } else if (selected_node->kind ==
                            carto::project::MaterialGraphNodeKind::constant_color) {
                            float values[4]{static_cast<float>(selected_node->constants[0]),
                                static_cast<float>(selected_node->constants[1]),
                                static_cast<float>(selected_node->constants[2]),
                                static_cast<float>(selected_node->constants[3])};
                            if (ImGui::ColorEdit4("Color value", values,
                                    ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar)) {
                                auto mutable_node = std::find_if(
                                    material_graph_draft.nodes.begin(), material_graph_draft.nodes.end(),
                                    [this](const auto& node) { return node.id == selected_material_graph_node; });
                                mutable_node->constants = {values[0], values[1], values[2], values[3]};
                                node_changed = true;
                            }
                        } else if (selected_node->kind ==
                                carto::project::MaterialGraphNodeKind::parameter_scalar ||
                            selected_node->kind ==
                                carto::project::MaterialGraphNodeKind::parameter_color) {
                            if (!material_graph_parameter_draft_loaded ||
                                material_graph_parameter_node != selected_node->id ||
                                material_graph_parameter_identity !=
                                    selected_material_graph_identity) {
                                material_graph_parameter_draft.fill('\0');
                                const auto count = std::min(
                                    selected_node->parameter_name.size(),
                                    material_graph_parameter_draft.size() - 1U);
                                std::copy_n(selected_node->parameter_name.data(), count,
                                    material_graph_parameter_draft.data());
                                material_graph_parameter_node = selected_node->id;
                                material_graph_parameter_identity =
                                    selected_material_graph_identity;
                                material_graph_parameter_draft_loaded = true;
                            }
                            if (ImGui::InputText("Parameter",
                                    material_graph_parameter_draft.data(),
                                    material_graph_parameter_draft.size())) {
                                auto edited = material_graph_draft;
                                const auto mutable_node = std::find_if(
                                    edited.nodes.begin(), edited.nodes.end(),
                                    [this](const auto& node) {
                                        return node.id == selected_material_graph_node;
                                    });
                                mutable_node->parameter_name =
                                    material_graph_parameter_draft.data();
                                static_cast<void>(accept_material_graph_candidate(
                                    std::move(edited)));
                            }
                        } else if (selected_node->kind ==
                            carto::project::MaterialGraphNodeKind::surface_input) {
                            static constexpr const char* surface_names[]{
                                "Curvature", "Thickness", "Contact", "Weathering",
                                "Edge process", "Surface domain",
                                "Material stratigraphy", "Authored mask"};
                            int surface_index = static_cast<int>(
                                selected_node->surface_input) - 1;
                            surface_index = std::clamp(surface_index, 0, 7);
                            if (ImGui::Combo("Surface data", &surface_index,
                                    surface_names, 8)) {
                                auto edited = material_graph_draft;
                                const auto mutable_node = std::find_if(
                                    edited.nodes.begin(), edited.nodes.end(),
                                    [this](const auto& node) {
                                        return node.id == selected_material_graph_node;
                                    });
                                mutable_node->surface_input =
                                    static_cast<carto::project::MaterialSurfaceInput>(
                                        surface_index + 1);
                                static_cast<void>(accept_material_graph_candidate(
                                    std::move(edited)));
                            }
                        } else {
                            ImGui::TextDisabled(
                                "This node's authored parameters are preserved read-only here; "
                                "typed input links can be changed below.");
                        }
                        if (node_changed) {
                            material_graph_draft_dirty = true;
                            refresh_material_graph_report();
                        }
                    }
                    draw_material_graph_topology_controls(snapshot);
                    if (material_graph_compile_report.has_value()) {
                        if (material_graph_compile_report->supported) {
                            ImGui::TextColored(color(snapshot.colors.semantic_success),
                                "Structural graph checks passed · %zu nodes",
                                material_graph_compile_report->topological_order.size());
                        } else {
                            ImGui::TextColored(color(snapshot.colors.semantic_warning),
                                "%zu unsupported node(s) retained; shader lowering is unavailable",
                                material_graph_compile_report->diagnostics.size());
                        }
                    } else {
                        ImGui::TextColored(color(snapshot.colors.semantic_warning),
                            "Graph structure is invalid; apply is disabled.");
                    }
                    ImGui::BeginDisabled(!material_graph_draft_dirty || graph_stale ||
                        !material_graph_draft_digest.has_value() ||
                        material_graph_draft.revision.exhausted() ||
                        !material_graph_compile_report.has_value());
                    if (ImGui::Button("Apply graph source", ImVec2(-1, 26))) {
                        auto updated = material_graph_draft;
                        updated.revision = material_graph_draft.revision.next();
                        if (dispatch(carto::application::SetMaterialGraphAction{
                                snapshot.project_revision, material_graph_draft.revision,
                                material_graph_draft_digest, std::move(updated)})) {
                            material_graph_draft_loaded = false;
                            material_graph_draft_dirty = false;
                        }
                    }
                    ImGui::EndDisabled();
                } else {
                    ImGui::TextDisabled("Select a valid graph source to inspect its nodes.");
                }
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Mesh-region assignment");
        std::optional<std::uint64_t> selected_mesh_asset;
        if (snapshot.selection.mode == carto::editor::SelectionMode::object &&
            snapshot.selection.objects.size() == 1U) {
            const auto object = std::find_if(
                snapshot.objects.begin(), snapshot.objects.end(),
                [&snapshot](const auto& value) {
                    return value.object.id == snapshot.selection.objects.front();
                });
            if (object != snapshot.objects.end()) selected_mesh_asset = object->object.mesh_asset;
        }
        if (!selected_mesh_asset.has_value()) {
            ImGui::TextWrapped("Select one mesh object to author a region binding.");
        } else {
            ImGui::Text("Mesh asset %llu · affects every instance",
                static_cast<unsigned long long>(*selected_mesh_asset));
            ImGui::InputScalar("Region ID", ImGuiDataType_U32, &material_region_id,
                               nullptr, nullptr, "%u");
            ImGui::TextDisabled(
                "Uses authored condition.material_region or cartographer.material_slot IDs.");
            const carto::project::MaterialRegionKey key{
                *selected_mesh_asset, material_region_id};
            const auto assignment = std::find_if(
                snapshot.material_assignments.begin(), snapshot.material_assignments.end(),
                [&key](const auto& value) { return value.key == key; });
            if (assignment == snapshot.material_assignments.end()) {
                ImGui::TextDisabled("No material is assigned to this region.");
            } else {
                const auto assigned_material = std::find_if(
                    snapshot.materials.begin(), snapshot.materials.end(),
                    [assignment](const auto& value) {
                        return value.id == assignment->material;
                    });
                const char* assigned_name = assigned_material == snapshot.materials.end()
                    ? "unresolved material" : assigned_material->name.c_str();
                if (assignment->variant.has_value()) {
                    ImGui::TextWrapped("Current: %s · variant %llu",
                        assigned_name,
                        static_cast<unsigned long long>(*assignment->variant));
                } else {
                    ImGui::TextWrapped("Current: %s", assigned_name);
                }
            }
            const bool material_selected = std::any_of(
                snapshot.materials.begin(), snapshot.materials.end(),
                [this](const auto& value) { return value.id == selected_material_id; });
            const bool same_assignment = assignment != snapshot.material_assignments.end() &&
                assignment->material == selected_material_id &&
                !assignment->variant.has_value();
            ImGui::BeginDisabled(!material_selected || material_region_id == 0U ||
                                 same_assignment);
            const char* assignment_label =
                assignment != snapshot.material_assignments.end() &&
                    assignment->variant.has_value()
                ? "Replace variant with base material" : "Assign selected material";
            if (ImGui::Button(assignment_label, ImVec2(-1, 26))) {
                static_cast<void>(dispatch(
                    carto::application::SetMaterialRegionAssignmentAction{
                        snapshot.project_revision, key, selected_material_id, std::nullopt}));
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(assignment == snapshot.material_assignments.end());
            if (ImGui::Button("Clear region assignment", ImVec2(-1, 24))) {
                static_cast<void>(dispatch(
                    carto::application::SetMaterialRegionAssignmentAction{
                        snapshot.project_revision, key, std::nullopt, std::nullopt}));
            }
            ImGui::EndDisabled();
        }

        ImGui::Separator();
        ImGui::InputText("New material", material_name_buffer.data(),
                         material_name_buffer.size());
        ImGui::BeginDisabled(material_name_buffer.front() == '\0');
        if (ImGui::Button("Create PBR source", ImVec2(-1, 28))) {
            carto::render::StandardMaterial definition;
            definition.base_color_factor = {0.62, 0.66, 0.70, 1.0};
            if (dispatch(carto::application::CreateMaterialAction{
                    snapshot.project_revision, material_name_buffer.data(), definition})) {
                selected_material_id = 0U;
                material_draft_loaded = false;
                material_draft_dirty = false;
                material_name_buffer.fill('\0');
            }
        }
        ImGui::EndDisabled();
        if (!action_feedback.empty()) draw_action_feedback(snapshot, true);
        end_instrument(carto::ui::WorkbenchInstrument::material, workbench.material);
    }

    void draw_tools_instrument(const UiSnapshot& snapshot) {
        const bool sculpt_workspace =
            snapshot.workspace_kind == carto::ui::Workspace::sculpt;
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::tools,
                              sculpt_workspace ? "Sculpt" : "Tools", workbench.tools,
                              ImVec2(118, 390), ImVec2(318, 330),
                              sculpt_workspace ? snapshot.colors.accent_secondary
                                               : snapshot.colors.accent_primary)) return;
        if (sculpt_workspace) {
            ImGui::TextDisabled("Project-bound SculptStack authoring");
            ImGui::TextWrapped(
                "Drag across the selected mesh surface to paint a surface-anchored stroke. "
                "The stroke commits through the project command path on mouse-up.");
            ImGui::Text("Project revision %llu",
                static_cast<unsigned long long>(snapshot.project_revision.value()));
            ImGui::Separator();
            if (snapshot.selection.mode != carto::editor::SelectionMode::vertex) {
                if (ImGui::Button("Switch to vertex selection", ImVec2(-1, 30))) {
                    static_cast<void>(dispatch(carto::application::SetSelectionModeAction{
                        carto::editor::SelectionMode::vertex}));
                }
            }
            static constexpr const char* sculpt_modes[]{
                "Add", "Subtract", "Inflate", "Clay", "Grab", "Smooth",
                "Pinch", "Flatten", "Scrape", "Polish", "Crease", "Surface noise"};
            constexpr int sculpt_mode_count =
                static_cast<int>(sizeof(sculpt_modes) / sizeof(sculpt_modes[0]));
            sculpt_mode_index = std::clamp(sculpt_mode_index, 0, sculpt_mode_count - 1);
            ImGui::Combo("Brush mode", &sculpt_mode_index, sculpt_modes, sculpt_mode_count);
            ImGui::InputDouble("Radius", &sculpt_radius, 0.01, 0.1, "%.4f");
            ImGui::InputDouble("Strength", &sculpt_strength, 0.01, 0.1, "%.4f");
            const bool one_vertex =
                snapshot.selection.mode == carto::editor::SelectionMode::vertex &&
                snapshot.selection.vertices.size() == 1U &&
                snapshot.selection.component_object.has_value();
            ImGui::BeginDisabled(!one_vertex);
            if (ImGui::Button("Add stroke at selected vertex", ImVec2(-1, 34))) {
                const bool accepted = dispatch(carto::application::AppendSculptStrokeAction{
                    snapshot.project_revision,
                    static_cast<carto::production::SculptStrokeMode>(sculpt_mode_index),
                    sculpt_radius,
                    sculpt_strength});
                if (accepted) {
                    action_feedback = "Sculpt stroke recorded in the project-bound stack.";
                    action_feedback_error = false;
                }
            }
            ImGui::EndDisabled();
            if (!one_vertex) {
                ImGui::TextDisabled(
                    "Point strokes need one selected vertex; surface drags use the selected mesh object.");
            }
            ImGui::Separator();
            ImGui::Text("Sculpt layer stack");
            if (!snapshot.sculpt_mesh_asset.has_value()) {
                ImGui::TextDisabled("Select a mesh component to inspect its stack.");
            } else if (snapshot.sculpt_layers.empty()) {
                ImGui::TextDisabled("No authored layers. The first stroke creates one.");
            } else {
                ImGui::TextDisabled(
                    "Visibility is persisted and used by CPU evaluation; viewport comparison is pending.");
                for (const auto& layer : snapshot.sculpt_layers) {
                    ImGui::PushID(layer.identity.c_str());
                    bool visible = layer.visible;
                    if (ImGui::Checkbox(layer.name.c_str(), &visible)) {
                        static_cast<void>(dispatch(
                            carto::application::SetSculptLayerVisibilityAction{
                                snapshot.project_revision,
                                *snapshot.sculpt_mesh_asset,
                                layer.identity,
                                visible}));
                    }
                    ImGui::TextDisabled(
                        "weight %.2f · %zu stroke%s",
                        layer.weight,
                        layer.stroke_count,
                        layer.stroke_count == 1U ? "" : "s");
                    ImGui::PopID();
                }
            }
            if (!action_feedback.empty()) {
                ImGui::Separator();
                draw_action_feedback(snapshot);
            }
            if (!snapshot.operations.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Latest session receipt: %s · rev %llu",
                    snapshot.operations.back().action.c_str(),
                    static_cast<unsigned long long>(
                        snapshot.operations.back().revision_after.value()));
            }
            end_instrument(carto::ui::WorkbenchInstrument::tools, workbench.tools);
            return;
        }
        ImGui::TextDisabled("Authoring instruments");
        if (ImGui::Button("New Box", ImVec2(124, 30))) {
            static_cast<void>(dispatch(carto::application::CreateBoxAction{"Box", {2.0, 2.0, 2.0}}));
        }
        ImGui::SameLine();
        if (ImGui::Button("New Plane", ImVec2(124, 30))) {
            static_cast<void>(dispatch(carto::application::CreatePlaneAction{"Plane", 2.0, 2.0}));
        }
        if (ImGui::Button("Transform Instrument", ImVec2(-1, 30))) workbench.transform = true;
        const bool face_available = snapshot.selection.mode == carto::editor::SelectionMode::face &&
            !snapshot.selection.faces.empty();
        const bool edge_available = snapshot.selection.mode == carto::editor::SelectionMode::edge &&
            !snapshot.selection.edges.empty();
        if (face_available) {
            if (ImGui::Button("Extrude Instrument", ImVec2(-1, 30))) workbench.extrude = true;
            if (ImGui::Button("Inset Selected Face", ImVec2(-1, 30))) {
                carto::editor::ToolArguments arguments;
                arguments.distance = inset_distance;
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.inset-face", arguments}));
            }
            if (ImGui::Button("Delete Selected Face", ImVec2(-1, 30))) {
                carto::editor::ToolArguments arguments;
                arguments.remove_orphaned_vertices = true;
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.remove-face", arguments}));
            }
        } else if (edge_available) {
            if (ImGui::Button("Split Selected Edge", ImVec2(-1, 30))) {
                carto::editor::ToolArguments arguments;
                arguments.factor = edge_split_factor;
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.split-edge", arguments}));
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("Extrude requires a selected face", ImVec2(-1, 30));
            ImGui::EndDisabled();
        }
        if (ImGui::Button("Drafting Board", ImVec2(-1, 30))) workbench.draft = true;
        if (ImGui::Button("Fidelity Instrument", ImVec2(-1, 30))) workbench.measure = true;
        ImGui::Separator();
        if (ImGui::Button("Undo", ImVec2(124, 30))) static_cast<void>(dispatch(carto::application::UndoAction{}));
        ImGui::SameLine();
        if (ImGui::Button("Redo", ImVec2(124, 30))) static_cast<void>(dispatch(carto::application::RedoAction{}));
        end_instrument(carto::ui::WorkbenchInstrument::tools, workbench.tools);
    }

    void draw_transform_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::transform,
                              "Transform", workbench.transform, ImVec2(420, 50),
                              ImVec2(270, 210), snapshot.colors.accent_secondary)) return;
        const auto selected_id = snapshot.selection.component_object.value_or(
            snapshot.selection.objects.empty() ? carto::scene::ObjectId{} : snapshot.selection.objects.front());
        const auto object = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
                                         [selected_id](const auto& value) { return value.object.id == selected_id; });
        if (object == snapshot.objects.end()) {
            ImGui::TextDisabled("Select an object to expose transform controls.");
            end_instrument(carto::ui::WorkbenchInstrument::transform, workbench.transform);
            return;
        }
        if (inspector_object != selected_id) {
            inspector_object = selected_id;
            inspector_transform = object->object.local_transform;
        }
        ImGui::Text("%s", object->object.name.c_str());
        ImGui::InputDouble("X", &inspector_transform.translation.x);
        ImGui::InputDouble("Y", &inspector_transform.translation.y);
        ImGui::InputDouble("Z", &inspector_transform.translation.z);
        if (ImGui::Button("Apply", ImVec2(112, 30))) {
            static_cast<void>(dispatch(SetObjectTransformAction{
                selected_id, inspector_transform, snapshot.project_revision}));
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(112, 30))) {
            inspector_transform = object->object.local_transform;
        }
        end_instrument(carto::ui::WorkbenchInstrument::transform, workbench.transform);
    }

    void draw_extrude_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::extrude,
                              "Extrude", workbench.extrude, ImVec2(370, 280),
                              ImVec2(250, 190), snapshot.colors.semantic_warning)) return;
        if (snapshot.selection.mode != carto::editor::SelectionMode::face ||
            snapshot.selection.faces.empty()) {
            ImGui::TextDisabled("Select one face to arm this instrument.");
            end_instrument(carto::ui::WorkbenchInstrument::extrude, workbench.extrude);
            return;
        }
        ImGui::Text("Face %llu", static_cast<unsigned long long>(snapshot.selection.faces.front().value));
        ImGui::InputDouble("Distance", &extrude_distance);
        ImGui::TextDisabled("Direction  Normal");
        ImGui::TextDisabled("Segments   1");
        if (ImGui::Button("Cancel", ImVec2(108, 30))) workbench.extrude = false;
        ImGui::SameLine();
        if (ImGui::Button("Apply", ImVec2(108, 30))) {
            carto::editor::ToolArguments arguments;
            arguments.distance = extrude_distance;
            if (dispatch(carto::application::InvokeToolAction{"mesh.extrude-face", arguments})) {
                workbench.extrude = false;
            }
        }
        end_instrument(carto::ui::WorkbenchInstrument::extrude, workbench.extrude);
    }

    void draw_modify_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::modify,
                              "Modify", workbench.modify, ImVec2(-8, 485),
                              ImVec2(310, 390), snapshot.colors.semantic_warning)) return;
        ImGui::TextDisabled("Revision-bound native mesh tools");
        ImGui::Separator();

        if (snapshot.selection.mode == carto::editor::SelectionMode::object) {
            ImGui::Text("Object selection · %zu", snapshot.selection.objects.size());
            const bool available = snapshot.selection.objects.size() == 1U;
            ImGui::BeginDisabled(!available);
            if (ImGui::Button("Duplicate instance", ImVec2(-1, 30))) {
                duplicate_selected_object(snapshot);
            }
            ImGui::EndDisabled();
            if (!available) ImGui::TextDisabled("Select exactly one object.");
        } else if (snapshot.selection.mode == carto::editor::SelectionMode::face) {
            const std::size_t count = snapshot.selection.faces.size();
            ImGui::Text("Face selection · %zu", count);
            const bool one_face = count == 1U;
            ImGui::BeginDisabled(!one_face);
            if (ImGui::Button("Open Extrude", ImVec2(-1, 30))) workbench.extrude = true;
            ImGui::InputDouble("Inset distance", &inset_distance, 0.01, 0.1, "%.4f");
            if (ImGui::Button("Inset face", ImVec2(-1, 30))) {
                carto::editor::ToolArguments arguments;
                arguments.distance = inset_distance;
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.inset-face", arguments}));
            }
            if (ImGui::Button("Poke face", ImVec2(-1, 30))) {
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.poke-face", {}}));
            }
            ImGui::Checkbox("Remove orphaned vertices", &remove_orphaned_vertices);
            if (ImGui::Button("Delete face", ImVec2(-1, 30))) {
                carto::editor::ToolArguments arguments;
                arguments.remove_orphaned_vertices = remove_orphaned_vertices;
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.remove-face", arguments}));
            }
            ImGui::EndDisabled();
            const bool two_faces = count == 2U;
            ImGui::BeginDisabled(!two_faces);
            if (ImGui::Button("Triangles to quad", ImVec2(-1, 30))) {
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.tri-to-quad", {}}));
            }
            ImGui::EndDisabled();
            if (!one_face && !two_faces) {
                ImGui::TextDisabled("Select one face, or two adjacent triangles.");
            }
        } else if (snapshot.selection.mode == carto::editor::SelectionMode::edge) {
            const std::size_t count = snapshot.selection.edges.size();
            ImGui::Text("Edge selection · %zu", count);
            const bool available = count == 1U;
            ImGui::BeginDisabled(!available);
            edge_split_factor = std::clamp(edge_split_factor, 0.001, 0.999);
            ImGui::InputDouble("Split factor", &edge_split_factor, 0.05, 0.1, "%.3f");
            if (ImGui::Button("Split edge", ImVec2(-1, 30))) {
                carto::editor::ToolArguments arguments;
                arguments.factor = edge_split_factor;
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.split-edge", arguments}));
            }
            bevel_width = std::max(bevel_width, 0.0001);
            bevel_segments = std::max(bevel_segments, 1);
            ImGui::InputDouble("Bevel width", &bevel_width, 0.01, 0.1, "%.4f");
            ImGui::InputInt("Segments", &bevel_segments);
            if (ImGui::Button("Bevel edge", ImVec2(-1, 30))) {
                carto::editor::ToolArguments arguments;
                arguments.distance = bevel_width;
                arguments.segments = static_cast<std::uint32_t>(
                    std::clamp(bevel_segments, 1, 64));
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.bevel-edge", arguments}));
            }
            if (ImGui::Button("Dissolve edge", ImVec2(-1, 30))) {
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.dissolve-edge", {}}));
            }
            ImGui::EndDisabled();
            if (!available) ImGui::TextDisabled("Select exactly one edge.");
        } else {
            const std::size_t count = snapshot.selection.vertices.size();
            ImGui::Text("Vertex selection · %zu", count);
            const bool one_vertex = count == 1U;
            ImGui::BeginDisabled(!one_vertex);
            vertex_slide_factor = std::clamp(vertex_slide_factor, 0.001, 0.999);
            ImGui::InputDouble("Slide factor", &vertex_slide_factor, 0.05, 0.1, "%.3f");
            if (ImGui::Button("Slide vertex", ImVec2(-1, 30))) {
                carto::editor::ToolArguments arguments;
                arguments.factor = vertex_slide_factor;
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.slide-vertex", arguments}));
            }
            bevel_width = std::max(bevel_width, 0.0001);
            bevel_segments = std::max(bevel_segments, 1);
            ImGui::InputDouble("Bevel width", &bevel_width, 0.01, 0.1, "%.4f");
            ImGui::InputInt("Segments", &bevel_segments);
            if (ImGui::Button("Bevel vertex", ImVec2(-1, 30))) {
                carto::editor::ToolArguments arguments;
                arguments.distance = bevel_width;
                arguments.segments = static_cast<std::uint32_t>(
                    std::clamp(bevel_segments, 1, 64));
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.bevel-vertex", arguments}));
            }
            ImGui::EndDisabled();
            const bool two_vertices = count == 2U;
            ImGui::BeginDisabled(!two_vertices);
            if (ImGui::Button("Merge vertices", ImVec2(-1, 30))) {
                static_cast<void>(dispatch(carto::application::InvokeToolAction{
                    "mesh.merge-vertices", {}}));
            }
            ImGui::EndDisabled();
            if (!one_vertex && !two_vertices) {
                ImGui::TextDisabled("Select one vertex, or two vertices to merge.");
            }
        }

        if (!action_feedback.empty()) {
            ImGui::Separator();
            draw_action_feedback(snapshot);
        }
        end_instrument(carto::ui::WorkbenchInstrument::modify, workbench.modify);
    }

    void draw_surface_fidelity_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::measure,
                              "Surface Fidelity", workbench.measure, ImVec2(520, 110),
                              ImVec2(340, 430), snapshot.colors.accent_secondary)) return;

        ImGui::TextDisabled("Revision-bound, read-only compiler preview");
        ImGui::TextWrapped(
            "Compile a representation for the selected object. The result is evidence only; "
            "authoring and renderer state remain unchanged.");
        ImGui::Spacing();

        surface_target_triangles = std::clamp(
            surface_target_triangles, 1,
            static_cast<int>(carto::ui::kMaxSurfacePreviewTriangles));
        ImGui::InputInt("Target triangles", &surface_target_triangles);
        surface_target_triangles = std::clamp(
            surface_target_triangles, 1,
            static_cast<int>(carto::ui::kMaxSurfacePreviewTriangles));
        ImGui::TextDisabled("CPU preview budget: %u source triangles",
                            carto::ui::kMaxSurfacePreviewTriangles);
        int mode = surface_mode == carto::surface::RepresentationMode::fidelity_closure ? 1 : 0;
        if (ImGui::RadioButton("Fidelity closure", mode == 1)) mode = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("Geometry only", mode == 0)) mode = 0;
        surface_mode = mode == 1
            ? carto::surface::RepresentationMode::fidelity_closure
            : carto::surface::RepresentationMode::geometry_only;

        bool refresh_fidelity = false;
        if (ImGui::Button("Compile preview", ImVec2(160, 30))) {
            static_cast<void>(ui.compile_surface_preview(
                static_cast<std::uint32_t>(surface_target_triangles), surface_mode));
            refresh_fidelity = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(80, 30))) {
            static_cast<void>(ui.clear_surface_preview());
            refresh_fidelity = true;
        }

        auto fidelity = snapshot.surface_fidelity;
        if (refresh_fidelity) fidelity = ui.snapshot().surface_fidelity;
        ImGui::Separator();
        const auto status_color_for = [&snapshot](carto::ui::SurfaceFidelityStatus status) {
            switch (status) {
            case carto::ui::SurfaceFidelityStatus::ready:
                return color(snapshot.colors.semantic_success);
            case carto::ui::SurfaceFidelityStatus::rejected:
                return color(snapshot.colors.semantic_warning);
            case carto::ui::SurfaceFidelityStatus::stale:
                return color(snapshot.colors.stale);
            case carto::ui::SurfaceFidelityStatus::unavailable:
                return color(snapshot.colors.semantic_error);
            case carto::ui::SurfaceFidelityStatus::idle:
                return color(snapshot.colors.text_secondary);
            }
            return color(snapshot.colors.text_secondary);
        };
        ImGui::TextColored(status_color_for(fidelity.status), "%s · %s",
                           carto::ui::surface_fidelity_status_name(fidelity.status),
                           carto::ui::surface_fidelity_mode_name(fidelity.mode));
        if (!fidelity.status_text.empty()) {
            ImGui::TextWrapped("%s", fidelity.status_text.c_str());
        }
        if (fidelity.object.has_value()) {
            ImGui::Text("Object %llu  ·  rev %llu",
                        static_cast<unsigned long long>(fidelity.object->value),
                        static_cast<unsigned long long>(fidelity.source_revision.value()));
            ImGui::Text("Triangles  %u source  ->  %u target",
                        fidelity.source_triangles, fidelity.target_triangles);
            ImGui::Text("RMS radiance  %.6f", fidelity.error.rms_radiance_error);
            ImGui::Text("Max radiance  %.6f", fidelity.error.max_radiance_error);
            ImGui::Text("Specular  %.6f  ·  Visibility  %.6f",
                        fidelity.error.specular_error, fidelity.error.visibility_error);
            ImGui::Text("Silhouette  %.6f  ·  Energy  %.6f",
                        fidelity.error.max_silhouette_error, fidelity.error.energy_error);
            ImGui::TextColored(
                fidelity.admitted ? color(snapshot.colors.semantic_success)
                                  : color(snapshot.colors.semantic_warning),
                fidelity.admitted ? "Appearance policy: admitted"
                                  : "Appearance policy: not admitted");
        }
        if (fidelity.diagnostic.has_value()) {
            ImGui::TextColored(color(snapshot.colors.semantic_warning), "%s",
                               fidelity.diagnostic->message.c_str());
        }
        end_instrument(carto::ui::WorkbenchInstrument::measure, workbench.measure);
    }

    void draw_draft_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::draft,
                              "Agent Workbench", workbench.draft, ImVec2(360, 430),
                              ImVec2(500, 680), snapshot.colors.provisional)) return;
        ImGui::TextDisabled("AI study · non-authoritative");
        std::array<char, 4097> intent{};
        const std::size_t copy_bytes = std::min(ai_intent.size(), intent.size() - 1U);
        std::copy_n(ai_intent.data(), copy_bytes, intent.data());
        if (ImGui::InputTextMultiline("##draft-intent", intent.data(), intent.size(), ImVec2(-1, 90))) {
            ai_intent.assign(intent.data());
        }
        ImGui::TextDisabled("Constraints");
        ImGui::BulletText("Existing geometry remains authoritative");
        if (snapshot.ai_auto_approve) {
            ImGui::BulletText("Safe native studies auto-apply");
        } else {
            ImGui::BulletText("Proposals require explicit review");
        }
        if (!ai_preview.has_value()) {
            ImGui::BeginDisabled(!snapshot.ai_available || ai_intent.empty());
            const char* generate_label = snapshot.ai_auto_approve
                ? "Generate & Apply Study" : "Generate Study";
            if (ImGui::Button(generate_label, ImVec2(-1, 30))) {
                if (snapshot.ai_auto_approve) {
                    const auto committed = ui.apply_ai_intent(ai_intent);
                    if (committed) {
                        ai_feedback = "Auto-approved and applied through the native command boundary.";
                    } else {
                        ai_feedback = committed.error().message;
                    }
                } else {
                    const auto proposal = ui.propose_ai(ai_intent);
                    if (!proposal) {
                        ai_feedback = proposal.error().message;
                        ai_proposal.reset();
                        ai_preview.reset();
                    } else {
                        const auto preview = ui.begin_ai_preview(proposal.value());
                        if (!preview) {
                            ai_feedback = preview.error().message;
                            ai_proposal.reset();
                            ai_preview.reset();
                        } else {
                            ai_proposal = proposal.value();
                            ai_preview = std::move(preview.value());
                            ai_feedback = "Preview ready; the scene is unchanged.";
                        }
                    }
                }
            }
            ImGui::EndDisabled();
        } else {
            ImGui::Text("Proposal: %s", ai_proposal->tool_id.c_str());
            ImGui::TextWrapped("%s", ai_proposal->rationale.c_str());
            if (ImGui::Button("Apply Approved Study", ImVec2(-1, 30))) {
                const auto committed = ui.commit_ai_preview(*ai_preview, *ai_proposal);
                if (committed) {
                    ai_feedback = "Applied through the native command boundary.";
                    ai_proposal.reset();
                    ai_preview.reset();
                } else {
                    ai_feedback = committed.error().message;
                }
            }
            if (ImGui::Button("Discard Study", ImVec2(-1, 30))) {
                ai_proposal.reset();
                ai_preview.reset();
                ai_feedback = "Proposal discarded; the scene is unchanged.";
            }
        }
        if (!ai_feedback.empty()) {
            ImGui::TextWrapped("%s", ai_feedback.c_str());
        }

        ImGui::Separator();
        heading_text("Speculative task");
        ImGui::TextDisabled(
            "Queue native steps from one object, face, or vertex. The source stays unchanged until acceptance.");
        std::array<char, 1025> goal{};
        const std::size_t goal_bytes = std::min(agent_goal.size(), goal.size() - 1U);
        std::copy_n(agent_goal.data(), goal_bytes, goal.data());
        if (agent_goal_focus_requested) {
            ImGui::SetKeyboardFocusHere();
            agent_goal_focus_requested = false;
        }
        if (ImGui::InputText("Goal", goal.data(), goal.size())) {
            agent_goal.assign(goal.data());
        }
        const auto compiled_goal_constraints =
            carto::agent::compile_explicit_hard_constraints(agent_goal);
        const bool objective_constraints_valid =
            agent_goal.empty() || static_cast<bool>(compiled_goal_constraints);
        const bool goal_preserves_topology = compiled_goal_constraints && std::any_of(
            compiled_goal_constraints.value().begin(), compiled_goal_constraints.value().end(),
            [](const auto& constraint) {
                return constraint.kind ==
                    carto::agent::ObjectiveConstraintKind::preserve_topology;
            });
        if (!agent_goal.empty() && !compiled_goal_constraints) {
            ImGui::TextColored(color(snapshot.colors.semantic_warning),
                "Goal restriction is not enforceable: %s",
                compiled_goal_constraints.error().message.c_str());
        } else if (goal_preserves_topology) {
            ImGui::TextDisabled(
                "Goal-derived hard limit: topology must remain unchanged.");
        }
        const auto& agent_recipes = carto::agent::agent_recipe_catalog();
        std::vector<std::string> recipe_labels{"No recipe"};
        recipe_labels.reserve(agent_recipes.size() + 1U);
        for (const auto& recipe : agent_recipes) {
            recipe_labels.emplace_back(recipe.label);
        }
        std::vector<const char*> recipe_items;
        recipe_items.reserve(recipe_labels.size());
        for (const auto& label : recipe_labels) recipe_items.push_back(label.c_str());
        int selected_recipe_index = 0;
        for (std::size_t index = 0U; index < agent_recipes.size(); ++index) {
            if (agent_recipes[index].id == snapshot.agent_recipe_id) {
                selected_recipe_index = static_cast<int>(index + 1U);
                break;
            }
        }
        const bool recipe_task_active = snapshot.agent_task.has_value() &&
            snapshot.agent_task->status != carto::agent::TaskStatus::accepted &&
            snapshot.agent_task->status != carto::agent::TaskStatus::rejected;
        ImGui::BeginDisabled(recipe_task_active || snapshot.agent_checkpoint_loaded);
        const bool recipe_changed = ImGui::Combo(
            "Recipe profile", &selected_recipe_index,
            recipe_items.data(), static_cast<int>(recipe_items.size()));
        ImGui::EndDisabled();
        if (recipe_changed && selected_recipe_index >= 0 &&
            selected_recipe_index < static_cast<int>(recipe_items.size())) {
            std::string recipe_id;
            if (selected_recipe_index > 0) {
                recipe_id = agent_recipes[
                    static_cast<std::size_t>(selected_recipe_index - 1)].id;
            }
            const auto selected = ui.set_agent_recipe(std::move(recipe_id));
            agent_feedback = selected
                ? "Recipe contract selected; it constrains review gates and does not add plan steps."
                : selected.error().message;
        }
        if (const auto* selected_recipe =
                carto::agent::find_agent_recipe(snapshot.agent_recipe_id)) {
            ImGui::TextWrapped("%.*s",
                static_cast<int>(selected_recipe->description.size()),
                selected_recipe->description.data());
        }
        std::array<char, 1025> step{};
        const std::size_t step_bytes = std::min(agent_step.size(), step.size() - 1U);
        std::copy_n(agent_step.data(), step_bytes, step.data());
        if (ImGui::InputText("Native step", step.data(), step.size())) {
            agent_step.assign(step.data());
        }
        std::vector<const char*> dependency_labels{"Independent"};
        dependency_labels.reserve(snapshot.agent_plan_draft.size() + 1U);
        for (const auto& planned : snapshot.agent_plan_draft) {
            dependency_labels.push_back(planned.id.c_str());
        }
        if (agent_step_dependency < 0 ||
            agent_step_dependency >= static_cast<int>(dependency_labels.size())) {
            agent_step_dependency = 0;
        }
        ImGui::Combo("Depends on", &agent_step_dependency,
            dependency_labels.data(), static_cast<int>(dependency_labels.size()));
        const bool selected_target =
            (snapshot.selection.mode == carto::editor::SelectionMode::object &&
                snapshot.selection.objects.size() == 1U) ||
            (snapshot.selection.mode == carto::editor::SelectionMode::face &&
                snapshot.selection.component_object.has_value() &&
                snapshot.selection.faces.size() == 1U) ||
            (snapshot.selection.mode == carto::editor::SelectionMode::vertex &&
                snapshot.selection.component_object.has_value() &&
                snapshot.selection.vertices.size() == 1U);
        if (snapshot.selection.mode == carto::editor::SelectionMode::object &&
            snapshot.selection.objects.size() == 1U) {
            ImGui::TextDisabled("Target: object %llu",
                static_cast<unsigned long long>(snapshot.selection.objects.front().value));
        } else if (snapshot.selection.mode == carto::editor::SelectionMode::face &&
            snapshot.selection.component_object.has_value() &&
            snapshot.selection.faces.size() == 1U) {
            ImGui::TextDisabled("Target: object %llu · face %llu",
                static_cast<unsigned long long>(snapshot.selection.component_object->value),
                static_cast<unsigned long long>(snapshot.selection.faces.front().value));
        } else if (snapshot.selection.mode == carto::editor::SelectionMode::vertex &&
            snapshot.selection.component_object.has_value() &&
            snapshot.selection.vertices.size() == 1U) {
            ImGui::TextDisabled("Target: object %llu · vertex %llu",
                static_cast<unsigned long long>(snapshot.selection.component_object->value),
                static_cast<unsigned long long>(snapshot.selection.vertices.front().value));
        } else {
            ImGui::TextDisabled("Select exactly one object, face, or vertex to capture a plan step.");
        }
        const bool unresolved_task = snapshot.agent_task.has_value() &&
            snapshot.agent_task->status != carto::agent::TaskStatus::accepted &&
            snapshot.agent_task->status != carto::agent::TaskStatus::rejected;
        ImGui::BeginDisabled(!selected_target || agent_step.empty() || unresolved_task);
        if (ImGui::Button("Queue selected target", ImVec2(-1, 28))) {
            std::vector<std::string> dependencies;
            if (agent_step_dependency > 0) {
                dependencies.push_back(snapshot.agent_plan_draft[
                    static_cast<std::size_t>(agent_step_dependency - 1)].id);
            }
            const auto queued = ui.queue_agent_step(agent_step, std::move(dependencies));
            if (queued) {
                agent_feedback = "Step captured against revision " +
                    std::to_string(snapshot.project_revision.value()) + ".";
                agent_step.clear();
                agent_step_dependency = static_cast<int>(snapshot.agent_plan_draft.size());
            } else {
                agent_feedback = queued.error().message;
            }
        }
        ImGui::EndDisabled();
        if (!snapshot.agent_plan_draft.empty()) {
            ImGui::Text("Plan · %llu step(s)", static_cast<unsigned long long>(
                snapshot.agent_plan_draft.size()));
            for (const auto& planned : snapshot.agent_plan_draft) {
                const std::string dependency_text = planned.dependencies.empty()
                    ? "independent"
                    : "after " + planned.dependencies.front();
                ImGui::BulletText("%s · %s · object %llu · %s", planned.id.c_str(),
                    dependency_text.c_str(),
                    static_cast<unsigned long long>(planned.target.object.value),
                    planned.intent.c_str());
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Operator clarification · blocks the plan until answered");
        std::array<char, 1025> question_input{};
        const std::size_t question_bytes = std::min(
            agent_question.size(), question_input.size() - 1U);
        std::copy_n(agent_question.data(), question_bytes, question_input.data());
        if (ImGui::InputText("Decision question", question_input.data(), question_input.size())) {
            agent_question.assign(question_input.data());
        }
        constexpr std::array<const char*, 9> clarification_impact_names{
            "Object scope", "Topology deletion", "Familiarity-critical feature",
            "Collision or navigation", "Interaction anchor", "Source hierarchy",
            "Large spatial transform", "Imported source integrity", "Publication readiness"};
        constexpr std::array<carto::agent::ClarificationImpact, 9> clarification_impacts{
            carto::agent::ClarificationImpact::object_scope,
            carto::agent::ClarificationImpact::topology_deletion,
            carto::agent::ClarificationImpact::familiarity_critical_feature,
            carto::agent::ClarificationImpact::collision_or_navigation,
            carto::agent::ClarificationImpact::interaction_anchor,
            carto::agent::ClarificationImpact::source_hierarchy,
            carto::agent::ClarificationImpact::large_spatial_transform,
            carto::agent::ClarificationImpact::imported_source_integrity,
            carto::agent::ClarificationImpact::publication_readiness};
        if (agent_clarification_impact < 0 ||
            agent_clarification_impact >= static_cast<int>(clarification_impacts.size())) {
            agent_clarification_impact = 0;
        }
        ImGui::Combo("Consequence", &agent_clarification_impact,
            clarification_impact_names.data(),
            static_cast<int>(clarification_impact_names.size()));
        ImGui::BeginDisabled(agent_question.empty() || snapshot.agent_plan_draft.empty() ||
            snapshot.agent_task.has_value() || snapshot.agent_checkpoint_loaded);
        if (ImGui::Button("Require an answer", ImVec2(-1, 26))) {
            const auto queued = ui.queue_agent_clarification(
                agent_question, clarification_impacts[static_cast<std::size_t>(
                    agent_clarification_impact)]);
            agent_feedback = queued
                ? "Question bound to the current target scope; answer it before running."
                : queued.error().message;
            if (queued) agent_question.clear();
        }
        ImGui::EndDisabled();
        bool clarifications_answered = true;
        for (const auto& question : snapshot.agent_clarifications_draft) {
            ImGui::PushID(question.id.c_str());
            ImGui::TextWrapped("Question · %s", question.prompt.c_str());
            if (question.freeform_answer.has_value()) {
                ImGui::TextWrapped("Operator answer · %s", question.freeform_answer->c_str());
            } else if (question.selected_candidate_id.has_value()) {
                ImGui::Text("Operator choice · %s", question.selected_candidate_id->c_str());
            } else {
                clarifications_answered = false;
                std::array<char, 1025> answer_input{};
                const std::size_t answer_bytes = std::min(
                    agent_answer.size(), answer_input.size() - 1U);
                std::copy_n(agent_answer.data(), answer_bytes, answer_input.data());
                if (ImGui::InputText("Answer", answer_input.data(), answer_input.size())) {
                    agent_answer.assign(answer_input.data());
                }
                ImGui::BeginDisabled(agent_answer.empty() || agent_goal.empty());
                if (ImGui::Button("Confirm operator answer", ImVec2(-1, 26))) {
                    const auto answered = ui.answer_agent_clarification(
                        agent_goal, question.id, agent_answer);
                    agent_feedback = answered
                        ? "Answer recorded against this exact objective and target revision."
                        : answered.error().message;
                    if (answered) agent_answer.clear();
                }
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        const bool task_present = snapshot.agent_task.has_value();
        const bool task_active = task_present &&
            snapshot.agent_task->status != carto::agent::TaskStatus::accepted &&
            snapshot.agent_task->status != carto::agent::TaskStatus::rejected;
        ImGui::Separator();
        ImGui::TextDisabled("Objective constraints · hard limits on every plan interpretation");
        const auto has_constraint = [&snapshot](
            carto::agent::ObjectiveConstraintKind kind) {
            return std::any_of(snapshot.agent_hard_constraints_draft.begin(),
                snapshot.agent_hard_constraints_draft.end(), [kind](const auto& constraint) {
                    return constraint.kind == kind;
                });
        };
        bool preserve_topology = goal_preserves_topology || has_constraint(
            carto::agent::ObjectiveConstraintKind::preserve_topology);
        bool preserve_transform = has_constraint(
            carto::agent::ObjectiveConstraintKind::preserve_transform);
        bool limit_vertex_displacement = has_constraint(
            carto::agent::ObjectiveConstraintKind::maximum_vertex_displacement);
        double maximum_vertex_displacement = 0.25;
        const auto displacement_constraint = std::find_if(
            snapshot.agent_hard_constraints_draft.begin(),
            snapshot.agent_hard_constraints_draft.end(), [](const auto& constraint) {
                return constraint.kind ==
                    carto::agent::ObjectiveConstraintKind::maximum_vertex_displacement;
            });
        if (displacement_constraint != snapshot.agent_hard_constraints_draft.end()) {
            maximum_vertex_displacement = displacement_constraint->maximum_displacement;
        }
        const bool has_mesh_target = std::any_of(snapshot.agent_plan_draft.begin(),
            snapshot.agent_plan_draft.end(), [&snapshot](const auto& planned) {
                return std::any_of(snapshot.objects.begin(), snapshot.objects.end(),
                    [&planned](const auto& object) {
                        return object.object.id == planned.target.object &&
                            object.object.mesh_asset.has_value();
                    });
            });
        bool constraints_changed = false;
        ImGui::BeginDisabled(task_active || snapshot.agent_checkpoint_loaded ||
            snapshot.agent_plan_draft.empty());
        ImGui::BeginDisabled(goal_preserves_topology);
        constraints_changed |= ImGui::Checkbox(
            "Preserve topology", &preserve_topology);
        ImGui::EndDisabled();
        constraints_changed |= ImGui::Checkbox(
            "Preserve object transforms", &preserve_transform);
        ImGui::BeginDisabled(!has_mesh_target);
        constraints_changed |= ImGui::Checkbox(
            "Limit original vertex movement", &limit_vertex_displacement);
        ImGui::EndDisabled();
        if (limit_vertex_displacement) {
            constexpr double minimum_displacement = 0.001;
            constexpr double maximum_displacement = 1000.0;
            constraints_changed |= ImGui::DragScalar(
                "Maximum vertex displacement (m)", ImGuiDataType_Double,
                &maximum_vertex_displacement, 0.02F,
                &minimum_displacement, &maximum_displacement, "%.3f");
        }
        ImGui::EndDisabled();
        if (constraints_changed) {
            auto constraints = snapshot.agent_hard_constraints_draft;
            std::erase_if(constraints, [](const auto& constraint) {
                return constraint.id == "ui.preserve-topology" ||
                    constraint.id.starts_with("ui.preserve-transform.") ||
                    constraint.id.starts_with("ui.vertex-displacement.");
            });
            if (preserve_topology && !goal_preserves_topology) {
                constraints.push_back({
                    "ui.preserve-topology",
                    carto::agent::ObjectiveConstraintKind::preserve_topology,
                    std::nullopt, 0.0,
                    "Operator requires topology to remain unchanged."});
            }
            std::vector<carto::scene::ObjectId> constrained_objects;
            for (const auto& planned : snapshot.agent_plan_draft) {
                if (std::find(constrained_objects.begin(), constrained_objects.end(),
                        planned.target.object) == constrained_objects.end()) {
                    constrained_objects.push_back(planned.target.object);
                }
            }
            for (const auto object : constrained_objects) {
                if (preserve_transform) {
                    constraints.push_back({
                        "ui.preserve-transform." + std::to_string(object.value),
                        carto::agent::ObjectiveConstraintKind::preserve_transform,
                        object, 0.0,
                        "Operator requires this object's transform to remain unchanged."});
                }
                if (limit_vertex_displacement) {
                    const auto view = std::find_if(snapshot.objects.begin(),
                        snapshot.objects.end(), [object](const auto& candidate) {
                            return candidate.object.id == object &&
                                candidate.object.mesh_asset.has_value();
                        });
                    if (view != snapshot.objects.end()) {
                        constraints.push_back({
                            "ui.vertex-displacement." + std::to_string(object.value),
                            carto::agent::ObjectiveConstraintKind::maximum_vertex_displacement,
                            object, maximum_vertex_displacement,
                            "Operator bounds movement of this object's original vertices."});
                    }
                }
            }
            const auto updated = ui.set_agent_hard_constraints(std::move(constraints));
            agent_feedback = updated
                ? "Hard constraints are bound to the captured objective and scope."
                : updated.error().message;
        }
        if (!clarifications_answered) {
            ImGui::TextColored(color(snapshot.colors.semantic_warning),
                "Plan blocked until every required clarification has an operator answer.");
        }
        ImGui::Checkbox("Permit root-object translation", &agent_allow_root_translation);
        if (agent_allow_root_translation) {
            constexpr double minimum_translation_distance = 0.01;
            constexpr double maximum_translation_distance = 1000.0;
            ImGui::DragScalar("Maximum cumulative translation (m)", ImGuiDataType_Double,
                &agent_translation_distance_meters, 0.05F,
                &minimum_translation_distance, &maximum_translation_distance, "%.2f");
            ImGui::TextDisabled(
                "Applies to a root object and its full unlocked subtree within the object-touch budget. "
                "Native step: "
                "translate object by <dx> <dy> <dz>.");
        }
        ImGui::BeginDisabled(task_present || snapshot.agent_checkpoint_loaded ||
            snapshot.agent_plan_draft.empty() ||
            agent_goal.empty() || !clarifications_answered ||
            !objective_constraints_valid);
        if (ImGui::Button("Prepare task for preflight", ImVec2(-1, 30))) {
            std::optional<carto::agent::AgentWorkEnvelope> requested_envelope;
            if (agent_allow_root_translation) {
                requested_envelope.emplace();
                requested_envelope->permitted_effects |=
                    carto::agent::effect_mask(carto::agent::Effect::transform);
                requested_envelope->max_transform_distance_meters =
                    agent_translation_distance_meters;
            }
            const auto prepared = ui.prepare_agent_plan(
                agent_goal, std::move(requested_envelope));
            agent_feedback = prepared
                ? "Task prepared at the captured revision. Inspect scoped CONDITION data before running."
                : prepared.error().message;
        }
        ImGui::TextDisabled(
            "Preparation locks the objective and envelope. You can inspect read-only CONDITION facts "
            "before native commands run.");
        if (ImGui::Button("Run speculative plan", ImVec2(-1, 30))) {
            std::optional<carto::agent::AgentWorkEnvelope> requested_envelope;
            if (agent_allow_root_translation) {
                requested_envelope.emplace();
                requested_envelope->permitted_effects |=
                    carto::agent::effect_mask(carto::agent::Effect::transform);
                requested_envelope->max_transform_distance_meters =
                    agent_translation_distance_meters;
            }
            const auto run = ui.run_agent_plan(agent_goal, std::move(requested_envelope));
            if (run) {
                agent_feedback = run.value().status == carto::agent::TaskStatus::review
                    ? "Speculative result is ready for review; the source is unchanged."
                    : (run.value().diagnostic.has_value()
                        ? run.value().diagnostic->message
                        : "Plan is blocked by an acceptance gate.");
            } else {
                agent_feedback = run.error().message;
            }
        }
        ImGui::EndDisabled();
        if (snapshot.agent_checkpoint_loaded) {
            const bool checkpoint_source_advanced = snapshot.project_revision !=
                snapshot.agent_checkpoint_base_revision;
            const bool checkpoint_rebase_stale = snapshot.agent_checkpoint_rebased &&
                snapshot.project_revision != snapshot.agent_checkpoint_rebase_revision;
            ImGui::Text("Saved task %s · source R%llu", snapshot.agent_checkpoint_task_id.c_str(),
                static_cast<unsigned long long>(
                    snapshot.agent_checkpoint_base_revision.value()));
            ImGui::TextDisabled(
                "The saved speculative branch is re-created and checked before review. "
                "Any consequential clarification must be answered again.");
            if (checkpoint_source_advanced &&
                (!snapshot.agent_checkpoint_rebased || checkpoint_rebase_stale)) {
                ImGui::TextColored(color(snapshot.colors.semantic_warning),
                    "Source advanced. Analyze read/write conflicts before resuming.");
                if (ImGui::SmallButton(snapshot.agent_checkpoint_rebased
                        ? "Reanalyze source changes" : "Analyze and prepare safe rebase")) {
                    const auto rebase = ui.rebase_agent_checkpoint();
                    agent_feedback = rebase
                        ? "Read/write sets are disjoint. The plan will replay against the current source; "
                          "answer consequential questions again."
                        : rebase.error().message;
                }
            } else if (snapshot.agent_checkpoint_rebased) {
                ImGui::TextDisabled(
                    "Conflict-free rebase prepared for source R%llu; the complete native plan will replay.",
                    static_cast<unsigned long long>(
                        snapshot.agent_checkpoint_rebase_revision.value()));
            }
            ImGui::BeginDisabled(task_present || !clarifications_answered ||
                (checkpoint_source_advanced &&
                    (!snapshot.agent_checkpoint_rebased || checkpoint_rebase_stale)));
            if (ImGui::Button("Resume and revalidate saved task", ImVec2(-1, 30))) {
                const auto resumed = ui.resume_agent_checkpoint();
                agent_feedback = resumed
                    ? (resumed.value().status == carto::agent::TaskStatus::review
                        ? (snapshot.agent_checkpoint_rebased
                            ? "Saved task safely rebased and replayed; the source is unchanged."
                            : "Saved task replayed and validated; the source is unchanged.")
                        : (resumed.value().diagnostic.has_value()
                            ? resumed.value().diagnostic->message
                            : "Saved task replay is blocked."))
                    : resumed.error().message;
            }
            ImGui::EndDisabled();
        } else if (!task_present) {
            if (ImGui::SmallButton("Open saved task checkpoint...")) {
                const auto path = choose_file(false, FileDialogKind::agent_checkpoint);
                if (path.has_value()) {
                    const auto loaded = ui.load_agent_checkpoint(*path);
                    agent_feedback = loaded
                        ? "Checkpoint loaded. Review its objective and re-answer any required questions."
                        : loaded.error().message;
                }
            }
        }

        if (!task_present) {
            const auto& recent_trace = snapshot.native_operation_trace;
            ImGui::Separator();
            ImGui::Text("Teach by demonstration");
            ImGui::TextDisabled(
                "Capture a contiguous range of successful human native edits. Trace files are not executable.");
            if (recent_trace.empty()) {
                ImGui::TextDisabled("No recent native edits are available for trace capture.");
            } else {
                const int maximum_index = static_cast<int>(recent_trace.size() - 1U);
                agent_trace_start_index = std::clamp(agent_trace_start_index, 0, maximum_index);
                agent_trace_end_index = std::clamp(agent_trace_end_index, 0, maximum_index);
                if (ImGui::BeginChild("AgentNativeTraceHistory", ImVec2(0, 96), true)) {
                    const std::size_t first_visible = recent_trace.size() > 16U
                        ? recent_trace.size() - 16U : 0U;
                    for (std::size_t index = first_visible; index < recent_trace.size(); ++index) {
                        const auto& operation = recent_trace[index];
                        ImGui::BulletText("%llu · %s · revision %llu to %llu",
                            static_cast<unsigned long long>(operation.operation_id),
                            operation.tool_id.c_str(),
                            static_cast<unsigned long long>(operation.revision_before.value()),
                            static_cast<unsigned long long>(operation.revision_after.value()));
                    }
                }
                ImGui::EndChild();
                if (maximum_index > 0) {
                    ImGui::SliderInt("Range start", &agent_trace_start_index, 0, maximum_index);
                    ImGui::SliderInt("Range end", &agent_trace_end_index, 0, maximum_index);
                } else {
                    ImGui::TextDisabled("The range contains one recent operation.");
                }
                ImGui::BeginDisabled(agent_trace_start_index > agent_trace_end_index);
                if (ImGui::Button("Capture selected command range", ImVec2(-1, 28))) {
                    const auto captured = ui.capture_agent_demonstration_trace(
                        recent_trace[static_cast<std::size_t>(agent_trace_start_index)].operation_id,
                        recent_trace[static_cast<std::size_t>(agent_trace_end_index)].operation_id);
                    agent_feedback = captured
                        ? "Human command range captured as an alias-only offline trace."
                        : captured.error().message;
                }
                ImGui::EndDisabled();
            }
            if (ImGui::SmallButton("Open saved native trace...")) {
                const auto path = choose_file(false, FileDialogKind::agent_demonstration_trace);
                if (path.has_value()) {
                    const auto loaded = ui.load_agent_demonstration_trace(*path);
                    agent_feedback = loaded
                        ? "Alias-only trace opened. Bind targets explicitly before a speculative preview."
                        : loaded.error().message;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Open demonstration recipe...")) {
                const auto path = choose_file(false, FileDialogKind::agent_demonstration_recipe);
                if (path.has_value()) {
                    const auto loaded = ui.load_agent_demonstration_recipe(*path);
                    agent_feedback = loaded
                        ? "Named demonstration recipe opened read-only. Bind targets and review a speculative preview before acceptance."
                        : loaded.error().message;
                }
            }
            if (snapshot.agent_demonstration_trace.has_value()) {
                const auto& trace = *snapshot.agent_demonstration_trace;
                ImGui::Text("Captured range · %zu steps · revision span +%llu",
                    trace.steps.size(),
                    static_cast<unsigned long long>(trace.result_revision_span));
                if (!snapshot.agent_demonstration_recipe_name.empty()) {
                    ImGui::Text("Recipe: %s",
                        snapshot.agent_demonstration_recipe_name.c_str());
                }
                ImGui::TextDisabled(
                    "The file has aliases, typed deltas, and optional topology-derived signatures; it has no target bindings and runs only through speculative preview.");
                const auto visible_steps = std::min<std::size_t>(trace.steps.size(), 16U);
                for (std::size_t index = 0U; index < visible_steps; ++index) {
                    const auto& trace_step = trace.steps[index];
                    ImGui::BulletText("%zu. %s · target %u / component %u",
                        index + 1U, trace_step.tool_id.c_str(), trace_step.target_alias,
                        trace_step.component_alias);
                }
                ImGui::SeparatorText("Bind trace aliases to selected targets");
                ImGui::TextDisabled(
                    "Use explicit bindings for exact target choices, or select the completed exemplar object and apply its topology-only matches.");
                const bool has_structural_evidence = trace.steps.size() == 1U &&
                    trace.steps.front().structure_signature.has_value() &&
                    trace.steps.front().result_object_structure_signature.has_value();
                ImGui::BeginDisabled(!has_structural_evidence);
                if (ImGui::SmallButton("Apply to similar")) {
                    const auto applied = ui.apply_agent_demonstration_to_similar();
                    agent_feedback = applied
                        ? "Bound " + std::to_string(applied.value()) +
                            " unique structural-signature matches. Review the speculative diff and gates before acceptance."
                        : applied.error().message;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "One command only; ambiguous signature matches are skipped. Select the completed source object as the exemplar.");
                int target_set_count = static_cast<int>(
                    snapshot.agent_demonstration_target_set_count);
                ImGui::SetNextItemWidth(120.0F);
                if (ImGui::InputInt("Target sets for Apply to selection", &target_set_count)) {
                    if (target_set_count < 0) {
                        agent_feedback =
                            "demonstration target set count must be between 1 and 16";
                    } else {
                        const auto configured = ui.set_agent_demonstration_target_set_count(
                            static_cast<std::size_t>(target_set_count));
                        agent_feedback = configured
                            ? "Target set count changed; bind every row again."
                            : configured.error().message;
                    }
                }
                for (const auto& binding : snapshot.agent_demonstration_bindings) {
                    const char* target_kind =
                        binding.target_kind == carto::agent::AgentDemonstrationTargetKind::object
                            ? "object" :
                        binding.target_kind == carto::agent::AgentDemonstrationTargetKind::face
                            ? "face" : "vertex";
                    ImGui::PushID(static_cast<int>(binding.target_set_index));
                    ImGui::PushID(static_cast<int>(binding.target_alias));
                    ImGui::PushID(static_cast<int>(binding.component_alias));
                    ImGui::Text("Set %zu · object %u / component %u · %s · %s",
                        binding.target_set_index + 1U, binding.target_alias,
                        binding.component_alias, target_kind,
                        binding.bound ? "bound" : "unbound");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Bind current selection")) {
                        const auto bound = ui.bind_agent_demonstration_target(
                            binding.target_alias, binding.component_alias,
                            binding.target_set_index);
                        agent_feedback = bound
                            ? "Explicit demonstration target binding recorded for this project revision."
                            : bound.error().message;
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                    ImGui::PopID();
                }
                if (ImGui::SmallButton("Clear target bindings")) {
                    const auto cleared = ui.clear_agent_demonstration_target_bindings();
                    if (!cleared) agent_feedback = cleared.error().message;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!snapshot.agent_demonstration_ready_to_preview);
                if (ImGui::SmallButton("Preview bound trace")) {
                    const auto preview = ui.preview_agent_demonstration_trace();
                    agent_feedback = preview
                        ? "Speculative trace preview ready. Review its diff and gates before acceptance."
                        : preview.error().message;
                }
                ImGui::EndDisabled();
                if (ImGui::SmallButton("Save as reusable recipe...")) {
                    const auto path = choose_file(true, FileDialogKind::agent_demonstration_recipe);
                    if (path.has_value()) {
                        const auto saved = ui.save_agent_demonstration_recipe(*path);
                        agent_feedback = saved
                            ? "Named alias-only recipe saved immutably; it carries no target bindings or task authority."
                            : saved.error().message;
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Save captured native trace...")) {
                    const auto path = choose_file(true, FileDialogKind::agent_demonstration_trace);
                    if (path.has_value()) {
                        const auto saved = ui.save_agent_demonstration_trace(*path);
                        agent_feedback = saved
                            ? "Immutable trace saved. Applying it later requires explicit bindings and human acceptance."
                            : saved.error().message;
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear native trace")) {
                    const auto cleared = ui.clear_agent_demonstration_trace();
                    if (!cleared) agent_feedback = cleared.error().message;
                }
            }
        }

        if (!task_present && ImGui::SmallButton("Open demonstration record...")) {
            const auto path = choose_file(false, FileDialogKind::agent_demonstration);
            if (path.has_value()) {
                const auto loaded = ui.load_agent_demonstration(*path);
                agent_feedback = loaded
                    ? "Redacted demonstration opened as a read-only offline trace."
                    : loaded.error().message;
            }
        }
        if (snapshot.agent_demonstration.has_value()) {
            const auto& demonstration = *snapshot.agent_demonstration;
            ImGui::Separator();
            ImGui::Text("Redacted demonstration - offline trace");
            ImGui::Text("Format v%u", demonstration.format_version);
            ImGui::Text("Recipe %s | %zu steps | %zu gates",
                demonstration.recipe_id.empty() ? "(none)" : demonstration.recipe_id.c_str(),
                demonstration.steps.size(), demonstration.gates.size());
            ImGui::Text("Revision span +%llu | repairs %zu | rebases %zu | ghost edits %zu",
                static_cast<unsigned long long>(demonstration.result_revision_span),
                demonstration.repair_count, demonstration.rebase_count,
                demonstration.ghost_correction_count);
            ImGui::TextDisabled(
                "Redacted, not anonymous: hashes can match known inputs. The checksum does not prove authorship, and this record cannot run.");
            const std::size_t visible_steps = std::min<std::size_t>(
                demonstration.steps.size(), 64U);
            for (std::size_t index = 0U; index < visible_steps; ++index) {
                const auto& demo_step = demonstration.steps[index];
                ImGui::BulletText("%zu. %s on anonymized object %u (revision +%llu to +%llu)",
                    index + 1U, demo_step.tool_id.c_str(), demo_step.target_alias,
                    static_cast<unsigned long long>(demo_step.revision_before_offset),
                    static_cast<unsigned long long>(demo_step.revision_after_offset));
                if (!demo_step.parameters.has_value()) {
                    ImGui::TextDisabled("   No transferable typed parameters (legacy trace).");
                } else if (demo_step.parameters->parameterization ==
                    carto::agent::AgentDemonstrationParameterization::normalized_face_distance) {
                    ImGui::TextDisabled("   Face distance: %.6g × source longest edge",
                        demo_step.parameters->normalized_distance);
                } else {
                    const auto displacement = demo_step.parameters->relative_displacement;
                    ImGui::TextDisabled("   Relative displacement: %.6g, %.6g, %.6g",
                        displacement.x, displacement.y, displacement.z);
                }
            }
            if (visible_steps < demonstration.steps.size()) {
                ImGui::TextDisabled("Showing 64 of %zu steps.", demonstration.steps.size());
            }
            for (const auto& gate : demonstration.gates) {
                ImGui::BulletText("%s: %s (%s)", gate.id.c_str(),
                    gate.passed ? "passed" : "failed", gate.validator_version.c_str());
            }
            if (ImGui::SmallButton("Clear demonstration record")) {
                const auto cleared = ui.clear_agent_demonstration();
                if (!cleared) agent_feedback = cleared.error().message;
            }
        }
        if (task_present) {
            const auto& task = *snapshot.agent_task;
            ImGui::BeginDisabled(task.status != carto::agent::TaskStatus::accepted);
            if (ImGui::SmallButton("Save redacted demonstration...")) {
                const auto path = choose_file(true, FileDialogKind::agent_demonstration);
                if (path.has_value()) {
                    const auto saved = ui.save_agent_demonstration(*path);
                    agent_feedback = saved
                        ? "Redacted trace saved. Review its digests before sharing; it is not authenticated or replayable."
                        : saved.error().message;
                }
            }
            ImGui::EndDisabled();

            if (ImGui::SmallButton("Save persistent task checkpoint...")) {
                const auto path = choose_file(true, FileDialogKind::agent_checkpoint);
                if (path.has_value()) {
                    const auto saved = ui.save_agent_checkpoint(*path);
                    agent_feedback = saved
                        ? "Immutable task checkpoint saved with source, speculative artifact, and evidence digests."
                        : saved.error().message;
                }
            }
            ImGui::Text("Task %s · %s", task.task_id.c_str(),
                        carto::agent::task_status_name(task.status));
            ImGui::TextWrapped("%s", task.goal.c_str());
            const std::string objective_digest = task.objective_digest.hex();
            ImGui::TextDisabled("Objective %.12s · based on R%llu",
                objective_digest.c_str(),
                static_cast<unsigned long long>(task.base_revision.value()));
            for (const auto& question : task.clarifications) {
                ImGui::TextWrapped("Operator question · %s", question.prompt.c_str());
                if (question.freeform_answer.has_value()) {
                    ImGui::TextWrapped("Recorded answer · %s",
                        question.freeform_answer->c_str());
                } else if (question.selected_candidate_id.has_value()) {
                    ImGui::Text("Recorded choice · %s",
                        question.selected_candidate_id->c_str());
                }
            }
            if (task.status == carto::agent::TaskStatus::prepared) {
                ImGui::TextDisabled(
                    "Prepared at R%llu. Query inventory before running if CONDITION source context matters.",
                    static_cast<unsigned long long>(task.base_revision.value()));
                if (ImGui::Button("Run prepared plan", ImVec2(-1, 30))) {
                    const auto run = ui.run_prepared_agent_plan();
                    if (run) {
                        agent_feedback = run.value().status == carto::agent::TaskStatus::review
                            ? "Speculative result is ready for review; the source is unchanged."
                            : (run.value().diagnostic.has_value()
                                ? run.value().diagnostic->message
                                : "Prepared plan is blocked by an acceptance gate.");
                    } else {
                        agent_feedback = run.error().message;
                    }
                }
            }
            ImGui::BeginDisabled(task.status == carto::agent::TaskStatus::accepted ||
                task.status == carto::agent::TaskStatus::rejected);
            if (ImGui::SmallButton("Inspect scoped CONDITION inventory")) {
                const auto inventory = ui.query_agent_condition_inventory();
                agent_feedback = inventory
                    ? (inventory.value().truncated
                        ? "Scoped inventory is partial; inspect truncation and per-asset completeness."
                        : "CONDITION inventory read at the task revision; project data is unchanged.")
                    : inventory.error().message;
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled(
                "Inventory is read-only, limited to this task's object scope, and uses its query budget. "
                "Counts do not certify production readiness.");
            if (snapshot.agent_condition_inventory.has_value()) {
                draw_agent_condition_inventory(
                    snapshot, *snapshot.agent_condition_inventory);
            }
            if (ImGui::SmallButton("Show source")) {
                agent_preview_mode = 0;
                agent_ghost_edit_mode = false;
                agent_ghost_selected_object = {};
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Show before")) {
                agent_preview_mode = 1;
                agent_ghost_edit_mode = false;
                agent_ghost_selected_object = {};
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Show after")) agent_preview_mode = 2;
            const bool ghost_task_editable = task.status == carto::agent::TaskStatus::review ||
                task.status == carto::agent::TaskStatus::blocked;
            ImGui::SameLine();
            ImGui::BeginDisabled(!ghost_task_editable ||
                snapshot.project_revision != task.base_revision);
            if (ImGui::SmallButton(agent_ghost_edit_mode ? "Stop ghost editing" : "Edit ghost")) {
                agent_ghost_edit_mode = !agent_ghost_edit_mode;
                agent_preview_mode = 2;
                agent_ghost_selected_object = {};
                if (!agent_ghost_edit_mode) agent_ghost_justification.clear();
            }
            ImGui::EndDisabled();
            if (agent_ghost_edit_mode) {
                if (snapshot.project_revision != task.base_revision) {
                    ImGui::TextColored(color(snapshot.colors.semantic_warning),
                        "Source advanced. Ghost editing is locked until a safe rebase is prepared.");
                }
                ImGui::TextDisabled(
                    "Click an in-scope root object, then drag its X or Z handle. "
                    "Each change stays on the speculative branch and reruns gates.");
                std::array<char, 1025> correction_reason{};
                const std::size_t correction_reason_bytes = std::min(
                    agent_ghost_justification.size(), correction_reason.size() - 1U);
                std::copy_n(agent_ghost_justification.data(), correction_reason_bytes,
                    correction_reason.data());
                if (ImGui::InputTextMultiline("Ghost correction reason",
                        correction_reason.data(), correction_reason.size(), ImVec2(-1, 54))) {
                    agent_ghost_justification.assign(correction_reason.data());
                }
                if (agent_ghost_selected_object) {
                    ImGui::TextDisabled("Selected ghost object %llu",
                        static_cast<unsigned long long>(agent_ghost_selected_object.value));
                }
            }
            if (!task.ghost_corrections.empty()) {
                ImGui::Text("Locked human corrections · %zu", task.ghost_corrections.size());
                for (const auto& correction : task.ghost_corrections) {
                    ImGui::BulletText("%s · object %llu · %s", correction.task_id.c_str(),
                        static_cast<unsigned long long>(correction.object.value),
                        correction.justification.c_str());
                }
            }
            ImGui::TextDisabled(
                "Accept keeps one undo step for this result and replaces the previous command history.");
            for (const auto& planned : task.plan_steps) {
                std::string dependencies = "ready";
                if (!planned.dependencies.empty()) {
                    dependencies = "after ";
                    for (std::size_t index = 0U; index < planned.dependencies.size(); ++index) {
                        if (index != 0U) dependencies += ", ";
                        dependencies += planned.dependencies[index];
                    }
                }
                ImGui::BulletText("%s · %s · %s", planned.step_id.c_str(),
                    carto::agent::step_status_name(planned.status), dependencies.c_str());
                if (planned.diagnostic.has_value()) {
                    ImGui::TextDisabled("  %s", planned.diagnostic->message.c_str());
                }
            }
            if (!task.repairs.empty()) {
                ImGui::Separator();
                ImGui::Text("Repair lineage · %llu attempt(s)",
                    static_cast<unsigned long long>(task.repairs.size()));
                for (const auto& repair : task.repairs) {
                    ImGui::TextWrapped("%s → %s · %s", repair.previous_task_id.c_str(),
                        repair.repaired_task_id.c_str(), repair.justification.c_str());
                    const std::string failure_digest = repair.failure_evidence_digest.hex();
                    ImGui::TextDisabled("Prior failure evidence %.12s · %llu retained step receipt(s)",
                        failure_digest.c_str(), static_cast<unsigned long long>(
                            repair.previous_step_receipts.size()));
                    for (const auto& prior_receipt : repair.previous_step_receipts) {
                        ImGui::BulletText("Prior %s · %s · %s",
                            prior_receipt.step_id.c_str(),
                            carto::agent::step_status_name(prior_receipt.status),
                            prior_receipt.intent.c_str());
                        if (prior_receipt.diagnostic.has_value()) {
                            ImGui::TextDisabled("  %s",
                                prior_receipt.diagnostic->message.c_str());
                        }
                    }
                }
            }
            ImGui::Text("Diff · %llu objects · %llu meshes",
                static_cast<unsigned long long>(task.diff.changed_object_count()),
                static_cast<unsigned long long>(task.diff.changed_mesh_count()));
            for (const auto& object_change : task.diff.objects) {
                if (object_change.mesh_content_changed || object_change.transform_changed ||
                    object_change.mesh_binding_changed || object_change.name_changed) {
                    ImGui::BulletText("Object %llu ·%s%s%s%s",
                        static_cast<unsigned long long>(object_change.id.value),
                        object_change.mesh_content_changed ? " mesh" : "",
                        object_change.transform_changed ? " transform" : "",
                        object_change.mesh_binding_changed ? " binding" : "",
                        object_change.name_changed ? " name" : "");
                }
            }
            const auto format_vertex_ids = [](
                const std::vector<carto::geometry::VertexId>& vertex_ids) {
                std::string result;
                for (std::size_t index = 0U; index < vertex_ids.size(); ++index) {
                    if (index != 0U) result += ", ";
                    result += std::to_string(vertex_ids[index].value);
                }
                return result.empty() ? std::string("none") : result;
            };
            for (const auto& mesh_change : task.diff.meshes) {
                ImGui::Text("Mesh %llu · vertices %llu → %llu · faces %llu → %llu",
                    static_cast<unsigned long long>(mesh_change.asset_id),
                    static_cast<unsigned long long>(mesh_change.before_vertices),
                    static_cast<unsigned long long>(mesh_change.after_vertices),
                    static_cast<unsigned long long>(mesh_change.before_faces),
                    static_cast<unsigned long long>(mesh_change.after_faces));
                ImGui::TextDisabled("%llu vertex changes · %llu face changes",
                    static_cast<unsigned long long>(mesh_change.vertices.size()),
                    static_cast<unsigned long long>(mesh_change.faces.size()));
                const std::string mesh_id = "mesh-" +
                    std::to_string(mesh_change.asset_id);
                ImGui::PushID(mesh_id.c_str());
                if (!mesh_change.vertices.empty()) {
                    const std::string label = "Changed vertex positions (" +
                        std::to_string(mesh_change.vertices.size()) + ")";
                    if (ImGui::TreeNode(label.c_str())) {
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(mesh_change.vertices.size()));
                        while (clipper.Step()) {
                            for (int row = clipper.DisplayStart;
                                 row < clipper.DisplayEnd; ++row) {
                                const auto& vertex = mesh_change.vertices[
                                    static_cast<std::size_t>(row)];
                                ImGui::TextDisabled(
                                    "Vertex %llu · (%.9g, %.9g, %.9g) → (%.9g, %.9g, %.9g)",
                                    static_cast<unsigned long long>(vertex.id.value),
                                    vertex.before_position.x, vertex.before_position.y,
                                    vertex.before_position.z, vertex.after_position.x,
                                    vertex.after_position.y, vertex.after_position.z);
                            }
                        }
                        ImGui::TreePop();
                    }
                }
                if (!mesh_change.faces.empty()) {
                    const std::string label = "Changed face connectivity (" +
                        std::to_string(mesh_change.faces.size()) + ")";
                    if (ImGui::TreeNode(label.c_str())) {
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(mesh_change.faces.size()));
                        while (clipper.Step()) {
                            for (int row = clipper.DisplayStart;
                                 row < clipper.DisplayEnd; ++row) {
                                const auto& face = mesh_change.faces[
                                    static_cast<std::size_t>(row)];
                                const char* change = face.added ? "added" :
                                    face.removed ? "removed" : "connectivity changed";
                                ImGui::TextDisabled(
                                    "Face %llu · %s · vertices %zu → %zu · hover for IDs",
                                    static_cast<unsigned long long>(face.id.value), change,
                                    face.before_vertices.size(), face.after_vertices.size());
                                if (ImGui::IsItemHovered()) {
                                    ImGui::BeginTooltip();
                                    const std::string before = format_vertex_ids(
                                        face.before_vertices);
                                    const std::string after = format_vertex_ids(
                                        face.after_vertices);
                                    ImGui::TextWrapped("Before vertex IDs: %s", before.c_str());
                                    ImGui::TextWrapped("After vertex IDs: %s", after.c_str());
                                    ImGui::EndTooltip();
                                }
                            }
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::PopID();
            }
            const std::string diff_digest = task.diff.digest.hex();
            ImGui::TextDisabled("Diff digest %.12s · R%llu → R%llu",
                diff_digest.c_str(),
                static_cast<unsigned long long>(task.base_revision.value()),
                static_cast<unsigned long long>(task.speculative_revision.value()));
            for (const auto& gate : task.gates) {
                ImGui::BulletText("%s · %s · %s", gate.passed ? "PASS" : "FAIL",
                    gate.id.c_str(), gate.evidence.c_str());
                const std::string evidence_digest = gate.evidence_digest.hex();
                ImGui::TextDisabled("  %s · R%llu → R%llu · evidence %.12s",
                    gate.validator_version.c_str(),
                    static_cast<unsigned long long>(gate.source_revision.value()),
                    static_cast<unsigned long long>(gate.evaluated_revision.value()),
                    evidence_digest.c_str());
                if (gate.diagnostic.has_value()) {
                    ImGui::TextColored(color(snapshot.colors.semantic_warning), "  %s",
                        gate.diagnostic->message.c_str());
                }
            }
            for (const auto& receipt : task.steps) {
                ImGui::TextWrapped("%s · %s · %s", receipt.step_id.c_str(),
                    receipt.tool_id.c_str(), receipt.intent.c_str());
                if (receipt.diagnostic.has_value()) {
                    ImGui::TextColored(color(snapshot.colors.semantic_warning), "%s",
                                       receipt.diagnostic->message.c_str());
                }
            }
            if (task.diagnostic.has_value()) {
                ImGui::TextColored(color(snapshot.colors.semantic_warning), "%s",
                                   task.diagnostic->message.c_str());
            }
            if (task.status == carto::agent::TaskStatus::blocked &&
                task.repair_attempts_remaining > 0U && !snapshot.agent_plan_draft.empty()) {
                ImGui::Separator();
                ImGui::Text("Bounded repair · %llu attempt(s) remain",
                    static_cast<unsigned long long>(task.repair_attempts_remaining));
                std::vector<const char*> repair_step_labels;
                repair_step_labels.reserve(snapshot.agent_plan_draft.size());
                for (const auto& planned : snapshot.agent_plan_draft) {
                    repair_step_labels.push_back(planned.id.c_str());
                }
                if (agent_repair_step_index < 0 ||
                    agent_repair_step_index >= static_cast<int>(repair_step_labels.size())) {
                    agent_repair_step_index = 0;
                }
                ImGui::Combo("Step to revise", &agent_repair_step_index,
                    repair_step_labels.data(), static_cast<int>(repair_step_labels.size()));
                const auto& selected_step = snapshot.agent_plan_draft[
                    static_cast<std::size_t>(agent_repair_step_index)];
                if (agent_repair_step_id != selected_step.id) {
                    agent_repair_step_id = selected_step.id;
                    agent_repair_intent = selected_step.intent;
                }
                std::array<char, 1025> revised_intent{};
                const std::size_t revised_intent_bytes = std::min(
                    agent_repair_intent.size(), revised_intent.size() - 1U);
                std::copy_n(agent_repair_intent.data(), revised_intent_bytes,
                    revised_intent.data());
                if (ImGui::InputText("Revised native intent", revised_intent.data(),
                        revised_intent.size())) {
                    agent_repair_intent.assign(revised_intent.data());
                }
                std::array<char, 1025> repair_reason{};
                const std::size_t repair_reason_bytes = std::min(
                    agent_repair_justification.size(), repair_reason.size() - 1U);
                std::copy_n(agent_repair_justification.data(), repair_reason_bytes,
                    repair_reason.data());
                if (ImGui::InputTextMultiline("Repair justification", repair_reason.data(),
                        repair_reason.size(), ImVec2(-1, 54))) {
                    agent_repair_justification.assign(repair_reason.data());
                }
                ImGui::BeginDisabled(agent_repair_intent.empty());
                if (ImGui::Button("Revise selected step", ImVec2(-1, 26))) {
                    auto revised_step = selected_step;
                    revised_step.intent = agent_repair_intent;
                    const auto revised = ui.revise_agent_plan_step(std::move(revised_step));
                    agent_feedback = revised
                        ? "Plan changed. Review and re-answer any invalidated operator questions before repair."
                        : revised.error().message;
                }
                ImGui::EndDisabled();
                if (snapshot.agent_repair_ready) {
                    ImGui::BeginDisabled(!clarifications_answered ||
                        agent_repair_justification.empty());
                    if (ImGui::Button("Run bounded repair", ImVec2(-1, 30))) {
                        const auto repaired = ui.repair_agent_plan(agent_repair_justification);
                        agent_feedback = repaired
                            ? (repaired.value().status == carto::agent::TaskStatus::review
                                ? "Repair passed validation and is ready for review; the source is unchanged."
                                : (repaired.value().diagnostic.has_value()
                                    ? repaired.value().diagnostic->message
                                    : "Repair is blocked by an acceptance gate."))
                            : repaired.error().message;
                    }
                    ImGui::EndDisabled();
                }
            }
            if (task.can_accept) {
                if (ImGui::Button("Accept result", ImVec2(-1, 30))) {
                    const auto accepted = ui.accept_agent_plan();
                    agent_feedback = accepted
                        ? "Accepted through the application journal boundary."
                        : accepted.error().message;
                }
            }
            if (task.status != carto::agent::TaskStatus::accepted &&
                task.status != carto::agent::TaskStatus::rejected) {
                if (ImGui::Button("Reject result", ImVec2(-1, 28))) {
                    const auto rejected = ui.reject_agent_plan();
                    agent_feedback = rejected
                        ? "Speculative branch rejected; the source is unchanged."
                        : rejected.error().message;
                }
            }
        }
        if (!unresolved_task) {
            ImGui::Separator();
            ImGui::TextDisabled(
                "Read-only inspection uses the selected object scope and does not create a task.");
            const bool has_inspection_scope =
                !snapshot.selection.objects.empty() ||
                snapshot.selection.component_object.has_value();
            ImGui::BeginDisabled(!has_inspection_scope);
            if (ImGui::Button("Inspect selected CONDITION inventory", ImVec2(-1, 28))) {
                const auto inventory = ui.query_agent_condition_inventory();
                agent_feedback = inventory
                    ? (inventory.value().truncated
                        ? "Selected-scope inventory is partial; inspect its diagnostics and completeness."
                        : "Selected-scope CONDITION inventory loaded at the current project revision.")
                    : inventory.error().message;
            }
            ImGui::EndDisabled();
            if (snapshot.agent_condition_inventory.has_value()) {
                draw_agent_condition_inventory(
                    snapshot, *snapshot.agent_condition_inventory);
            }
        }
        if (!agent_feedback.empty()) ImGui::TextWrapped("%s", agent_feedback.c_str());
        if (!task_active && !snapshot.agent_plan_draft.empty() &&
            ImGui::SmallButton("Clear queued steps")) {
            const auto cleared = ui.clear_agent_plan();
            agent_feedback = cleared ? "Queued steps cleared." : cleared.error().message;
        }
        end_instrument(carto::ui::WorkbenchInstrument::draft, workbench.draft);
    }

    void draw_engineering_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::references,
                              "Engineering", workbench.references, ImVec2(250, 90),
                              ImVec2(390, 430), snapshot.colors.accent_primary)) return;
        ImGui::TextDisabled("DV012 authoring boundary");
        ImGui::TextWrapped(
            "Engineering records stay authoritative in the project. The panel exposes only "
            "validated evidence and an immutable package handoff.");
        ImGui::Separator();
        if (!snapshot.engineering.present) {
            ImGui::TextDisabled("No engineering authoring is loaded.");
            if (ImGui::Button("Import authoring document...", ImVec2(-1, 30))) {
                import_engineering_authoring();
            }
            if (ImGui::Button("Load DV012 demo fixture", ImVec2(-1, 30))) {
                static_cast<void>(dispatch(
                    carto::application::ImportEngineeringFixtureAction{}));
            }
        } else {
            const auto& engineering = snapshot.engineering;
            ImGui::Text("Sources %zu  ·  Parts %zu  ·  Occurrences %zu",
                        engineering.source_count, engineering.part_count,
                        engineering.occurrence_count);
            ImGui::Text("Topology %zu  ·  BOM %zu  ·  Requirements %zu",
                        engineering.topology_count, engineering.bom_count,
                        engineering.requirement_count);
            ImGui::Text("Characteristics %zu  ·  Targets %zu",
                        engineering.characteristic_count, engineering.target_count);
            ImGui::Text("Derived products %zu  ·  %s",
                        engineering.derived_product_count,
                        engineering.workcell_present ? "Workcell bound" : "Workcell missing");
            if (engineering.path_present) ImGui::TextDisabled("Inspection path record present");
            if (!engineering.digest.is_zero()) {
                mono_text_disabled(("Authoring " + engineering.digest.hex()).c_str());
            }
            if (ImGui::BeginChild("Engineering source records", ImVec2(0, 92), true)) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(engineering.sources.size()), 70.0F);
                while (clipper.Step()) {
                    for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                        const auto& source = engineering.sources[static_cast<std::size_t>(index)];
                        const std::string source_id = std::to_string(source.id);
                        ImGui::PushID(source_id.c_str());
                        ImGui::Text("Source %s · %s", source_id.c_str(), source.kind.c_str());
                        ImGui::TextDisabled("%s · revision %s", source.locator.c_str(),
                                            source.revision.c_str());
                        mono_text_disabled(source.digest.hex().c_str());
                        if (ImGui::SmallButton("Record changed source...")) {
                            engineering_source_update_id = source.id;
                            engineering_source_revision.fill('\0');
                            engineering_source_digest.fill('\0');
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
            if (engineering_source_update_id.has_value()) {
                ImGui::InputText("New source revision", engineering_source_revision.data(),
                                 engineering_source_revision.size());
                ImGui::InputText("New source SHA-256", engineering_source_digest.data(),
                                 engineering_source_digest.size());
                if (ImGui::Button("Record drift and stale dependent evidence")) {
                    record_engineering_source_change();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel source update")) {
                    engineering_source_update_id.reset();
                    engineering_source_revision.fill('\0');
                    engineering_source_digest.fill('\0');
                }
            }
            if (engineering.stale) {
                ImGui::TextColored(color(snapshot.colors.semantic_error),
                    "Source drift is recorded; import reconciled authoring to restore admission.");
            }
            const ImVec4 status_color = engineering.vanta_admissible
                ? color(snapshot.colors.semantic_success)
                : engineering.publication_ready
                    ? color(snapshot.colors.semantic_warning)
                    : color(snapshot.colors.semantic_error);
            ImGui::TextColored(status_color, "%s", engineering.status.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Import replacement authoring...", ImVec2(-1, 30))) {
                import_engineering_authoring();
            }
            if (ImGui::Button("Export authoring document...", ImVec2(-1, 30))) {
                export_engineering_authoring();
            }
            ImGui::BeginDisabled(!engineering.publication_ready || engineering.stale);
            if (ImGui::Button("Publish .carto-inspection...", ImVec2(-1, 30))) {
                publish_engineering_package();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Replace with DV012 demo fixture", ImVec2(-1, 30))) {
                if (dispatch(carto::application::ImportEngineeringFixtureAction{})) {
                    engineering_source_update_id.reset();
                    engineering_source_revision.fill('\0');
                    engineering_source_digest.fill('\0');
                }
            }
        }
        if (!action_feedback.empty()) {
            ImGui::Separator();
            draw_action_feedback(snapshot);
        }
        end_instrument(carto::ui::WorkbenchInstrument::references, workbench.references);
    }

    void draw_condition_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::references,
                              "CONDITION", workbench.references, ImVec2(250, 90),
                              ImVec2(390, 430), snapshot.colors.accent_primary)) return;
        ImGui::TextDisabled("Source authoring boundary");
        ImGui::TextWrapped(
            "The project stores source-derived CONDITION records. Runtime condition and "
            "game-owned state remain outside this package.");
        ImGui::Separator();
        const bool active_agent_task = snapshot.agent_task.has_value() &&
            snapshot.agent_task->status != carto::agent::TaskStatus::accepted &&
            snapshot.agent_task->status != carto::agent::TaskStatus::rejected;
        const bool has_condition_scope = !snapshot.selection.objects.empty() ||
            snapshot.selection.component_object.has_value();
        ImGui::TextDisabled("Agent inspection");
        ImGui::BeginDisabled(!snapshot.condition.present ||
            (!active_agent_task && !has_condition_scope));
        if (ImGui::Button(active_agent_task
                ? "Inspect task scope with Cartographer"
                : "Inspect selected asset with Cartographer",
                ImVec2(-1, 28))) {
            const auto inventory = ui.query_agent_condition_inventory();
            if (!inventory) {
                action_feedback = inventory.error().message;
                action_feedback_error = true;
            } else {
                action_feedback = inventory.value().truncated
                    ? "Read-only inventory is partial; inspect diagnostics and completeness in Agent."
                    : "Read-only scoped inventory loaded in Agent at its source revision.";
                action_feedback_error = false;
                static_cast<void>(open_agent_workspace(false));
            }
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            "This reads scoped source facts only; it does not certify readiness or change project data.");
        if (!snapshot.condition.present) {
            ImGui::TextDisabled("No CONDITION source authoring is loaded.");
            if (ImGui::Button("Import authoring document...", ImVec2(-1, 30))) {
                import_condition_authoring();
            }
            if (ImGui::Button("Load CONDITION demo fixture", ImVec2(-1, 30))) {
                static_cast<void>(dispatch(
                    carto::application::ImportConditionFixtureAction{}));
            }
        } else {
            const auto& condition = snapshot.condition;
            ImGui::Text("Sources %zu  ·  Assets %zu  ·  Safe places %zu",
                        condition.source_count, condition.asset_count,
                        condition.safe_place_count);
            ImGui::Text("Familiarity %zu  ·  Views %zu  ·  Variants %zu",
                        condition.familiarity_count, condition.view_condition_count,
                        condition.variant_count);
            ImGui::Text("Temporal packages %zu  ·  Surface states %zu",
                        condition.temporal_package_count, condition.surface_state_count);
            ImGui::Text("Anchors %zu  ·  Collections %zu  ·  Prefabs %zu",
                        condition.interaction_anchor_count, condition.collection_count,
                        condition.prefab_count);
            ImGui::Text("Region kits %zu", condition.region_kit_count);
            ImGui::Text("Audio sources %zu  ·  Acoustic zones %zu",
                condition.audio_source_count, condition.acoustic_zone_count);
            ImGui::TextDisabled("Authoring revision r%llu",
                static_cast<unsigned long long>(condition.authored_revision.value()));
            if (!condition.digest.is_zero()) {
                mono_text_disabled(("Authoring " + condition.digest.hex()).c_str());
            }
            if (ImGui::BeginChild("CONDITION source records", ImVec2(0, 92), true)) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(condition.sources.size()), 70.0F);
                while (clipper.Step()) {
                    for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                        const auto& source = condition.sources[static_cast<std::size_t>(index)];
                        const std::string source_id = std::to_string(source.id);
                        ImGui::PushID(source_id.c_str());
                        ImGui::Text("Source %s · %s%s", source_id.c_str(), source.kind.c_str(),
                                    source.current ? "" : " · stale");
                        ImGui::TextDisabled("%s · revision %s",
                                            source.label.empty() ? source.locator.c_str()
                                                                 : source.label.c_str(),
                                            source.revision.c_str());
                        mono_text_disabled(source.digest.hex().c_str());
                        if (ImGui::SmallButton("Record changed source...")) {
                            condition_source_update_id = source.id;
                            condition_source_revision.fill('\0');
                            condition_source_digest.fill('\0');
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
            heading_text("Audio source and acoustic-zone intent");
            ImGui::TextWrapped(
                "WAVE import validates bounded PCM/float metadata and records a content digest. "
                "Samples are not embedded or played; source evidence requires explicit review.");
            if (ImGui::Button("Import WAVE clip evidence...", ImVec2(-1, 28))) {
                import_condition_audio_clip();
            }
            for (const auto& source : condition.sources) {
                if (source.kind != "imported-asset") continue;
                for (const auto& diagnostic : source.diagnostics) {
                    if (diagnostic.starts_with("WAVE inspected:")) {
                        ImGui::TextWrapped("%s", diagnostic.c_str());
                        ImGui::TextDisabled("Clip evidence %llu · %s%s",
                            static_cast<unsigned long long>(source.id),
                            source.current ? "current" : "stale",
                            source.reviewed ? " · reviewed" : " · not reviewed");
                        if (!source.reviewed && source.current &&
                            ImGui::SmallButton(("Review metadata##wave-" +
                                std::to_string(source.id)).c_str())) {
                            const auto reviewed = ui.set_condition_source_reviewed(source.id, true);
                            action_feedback = reviewed
                                ? "Marked current WAVE source metadata reviewed; no audio audition occurred."
                                : reviewed.error().message;
                            action_feedback_error = !reviewed;
                        }
                        if (source.current && ImGui::SmallButton(("Audition 3 s excerpt##wave-" +
                                std::to_string(source.id)).c_str())) {
                            audition_condition_audio_clip(source.id);
                        }
                    }
                }
            }
            if (!condition_audio_preview_buffer.empty() &&
                ImGui::SmallButton("Stop WAVE audition")) {
                stop_condition_audio_audition();
                action_feedback = "Stopped the transient WAVE audition.";
                action_feedback_error = false;
            }
            std::vector<const char*> condition_asset_labels;
            condition_asset_labels.reserve(condition.assets.size());
            for (const auto& asset : condition.assets) {
                condition_asset_labels.push_back(asset.stable_name.c_str());
            }
            ImGui::Separator();
            ImGui::TextDisabled("Scene-object binding");
            if (snapshot.selection.objects.size() == 1U) {
                const auto selected_object = snapshot.selection.objects.front();
                ImGui::TextDisabled("Selected object #%llu",
                    static_cast<unsigned long long>(selected_object.value));
                for (const auto& asset : condition.assets) {
                    const std::string asset_id_text = std::to_string(asset.id);
                    const std::string binding_text = asset.authored_object_id == 0U
                        ? "unbound"
                        : "object #" + std::to_string(asset.authored_object_id);
                    ImGui::PushID(asset_id_text.c_str());
                    ImGui::Text("%s · asset %llu · %s",
                        asset.stable_name.c_str(),
                        static_cast<unsigned long long>(asset.id),
                        binding_text.c_str());
                    if (asset.authored_object_id != selected_object.value &&
                        ImGui::SmallButton("Bind selected object")) {
                        const auto bound = ui.bind_condition_asset_to_object(
                            asset.id, selected_object, snapshot.project_revision,
                            condition.authored_revision);
                        action_feedback = bound
                            ? "Bound CONDITION asset through revision-checked project history."
                            : bound.error().message;
                        action_feedback_error = !bound;
                    }
                    ImGui::PopID();
                }
            } else {
                ImGui::TextDisabled(
                    "Select exactly one scene object to bind CONDITION assets for native previews.");
            }
            if (!condition_asset_labels.empty()) {
                condition_audio_asset_index = std::clamp(condition_audio_asset_index, 0,
                    static_cast<int>(condition_asset_labels.size()) - 1);
                ImGui::Combo("Audio owner asset", &condition_audio_asset_index,
                    condition_asset_labels.data(),
                    static_cast<int>(condition_asset_labels.size()));
            }
            std::vector<const carto::application::ConditionAuthoringView::Source*>
                clip_sources;
            std::vector<std::string> clip_labels;
            for (const auto& source : condition.sources) {
                if (source.kind != "imported-asset" || !source.current || !source.reviewed) continue;
                clip_sources.push_back(&source);
                clip_labels.push_back(std::to_string(source.id) + " · " +
                    (source.label.empty() ? source.locator : source.label));
            }
            std::vector<const char*> clip_label_pointers;
            clip_label_pointers.reserve(clip_labels.size());
            for (const auto& label : clip_labels) clip_label_pointers.push_back(label.c_str());
            if (!clip_label_pointers.empty()) {
                condition_audio_clip_index = std::clamp(condition_audio_clip_index, 0,
                    static_cast<int>(clip_label_pointers.size()) - 1);
                ImGui::Combo("Reviewed clip source", &condition_audio_clip_index,
                    clip_label_pointers.data(), static_cast<int>(clip_label_pointers.size()));
                ImGui::InputDouble("Audio source X", &condition_audio_x, 0.1, 1.0, "%.3f");
                ImGui::InputDouble("Audio source Y", &condition_audio_y, 0.1, 1.0, "%.3f");
                ImGui::InputDouble("Audio source Z", &condition_audio_z, 0.1, 1.0, "%.3f");
                ImGui::InputDouble("Audio gain", &condition_audio_gain, 0.05, 0.25, "%.3f");
                ImGui::InputDouble("Audio pitch", &condition_audio_pitch, 0.05, 0.25, "%.3f");
                ImGui::InputDouble("Minimum distance", &condition_audio_minimum_distance,
                    0.1, 1.0, "%.3f");
                ImGui::InputDouble("Maximum distance", &condition_audio_maximum_distance,
                    0.1, 1.0, "%.3f");
                ImGui::InputDouble("Reverb send", &condition_audio_reverb_send, 0.05, 0.25, "%.3f");
                ImGui::InputInt("Audio priority", &condition_audio_priority);
                condition_audio_priority = std::clamp(condition_audio_priority, 0, 65535);
                constexpr std::array<const char*, 7> audio_bus_labels{
                    "Ambient", "Machinery", "Vehicle", "Radio", "Speech", "Music", "Effects"};
                constexpr std::array<carto::condition::AudioBusClass, 7> audio_bus_values{
                    carto::condition::AudioBusClass::ambient,
                    carto::condition::AudioBusClass::machinery,
                    carto::condition::AudioBusClass::vehicle,
                    carto::condition::AudioBusClass::radio,
                    carto::condition::AudioBusClass::speech,
                    carto::condition::AudioBusClass::music,
                    carto::condition::AudioBusClass::effects};
                condition_audio_bus_index = std::clamp(condition_audio_bus_index, 0, 6);
                ImGui::Combo("Audio bus class", &condition_audio_bus_index,
                    audio_bus_labels.data(), static_cast<int>(audio_bus_labels.size()));
                constexpr std::array<const char*, 3> audio_occlusion_labels{
                    "None", "Static geometry", "Authored geometry"};
                constexpr std::array<carto::condition::AudioOcclusionIntent, 3>
                    audio_occlusion_values{
                        carto::condition::AudioOcclusionIntent::none,
                        carto::condition::AudioOcclusionIntent::static_geometry,
                        carto::condition::AudioOcclusionIntent::authored_geometry};
                condition_audio_occlusion_index = std::clamp(
                    condition_audio_occlusion_index, 0, 2);
                ImGui::Combo("Occlusion intent", &condition_audio_occlusion_index,
                    audio_occlusion_labels.data(),
                    static_cast<int>(audio_occlusion_labels.size()));
                ImGui::Checkbox("Loop intent", &condition_audio_loop);
                ImGui::Checkbox("Spatial intent", &condition_audio_spatial);
                ImGui::Checkbox("Source enabled", &condition_audio_enabled);
                ImGui::InputText("Normal operating state", condition_audio_state.data(),
                    condition_audio_state.size());
                ImGui::InputText("Familiarity role", condition_audio_familiarity_role.data(),
                    condition_audio_familiarity_role.size());
                ImGui::BeginDisabled(condition.assets.empty());
                if (ImGui::Button("Add audio source descriptor", ImVec2(-1, 28))) {
                    carto::condition::AuthoredAudioSource source;
                    source.asset_id = condition.assets[static_cast<std::size_t>(
                        condition_audio_asset_index)].id;
                    source.clip_source_id = clip_sources[static_cast<std::size_t>(
                        condition_audio_clip_index)]->id;
                    source.local_transform.translation = {
                        condition_audio_x, condition_audio_y, condition_audio_z};
                    source.gain = condition_audio_gain;
                    source.pitch = condition_audio_pitch;
                    source.loop = condition_audio_loop;
                    source.spatial = condition_audio_spatial;
                    source.enabled = condition_audio_enabled;
                    source.minimum_distance = condition_audio_minimum_distance;
                    source.maximum_distance = condition_audio_maximum_distance;
                    source.priority = static_cast<std::uint16_t>(condition_audio_priority);
                    source.bus = audio_bus_values[static_cast<std::size_t>(condition_audio_bus_index)];
                    source.occlusion = audio_occlusion_values[
                        static_cast<std::size_t>(condition_audio_occlusion_index)];
                    source.reverb_send = condition_audio_reverb_send;
                    source.normal_operating_state = condition_audio_state.data();
                    source.familiarity_role = condition_audio_familiarity_role.data();
                    const auto added = ui.add_condition_audio_source(std::move(source));
                    action_feedback = added
                        ? "Added revision-bound CONDITION audio-source intent."
                        : added.error().message;
                    action_feedback_error = !added;
                }
                ImGui::EndDisabled();
            } else {
                ImGui::TextDisabled(
                    "No reviewed, current imported clip evidence is available; no placeholder clip is fabricated.");
            }
            for (const auto& source : condition.audio_sources) {
                ImGui::BulletText("Audio %llu · asset %llu · clip source %llu · %s · %.2f · %.1f–%.1f m%s",
                    static_cast<unsigned long long>(source.id),
                    static_cast<unsigned long long>(source.asset_id),
                    static_cast<unsigned long long>(source.clip_source_id), source.bus.c_str(),
                    source.gain, source.minimum_distance, source.maximum_distance,
                    source.enabled ? "" : " · disabled");
                ImGui::SameLine();
                const std::string toggle_id = "Audio source state##" +
                    std::to_string(source.id);
                if (ImGui::SmallButton(toggle_id.c_str())) {
                    const auto toggled = ui.set_condition_audio_source_enabled(
                        source.id, condition.authored_revision, !source.enabled);
                    action_feedback = toggled
                        ? (source.enabled
                            ? "Disabled audio source; authored failure variant is active."
                            : "Re-enabled audio source.")
                        : toggled.error().message;
                    action_feedback_error = !toggled;
                }
            }

            ImGui::Separator();
            ImGui::TextDisabled("Acoustic zone descriptor");
            if (!condition_asset_labels.empty()) {
                condition_zone_asset_index = std::clamp(condition_zone_asset_index, 0,
                    static_cast<int>(condition_asset_labels.size()) - 1);
                ImGui::Combo("Zone owner asset", &condition_zone_asset_index,
                    condition_asset_labels.data(),
                    static_cast<int>(condition_asset_labels.size()));
            }
            constexpr std::array<const char*, 2> zone_shape_labels{"Box", "Sphere"};
            condition_zone_shape_index = std::clamp(condition_zone_shape_index, 0, 1);
            ImGui::Combo("Zone shape", &condition_zone_shape_index,
                zone_shape_labels.data(), static_cast<int>(zone_shape_labels.size()));
            ImGui::InputDouble("Zone center X", &condition_zone_center_x, 0.1, 1.0, "%.3f");
            ImGui::InputDouble("Zone center Y", &condition_zone_center_y, 0.1, 1.0, "%.3f");
            ImGui::InputDouble("Zone center Z", &condition_zone_center_z, 0.1, 1.0, "%.3f");
            if (condition_zone_shape_index == 0) {
                ImGui::InputDouble("Zone half-width", &condition_zone_half_x, 0.1, 1.0, "%.3f");
                ImGui::InputDouble("Zone half-height", &condition_zone_half_y, 0.1, 1.0, "%.3f");
                ImGui::InputDouble("Zone half-depth", &condition_zone_half_z, 0.1, 1.0, "%.3f");
            } else {
                ImGui::InputDouble("Zone radius", &condition_zone_radius, 0.1, 1.0, "%.3f");
            }
            ImGui::InputText("Acoustic class", condition_zone_class.data(), condition_zone_class.size());
            ImGui::InputText("Reverb intent", condition_zone_reverb.data(), condition_zone_reverb.size());
            ImGui::InputDouble("Zone gain modifier", &condition_zone_gain, 0.05, 0.25, "%.3f");
            ImGui::InputInt("Zone priority", &condition_zone_priority);
            condition_zone_priority = std::clamp(condition_zone_priority, 0, 65535);
            ImGui::InputText("Semantic room / region", condition_zone_semantic.data(),
                condition_zone_semantic.size());
            ImGui::BeginDisabled(condition.assets.empty());
            if (ImGui::Button("Add acoustic zone descriptor", ImVec2(-1, 28))) {
                carto::condition::AuthoredAcousticZone zone;
                zone.asset_id = condition.assets[static_cast<std::size_t>(
                    condition_zone_asset_index)].id;
                zone.shape = condition_zone_shape_index == 0
                    ? carto::condition::AcousticZoneShape::box
                    : carto::condition::AcousticZoneShape::sphere;
                zone.local_transform.translation = {
                    condition_zone_center_x, condition_zone_center_y,
                    condition_zone_center_z};
                zone.half_extents = condition_zone_shape_index == 0
                    ? carto::core::Vec3d{
                        condition_zone_half_x, condition_zone_half_y,
                        condition_zone_half_z}
                    : carto::core::Vec3d{};
                zone.radius = condition_zone_shape_index == 0 ? 0.0 : condition_zone_radius;
                zone.acoustic_class = condition_zone_class.data();
                zone.gain_modifier = condition_zone_gain;
                zone.reverb_intent = condition_zone_reverb.data();
                zone.priority = static_cast<std::uint16_t>(condition_zone_priority);
                zone.semantic_region = condition_zone_semantic.data();
                const auto added = ui.add_condition_acoustic_zone(std::move(zone));
                action_feedback = added
                    ? "Added revision-bound acoustic-zone intent."
                    : added.error().message;
                action_feedback_error = !added;
            }
            ImGui::EndDisabled();
            for (const auto& zone : condition.acoustic_zones) {
                ImGui::BulletText("Zone %llu · %s · %s · priority %u · %s%s",
                    static_cast<unsigned long long>(zone.id), zone.shape.c_str(),
                    zone.acoustic_class.c_str(), static_cast<unsigned>(zone.priority),
                    zone.semantic_region.c_str(), zone.enabled ? "" : " · disabled");
            }
            if (!condition.acoustic_zones.empty()) {
                constexpr std::size_t kOverlapCandidateLimit = 128U;
                const auto zone_count = std::min(
                    condition.acoustic_zones.size(), kOverlapCandidateLimit);
                std::size_t overlap_count = 0U;
                std::size_t overlap_rows = 0U;
                for (std::size_t first = 0U; first < zone_count; ++first) {
                    const auto& left = condition.acoustic_zones[first];
                    if (!left.enabled) continue;
                    const double left_radius = left.shape == "sphere"
                        ? left.radius * std::max(left.local_transform.scale.x,
                            left.local_transform.scale.z)
                        : std::hypot(left.half_extents.x * left.local_transform.scale.x,
                            left.half_extents.z * left.local_transform.scale.z);
                    for (std::size_t second = first + 1U; second < zone_count; ++second) {
                        const auto& right = condition.acoustic_zones[second];
                        if (!right.enabled || left.asset_id != right.asset_id) continue;
                        const double right_radius = right.shape == "sphere"
                            ? right.radius * std::max(right.local_transform.scale.x,
                                right.local_transform.scale.z)
                            : std::hypot(right.half_extents.x * right.local_transform.scale.x,
                                right.half_extents.z * right.local_transform.scale.z);
                        const double dx = left.local_transform.translation.x -
                            right.local_transform.translation.x;
                        const double dz = left.local_transform.translation.z -
                            right.local_transform.translation.z;
                        if (std::hypot(dx, dz) > left_radius + right_radius) continue;
                        ++overlap_count;
                        if (overlap_rows < 12U) {
                            const auto higher = left.priority > right.priority ? &left
                                : right.priority > left.priority ? &right : nullptr;
                            if (higher != nullptr) {
                                ImGui::BulletText(
                                    "Possible overlap · zones %llu / %llu · authored P%u has higher priority",
                                    static_cast<unsigned long long>(left.id),
                                    static_cast<unsigned long long>(right.id),
                                    static_cast<unsigned>(higher->priority));
                            } else {
                                ImGui::BulletText(
                                    "Possible overlap · zones %llu / %llu · equal priority",
                                    static_cast<unsigned long long>(left.id),
                                    static_cast<unsigned long long>(right.id));
                            }
                            ++overlap_rows;
                        }
                    }
                }
                if (overlap_count == 0U) {
                    ImGui::TextDisabled("No broad-phase overlap candidates among enabled zones.");
                } else if (overlap_count > overlap_rows) {
                    ImGui::TextDisabled("%zu possible pairs total; showing the first %zu.",
                        overlap_count, overlap_rows);
                }
                if (condition.acoustic_zones.size() > kOverlapCandidateLimit) {
                    ImGui::TextDisabled(
                        "Overlap check is bounded to the first %zu zone records.",
                        kOverlapCandidateLimit);
                }
            }

            ImGui::Separator();
            heading_text("Effect source and deterministic preview");
            ImGui::TextWrapped(
                "Effect previews are ephemeral, deterministic authoring samples. They do not "
                "create runtime pools or change the project; bound spatial fields must match "
                "their recorded revision and digest.");
            if (!condition_asset_labels.empty()) {
                condition_effect_asset_index = std::clamp(condition_effect_asset_index, 0,
                    static_cast<int>(condition_asset_labels.size()) - 1);
                ImGui::Combo("Effect owner asset", &condition_effect_asset_index,
                    condition_asset_labels.data(),
                    static_cast<int>(condition_asset_labels.size()));
            }
            constexpr std::array<const char*, 3> effect_shape_labels{
                "Point", "Box", "Sphere"};
            constexpr std::array<carto::condition::EffectEmissionShape, 3> effect_shape_values{
                carto::condition::EffectEmissionShape::point,
                carto::condition::EffectEmissionShape::box,
                carto::condition::EffectEmissionShape::sphere};
            condition_effect_shape_index = std::clamp(condition_effect_shape_index, 0, 2);
            ImGui::Combo("Emission volume", &condition_effect_shape_index,
                effect_shape_labels.data(), static_cast<int>(effect_shape_labels.size()));
            if (condition_effect_shape_index == 1) {
                ImGui::InputDouble("Volume half-width", &condition_effect_half_x, 0.1, 1.0, "%.3f");
                ImGui::InputDouble("Volume half-height", &condition_effect_half_y, 0.1, 1.0, "%.3f");
                ImGui::InputDouble("Volume half-depth", &condition_effect_half_z, 0.1, 1.0, "%.3f");
            } else if (condition_effect_shape_index == 2) {
                ImGui::InputDouble("Volume radius", &condition_effect_radius, 0.1, 1.0, "%.3f");
            }
            ImGui::InputDouble("Effect origin X", &condition_effect_x, 0.1, 1.0, "%.3f");
            ImGui::InputDouble("Effect origin Y", &condition_effect_y, 0.1, 1.0, "%.3f");
            ImGui::InputDouble("Effect origin Z", &condition_effect_z, 0.1, 1.0, "%.3f");
            ImGui::InputText("Effect semantic owner", condition_effect_owner.data(),
                condition_effect_owner.size());
            ImGui::InputText("Normal operating state", condition_effect_normal_state.data(),
                condition_effect_normal_state.size());
            ImGui::InputText("Disabled operating state", condition_effect_disabled_state.data(),
                condition_effect_disabled_state.size());
            constexpr std::array<const char*, 2> effect_intent_labels{
                "Physical state", "Presentation only"};
            condition_effect_intent_index = std::clamp(condition_effect_intent_index, 0, 1);
            ImGui::Combo("Effect consequence", &condition_effect_intent_index,
                effect_intent_labels.data(), static_cast<int>(effect_intent_labels.size()));
            ImGui::BeginDisabled(condition.assets.empty());
            if (ImGui::Button("Add effect source", ImVec2(-1, 28))) {
                carto::condition::EffectSource source;
                source.asset_id = condition.assets[static_cast<std::size_t>(
                    condition_effect_asset_index)].id;
                source.semantic_owner = condition_effect_owner.data();
                source.shape = effect_shape_values[static_cast<std::size_t>(
                    condition_effect_shape_index)];
                source.local_transform.translation = {
                    condition_effect_x, condition_effect_y, condition_effect_z};
                source.half_extents = condition_effect_shape_index == 1
                    ? carto::core::Vec3d{condition_effect_half_x, condition_effect_half_y,
                        condition_effect_half_z}
                    : carto::core::Vec3d{};
                source.radius = condition_effect_shape_index == 2
                    ? condition_effect_radius : 0.0;
                source.intent_class = condition_effect_intent_index == 0
                    ? carto::condition::EffectIntentClass::physical_state
                    : carto::condition::EffectIntentClass::presentation_only;
                source.normal_operating_state = condition_effect_normal_state.data();
                source.disabled_operating_state = condition_effect_disabled_state.data();
                const auto added = ui.add_condition_effect_source(std::move(source));
                action_feedback = added
                    ? "Added revision-bound CONDITION effect source."
                    : added.error().message;
                action_feedback_error = !added;
            }
            ImGui::EndDisabled();

            std::vector<std::string> effect_labels;
            std::vector<const char*> effect_label_pointers;
            effect_labels.reserve(condition.effect_sources.size());
            for (const auto& effect : condition.effect_sources) {
                effect_labels.push_back(std::to_string(effect.id) + " · " +
                    effect.semantic_owner + " · asset " + std::to_string(effect.asset_id));
            }
            effect_label_pointers.reserve(effect_labels.size());
            for (const auto& label : effect_labels) effect_label_pointers.push_back(label.c_str());
            if (!effect_label_pointers.empty()) {
                condition_effect_source_index = std::clamp(condition_effect_source_index, 0,
                    static_cast<int>(effect_label_pointers.size()) - 1);
                ImGui::Combo("Emitter owner effect", &condition_effect_source_index,
                    effect_label_pointers.data(), static_cast<int>(effect_label_pointers.size()));
                ImGui::InputDouble("Emission rate / second", &condition_emitter_rate,
                    0.5, 5.0, "%.3f");
                ImGui::InputDouble("Particle lifetime seconds", &condition_emitter_lifetime,
                    0.1, 1.0, "%.3f");
                ImGui::InputDouble("Emission duration seconds", &condition_emitter_duration,
                    0.1, 1.0, "%.3f");
                ImGui::InputDouble("Initial velocity Y", &condition_emitter_velocity_y,
                    0.1, 1.0, "%.3f");
                ImGui::InputInt("Particle preview bound", &condition_effect_particle_bound);
                condition_effect_particle_bound = std::clamp(condition_effect_particle_bound, 1, 2048);
                ImGui::InputInt("Deterministic seed", &condition_effect_seed);
                if (ImGui::Button("Add particle emitter", ImVec2(-1, 28))) {
                    carto::condition::ParticleEmitterSource emitter;
                    emitter.effect_source_id = condition.effect_sources[
                        static_cast<std::size_t>(condition_effect_source_index)].id;
                    emitter.emission_rate_per_second = condition_emitter_rate;
                    emitter.particle_lifetime_seconds = condition_emitter_lifetime;
                    emitter.duration_seconds = condition_emitter_duration;
                    emitter.initial_velocity.y = condition_emitter_velocity_y;
                    emitter.fixed_timestep_seconds = 1.0 / 60.0;
                    emitter.total_particle_bound = static_cast<std::uint32_t>(
                        condition_effect_particle_bound);
                    emitter.deterministic_seed = condition_effect_seed <= 0
                        ? 1U : static_cast<std::uint64_t>(condition_effect_seed);
                    const auto added = ui.add_condition_particle_emitter(std::move(emitter));
                    action_feedback = added
                        ? "Added revision-bound deterministic particle emitter."
                        : added.error().message;
                    action_feedback_error = !added;
                }

                ImGui::Separator();
                ImGui::TextDisabled("Bind an exact project field as authored influence");
                std::vector<std::string> field_labels;
                std::vector<const char*> field_label_pointers;
                field_labels.reserve(snapshot.spatial_fields.size());
                for (const auto& field : snapshot.spatial_fields) {
                    field_labels.push_back(field.identity + " · " + std::string(
                        carto::spatial_field::field_semantic_name(field.semantic)));
                }
                field_label_pointers.reserve(field_labels.size());
                for (const auto& label : field_labels) field_label_pointers.push_back(label.c_str());
                if (!field_label_pointers.empty()) {
                    condition_influence_field_index = std::clamp(condition_influence_field_index, 0,
                        static_cast<int>(field_label_pointers.size()) - 1);
                    ImGui::Combo("Influence field", &condition_influence_field_index,
                        field_label_pointers.data(), static_cast<int>(field_label_pointers.size()));
                    constexpr std::array<const char*, 5> influence_kind_labels{
                        "Wind", "Thermal lift", "Drainage", "Avoidance", "Turbulence"};
                    constexpr std::array<carto::condition::EffectInfluenceKind, 5>
                        influence_kind_values{
                            carto::condition::EffectInfluenceKind::wind,
                            carto::condition::EffectInfluenceKind::thermal_lift,
                            carto::condition::EffectInfluenceKind::drainage,
                            carto::condition::EffectInfluenceKind::avoidance,
                            carto::condition::EffectInfluenceKind::turbulence};
                    condition_influence_kind_index = std::clamp(
                        condition_influence_kind_index, 0, 4);
                    ImGui::Combo("Influence kind", &condition_influence_kind_index,
                        influence_kind_labels.data(),
                        static_cast<int>(influence_kind_labels.size()));
                    ImGui::InputDouble("Influence strength", &condition_influence_strength,
                        0.05, 0.25, "%.3f");
                    if (ImGui::Button("Bind field to selected effect", ImVec2(-1, 28))) {
                        const auto& field = snapshot.spatial_fields[
                            static_cast<std::size_t>(condition_influence_field_index)];
                        carto::condition::FieldInfluenceBinding binding;
                        binding.effect_source_id = condition.effect_sources[
                            static_cast<std::size_t>(condition_effect_source_index)].id;
                        binding.kind = influence_kind_values[
                            static_cast<std::size_t>(condition_influence_kind_index)];
                        binding.field_identity = field.identity;
                        binding.expected_field_revision = field.source_revision;
                        binding.expected_field_digest = field.digest;
                        binding.strength = condition_influence_strength;
                        const auto added = ui.add_condition_field_influence(std::move(binding));
                        action_feedback = added
                            ? "Bound CONDITION effect to the exact project field revision and digest."
                            : added.error().message;
                        action_feedback_error = !added;
                    }
                } else {
                    ImGui::TextDisabled("Create a project spatial field before binding influences.");
                }

                ImGui::InputDouble("Preview elapsed seconds", &condition_preview_elapsed,
                    0.1, 1.0, "%.3f");
                for (const auto& effect : condition.effect_sources) {
                    const std::string id = std::to_string(effect.id);
                    ImGui::PushID(id.c_str());
                    ImGui::Text("Effect %s · %s · asset %llu", id.c_str(),
                        effect.semantic_owner.c_str(),
                        static_cast<unsigned long long>(effect.asset_id));
                    if (ImGui::SmallButton("Preview normal state")) {
                        const auto preview = ui.preview_condition_effect(
                            effect.id, effect.normal_state, condition_preview_elapsed);
                        if (preview) {
                            condition_effect_preview_result = preview.value();
                            action_feedback = "Deterministic effect preview ready: " +
                                std::to_string(preview.value().particles.size()) +
                                " ephemeral particles.";
                            action_feedback_error = false;
                        } else {
                            condition_effect_preview_result.reset();
                            action_feedback = preview.error().message;
                            action_feedback_error = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Preview disabled state")) {
                        const auto preview = ui.preview_condition_effect(
                            effect.id, effect.disabled_state, condition_preview_elapsed);
                        if (preview) {
                            condition_effect_preview_result = preview.value();
                            action_feedback = "Disabled-state preview ready; no particles emitted.";
                            action_feedback_error = false;
                        } else {
                            condition_effect_preview_result.reset();
                            action_feedback = preview.error().message;
                            action_feedback_error = true;
                        }
                    }
                    ImGui::PopID();
                }
                if (condition_effect_preview_result.has_value()) {
                    const auto& preview = *condition_effect_preview_result;
                    bool preview_current = preview.authoring_revision ==
                        condition.authored_revision && preview.authoring_digest == condition.digest;
                    for (const auto& receipt : preview.field_receipts) {
                        const auto field = std::find_if(snapshot.spatial_fields.begin(),
                            snapshot.spatial_fields.end(), [&receipt](const auto& candidate) {
                                return candidate.identity == receipt.identity;
                            });
                        preview_current = preview_current && field != snapshot.spatial_fields.end() &&
                            field->source_revision == receipt.revision && field->digest == receipt.digest;
                    }
                    if (!preview_current) {
                        ImGui::TextColored(color(snapshot.colors.semantic_warning),
                            "Preview inputs changed; run the preview again.");
                    } else {
                        ImGui::TextDisabled("Preview %s · %zu points · r%llu",
                            preview.operating_state.c_str(), preview.particles.size(),
                            static_cast<unsigned long long>(preview.authoring_revision.value()));
                        mono_text_disabled(preview.preview_digest.hex().c_str());
                        const ImVec2 canvas_size{ImGui::GetContentRegionAvail().x, 126.0F};
                        ImGui::InvisibleButton("CONDITION effect preview canvas", canvas_size);
                        const ImVec2 canvas_min = ImGui::GetItemRectMin();
                        const ImVec2 canvas_max = ImGui::GetItemRectMax();
                        auto* draw_list = ImGui::GetWindowDrawList();
                        draw_list->AddRectFilled(canvas_min, canvas_max,
                            IM_COL32(9, 16, 26, 255), 3.0F);
                        draw_list->AddRect(canvas_min, canvas_max,
                            IM_COL32(82, 112, 145, 180), 3.0F);
                        if (!preview.particles.empty()) {
                            double min_x = preview.particles.front().position.x;
                            double max_x = min_x;
                            double min_z = preview.particles.front().position.z;
                            double max_z = min_z;
                            for (const auto& particle : preview.particles) {
                                min_x = std::min(min_x, particle.position.x);
                                max_x = std::max(max_x, particle.position.x);
                                min_z = std::min(min_z, particle.position.z);
                                max_z = std::max(max_z, particle.position.z);
                            }
                            const double range_x = std::max(0.01, max_x - min_x);
                            const double range_z = std::max(0.01, max_z - min_z);
                            for (const auto& particle : preview.particles) {
                                const float x = static_cast<float>((particle.position.x - min_x) /
                                    range_x) * (canvas_max.x - canvas_min.x - 16.0F) +
                                    canvas_min.x + 8.0F;
                                const float y = static_cast<float>((particle.position.z - min_z) /
                                    range_z) * (canvas_max.y - canvas_min.y - 16.0F) +
                                    canvas_min.y + 8.0F;
                                draw_list->AddCircleFilled(ImVec2(x, y), 3.0F,
                                    IM_COL32(77, 167, 255, 235));
                            }
                        } else {
                            draw_list->AddText(ImVec2(canvas_min.x + 10.0F, canvas_min.y + 10.0F),
                                IM_COL32(155, 170, 190, 255), "No particles in this authored state/time");
                        }
                    }
                }
            } else {
                ImGui::TextDisabled("Add an effect source to author its deterministic emitter.");
            }
            ImGui::Separator();
            heading_text("Field-driven vegetation placement");
            ImGui::TextWrapped(
                "A scatter recipe binds moisture, shade, disturbance, suitability, and elevation "
                "sources exactly. Preview points are derived only; they are not placed into the scene.");
            constexpr std::array<carto::spatial_field::FieldSemantic, 5> vegetation_semantics{
                carto::spatial_field::FieldSemantic::moisture,
                carto::spatial_field::FieldSemantic::shade,
                carto::spatial_field::FieldSemantic::disturbance,
                carto::spatial_field::FieldSemantic::vegetation_suitability,
                carto::spatial_field::FieldSemantic::terrain_elevation};
            constexpr std::array<const char*, 5> vegetation_field_labels{
                "Moisture field", "Shade field", "Disturbance field",
                "Species suitability field", "Terrain elevation field"};
            std::array<std::vector<const carto::application::SpatialFieldView*>, 5>
                vegetation_fields;
            for (std::size_t semantic_index = 0U;
                 semantic_index < vegetation_semantics.size(); ++semantic_index) {
                for (const auto& field : snapshot.spatial_fields) {
                    if (field.semantic == vegetation_semantics[semantic_index]) {
                        vegetation_fields[semantic_index].push_back(&field);
                    }
                }
            }
            std::vector<std::size_t> vegetation_assets;
            std::vector<std::string> vegetation_asset_names;
            std::vector<const char*> vegetation_asset_labels;
            for (std::size_t asset_index = 0U;
                 asset_index < condition.assets.size(); ++asset_index) {
                if (condition.assets[asset_index].asset_class == "vegetation") {
                    vegetation_assets.push_back(asset_index);
                    vegetation_asset_names.push_back(condition.assets[asset_index].stable_name +
                        " · " + std::to_string(condition.assets[asset_index].id));
                }
            }
            for (const auto& label : vegetation_asset_names) {
                vegetation_asset_labels.push_back(label.c_str());
            }
            if (!vegetation_asset_labels.empty()) {
                condition_vegetation_asset_index = std::clamp(
                    condition_vegetation_asset_index, 0,
                    static_cast<int>(vegetation_asset_labels.size()) - 1);
                ImGui::Combo("Vegetation species", &condition_vegetation_asset_index,
                    vegetation_asset_labels.data(),
                    static_cast<int>(vegetation_asset_labels.size()));
            } else {
                ImGui::TextDisabled("Import or classify a source asset as vegetation first.");
            }
            bool all_vegetation_fields_available = true;
            for (std::size_t field_index = 0U;
                 field_index < vegetation_fields.size(); ++field_index) {
                const auto& choices = vegetation_fields[field_index];
                if (choices.empty()) {
                    all_vegetation_fields_available = false;
                    ImGui::TextDisabled("%s: no exact-semantic project field",
                        vegetation_field_labels[field_index]);
                    continue;
                }
                std::vector<std::string> labels;
                std::vector<const char*> label_pointers;
                labels.reserve(choices.size());
                for (const auto* field : choices) {
                    labels.push_back(field->identity + " · r" +
                        std::to_string(field->source_revision.value()));
                }
                label_pointers.reserve(labels.size());
                for (const auto& label : labels) label_pointers.push_back(label.c_str());
                condition_vegetation_field_indices[field_index] = std::clamp(
                    condition_vegetation_field_indices[field_index], 0,
                    static_cast<int>(label_pointers.size()) - 1);
                const std::string combo_id = std::string(vegetation_field_labels[field_index]) +
                    "##vegetation-field-" + std::to_string(field_index);
                ImGui::Combo(combo_id.c_str(),
                    &condition_vegetation_field_indices[field_index],
                    label_pointers.data(), static_cast<int>(label_pointers.size()));
            }
            ImGui::InputDouble("Cell spacing (m)", &condition_vegetation_spacing,
                0.25, 1.0, "%.2f");
            ImGui::InputDouble("Density / m²", &condition_vegetation_density,
                0.01, 0.1, "%.3f");
            ImGui::InputDouble("Minimum suitability", &condition_vegetation_threshold,
                0.05, 0.1, "%.3f");
            ImGui::InputDouble("Minimum scale", &condition_vegetation_min_scale,
                0.05, 0.1, "%.3f");
            ImGui::InputDouble("Maximum scale", &condition_vegetation_max_scale,
                0.05, 0.1, "%.3f");
            ImGui::InputInt("Scatter seed", &condition_vegetation_seed);
            std::uint64_t next_vegetation_recipe_id = 1U;
            bool vegetation_recipe_id_available = true;
            if (!condition.vegetation_scatter_ids.empty()) {
                const auto maximum_id = *std::max_element(
                    condition.vegetation_scatter_ids.begin(),
                    condition.vegetation_scatter_ids.end());
                if (maximum_id == std::numeric_limits<std::uint64_t>::max()) {
                    vegetation_recipe_id_available = false;
                } else {
                    next_vegetation_recipe_id = maximum_id + 1U;
                }
            }
            const bool can_author_scatter = !vegetation_asset_labels.empty() &&
                all_vegetation_fields_available && vegetation_recipe_id_available;
            ImGui::BeginDisabled(!can_author_scatter);
            if (ImGui::Button("Add field-bound scatter recipe", ImVec2(-1, 28))) {
                carto::condition::VegetationScatterRecipe recipe;
                recipe.id = next_vegetation_recipe_id;
                const auto asset_index = vegetation_assets[
                    static_cast<std::size_t>(condition_vegetation_asset_index)];
                recipe.species_asset_id = condition.assets[asset_index].id;
                const auto make_binding = [this, &vegetation_fields](std::size_t index) {
                    const auto& field = *vegetation_fields[index][static_cast<std::size_t>(
                        condition_vegetation_field_indices[index])];
                    return carto::condition::FieldSourceBinding{
                        field.identity, field.source_revision, field.digest};
                };
                recipe.moisture = make_binding(0U);
                recipe.shade = make_binding(1U);
                recipe.disturbance = make_binding(2U);
                recipe.suitability = make_binding(3U);
                recipe.elevation = make_binding(4U);
                recipe.cell_spacing = condition_vegetation_spacing;
                recipe.density_per_square_meter = condition_vegetation_density;
                recipe.minimum_suitability = condition_vegetation_threshold;
                recipe.minimum_scale = condition_vegetation_min_scale;
                recipe.maximum_scale = condition_vegetation_max_scale;
                recipe.deterministic_seed = condition_vegetation_seed <= 0
                    ? 1U : static_cast<std::uint64_t>(condition_vegetation_seed);
                const auto added = ui.add_condition_vegetation_scatter(std::move(recipe));
                action_feedback = added
                    ? "Added revision-bound field-driven vegetation scatter recipe."
                    : added.error().message;
                action_feedback_error = !added;
            }
            ImGui::EndDisabled();
            if (!vegetation_recipe_id_available) {
                ImGui::TextColored(color(snapshot.colors.semantic_warning),
                    "Recipe identity space is exhausted.");
            }
            for (const auto recipe_id : condition.vegetation_scatter_ids) {
                const std::string recipe_id_text = std::to_string(recipe_id);
                ImGui::PushID(recipe_id_text.c_str());
                ImGui::Text("Scatter recipe %llu", static_cast<unsigned long long>(recipe_id));
                if (ImGui::SmallButton("Preview placement")) {
                    const auto preview = ui.preview_condition_vegetation(recipe_id);
                    if (preview) {
                        condition_vegetation_preview_result = preview.value();
                        action_feedback = "Deterministic vegetation preview ready: " +
                            std::to_string(preview.value().instances.size()) +
                            " derived placements; scene unchanged.";
                        action_feedback_error = false;
                    } else {
                        condition_vegetation_preview_result.reset();
                        action_feedback = preview.error().message;
                        action_feedback_error = true;
                    }
                }
                ImGui::PopID();
            }
            if (condition_vegetation_preview_result.has_value()) {
                const auto& preview = *condition_vegetation_preview_result;
                bool preview_current = preview.project_revision == condition.authored_revision &&
                    preview.authoring_digest == condition.digest;
                for (const auto& receipt : preview.fields) {
                    const auto field = std::find_if(snapshot.spatial_fields.begin(),
                        snapshot.spatial_fields.end(), [&receipt](const auto& candidate) {
                            return candidate.identity == receipt.identity;
                        });
                    preview_current = preview_current && field != snapshot.spatial_fields.end() &&
                        field->semantic == receipt.semantic &&
                        field->source_revision == receipt.source_revision &&
                        field->digest == receipt.digest;
                }
                if (!preview_current) {
                    ImGui::TextColored(color(snapshot.colors.semantic_warning),
                        "Scatter preview inputs changed; regenerate before relying on it.");
                } else {
                    ImGui::TextDisabled("%zu derived placements from %zu candidates · preview only",
                        preview.instances.size(), preview.candidate_count);
                    mono_text_disabled(preview.preview_digest.hex().c_str());
                    const ImVec2 canvas_size{ImGui::GetContentRegionAvail().x, 126.0F};
                    ImGui::InvisibleButton("CONDITION vegetation preview canvas", canvas_size);
                    const ImVec2 canvas_min = ImGui::GetItemRectMin();
                    const ImVec2 canvas_max = ImGui::GetItemRectMax();
                    auto* draw_list = ImGui::GetWindowDrawList();
                    draw_list->AddRectFilled(canvas_min, canvas_max,
                        IM_COL32(9, 16, 26, 255), 3.0F);
                    draw_list->AddRect(canvas_min, canvas_max,
                        IM_COL32(82, 112, 145, 180), 3.0F);
                    if (!preview.instances.empty()) {
                        double min_x = preview.instances.front().position.x;
                        double max_x = min_x;
                        double min_z = preview.instances.front().position.z;
                        double max_z = min_z;
                        for (const auto& instance : preview.instances) {
                            min_x = std::min(min_x, instance.position.x);
                            max_x = std::max(max_x, instance.position.x);
                            min_z = std::min(min_z, instance.position.z);
                            max_z = std::max(max_z, instance.position.z);
                        }
                        const double range_x = std::max(0.01, max_x - min_x);
                        const double range_z = std::max(0.01, max_z - min_z);
                        for (const auto& instance : preview.instances) {
                            const float x = static_cast<float>((instance.position.x - min_x) /
                                range_x) * (canvas_max.x - canvas_min.x - 16.0F) +
                                canvas_min.x + 8.0F;
                            const float y = static_cast<float>((instance.position.z - min_z) /
                                range_z) * (canvas_max.y - canvas_min.y - 16.0F) +
                                canvas_min.y + 8.0F;
                            draw_list->AddCircleFilled(ImVec2(x, y), 3.0F,
                                IM_COL32(126, 190, 114, 235));
                        }
                    } else {
                        draw_list->AddText(ImVec2(canvas_min.x + 10.0F, canvas_min.y + 10.0F),
                            IM_COL32(155, 170, 190, 255),
                            "No placements satisfy this exact field-bound recipe");
                    }
                }
            }
            ImGui::Separator();
            heading_text("Safe Place state branches");
            ImGui::TextWrapped(
                "Branches preserve the source baseline. Compare alternatives before any explicit resolution; "
                "contested edits remain visible and are never auto-merged.");
            constexpr std::array<carto::condition::StateProperty, 8> safe_place_properties{
                carto::condition::StateProperty::privacy,
                carto::condition::StateProperty::human_proximity,
                carto::condition::StateProperty::controllable_entries,
                carto::condition::StateProperty::supply_access,
                carto::condition::StateProperty::ambient_noise,
                carto::condition::StateProperty::visibility_outward,
                carto::condition::StateProperty::visibility_inward,
                carto::condition::StateProperty::isolation};
            constexpr std::array<const char*, 8> safe_place_property_labels{
                "Privacy", "Human proximity", "Controllable entries", "Supply access",
                "Ambient noise", "Visibility outward", "Visibility inward", "Isolation"};
            constexpr const char* state_target_labels[]{"Safe Place profile", "Surface state"};
            const bool has_safe_place_states = !condition.safe_places.empty();
            const bool has_surface_states = !condition.surface_states.empty();
            if (!has_safe_place_states && has_surface_states) {
                condition_state_target_kind_index = 1;
            }
            ImGui::Combo("Branch target", &condition_state_target_kind_index,
                state_target_labels, 2);
            const bool surface_target = condition_state_target_kind_index == 1;
            const bool target_records_available = surface_target
                ? has_surface_states : has_safe_place_states;
            carto::condition::AuthoredStateDelta state_branch_delta;
            carto::condition::StateValue state_branch_before;
            carto::condition::StateValue state_branch_after;
            bool state_branch_target_current = target_records_available;
            if (surface_target && has_surface_states) {
                condition_surface_state_index = std::clamp(
                    condition_surface_state_index, 0,
                    static_cast<int>(condition.surface_states.size()) - 1);
                std::vector<std::string> surface_labels;
                std::vector<const char*> surface_label_pointers;
                surface_labels.reserve(condition.surface_states.size());
                for (const auto& surface_state : condition.surface_states) {
                    const auto asset = std::find_if(condition.assets.begin(), condition.assets.end(),
                        [&surface_state](const auto& item) {
                            return item.id == surface_state.asset_id;
                        });
                    surface_labels.push_back("Surface " + std::to_string(surface_state.id) + " · " +
                        (asset == condition.assets.end() ? std::to_string(surface_state.asset_id)
                                                         : asset->stable_name));
                }
                surface_label_pointers.reserve(surface_labels.size());
                for (const auto& label : surface_labels) {
                    surface_label_pointers.push_back(label.c_str());
                }
                ImGui::Combo("Surface baseline", &condition_surface_state_index,
                    surface_label_pointers.data(),
                    static_cast<int>(surface_label_pointers.size()));
                constexpr std::array<carto::condition::StateProperty, 4> surface_properties{
                    carto::condition::StateProperty::occupancy,
                    carto::condition::StateProperty::maintenance,
                    carto::condition::StateProperty::damage,
                    carto::condition::StateProperty::wetness};
                constexpr const char* surface_property_labels[]{
                    "Occupancy", "Maintenance", "Damage", "Wetness"};
                condition_surface_state_property_index = std::clamp(
                    condition_surface_state_property_index, 0, 3);
                ImGui::Combo("State property", &condition_surface_state_property_index,
                    surface_property_labels, 4);
                const auto& surface_state = condition.surface_states[static_cast<std::size_t>(
                    condition_surface_state_index)];
                state_branch_delta.target_record_id = surface_state.id;
                state_branch_delta.target_asset_id = surface_state.asset_id;
                state_branch_delta.target_kind =
                    carto::condition::StateTargetKind::surface_state;
                state_branch_delta.property = surface_properties[static_cast<std::size_t>(
                    condition_surface_state_property_index)];
                switch (state_branch_delta.property) {
                case carto::condition::StateProperty::occupancy:
                    state_branch_before.text_value = surface_state.occupancy;
                    break;
                case carto::condition::StateProperty::maintenance:
                    state_branch_before.text_value = surface_state.maintenance;
                    break;
                case carto::condition::StateProperty::damage:
                    state_branch_before.text_value = surface_state.damage;
                    break;
                case carto::condition::StateProperty::wetness:
                    state_branch_before.numeric_value = surface_state.wetness;
                    break;
                default:
                    state_branch_target_current = false;
                    break;
                }
                const int property_index = condition_surface_state_property_index;
                const bool surface_value_selection_changed =
                    condition_surface_enum_record_id != surface_state.id ||
                    condition_surface_enum_property_index != property_index ||
                    condition_surface_enum_source_value != state_branch_before.text_value;
                if (state_branch_delta.property == carto::condition::StateProperty::wetness) {
                    const bool numeric_source_changed =
                        condition_surface_enum_record_id != surface_state.id ||
                        condition_surface_enum_property_index != property_index ||
                        condition_surface_numeric_source_value != state_branch_before.numeric_value;
                    if (numeric_source_changed) {
                        condition_state_branch_value = state_branch_before.numeric_value;
                        condition_surface_enum_record_id = surface_state.id;
                        condition_surface_enum_property_index = property_index;
                        condition_surface_numeric_source_value = state_branch_before.numeric_value;
                    }
                    ImGui::TextDisabled("Source baseline is %.3f",
                        state_branch_before.numeric_value);
                    ImGui::InputDouble("Alternative value", &condition_state_branch_value,
                        0.05, 0.1, "%.3f");
                    state_branch_after.numeric_value = condition_state_branch_value;
                } else {
                    std::vector<std::string> enum_values;
                    if (state_branch_delta.property == carto::condition::StateProperty::occupancy) {
                        constexpr std::array values{
                            carto::condition::OccupancyState::empty,
                            carto::condition::OccupancyState::recently_used,
                            carto::condition::OccupancyState::actively_occupied,
                            carto::condition::OccupancyState::long_term_occupied,
                            carto::condition::OccupancyState::abandoned};
                        for (const auto value : values) {
                            enum_values.emplace_back(carto::condition::occupancy_state_name(value));
                        }
                    } else if (state_branch_delta.property == carto::condition::StateProperty::maintenance) {
                        constexpr std::array values{
                            carto::condition::MaintenanceState::maintained,
                            carto::condition::MaintenanceState::aging,
                            carto::condition::MaintenanceState::neglected,
                            carto::condition::MaintenanceState::improvised_repair,
                            carto::condition::MaintenanceState::partially_renovated,
                            carto::condition::MaintenanceState::condemned};
                        for (const auto value : values) {
                            enum_values.emplace_back(carto::condition::maintenance_state_name(value));
                        }
                    } else {
                        constexpr std::array values{
                            carto::condition::DamageState::intact,
                            carto::condition::DamageState::broken_window,
                            carto::condition::DamageState::damaged_door,
                            carto::condition::DamageState::dented_sign,
                            carto::condition::DamageState::collapsed_shelf,
                            carto::condition::DamageState::cracked_wall,
                            carto::condition::DamageState::damaged_railing,
                            carto::condition::DamageState::roof_damage,
                            carto::condition::DamageState::collision_damage,
                            carto::condition::DamageState::burned};
                        for (const auto value : values) {
                            enum_values.emplace_back(carto::condition::damage_state_name(value));
                        }
                    }
                    if (surface_value_selection_changed) {
                        const auto current = std::find(enum_values.begin(), enum_values.end(),
                            state_branch_before.text_value);
                        condition_surface_enum_choice = current == enum_values.end() ? 0
                            : static_cast<int>(std::distance(enum_values.begin(), current));
                        condition_surface_enum_record_id = surface_state.id;
                        condition_surface_enum_property_index = property_index;
                        condition_surface_enum_source_value = state_branch_before.text_value;
                    }
                    std::vector<const char*> enum_value_pointers;
                    enum_value_pointers.reserve(enum_values.size());
                    for (const auto& value : enum_values) enum_value_pointers.push_back(value.c_str());
                    if (!enum_values.empty()) {
                        condition_surface_enum_choice = std::clamp(condition_surface_enum_choice,
                            0, static_cast<int>(enum_values.size()) - 1);
                        ImGui::TextDisabled("Source baseline is %s",
                            state_branch_before.text_value.c_str());
                        ImGui::Combo("Alternative state", &condition_surface_enum_choice,
                            enum_value_pointers.data(), static_cast<int>(enum_value_pointers.size()));
                        state_branch_after.text_value = enum_values[
                            static_cast<std::size_t>(condition_surface_enum_choice)];
                    }
                }
            } else if (!surface_target && has_safe_place_states) {
                condition_safe_place_profile_index = std::clamp(
                    condition_safe_place_profile_index, 0,
                    static_cast<int>(condition.safe_places.size()) - 1);
                std::vector<std::string> safe_place_labels;
                std::vector<const char*> safe_place_label_pointers;
                safe_place_labels.reserve(condition.safe_places.size());
                for (const auto& profile : condition.safe_places) {
                    const auto asset = std::find_if(condition.assets.begin(), condition.assets.end(),
                        [&profile](const auto& item) { return item.id == profile.asset_id; });
                    safe_place_labels.push_back("Profile " + std::to_string(profile.id) + " · " +
                        (asset == condition.assets.end() ? std::to_string(profile.asset_id)
                                                         : asset->stable_name));
                }
                safe_place_label_pointers.reserve(safe_place_labels.size());
                for (const auto& label : safe_place_labels) {
                    safe_place_label_pointers.push_back(label.c_str());
                }
                ImGui::Combo("Safe Place baseline",
                    &condition_safe_place_profile_index,
                    safe_place_label_pointers.data(),
                    static_cast<int>(safe_place_label_pointers.size()));
                condition_safe_place_property_index = std::clamp(
                    condition_safe_place_property_index, 0, 7);
                ImGui::Combo("State property", &condition_safe_place_property_index,
                    safe_place_property_labels.data(),
                    static_cast<int>(safe_place_property_labels.size()));
                const auto& profile = condition.safe_places[static_cast<std::size_t>(
                    condition_safe_place_profile_index)];
                state_branch_delta.target_record_id = profile.id;
                state_branch_delta.target_asset_id = profile.asset_id;
                state_branch_delta.target_kind =
                    carto::condition::StateTargetKind::safe_place_profile;
                state_branch_delta.property = safe_place_properties[static_cast<std::size_t>(
                    condition_safe_place_property_index)];
                const double baseline_value = profile.values[
                    static_cast<std::size_t>(condition_safe_place_property_index)];
                ImGui::TextDisabled("Baseline value %.3f", baseline_value);
                state_branch_before.numeric_value = baseline_value;
                const int selected_profile = condition_safe_place_profile_index;
                const int selected_property = condition_safe_place_property_index;
                static int previous_safe_place_profile = -1;
                static int previous_safe_place_property = -1;
                static double previous_safe_place_baseline = -1.0;
                if (previous_safe_place_profile != selected_profile ||
                    previous_safe_place_property != selected_property ||
                    previous_safe_place_baseline != baseline_value) {
                    condition_state_branch_value = baseline_value;
                    previous_safe_place_profile = selected_profile;
                    previous_safe_place_property = selected_property;
                    previous_safe_place_baseline = baseline_value;
                }
                ImGui::InputDouble("Alternative value", &condition_state_branch_value,
                    0.05, 0.1, "%.3f");
                state_branch_after.numeric_value = condition_state_branch_value;
            }
            if (!target_records_available) {
                ImGui::TextDisabled(surface_target
                    ? "Import a surface-state record before branching surface history."
                    : "Import a Safe Place profile before creating place states.");
            }
            ImGui::InputText("Branch name", condition_state_branch_name.data(),
                condition_state_branch_name.size());
            ImGui::InputText("Applicability note", condition_state_branch_applicability.data(),
                condition_state_branch_applicability.size());
            std::vector<carto::condition::Id> branch_ids{0U};
            std::vector<std::string> branch_labels{"Baseline"};
            for (const auto& branch : condition.state_branches) {
                branch_ids.push_back(branch.id);
                branch_labels.push_back(branch.name + " · " + std::to_string(branch.id));
            }
            std::vector<const char*> branch_label_pointers;
            branch_label_pointers.reserve(branch_labels.size());
            for (const auto& label : branch_labels) branch_label_pointers.push_back(label.c_str());
            condition_state_parent_index = std::clamp(condition_state_parent_index, 0,
                static_cast<int>(branch_ids.size()) - 1);
            ImGui::Combo("Parent branch", &condition_state_parent_index,
                branch_label_pointers.data(), static_cast<int>(branch_label_pointers.size()));
            bool state_branch_parent_current = state_branch_target_current;
            const auto parent_id = branch_ids[static_cast<std::size_t>(
                condition_state_parent_index)];
            if (state_branch_parent_current && parent_id != 0U) {
                const auto parent_state = ui.compare_condition_state_branches(0U, parent_id);
                if (!parent_state) {
                    state_branch_parent_current = false;
                } else {
                    const auto inherited = std::find_if(
                        parent_state.value().differences.begin(),
                        parent_state.value().differences.end(),
                        [&state_branch_delta](const auto& diff) {
                            return diff.target_kind == state_branch_delta.target_kind &&
                                diff.target_record_id == state_branch_delta.target_record_id &&
                                diff.target_asset_id == state_branch_delta.target_asset_id &&
                                diff.property == state_branch_delta.property;
                        });
                    if (inherited != parent_state.value().differences.end()) {
                        state_branch_before = inherited->right_value;
                    }
                }
            }
            if (state_branch_delta.property == carto::condition::StateProperty::wetness ||
                state_branch_delta.target_kind ==
                    carto::condition::StateTargetKind::safe_place_profile) {
                ImGui::TextDisabled("Selected parent's value %.3f",
                    state_branch_before.numeric_value);
            } else {
                ImGui::TextDisabled("Selected parent's value %s",
                    state_branch_before.text_value.c_str());
            }
            std::uint64_t next_state_branch_id = 1U;
            bool state_branch_id_available = true;
            if (!condition.state_branches.empty()) {
                const auto maximum_id = std::max_element(condition.state_branches.begin(),
                    condition.state_branches.end(), [](const auto& left, const auto& right) {
                        return left.id < right.id;
                    })->id;
                if (maximum_id == std::numeric_limits<std::uint64_t>::max()) {
                    state_branch_id_available = false;
                } else {
                    next_state_branch_id = maximum_id + 1U;
                }
            }
            ImGui::BeginDisabled(!state_branch_target_current || !state_branch_id_available ||
                !state_branch_parent_current);
            if (ImGui::Button("Create branch from selected state", ImVec2(-1, 28))) {
                auto delta = state_branch_delta;
                delta.id = 1U;
                delta.before = state_branch_before;
                delta.after = state_branch_after;
                carto::condition::AuthoredStateBranch branch;
                branch.id = next_state_branch_id;
                branch.parent_branch_id = branch_ids[static_cast<std::size_t>(
                    condition_state_parent_index)];
                branch.name = condition_state_branch_name.data();
                branch.applicability = condition_state_branch_applicability.data();
                branch.deltas.push_back(std::move(delta));
                const auto added = ui.add_condition_state_branch(std::move(branch));
                action_feedback = added
                    ? "Created a non-destructive source-state branch."
                    : added.error().message;
                action_feedback_error = !added;
            }
            ImGui::EndDisabled();
            if (state_branch_id_available && !condition.state_branches.empty()) {
                condition_state_compare_left_index = std::clamp(
                    condition_state_compare_left_index, 0,
                    static_cast<int>(branch_ids.size()) - 1);
                condition_state_compare_right_index = std::clamp(
                    condition_state_compare_right_index, 0,
                    static_cast<int>(branch_ids.size()) - 1);
                ImGui::Combo("Compare left state", &condition_state_compare_left_index,
                    branch_label_pointers.data(),
                    static_cast<int>(branch_label_pointers.size()));
                ImGui::Combo("Compare right state", &condition_state_compare_right_index,
                    branch_label_pointers.data(),
                    static_cast<int>(branch_label_pointers.size()));
                if (ImGui::Button("Compare source states", ImVec2(-1, 28))) {
                    const auto compared = ui.compare_condition_state_branches(
                        branch_ids[static_cast<std::size_t>(condition_state_compare_left_index)],
                        branch_ids[static_cast<std::size_t>(condition_state_compare_right_index)]);
                    if (compared) {
                        condition_state_comparison_result = compared.value();
                        action_feedback = "State comparison ready; source baseline unchanged.";
                        action_feedback_error = false;
                    } else {
                        condition_state_comparison_result.reset();
                        action_feedback = compared.error().message;
                        action_feedback_error = true;
                    }
                }
                for (const auto& branch : condition.state_branches) {
                    ImGui::BulletText("%s · parent %llu · %zu deltas%s",
                        branch.name.c_str(),
                        static_cast<unsigned long long>(branch.parent_branch_id),
                        branch.delta_count,
                        branch.parent_branch_id == 0U ? " · baseline branch" : "");
                }
            }
            if (condition_state_comparison_result.has_value()) {
                const auto& comparison = *condition_state_comparison_result;
                if (comparison.project_revision != condition.authored_revision ||
                    comparison.authoring_digest != condition.digest) {
                    ImGui::TextColored(color(snapshot.colors.semantic_warning),
                        "State comparison is stale; compare again after source edits.");
                } else {
                    ImGui::TextDisabled("%zu explicit differences · common ancestor %llu",
                        comparison.differences.size(),
                        static_cast<unsigned long long>(comparison.common_ancestor_branch_id));
                    mono_text_disabled(comparison.comparison_digest.hex().c_str());
                    for (const auto& difference : comparison.differences) {
                        if (difference.property <= carto::condition::StateProperty::isolation ||
                            difference.property == carto::condition::StateProperty::wetness) {
                            const auto property_name = carto::condition::state_property_name(
                                difference.property);
                            ImGui::BulletText("%.*s · %.3f → %.3f%s",
                                static_cast<int>(property_name.size()), property_name.data(),
                                difference.left_value.numeric_value,
                                difference.right_value.numeric_value,
                                difference.contested ? " · CONTESTED" : "");
                        } else {
                            const auto property_name = carto::condition::state_property_name(
                                difference.property);
                            ImGui::BulletText("%.*s · %s → %s%s",
                                static_cast<int>(property_name.size()), property_name.data(),
                                difference.left_value.text_value.c_str(),
                                difference.right_value.text_value.c_str(),
                                difference.contested ? " · CONTESTED" : "");
                        }
                    }
                    if (comparison.differences.empty()) {
                        ImGui::TextDisabled("These authored states resolve to the same source values.");
                    }
                }
            }
            if (condition_source_update_id.has_value()) {
                ImGui::InputText("New source revision", condition_source_revision.data(),
                                 condition_source_revision.size());
                ImGui::InputText("New source SHA-256", condition_source_digest.data(),
                                 condition_source_digest.size());
                if (ImGui::Button("Record drift and stale dependent evidence")) {
                    record_condition_source_change();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel source update")) {
                    condition_source_update_id.reset();
                    condition_source_revision.fill('\0');
                    condition_source_digest.fill('\0');
                }
            }
            const ImVec4 status_color = condition.consumer_admissible
                ? color(snapshot.colors.semantic_success)
                : condition.publication_ready
                    ? color(snapshot.colors.semantic_warning)
                    : color(snapshot.colors.semantic_error);
            ImGui::TextColored(status_color, "%s", condition.status.c_str());
            if (condition.stale) {
                ImGui::TextColored(color(snapshot.colors.semantic_error),
                                   "Source drift is recorded; package publication is blocked.");
            }
            ImGui::Spacing();
            if (ImGui::Button("Import replacement authoring...", ImVec2(-1, 30))) {
                import_condition_authoring();
            }
            if (ImGui::Button("Export authoring document...", ImVec2(-1, 30))) {
                export_condition_authoring();
            }
            ImGui::BeginDisabled(!condition.publication_ready || condition.stale);
            if (ImGui::Button("Publish .carto-condition...", ImVec2(-1, 30))) {
                publish_condition_package();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Replace with CONDITION demo fixture", ImVec2(-1, 30))) {
                if (dispatch(carto::application::ImportConditionFixtureAction{})) {
                    condition_source_update_id.reset();
                    condition_source_revision.fill('\0');
                    condition_source_digest.fill('\0');
                }
            }
        }
        ImGui::Separator();
        heading_text("Spatial source field");
        ImGui::TextWrapped(
            "Author a project-local scalar source. Edits are revision checked, journaled, "
            "and undoable; no CONDITION runtime state is stored here.");
        ImGui::Combo("Semantic", &spatial_field_semantic,
            "Generic\0Moisture\0Sunlight\0Shade\0Disturbance\0Wind U\0Wind V\0"
            "Drainage flow\0Acoustic dampening\0Fog tendency\0Vegetation suitability\0"
            "Terrain elevation\0Ridge\0Basin\0Drainage\0Low ground\0Elevation band\0");
        const bool terrain_source = spatial_field_semantic == static_cast<int>(
            carto::spatial_field::FieldSemantic::terrain_elevation);
        if (terrain_source) {
            ImGui::InputInt("Tile X", &spatial_field_tile_x);
            ImGui::InputInt("Tile Z", &spatial_field_tile_z);
            const std::string tile_identity = carto::terrain::terrain_tile_identity({
                static_cast<std::int32_t>(spatial_field_tile_x),
                static_cast<std::int32_t>(spatial_field_tile_z)});
            ImGui::TextDisabled("Stable source: %s", tile_identity.c_str());
        } else {
            ImGui::InputText("Field identity", spatial_field_identity.data(),
                             spatial_field_identity.size());
        }
        ImGui::InputDouble("Origin U", &spatial_field_origin_u, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("Origin V", &spatial_field_origin_v, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("Cell size", &spatial_field_cell_size, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("Minimum", &spatial_field_minimum, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("Maximum", &spatial_field_maximum, 0.1, 1.0, "%.3f");
        ImGui::InputDouble("Initial value", &spatial_field_initial, 0.1, 1.0, "%.3f");
        const std::string terrain_identity = carto::terrain::terrain_tile_identity({
            static_cast<std::int32_t>(spatial_field_tile_x),
            static_cast<std::int32_t>(spatial_field_tile_z)});
        if (ImGui::Button(terrain_source ? "Create 32 × 32 terrain tile" :
                "Create 32 × 32 field", ImVec2(-1, 30))) {
            static_cast<void>(dispatch(carto::application::CreateSpatialFieldAction{
                terrain_source ? terrain_identity : spatial_field_identity.data(),
                static_cast<carto::spatial_field::FieldSemantic>(spatial_field_semantic),
                {spatial_field_origin_u, spatial_field_origin_v},
                spatial_field_cell_size, 32U, 32U, spatial_field_minimum,
                spatial_field_maximum, spatial_field_initial}));
        }
        if (condition_vector_preview.has_value() && snapshot.spatial_fields.empty()) {
            ImGui::TextDisabled("Wind vector overlay is source-bound and may be stale.");
            if (ImGui::SmallButton("Clear wind overlay")) {
                condition_vector_preview.reset();
                work_surface_camera_framed = false;
            }
        }
        if (!snapshot.spatial_fields.empty()) {
            const auto first_with_semantic = [&snapshot](
                carto::spatial_field::FieldSemantic semantic) {
                return std::find_if(snapshot.spatial_fields.begin(),
                    snapshot.spatial_fields.end(), [semantic](const auto& field) {
                        return field.semantic == semantic;
                    });
            };
            const auto u_field = first_with_semantic(
                carto::spatial_field::FieldSemantic::wind_u);
            const auto v_field = first_with_semantic(
                carto::spatial_field::FieldSemantic::wind_v);
            const auto has_component = [&snapshot](std::string_view identity,
                carto::spatial_field::FieldSemantic semantic) {
                return std::any_of(snapshot.spatial_fields.begin(),
                    snapshot.spatial_fields.end(), [identity, semantic](const auto& field) {
                        return field.identity == identity && field.semantic == semantic;
                    });
            };
            if (u_field != snapshot.spatial_fields.end() &&
                !has_component(selected_wind_u_field,
                    carto::spatial_field::FieldSemantic::wind_u)) {
                selected_wind_u_field = u_field->identity;
            }
            if (v_field != snapshot.spatial_fields.end() &&
                !has_component(selected_wind_v_field,
                    carto::spatial_field::FieldSemantic::wind_v)) {
                selected_wind_v_field = v_field->identity;
            }
            if (u_field != snapshot.spatial_fields.end() &&
                v_field != snapshot.spatial_fields.end()) {
                const auto draw_component_combo = [&snapshot](const char* label,
                    std::string& identity,
                    carto::spatial_field::FieldSemantic semantic) {
                    if (!ImGui::BeginCombo(label, identity.c_str())) return;
                    for (const auto& field : snapshot.spatial_fields) {
                        if (field.semantic != semantic) continue;
                        const bool selected = field.identity == identity;
                        if (ImGui::Selectable(field.identity.c_str(), selected)) {
                            identity = field.identity;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                };
                draw_component_combo("Wind U source", selected_wind_u_field,
                    carto::spatial_field::FieldSemantic::wind_u);
                draw_component_combo("Wind V source", selected_wind_v_field,
                    carto::spatial_field::FieldSemantic::wind_v);
                if (ImGui::Button("Preview wind vectors", ImVec2(-1, 28))) {
                    const auto preview = session.spatial_vector_field_preview(
                        selected_wind_u_field, selected_wind_v_field);
                    if (preview) {
                        condition_vector_preview = preview.value();
                        work_surface_camera_framed = false;
                        action_feedback = "Wind vector preview · " +
                            std::to_string(preview.value().samples.size()) +
                            " bounded samples · read-only";
                        action_feedback_error = false;
                    } else {
                        condition_vector_preview.reset();
                        action_feedback = preview.error().message;
                        action_feedback_error = true;
                    }
                }
            } else {
                ImGui::TextDisabled(
                    "Add aligned wind_u and wind_v fields to preview directional vectors.");
            }
            if (condition_vector_preview.has_value()) {
                const auto& product = *condition_vector_preview;
                const auto source_is_current = [&snapshot](
                    const carto::spatial_field::VectorFieldSourceReceipt& source) {
                    const auto field = std::find_if(snapshot.spatial_fields.begin(),
                        snapshot.spatial_fields.end(), [&source](const auto& candidate) {
                            return candidate.identity == source.identity;
                        });
                    return field != snapshot.spatial_fields.end() &&
                        field->source_revision == source.source_revision &&
                        field->digest == source.digest;
                };
                if (product.validate() && source_is_current(product.u_component) &&
                    source_is_current(product.v_component)) {
                    ImGui::TextDisabled("Viewport overlay: %s + %s · %u arrows · preview only",
                        product.u_component.identity.c_str(),
                        product.v_component.identity.c_str(),
                        static_cast<unsigned>(product.samples.size()));
                } else {
                    ImGui::TextColored(color(snapshot.colors.semantic_warning),
                        "Wind vector overlay stale; rebuild from current fields.");
                }
                if (ImGui::SmallButton("Clear wind overlay")) {
                    condition_vector_preview.reset();
                    work_surface_camera_framed = false;
                }
            }
            auto selected = std::find_if(
                snapshot.spatial_fields.begin(), snapshot.spatial_fields.end(),
                [this](const auto& field) { return field.identity == selected_spatial_field; });
            if (selected == snapshot.spatial_fields.end()) {
                selected = snapshot.spatial_fields.begin();
                selected_spatial_field = selected->identity;
            }
            if (ImGui::BeginCombo("Active field", selected->identity.c_str())) {
                for (const auto& field : snapshot.spatial_fields) {
                    const bool is_selected = field.identity == selected_spatial_field;
                    if (ImGui::Selectable(field.identity.c_str(), is_selected)) {
                        selected_spatial_field = field.identity;
                    }
                    if (is_selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (condition_terrain_preview.has_value() &&
                condition_terrain_preview->source_identity() != selected_spatial_field) {
                ImGui::TextDisabled("Viewport preview: %s · source r%llu",
                    condition_terrain_preview->source_identity().c_str(),
                    static_cast<unsigned long long>(
                        condition_terrain_preview->source_revision().value()));
                if (ImGui::SmallButton("Clear terrain overlay")) {
                    condition_terrain_preview.reset();
                    work_surface_camera_framed = false;
                }
            }
            selected = std::find_if(
                snapshot.spatial_fields.begin(), snapshot.spatial_fields.end(),
                [this](const auto& field) { return field.identity == selected_spatial_field; });
            if (selected != snapshot.spatial_fields.end()) {
                const std::string semantic(
                    carto::spatial_field::field_semantic_name(selected->semantic));
                ImGui::Text("%s · %u × %u · source r%llu", semantic.c_str(),
                    selected->width, selected->height,
                    static_cast<unsigned long long>(selected->source_revision.value()));
                if (selected->semantic ==
                    carto::spatial_field::FieldSemantic::terrain_elevation &&
                    ImGui::Button("Analyze terrain source", ImVec2(-1, 28))) {
                    constexpr std::uint64_t kMaxViewportTerrainTriangles = 16'384U;
                    const auto lod_step = carto::terrain::lod_step_for_triangle_budget(
                        selected->width, selected->height,
                        kMaxViewportTerrainTriangles);
                    const auto product = lod_step
                        ? session.terrain_tile_preview(selected->identity, lod_step.value())
                        : carto::core::Result<carto::terrain::TerrainTileProduct>::failure(
                            lod_step.error());
                    if (product) {
                        const auto passable = static_cast<std::size_t>(std::count(
                            product.value().navigation_passable().begin(),
                            product.value().navigation_passable().end(), 1U));
                        action_feedback = "Terrain analysis · r" +
                            std::to_string(product.value().source_revision().value()) + " · " +
                            std::to_string(product.value().vertices().size()) + " vertices · " +
                            std::to_string(product.value().indices().size() / 3U) +
                            " triangles · " + std::to_string(passable) + "/" +
                            std::to_string(product.value().navigation_passable().size()) +
                            " passable cells";
                        action_feedback_error = false;
                        condition_terrain_preview = product.value();
                        work_surface_camera_framed = false;
                    } else {
                        condition_terrain_preview.reset();
                        action_feedback = product.error().message;
                        action_feedback_error = true;
                    }
                }
                if (condition_terrain_preview.has_value() &&
                    condition_terrain_preview->source_identity() == selected->identity) {
                    if (condition_terrain_preview->source_revision() == selected->source_revision &&
                        condition_terrain_preview->source_digest() == selected->digest) {
                        ImGui::TextDisabled(
                            "Derived terrain mesh overlaid in work surface · %llu triangles · preview only",
                            static_cast<unsigned long long>(
                                condition_terrain_preview->indices().size() / 3U));
                        if (ImGui::SmallButton("Clear terrain overlay")) {
                            condition_terrain_preview.reset();
                            work_surface_camera_framed = false;
                        }
                    } else {
                        ImGui::TextColored(color(snapshot.colors.semantic_warning),
                            "Terrain overlay stale; analyze this field again before viewing.");
                    }
                }
                const auto preview = session.spatial_field_preview(selected->identity);
                if (preview) {
                    draw_spatial_field_canvas(snapshot, preview.value());
                } else {
                    ImGui::TextColored(color(snapshot.colors.semantic_error), "%s",
                                       preview.error().message.c_str());
                }
                ImGui::InputDouble("Brush U", &spatial_brush_u, 0.1, 1.0, "%.3f");
                ImGui::InputDouble("Brush V", &spatial_brush_v, 0.1, 1.0, "%.3f");
                ImGui::InputDouble("Radius", &spatial_brush_radius, 0.1, 1.0, "%.3f");
                ImGui::InputDouble("Value", &spatial_brush_value, 0.1, 1.0, "%.3f");
                ImGui::InputDouble("Strength", &spatial_brush_strength, 0.05, 0.2, "%.3f");
                ImGui::Combo("Brush mode", &spatial_brush_mode,
                             "Replace\0Add\0Erase\0Smooth\0");
                if (ImGui::Button("Apply field brush", ImVec2(-1, 30))) {
                    carto::spatial_field::FieldBrush brush;
                    brush.expected_revision = selected->source_revision;
                    brush.center_uv = {spatial_brush_u, spatial_brush_v};
                    brush.radius = spatial_brush_radius;
                    brush.value = spatial_brush_value;
                    brush.strength = spatial_brush_strength;
                    brush.mode = static_cast<carto::spatial_field::BrushMode>(spatial_brush_mode);
                    static_cast<void>(dispatch(carto::application::PaintSpatialFieldAction{
                        selected->identity, brush}));
                }
                mono_text_disabled(selected->digest.hex().c_str());
            }
        }
        ImGui::Separator();
        heading_text("Road / centerline source");
        constexpr std::size_t kMaxNativeRoadSegments = 16U;
        const auto pack_road_segment = [](const carto::geometry::CubicBezierSegment& segment) {
            return std::array<double, 12>{
                segment.p0.x, segment.p0.y, segment.p0.z,
                segment.p1.x, segment.p1.y, segment.p1.z,
                segment.p2.x, segment.p2.y, segment.p2.z,
                segment.p3.x, segment.p3.y, segment.p3.z};
        };
        const auto unpack_road_segment = [](const std::array<double, 12>& points) {
            return carto::geometry::CubicBezierSegment{
                {points[0], points[1], points[2]},
                {points[3], points[4], points[5]},
                {points[6], points[7], points[8]},
                {points[9], points[10], points[11]}};
        };
        ImGui::TextWrapped(
            "Author a connected cubic centerline of up to 16 segments and bind road design intent "
            "to its exact source revision. "
            "A disposable ribbon preview is available, but no road mesh, collision, or runtime state is committed.");
        if (!snapshot.curve_paths.empty()) {
            if (selected_curve_path.empty() || std::none_of(
                    snapshot.curve_paths.begin(), snapshot.curve_paths.end(),
                    [this](const auto& source) {
                        return source.identity == selected_curve_path;
                    })) {
                selected_curve_path = snapshot.curve_paths.front().identity;
            }
            if (ImGui::BeginCombo("Existing centerline", selected_curve_path.c_str())) {
                for (const auto& source : snapshot.curve_paths) {
                    const bool selected = source.identity == selected_curve_path;
                    if (ImGui::Selectable(source.identity.c_str(), selected)) {
                        selected_curve_path = source.identity;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::SmallButton("Load selected centerline for editing")) {
                const auto source = session.curve_path_source(selected_curve_path);
                if (!source) {
                    action_feedback = source.error().message;
                    action_feedback_error = true;
                } else if (source.value().segments.empty() ||
                    source.value().segments.size() > kMaxNativeRoadSegments) {
                    action_feedback =
                        "The native editor requires 1–16 cubic segments; this source remains unchanged.";
                    action_feedback_error = true;
                } else {
                    road_centerline_identity.fill('\0');
                    std::copy(source.value().source_identity.begin(),
                        source.value().source_identity.end(), road_centerline_identity.begin());
                    road_segment_control_points.clear();
                    road_segment_control_points.reserve(source.value().segments.size());
                    for (const auto& segment : source.value().segments) {
                        road_segment_control_points.push_back(pack_road_segment(segment));
                    }
                    road_selected_segment = 0;
                    action_feedback = "Loaded centerline source for revisioned editing.";
                    action_feedback_error = false;
                }
            }
        } else {
            ImGui::TextDisabled("No centerline sources are present yet.");
        }
        if (!snapshot.road_sources.empty()) {
            if (selected_road_source.empty() || std::none_of(
                    snapshot.road_sources.begin(), snapshot.road_sources.end(),
                    [this](const auto& source) {
                        return source.identity == selected_road_source;
                    })) {
                selected_road_source = snapshot.road_sources.front().identity;
            }
            if (ImGui::BeginCombo("Existing road source", selected_road_source.c_str())) {
                for (const auto& source : snapshot.road_sources) {
                    const bool selected = source.identity == selected_road_source;
                    if (ImGui::Selectable(source.identity.c_str(), selected)) {
                        selected_road_source = source.identity;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::SmallButton("Load road and centerline for editing")) {
                const auto road = std::find_if(
                    snapshot.road_sources.begin(), snapshot.road_sources.end(),
                    [this](const auto& source) {
                        return source.identity == selected_road_source;
                    });
                if (road == snapshot.road_sources.end()) {
                    action_feedback = "Selected road source is no longer in the project.";
                    action_feedback_error = true;
                } else {
                    const auto centerline = session.curve_path_source(
                        road->centerline_identity);
                    if (!centerline) {
                        action_feedback = centerline.error().message;
                        action_feedback_error = true;
                    } else if (centerline.value().segments.empty() ||
                        centerline.value().segments.size() > kMaxNativeRoadSegments) {
                        action_feedback =
                            "The native editor requires 1–16 cubic segments; this source remains unchanged.";
                        action_feedback_error = true;
                    } else {
                        road_segment_control_points.clear();
                        road_segment_control_points.reserve(centerline.value().segments.size());
                        for (const auto& segment : centerline.value().segments) {
                            road_segment_control_points.push_back(pack_road_segment(segment));
                        }
                        road_selected_segment = 0;
                        const auto copy_identity = [](std::string_view value, auto& target) {
                            target.fill('\0');
                            const std::size_t length = std::min(
                                value.size(), target.size() - 1U);
                            std::copy_n(value.begin(), length, target.begin());
                        };
                        copy_identity(centerline.value().source_identity,
                                      road_centerline_identity);
                        copy_identity(road->identity, road_source_identity);
                        selected_curve_path = centerline.value().source_identity;
                        road_width_meters = road->width_meters;
                        road_shoulder_meters = road->shoulder_meters;
                        road_maximum_grade = road->maximum_grade_ratio;
                        road_minimum_curve_radius = road->minimum_curve_radius_meters;
                        road_surface_class = static_cast<int>(road->surface_class);
                        road_function_class = static_cast<int>(road->function_class);
                        road_access_state = static_cast<int>(road->access_state);
                        road_drainage_intent = static_cast<int>(road->drainage_intent);
                        action_feedback = "Loaded the exact road / centerline source pair for editing.";
                        action_feedback_error = false;
                    }
                }
            }
        }
        ImGui::BeginDisabled(snapshot.road_sources.empty());
        if (ImGui::Button("Build selected road surface preview", ImVec2(-1, 30))) {
            const auto preview = session.road_surface_preview(selected_road_source);
            if (!preview) {
                condition_road_surface_preview.reset();
                action_feedback = preview.error().message;
                action_feedback_error = true;
            } else {
                condition_road_surface_preview = std::move(preview).value();
                work_surface_camera_framed = false;
                action_feedback = "Built a source-bound road ribbon preview; no project geometry was changed.";
                action_feedback_error = false;
            }
        }
        ImGui::EndDisabled();
        if (condition_road_surface_preview.has_value()) {
            if (ImGui::SmallButton("Clear road surface preview")) {
                condition_road_surface_preview.reset();
                work_surface_camera_framed = false;
            }
        }
        ImGui::TextDisabled(
            "Sampled grade/radius checks only. No terrain draping, frontage inference, "
            "driveways, drainage geometry, or collision output.");
        ImGui::InputText("Centerline identity", road_centerline_identity.data(),
                         road_centerline_identity.size());
        ImGui::InputText("Road source identity", road_source_identity.data(),
                         road_source_identity.size());
        if (road_segment_control_points.empty()) {
            road_segment_control_points.push_back({
                0.0, 0.0, 0.0, 3.0, 0.0, 0.0,
                7.0, 0.0, 2.0, 10.0, 0.0, 2.0});
        }
        ImGui::BeginDisabled(road_segment_control_points.size() >= kMaxNativeRoadSegments);
        if (ImGui::SmallButton("Add connected segment")) {
            const auto previous = unpack_road_segment(road_segment_control_points.back());
            auto tangent = previous.p3 - previous.p2;
            if (tangent.length_squared() <= 1.0e-12) tangent = {1.0, 0.0, 0.0};
            road_segment_control_points.push_back({
                previous.p3.x, previous.p3.y, previous.p3.z,
                previous.p3.x + tangent.x, previous.p3.y + tangent.y,
                previous.p3.z + tangent.z,
                previous.p3.x + 2.0 * tangent.x, previous.p3.y + 2.0 * tangent.y,
                previous.p3.z + 2.0 * tangent.z,
                previous.p3.x + 3.0 * tangent.x, previous.p3.y + 3.0 * tangent.y,
                previous.p3.z + 3.0 * tangent.z});
            road_selected_segment = static_cast<int>(road_segment_control_points.size()) - 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(road_segment_control_points.size() <= 1U);
        if (ImGui::SmallButton("Remove final segment")) {
            road_segment_control_points.pop_back();
            road_selected_segment = std::min(road_selected_segment,
                static_cast<int>(road_segment_control_points.size()) - 1);
        }
        ImGui::EndDisabled();
        std::vector<std::string> road_segment_labels;
        std::vector<const char*> road_segment_label_pointers;
        road_segment_labels.reserve(road_segment_control_points.size());
        for (std::size_t index = 0U; index < road_segment_control_points.size(); ++index) {
            road_segment_labels.push_back("Segment " + std::to_string(index + 1U));
        }
        road_segment_label_pointers.reserve(road_segment_labels.size());
        for (const auto& label : road_segment_labels) {
            road_segment_label_pointers.push_back(label.c_str());
        }
        road_selected_segment = std::clamp(road_selected_segment, 0,
            static_cast<int>(road_segment_control_points.size()) - 1);
        ImGui::Combo("Edit segment", &road_selected_segment,
            road_segment_label_pointers.data(),
            static_cast<int>(road_segment_label_pointers.size()));
        static constexpr std::array<const char*, 4> point_names{"P0", "P1", "P2", "P3"};
        for (std::size_t point = 0U; point < point_names.size(); ++point) {
            auto& road_control_points = road_segment_control_points[
                static_cast<std::size_t>(road_selected_segment)];
            ImGui::PushID(static_cast<int>(point));
            ImGui::TextDisabled("%s", point_names[point]);
            ImGui::SameLine(42.0F);
            ImGui::SetNextItemWidth(74.0F);
            ImGui::InputDouble("X", &road_control_points[point * 3U], 0.25, 1.0, "%.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(74.0F);
            ImGui::InputDouble("Y", &road_control_points[point * 3U + 1U], 0.25, 1.0, "%.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(74.0F);
            ImGui::InputDouble("Z", &road_control_points[point * 3U + 2U], 0.25, 1.0, "%.2f");
            ImGui::PopID();
        }
        ImGui::InputDouble("Road width (m)", &road_width_meters, 0.25, 1.0, "%.2f");
        ImGui::InputDouble("Shoulder (m)", &road_shoulder_meters, 0.1, 0.5, "%.2f");
        ImGui::InputDouble("Maximum grade", &road_maximum_grade, 0.02, 0.05, "%.3f");
        ImGui::InputDouble("Minimum curve radius (m)", &road_minimum_curve_radius, 1.0, 5.0, "%.2f");
        ImGui::Combo("Surface", &road_surface_class,
                     "Asphalt\0Concrete\0Gravel\0Dirt\0Trail\0");
        ImGui::Combo("Function", &road_function_class,
                     "Local\0Collector\0Arterial\0Service\0Driveway\0Pedestrian\0");
        ImGui::Combo("Access", &road_access_state, "Open\0Restricted\0Closed\0");
        ImGui::Combo("Drainage intent", &road_drainage_intent,
                     "None\0Roadside ditch\0Culvert\0Swale crossing\0");
        if (ImGui::Button("Commit road / centerline source", ImVec2(-1, 30))) {
            const std::string centerline_id(road_centerline_identity.data());
            const std::string road_id(road_source_identity.data());
            const auto next_centerline = session.next_curve_path_revision(centerline_id);
            const auto next_road = session.next_road_source_revision(road_id);
            if (!next_centerline || !next_road) {
                action_feedback = !next_centerline
                    ? next_centerline.error().message : next_road.error().message;
                action_feedback_error = true;
            } else {
                carto::geometry::CurvePath centerline;
                centerline.source_identity = centerline_id;
                centerline.source_revision = next_centerline.value();
                centerline.segments.reserve(road_segment_control_points.size());
                for (const auto& segment : road_segment_control_points) {
                    centerline.segments.push_back(unpack_road_segment(segment));
                }
                carto::project::RoadSource road;
                road.source_identity = road_id;
                road.source_revision = next_road.value();
                road.centerline_identity = centerline_id;
                road.centerline_revision = next_centerline.value();
                road.width_meters = road_width_meters;
                road.shoulder_meters = road_shoulder_meters;
                road.maximum_grade_ratio = road_maximum_grade;
                road.minimum_curve_radius_meters = road_minimum_curve_radius;
                road.surface_class = static_cast<carto::project::RoadSurfaceClass>(
                    road_surface_class);
                road.function_class = static_cast<carto::project::RoadFunctionClass>(
                    road_function_class);
                road.access_state = static_cast<carto::project::RoadAccessState>(
                    road_access_state);
                road.drainage_intent = static_cast<carto::project::RoadDrainageIntent>(
                    road_drainage_intent);
                const auto committed = dispatch(carto::application::SetRoadAuthoringAction{
                    snapshot.project_revision, std::move(centerline), std::move(road)});
                if (committed) selected_curve_path = centerline_id;
            }
        }
        for (const auto& road : snapshot.road_sources) {
            ImGui::Text("%s · r%llu · %s@r%llu · %.2f m",
                road.identity.c_str(),
                static_cast<unsigned long long>(road.source_revision.value()),
                road.centerline_identity.c_str(),
                static_cast<unsigned long long>(road.centerline_revision.value()),
                road.width_meters);
        }
        ImGui::Separator();
        heading_text("Procedural sweep source");
        ImGui::TextWrapped(
            "Author a Cartographer-owned convex profile sweep against the selected exact centerline revision. "
            "The recipe is journaled and undoable; its wireframe output is derived preview only.");
        if (!snapshot.sweep_sources.empty()) {
            if (selected_sweep_source.empty() || std::none_of(
                    snapshot.sweep_sources.begin(), snapshot.sweep_sources.end(),
                    [this](const auto& source) {
                        return source.source.source_identity == selected_sweep_source;
                    })) {
                selected_sweep_source = snapshot.sweep_sources.front().source.source_identity;
            }
            if (ImGui::BeginCombo("Existing sweep source", selected_sweep_source.c_str())) {
                for (const auto& view : snapshot.sweep_sources) {
                    const auto& source = view.source;
                    const bool selected = source.source_identity == selected_sweep_source;
                    if (ImGui::Selectable(source.source_identity.c_str(), selected)) {
                        selected_sweep_source = source.source_identity;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::SmallButton("Load radial sweep for editing")) {
                const auto view = std::find_if(
                    snapshot.sweep_sources.begin(), snapshot.sweep_sources.end(),
                    [this](const auto& item) {
                        return item.source.source_identity == selected_sweep_source;
                    });
                if (view == snapshot.sweep_sources.end()) {
                    action_feedback = "Selected sweep source is no longer in the project.";
                    action_feedback_error = true;
                } else {
                    const auto& source = view->source;
                    constexpr double tau = 6.283185307179586476925286766559;
                    const std::size_t sides = source.profile.size();
                    const double radius = sides >= 3U
                        ? std::hypot(source.profile.front().x, source.profile.front().y) : 0.0;
                    bool radial_profile = sides >= 3U && sides <= 64U &&
                        std::isfinite(radius) && radius > 1.0e-9;
                    for (std::size_t index = 0U; radial_profile && index < sides; ++index) {
                        const double angle = tau * static_cast<double>(index) /
                            static_cast<double>(sides);
                        const double expected_x = radius * std::cos(angle);
                        const double expected_y = radius * std::sin(angle);
                        const double tolerance = std::max(1.0e-9, radius * 1.0e-8);
                        radial_profile =
                            std::abs(source.profile[index].x - expected_x) <= tolerance &&
                            std::abs(source.profile[index].y - expected_y) <= tolerance;
                    }
                    const bool supported_frame =
                        std::abs(source.reference_up.x) <= 1.0e-9 &&
                        std::abs(source.reference_up.y - 1.0) <= 1.0e-9 &&
                        std::abs(source.reference_up.z) <= 1.0e-9;
                    if (!radial_profile || !supported_frame) {
                        action_feedback =
                            "This source uses a custom profile or reference frame; it remains preserved and previewable, "
                            "but the compact radial editor will not flatten it.";
                        action_feedback_error = true;
                    } else {
                        sweep_source_identity.fill('\0');
                        std::copy(source.source_identity.begin(), source.source_identity.end(),
                            sweep_source_identity.begin());
                        selected_curve_path = source.centerline_identity;
                        sweep_profile_sides = static_cast<int>(sides);
                        sweep_profile_radius = radius;
                        sweep_samples_per_segment = static_cast<int>(source.samples_per_segment);
                        sweep_start_scale = source.start_scale;
                        sweep_end_scale = source.end_scale;
                        sweep_cap_policy = static_cast<int>(source.cap_policy);
                        loaded_sweep_source_identity = source.source_identity;
                        action_feedback = "Loaded the exact radial sweep source for revisioned editing.";
                        action_feedback_error = false;
                    }
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(selected_sweep_source.empty());
            if (ImGui::SmallButton("Build selected sweep preview")) {
                const auto preview = session.sweep_source_preview(selected_sweep_source);
                if (!preview) {
                    condition_sweep_preview.reset();
                    action_feedback = preview.error().message;
                    action_feedback_error = true;
                } else {
                    condition_sweep_preview = std::move(preview).value();
                    condition_sweep_preview_generation = snapshot.project_generation;
                    work_surface_camera_framed = false;
                    action_feedback =
                        "Built a source-bound sweep wireframe; no project mesh was changed.";
                    action_feedback_error = false;
                }
            }
            ImGui::EndDisabled();
        } else {
            ImGui::TextDisabled("No sweep recipes are present yet.");
        }
        ImGui::InputText("Sweep source identity", sweep_source_identity.data(),
                         sweep_source_identity.size());
        ImGui::SliderInt("Profile sides", &sweep_profile_sides, 3, 64);
        ImGui::InputDouble("Profile radius (m)", &sweep_profile_radius, 0.05, 0.25, "%.4f");
        ImGui::SliderInt("Samples per segment", &sweep_samples_per_segment, 1, 128);
        ImGui::Combo("Cap policy", &sweep_cap_policy,
            "Open\0Both ends\0Start only\0End only\0");
        ImGui::InputDouble("Start scale", &sweep_start_scale, 0.05, 0.25, "%.4f");
        ImGui::InputDouble("End scale", &sweep_end_scale, 0.05, 0.25, "%.4f");
        ImGui::TextDisabled("Explicit profile reference-up: (0, 1, 0) · not an implicit imported-axis conversion.");
        const std::string sweep_edit_identity(sweep_source_identity.data());
        const bool sweep_identity_exists = std::any_of(
            snapshot.sweep_sources.begin(), snapshot.sweep_sources.end(),
            [&sweep_edit_identity](const auto& item) {
                return item.source.source_identity == sweep_edit_identity;
            });
        const bool loaded_exact_sweep_for_update = sweep_identity_exists &&
            loaded_sweep_source_identity == sweep_edit_identity;
        const bool can_author_sweep = !selected_curve_path.empty() &&
            !sweep_edit_identity.empty() &&
            (!sweep_identity_exists || loaded_exact_sweep_for_update);
        ImGui::BeginDisabled(!can_author_sweep);
        if (ImGui::Button(sweep_identity_exists
                ? "Update loaded sweep source" : "Create sweep source",
                ImVec2(-1, 30))) {
            const std::string identity = sweep_edit_identity;
            const auto centerline = session.curve_path_source(selected_curve_path);
            const auto next_revision = session.next_sweep_source_revision(identity);
            if (!centerline || !next_revision) {
                action_feedback = !centerline
                    ? centerline.error().message : next_revision.error().message;
                action_feedback_error = true;
            } else {
                carto::geometry::SweepSource source;
                source.source_identity = identity;
                source.source_revision = next_revision.value();
                source.centerline_identity = selected_curve_path;
                source.centerline_revision = centerline.value().source_revision;
                source.reference_up = {0.0, 1.0, 0.0};
                source.samples_per_segment = static_cast<std::uint32_t>(
                    sweep_samples_per_segment);
                source.start_scale = sweep_start_scale;
                source.end_scale = sweep_end_scale;
                source.cap_policy = static_cast<carto::geometry::SweepCapPolicy>(
                    sweep_cap_policy);
                constexpr double tau = 6.283185307179586476925286766559;
                source.profile.reserve(static_cast<std::size_t>(sweep_profile_sides));
                for (int side = 0; side < sweep_profile_sides; ++side) {
                    const double angle = tau * static_cast<double>(side) /
                        static_cast<double>(sweep_profile_sides);
                    source.profile.push_back({
                        sweep_profile_radius * std::cos(angle),
                        sweep_profile_radius * std::sin(angle)});
                }
                const auto committed = dispatch(carto::application::SetSweepSourceAction{
                    snapshot.project_revision, std::move(source)});
                if (committed) {
                    selected_sweep_source = identity;
                    loaded_sweep_source_identity = identity;
                    condition_sweep_preview.reset();
                    condition_sweep_preview_generation = 0U;
                    work_surface_camera_framed = false;
                    action_feedback =
                        "Saved the revision-bound sweep recipe. Build its derived preview explicitly.";
                    action_feedback_error = false;
                }
            }
        }
        ImGui::EndDisabled();
        if (sweep_identity_exists && !loaded_exact_sweep_for_update) {
            ImGui::TextDisabled(
                "Load the exact existing radial sweep before updating it; this prevents accidental source replacement.");
        }
        if (condition_sweep_preview.has_value()) {
            if (ImGui::SmallButton("Clear sweep preview")) {
                condition_sweep_preview.reset();
                condition_sweep_preview_generation = 0U;
                work_surface_camera_framed = false;
            }
        }
        if (!action_feedback.empty()) {
            ImGui::Separator();
            draw_action_feedback(snapshot);
        }
        end_instrument(carto::ui::WorkbenchInstrument::references, workbench.references);
    }

    void draw_spatial_field_canvas(
        const UiSnapshot& snapshot,
        const carto::application::SpatialFieldPreview& preview) {
        ImGui::TextDisabled(preview.semantic ==
                carto::spatial_field::FieldSemantic::terrain_elevation
            ? "Elevation relief · click the source map to edit this terrain tile."
            : "Click the source map to apply the selected brush.");
        const float width = std::max(120.0F, ImGui::GetContentRegionAvail().x);
        const ImVec2 canvas_size(width, 184.0F);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("Spatial field map", canvas_size);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin,
            ImVec2(origin.x + canvas_size.x, origin.y + canvas_size.y),
            ImGui::ColorConvertFloat4ToU32(color(snapshot.colors.panel_0)));

        const float cell_width = canvas_size.x / static_cast<float>(preview.preview_width);
        const float cell_height = canvas_size.y / static_cast<float>(preview.preview_height);
        const double value_span = preview.maximum_value - preview.minimum_value;
        for (std::uint32_t display_v = 0U; display_v < preview.preview_height; ++display_v) {
            const std::uint32_t source_v = preview.preview_height - 1U - display_v;
            for (std::uint32_t u = 0U; u < preview.preview_width; ++u) {
                const std::size_t index = static_cast<std::size_t>(source_v) *
                    preview.preview_width + u;
                if (index >= preview.values.size()) continue;
                const double normalized = value_span > 0.0
                    ? std::clamp((preview.values[index] - preview.minimum_value) / value_span,
                                 0.0, 1.0)
                    : 0.0;
                const float t = static_cast<float>(normalized);
                ImVec4 heat;
                if (preview.semantic == carto::spatial_field::FieldSemantic::terrain_elevation) {
                    const ImVec4 low{0.035F, 0.16F, 0.22F, 1.0F};
                    const ImVec4 middle{0.13F, 0.32F, 0.25F, 1.0F};
                    const ImVec4 high{0.48F, 0.40F, 0.23F, 1.0F};
                    const ImVec4 summit{0.76F, 0.81F, 0.82F, 1.0F};
                    const auto blend = [](ImVec4 a, ImVec4 b, float factor) {
                        return ImVec4(a.x + (b.x - a.x) * factor,
                            a.y + (b.y - a.y) * factor,
                            a.z + (b.z - a.z) * factor, 1.0F);
                    };
                    heat = t < 0.4F ? blend(low, middle, t / 0.4F)
                        : t < 0.8F ? blend(middle, high, (t - 0.4F) / 0.4F)
                                   : blend(high, summit, (t - 0.8F) / 0.2F);
                } else {
                    heat = {0.055F + 0.82F * t,
                            0.11F + 0.56F * t,
                            0.24F + 0.08F * (1.0F - t), 1.0F};
                }
                const ImVec2 cell_min{origin.x + static_cast<float>(u) * cell_width + 0.5F,
                    origin.y + static_cast<float>(display_v) * cell_height + 0.5F};
                const ImVec2 cell_max{origin.x + static_cast<float>(u + 1U) * cell_width - 0.5F,
                    origin.y + static_cast<float>(display_v + 1U) * cell_height - 0.5F};
                draw->AddRectFilled(cell_min, cell_max,
                                    ImGui::ColorConvertFloat4ToU32(heat));
            }
        }
        draw->AddRect(origin,
            ImVec2(origin.x + canvas_size.x, origin.y + canvas_size.y),
            ImGui::ColorConvertFloat4ToU32(color(snapshot.colors.border_soft)));

        if (hovered) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const double u_fraction = std::clamp(
                static_cast<double>(mouse.x - origin.x) / canvas_size.x, 0.0, 1.0);
            const double v_fraction = 1.0 - std::clamp(
                static_cast<double>(mouse.y - origin.y) / canvas_size.y, 0.0, 1.0);
            spatial_brush_u = preview.origin_uv.x + u_fraction *
                static_cast<double>(preview.source_width - 1U) * preview.cell_size;
            spatial_brush_v = preview.origin_uv.y + v_fraction *
                static_cast<double>(preview.source_height - 1U) * preview.cell_size;
            const float radius_pixels = static_cast<float>(spatial_brush_radius /
                preview.cell_size * cell_width);
            draw->AddCircle(ImGui::GetIO().MousePos,
                std::clamp(radius_pixels, 2.0F, canvas_size.x),
                ImGui::ColorConvertFloat4ToU32(color(snapshot.colors.accent_primary)),
                0, 1.5F);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                carto::spatial_field::FieldBrush brush;
                brush.expected_revision = preview.source_revision;
                brush.center_uv = {spatial_brush_u, spatial_brush_v};
                brush.radius = spatial_brush_radius;
                brush.value = spatial_brush_value;
                brush.strength = spatial_brush_strength;
                brush.mode = static_cast<carto::spatial_field::BrushMode>(spatial_brush_mode);
                static_cast<void>(dispatch(carto::application::PaintSpatialFieldAction{
                    preview.identity, brush}));
            }
        }
    }

    void draw_workbench_instruments(const UiSnapshot& snapshot) {
        if (workbench.scene) draw_scene_instrument(snapshot);
        if (workbench.tools) draw_tools_instrument(snapshot);
        if (workbench.inspector) {
            if (begin_instrument(snapshot, carto::ui::WorkbenchInstrument::inspector,
                                 "Inspector", workbench.inspector, ImVec2(-8, 48),
                                 ImVec2(300, 340), snapshot.colors.accent_primary)) {
                draw_inspector(snapshot);
                end_instrument(carto::ui::WorkbenchInstrument::inspector, workbench.inspector);
            }
        }
        if (workbench.transform) draw_transform_instrument(snapshot);
        if (workbench.extrude) draw_extrude_instrument(snapshot);
        if (workbench.draft) draw_draft_instrument(snapshot);
        if (workbench.assets) draw_assets_instrument(snapshot);
        if (workbench.layers) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::layers, "Layers", workbench.layers,
            ImVec2(135, 330), ImVec2(280, 190),
            "Layer rack", "Layer visibility and ordering are reserved for a future governed workspace layer.");
        if (snapshot.workspace_kind == carto::ui::Workspace::engineering) {
            if (workbench.references) draw_engineering_instrument(snapshot);
        } else if (snapshot.workspace_kind == carto::ui::Workspace::condition) {
            if (workbench.references) draw_condition_instrument(snapshot);
        } else if (workbench.references) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::references, "References", workbench.references,
            ImVec2(250, 430), ImVec2(300, 185),
            "Reference rack", "Reference drawings remain an explicit future input boundary.");
        if (workbench.material) draw_material_instrument(snapshot);
        if (workbench.constraint) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::constraint, "Constraint", workbench.constraint,
            ImVec2(-8, 210), ImVec2(285, 170),
            "Constraint bay", "Constraint solving is not available in this capability slice.");
        if (workbench.modify) draw_modify_instrument(snapshot);
        if (workbench.measure) draw_surface_fidelity_instrument(snapshot);
    }

    void draw_project_surface(const UiSnapshot& snapshot) {
        ImGui::BeginChild("ProjectSurface", ImVec2(0, 0), false);
        heading_text_colored(snapshot.colors.accent_primary, "Project");
        ImGui::TextDisabled("The durable boundary for this workspace.");
        ImGui::Dummy(ImVec2(0, 6));

        ImGui::BeginChild("ProjectOverview", ImVec2(0, 122), true);
        ImGui::TextDisabled("Current project");
        ImGui::Text("%s", snapshot.project_name.c_str());
        mono_text_disabled(project_location(snapshot).c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 275.0F);
        ImGui::TextColored(status_color(snapshot), "%s", snapshot.project_status_text.c_str());
        ImGui::Text("Revision %llu", static_cast<unsigned long long>(snapshot.project_revision.value()));
        ImGui::SameLine();
        ImGui::TextDisabled("·");
        ImGui::SameLine();
        ImGui::Text("%zu object%s", snapshot.objects.size(), snapshot.objects.size() == 1U ? "" : "s");
        ImGui::EndChild();

        ImGui::BeginChild("ProjectMetrics", ImVec2(0, 112), true);
        ImGui::TextDisabled("Project signals");
        ImGui::Columns(4, "ProjectMetricsColumns", false);
        ImGui::Text("%llu", static_cast<unsigned long long>(snapshot.project_revision.value()));
        ImGui::TextDisabled("Revision");
        ImGui::NextColumn();
        ImGui::Text("%zu", snapshot.operations.size());
        ImGui::TextDisabled("Operations");
        ImGui::NextColumn();
        ImGui::Text("%zu", snapshot.undo_count);
        ImGui::TextDisabled("Undo available");
        ImGui::NextColumn();
        ImGui::Text("%zu", snapshot.redo_count);
        ImGui::TextDisabled("Redo available");
        ImGui::Columns(1);
        ImGui::EndChild();

        ImGui::BeginChild("ProjectLineage", ImVec2(0, 0), true);
        ImGui::TextDisabled("Lineage and persistence");
        if (snapshot.project_path.has_value()) {
            ImGui::TextColored(color(snapshot.colors.semantic_success), "Project path bound");
            ImGui::TextWrapped("%s", snapshot.project_path->string().c_str());
            ImGui::TextDisabled("Authoring changes remain routed through the session and its journal boundary.");
        } else {
            ImGui::TextColored(color(snapshot.colors.semantic_warning), "Not saved to disk");
            ImGui::TextDisabled("Save the project to establish a durable project path and recovery journal.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Save Project", ImVec2(128, 32))) {
            static_cast<void>(save_project(snapshot));
        }
        ImGui::SameLine();
        if (ImGui::Button("Save As", ImVec2(108, 32))) {
            static_cast<void>(save_project(snapshot, true));
        }
        ImGui::SameLine();
        if (ImGui::Button("Open Another", ImVec2(128, 32))) {
            if (const auto path = choose_file(false); path.has_value()) {
                carto::application::OpenProjectAction action{*path};
                if (snapshot.dirty) {
                    action.discard_dirty = true;
                    request_discard(std::move(action));
                } else {
                    static_cast<void>(dispatch(action));
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void draw_market_surface(const UiSnapshot& snapshot) {
        ImGui::BeginChild("MarketSurface", ImVec2(0, 0), false);
        heading_text_colored(snapshot.colors.accent_primary, "Community Market");
        ImGui::TextDisabled(
            "Local provider-neutral package library. Packages are validated as data-only before install.");
        ImGui::Dummy(ImVec2(0, 6));

        ImGui::BeginChild("MarketOverview", ImVec2(0, 128), true);
        ImGui::TextDisabled("Library boundary");
        if (snapshot.market.library_root.empty()) {
            ImGui::TextColored(color(snapshot.colors.semantic_warning), "Not configured");
            ImGui::TextDisabled("The desktop shell has not configured a local package library.");
        } else {
            ImGui::Text("Status: %s", carto::ui::market_status_name(snapshot.market.status));
            mono_text_disabled(snapshot.market.library_root.string().c_str());
            ImGui::TextWrapped("%s", snapshot.market.status_text.c_str());
        }
        if (ImGui::Button("Install data-only package...", ImVec2(208, 32))) {
            install_market_package();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(96, 32))) {
            const auto refreshed = ui.refresh_market_library();
            if (!refreshed) {
                action_feedback = refreshed.error().message;
                action_feedback_error = true;
            } else {
                action_feedback = "Local market library refreshed.";
                action_feedback_error = false;
            }
        }
        ImGui::EndChild();

        ImGui::BeginChild("MarketPackages", ImVec2(0, 0), true);
        ImGui::TextDisabled("Installed packages");
        if (snapshot.market.status == carto::ui::MarketStatus::error) {
            ImGui::TextColored(color(snapshot.colors.semantic_error), "Market evidence is invalid");
            ImGui::TextWrapped("%s", snapshot.market.status_text.c_str());
            ImGui::TextDisabled(
                "The library is fail-closed. Repair or remove the invalid local package evidence, then refresh.");
        } else if (snapshot.market.packages.empty()) {
            ImGui::TextDisabled(
                "No validated package is installed. Use Install data-only package to add a local directory.");
        } else {
            for (const auto& package : snapshot.market.packages) {
                ImGui::PushID((package.package_id + "@" + package.listing_version_id).c_str());
                ImGui::BeginChild("PackageCard", ImVec2(0, 170), true);
                ImGui::TextColored(color(snapshot.colors.accent_secondary), "%s@%s",
                                   package.package_id.c_str(), package.listing_version_id.c_str());
                ImGui::TextDisabled("Listing %s  ·  Publisher %s  ·  License %s",
                                    package.listing_id.c_str(), package.publisher_id.c_str(),
                                    package.license_id.c_str());
                if (package.source_revision.has_value()) {
                    ImGui::TextDisabled("Source revision %s", package.source_revision->c_str());
                }
                ImGui::Text("Payload %llu bytes  ·  %u file(s)",
                            static_cast<unsigned long long>(package.payload_bytes),
                            package.payload_files);
                ImGui::TextDisabled("Content %s", package.content_digest.c_str());
                ImGui::TextDisabled("Package %s", package.package_digest.c_str());
                if (package.publication_receipt_bound) {
                    ImGui::TextColored(color(snapshot.colors.semantic_success),
                                       "Detached publication receipt bound at install");
                } else {
                    ImGui::TextColored(color(snapshot.colors.semantic_warning),
                                       "Local package · no publication receipt");
                }
                ImGui::EndChild();
                ImGui::PopID();
            }
        }
        if (!action_feedback.empty()) {
            ImGui::Separator();
            draw_action_feedback(snapshot);
        }
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void draw_diagnostics_surface(const UiSnapshot& snapshot) {
        ImGui::BeginChild("DiagnosticsSurface", ImVec2(0, 0), false);
        heading_text_colored(snapshot.colors.accent_primary, "Diagnostics");
        ImGui::TextDisabled("Evidence about the current shell and application snapshot.");
        ImGui::Dummy(ImVec2(0, 6));

        ImGui::BeginChild("CapabilityCards", ImVec2(0, 118), true);
        ImGui::TextDisabled("Capability state");
        ImGui::Columns(3, "CapabilityColumns", false);
        ImGui::TextColored(renderer_ready ? color(snapshot.colors.semantic_success)
                                          : color(snapshot.colors.semantic_error),
                          renderer_ready ? "Active" : "Unavailable");
        ImGui::TextDisabled("Native shell");
        ImGui::NextColumn();
        if (snapshot.viewport.error.has_value()) {
            ImGui::TextColored(color(snapshot.colors.semantic_warning), "Unavailable");
            ImGui::TextDisabled("Viewport snapshot");
        } else {
            ImGui::TextColored(color(snapshot.colors.semantic_success), "Ready");
            ImGui::TextDisabled("Viewport snapshot");
        }
        ImGui::NextColumn();
        ImGui::TextColored(snapshot.project_path.has_value()
                               ? color(snapshot.colors.semantic_success)
                               : color(snapshot.colors.semantic_warning),
                          snapshot.project_path.has_value() ? "Bound" : "Local only");
        ImGui::TextDisabled("Persistence");
        ImGui::Columns(1);
        ImGui::EndChild();

        ImGui::BeginChild("DiagnosticDetails", ImVec2(0, 0), true);
        ImGui::TextDisabled("Current report");
        if (snapshot.viewport.error.has_value()) {
            ImGui::TextColored(color(snapshot.colors.semantic_warning), "Viewport: %s",
                               snapshot.viewport.error->message.c_str());
        }
        if (snapshot.surface_fidelity.status != carto::ui::SurfaceFidelityStatus::idle) {
            ImGui::Text("Surface fidelity: %s · %s",
                        carto::ui::surface_fidelity_status_name(snapshot.surface_fidelity.status),
                        carto::ui::surface_fidelity_mode_name(snapshot.surface_fidelity.mode));
            if (snapshot.surface_fidelity.object.has_value()) {
                ImGui::TextDisabled("Object %llu · %u -> %u triangles · %s",
                                    static_cast<unsigned long long>(
                                        snapshot.surface_fidelity.object->value),
                                    snapshot.surface_fidelity.source_triangles,
                                    snapshot.surface_fidelity.target_triangles,
                                    snapshot.surface_fidelity.admitted ? "admitted" : "not admitted");
            }
            if (!snapshot.surface_fidelity.status_text.empty()) {
                ImGui::TextWrapped("%s", snapshot.surface_fidelity.status_text.c_str());
            }
        }
        if (snapshot.problems.empty() && snapshot.ui_problems.empty() &&
            !snapshot.viewport.error.has_value() &&
            snapshot.surface_fidelity.status == carto::ui::SurfaceFidelityStatus::idle) {
            ImGui::TextColored(color(snapshot.colors.semantic_success), "No active problems reported.");
        }
        for (const auto& problem : snapshot.problems) {
            ImGui::BulletText("%s: %s", carto::core::error_code_name(problem.code),
                              problem.message.c_str());
        }
        for (const auto& problem : snapshot.ui_problems) {
            ImGui::BulletText("UI: %s", problem.message.c_str());
        }
        ImGui::Spacing();
        ImGui::TextDisabled("This surface reports observed state only. It does not manufacture live renderer, GPU, or AI capability.");
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void draw_settings_surface(const UiSnapshot& snapshot) {
        ImGui::BeginChild("SettingsSurface", ImVec2(0, 0), false);
        heading_text_colored(snapshot.colors.accent_primary, "Settings");
        ImGui::TextDisabled("Presentation preferences are local UI state; project truth remains in the application session.");
        ImGui::Dummy(ImVec2(0, 6));

        ImGui::BeginChild("WorkflowShortcuts", ImVec2(0, 214), true);
        heading_text("Workflow shortcuts");
        ImGui::TextDisabled("Compose flows without leaving the work surface.");
        ImGui::Text("Ctrl+Shift+1   Focus");
        ImGui::Text("Ctrl+Shift+2   Author");
        ImGui::Text("Ctrl+Shift+3   Inspect");
        ImGui::Text("Ctrl+Shift+4   Draft");
        ImGui::Text("Ctrl+Shift+R   Resume learned flow");
        ImGui::Text("Ctrl+Shift+L / I / T   Ledger / Inspector / Transform");
        ImGui::Text("Ctrl+I / Ctrl+D   Import mesh / Duplicate selected");
        ImGui::EndChild();

        ImGui::BeginChild("AppearanceSettings", ImVec2(0, 180), true);
        ImGui::TextDisabled("Appearance");
        ImGui::Text("Theme: %s", carto::ui::theme_name(snapshot.theme));
        ImGui::SameLine(150.0F);
        if (ImGui::Button("Dark")) static_cast<void>(ui.set_theme(carto::ui::Theme::dark));
        ImGui::SameLine();
        if (ImGui::Button("Light")) static_cast<void>(ui.set_theme(carto::ui::Theme::light));
        ImGui::SameLine();
        if (ImGui::Button("High contrast")) static_cast<void>(ui.set_theme(carto::ui::Theme::high_contrast));
        ImGui::Text("Density");
        ImGui::SameLine(150.0F);
        if (ImGui::Button("Compact")) static_cast<void>(ui.set_density(carto::ui::Density::compact));
        ImGui::SameLine();
        if (ImGui::Button("Standard")) static_cast<void>(ui.set_density(carto::ui::Density::standard));
        ImGui::SameLine();
        if (ImGui::Button("Touch")) static_cast<void>(ui.set_density(carto::ui::Density::touch));
        ImGui::EndChild();

        ImGui::BeginChild("OperatorSettings", ImVec2(0, 190), true);
        ImGui::TextDisabled("Operator boundary");
        ImGui::Text("Current mode: %s", carto::ui::operator_mode_name(snapshot.operator_mode));
        if (ImGui::Button("Person-first", ImVec2(130, 32))) {
            static_cast<void>(ui.set_operator_mode(carto::ui::OperatorMode::person_first));
        }
        ImGui::SameLine();
        if (ImGui::Button("AI-first", ImVec2(130, 32))) {
            static_cast<void>(ui.set_operator_mode(carto::ui::OperatorMode::ai_first));
        }
        bool auto_approve = snapshot.ai_auto_approve;
        if (ImGui::Checkbox("Auto-approve safe native studies", &auto_approve)) {
            static_cast<void>(ui.set_ai_auto_approve(auto_approve));
        }
        ImGui::TextDisabled("Default on. The AI can invoke the same typed Apply boundary.");
        ImGui::EndChild();

        ImGui::BeginChild("PanelSettings", ImVec2(0, 0), true);
        ImGui::TextDisabled("Instrument rack state");
        const auto instrument_toggle = [this](const char* label, bool& visible) {
            if (ImGui::Checkbox(label, &visible)) mark_custom_workbench();
        };
        instrument_toggle("Scene", workbench.scene);
        instrument_toggle("Assets", workbench.assets);
        instrument_toggle("Tools", workbench.tools);
        instrument_toggle("Draft", workbench.draft);
        instrument_toggle("Inspector", workbench.inspector);
        instrument_toggle("Transform", workbench.transform);
        instrument_toggle("Material", workbench.material);
        instrument_toggle("Constraint", workbench.constraint);
        instrument_toggle("Modify", workbench.modify);
        instrument_toggle("Fidelity", workbench.measure);
        instrument_toggle("Extrude", workbench.extrude);
        ImGui::TextDisabled("The viewport and machine frame remain fixed.\nRack instruments can be reopened at any time.");
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void draw_agent_condition_inventory(
        const UiSnapshot& snapshot,
        const carto::agent::SceneQueryResult& inventory) {
        const bool active_task = snapshot.agent_task.has_value() &&
            snapshot.agent_task->status != carto::agent::TaskStatus::accepted &&
            snapshot.agent_task->status != carto::agent::TaskStatus::rejected;
        const char* scope_label = active_task ? "task scope" : "selected scope";
        ImGui::Text("CONDITION inventory · %s · R%llu · %zu asset fact(s)%s",
            scope_label,
            static_cast<unsigned long long>(inventory.source_revision.value()),
            inventory.rows.size(), inventory.truncated ? " · partial" : "");
        for (const auto& diagnostic : inventory.diagnostics) {
            ImGui::TextColored(color(snapshot.colors.semantic_warning),
                "%s", diagnostic.message.c_str());
        }
        for (const auto& row : inventory.rows) {
            const auto* fact = std::get_if<
                carto::agent::ConditionAssetInventoryFact>(&row);
            if (fact == nullptr) continue;
            const std::string asset_id_text = std::to_string(fact->asset);
            ImGui::PushID(asset_id_text.c_str());
            const auto asset_class =
                carto::condition::asset_class_name(fact->asset_class);
            const auto fidelity =
                carto::condition::fidelity_class_name(fact->fidelity);
            ImGui::Text("Asset %llu · object %llu · %.*s · %.*s",
                static_cast<unsigned long long>(fact->asset),
                static_cast<unsigned long long>(fact->authored_object.value),
                static_cast<int>(asset_class.size()), asset_class.data(),
                static_cast<int>(fidelity.size()), fidelity.data());
            ImGui::TextDisabled("Source %llu · %s · %s · revision %s",
                static_cast<unsigned long long>(fact->source_id),
                fact->source_current ? "current" : "stale",
                fact->source_reviewed ? "reviewed" : "unreviewed",
                fact->source_revision.c_str());
            ImGui::BulletText(
                "Safe Place %llu · familiarity %llu · views %llu · variants %llu · temporal %llu",
                static_cast<unsigned long long>(fact->safe_place_profiles),
                static_cast<unsigned long long>(fact->familiarity_records),
                static_cast<unsigned long long>(fact->view_conditions),
                static_cast<unsigned long long>(fact->variant_candidates),
                static_cast<unsigned long long>(fact->temporal_packages));
            ImGui::BulletText(
                "Surface %llu · anchors %llu · collections %llu · prefabs %llu · kits %llu · related counts %s",
                static_cast<unsigned long long>(fact->surface_states),
                static_cast<unsigned long long>(fact->interaction_anchors),
                static_cast<unsigned long long>(fact->collection_memberships),
                static_cast<unsigned long long>(fact->prefab_memberships),
                static_cast<unsigned long long>(fact->region_kit_memberships),
                fact->related_records_complete ? "complete" : "incomplete");
            if (!fact->protected_features.empty()) {
                std::string protected_features = "Protected features: ";
                for (std::size_t index = 0U;
                     index < fact->protected_features.size(); ++index) {
                    if (index != 0U) protected_features += ", ";
                    protected_features += fact->protected_features[index];
                }
                ImGui::TextWrapped("%s", protected_features.c_str());
            }
            ImGui::PopID();
        }
    }

    void draw_agent_task_strip(const UiSnapshot& snapshot) {
        if (!snapshot.agent_task_strip.has_value()) return;
        const auto& task = *snapshot.agent_task_strip;
        const carto::ui::UiColor status_token =
            task.status == carto::agent::TaskStatus::review
                ? snapshot.colors.semantic_success
                : task.status == carto::agent::TaskStatus::blocked
                    ? snapshot.colors.semantic_warning
                    : snapshot.colors.accent_primary;
        ImGui::BeginChild("AgentTaskStrip", ImVec2(0, 48), true,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(color(status_token), "Agent task %s · %s",
            task.task_id.c_str(), carto::agent::task_status_name(task.status));
        ImGui::SameLine();
        ImGui::TextDisabled("%zu/%zu steps · %zu/%zu required gates",
            task.passed_steps, task.total_steps,
            task.passed_required_gates, task.required_gates);
        ImGui::SameLine();
        const bool review_ready = task.status == carto::agent::TaskStatus::review;
        const char* action_label = review_ready
            ? "Review result" : task.status == carto::agent::TaskStatus::blocked
                ? "Resolve task" : "Open task";
        if (ImGui::SmallButton(action_label)) {
            if (review_ready) {
                static_cast<void>(open_agent_review_workspace());
            } else {
                static_cast<void>(open_agent_workspace(false));
            }
        }
        if (task.blocker.has_value() && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextWrapped("%s", task.blocker->message.c_str());
            ImGui::EndTooltip();
        }
        ImGui::EndChild();
    }

    void draw_workspace_surface(const UiSnapshot& snapshot) {
        draw_workspace_bar(snapshot);
        draw_agent_task_strip(snapshot);
        draw_toolbar(snapshot);
        workbench_bounds_valid = false;
        ImGui::BeginChild("WorkbenchLayout", ImVec2(0, 0), false);
        const float rack_width = kWorkbenchRackWidth;
        const float bay_gap = ImGui::GetStyle().ItemSpacing.x;
        const float center_width = std::max(
            320.0F, ImGui::GetContentRegionAvail().x - rack_width * 2.0F - bay_gap * 2.0F);
        draw_tool_rack(snapshot);
        ImGui::SameLine();
        ImGui::BeginChild("WorkSurfaceFrame", ImVec2(center_width, 0), false);
        workbench_origin = ImGui::GetWindowPos();
        workbench_extent = ImGui::GetWindowSize();
        workbench_bounds_valid = true;
        const float ledger_height = workbench.ledger_collapsed ? 48.0F : 176.0F;
        draw_viewport(snapshot, ImVec2(0, -ledger_height - ImGui::GetStyle().ItemSpacing.y));
        draw_bottom(snapshot, ledger_height);
        ImGui::EndChild();
        ImGui::SameLine();
        draw_bay_rack(snapshot);
        ImGui::EndChild();
    }

    void draw_ai_context(const UiSnapshot& snapshot) {
        ImGui::BeginChild("AI Context", ImVec2(260, 0), true);
        heading_text("AI first");
        ImGui::Separator();
        ImGui::TextDisabled("Intent");
        std::array<char, 8193> intent{};
        const std::size_t copy_bytes = std::min(ai_intent.size(), intent.size() - 1U);
        std::copy_n(ai_intent.data(), copy_bytes, intent.data());
        if (ImGui::InputTextMultiline("##intent", intent.data(), intent.size(), ImVec2(-1, 100))) {
            ai_intent.assign(intent.data());
        }
        ImGui::TextDisabled("Constraints");
        ImGui::BulletText("Native proposal planner");
        ImGui::BulletText("No proposal is authoritative");
        ImGui::Separator();
        ImGui::TextWrapped("%s", snapshot.ai_status.c_str());
        if (ai_proposal.has_value()) {
            ImGui::Text("Pending: %s", ai_proposal->tool_id.c_str());
            ImGui::TextDisabled("Revision %llu", static_cast<unsigned long long>(
                ai_proposal->base_revision.value()));
        }
        ImGui::EndChild();
    }

    void draw_bottom(const UiSnapshot& snapshot, float height) {
        ImGui::BeginChild("OperationLedger", ImVec2(0, height), true);
        heading_text_colored(snapshot.colors.accent_primary, "Operation ledger");
        ImGui::SameLine();
        ImGui::TextDisabled("revision-bound activity");
        ImGui::SameLine(ImGui::GetWindowWidth() - 90.0F);
        if (ImGui::SmallButton(workbench.ledger_collapsed ? "Expand" : "Collapse")) {
            workbench.ledger_collapsed = !workbench.ledger_collapsed;
        }

        if (selected_operation_id.has_value() && std::none_of(
                snapshot.operations.begin(), snapshot.operations.end(),
                [this](const auto& operation) {
                    return operation.operation_id == *selected_operation_id;
                })) {
            selected_operation_id.reset();
        }
        if (!selected_operation_id.has_value() && !snapshot.operations.empty()) {
            selected_operation_id = snapshot.operations.back().operation_id;
        }
        const auto selected_operation_iterator = std::find_if(
            snapshot.operations.begin(), snapshot.operations.end(),
            [this](const auto& operation) {
                return selected_operation_id.has_value() &&
                    operation.operation_id == *selected_operation_id;
            });
        const carto::ui::OperationView* selected_operation =
            selected_operation_iterator == snapshot.operations.end()
                ? nullptr : &*selected_operation_iterator;

        if (workbench.ledger_collapsed) {
            if (snapshot.operations.empty()) ImGui::TextDisabled("No committed operations yet");
            else {
                const auto& operation = selected_operation != nullptr
                    ? *selected_operation : snapshot.operations.back();
                ImGui::Text("%s  ·  rev %llu", operation.action.c_str(),
                            static_cast<unsigned long long>(operation.revision_after.value()));
                ImGui::SameLine();
                if (ImGui::SmallButton("Details")) workbench.ledger_collapsed = false;
            }
            ImGui::EndChild();
            return;
        }
        if (snapshot.operations.empty()) {
            ImGui::TextDisabled("No operations yet. Authoring receipts will settle here.");
        } else {
            ImGui::BeginChild("LedgerTimeline", ImVec2(0, 42), false,
                              ImGuiWindowFlags_HorizontalScrollbar);
            const std::size_t timeline_start = snapshot.operations.size() > 8U
                ? snapshot.operations.size() - 8U : 0U;
            for (std::size_t timeline_index = timeline_start;
                 timeline_index < snapshot.operations.size(); ++timeline_index) {
                const auto& operation = snapshot.operations.at(timeline_index);
                if (timeline_index != timeline_start) ImGui::SameLine();
                const bool selected = selected_operation_id == operation.operation_id;
                const std::string label = "● " + operation.action + "##ledger_operation_" +
                    std::to_string(operation.operation_id);
                if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(172, 28))) {
                    selected_operation_id = operation.operation_id;
                }
                if (timeline_index + 1U < snapshot.operations.size()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("›");
                }
            }
            ImGui::EndChild();
        }
        static constexpr std::array<std::pair<carto::ui::BottomPanel, const char*>, 6> tabs{{
            {carto::ui::BottomPanel::operations, "Operations"},
            {carto::ui::BottomPanel::graph, "Graph"},
            {carto::ui::BottomPanel::assets, "Assets"},
            {carto::ui::BottomPanel::timeline, "Timeline"},
            {carto::ui::BottomPanel::problems, "Problems"},
            {carto::ui::BottomPanel::console, "Console"},
        }};
        for (const auto& [panel, label] : tabs) {
            if (panel == snapshot.bottom_panel) ImGui::PushStyleColor(ImGuiCol_Button, color(snapshot.colors.panel_selected));
            if (ImGui::Button(label)) static_cast<void>(ui.set_bottom_panel(panel));
            if (panel == snapshot.bottom_panel) ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::Separator();
        if (snapshot.bottom_panel == carto::ui::BottomPanel::operations) {
            ImGui::Text("Operations  %llu  ·  Problems  %llu  ·  Undo  %llu  ·  Redo  %llu",
                    static_cast<unsigned long long>(snapshot.operations.size()),
                    static_cast<unsigned long long>(snapshot.problems.size()),
                    static_cast<unsigned long long>(snapshot.undo_count),
                    static_cast<unsigned long long>(snapshot.redo_count));
            if (selected_operation != nullptr) {
                ImGui::BeginChild("LedgerReceiptDetail", ImVec2(0, 86), true);
                ImGui::Text("Receipt #%llu  ·  %s",
                            static_cast<unsigned long long>(selected_operation->operation_id),
                            selected_operation->action.c_str());
                ImGui::Text("Revision %llu -> %llu  |  document %s",
                            static_cast<unsigned long long>(selected_operation->revision_before.value()),
                            static_cast<unsigned long long>(selected_operation->revision_after.value()),
                            selected_operation->document_changed ? "changed" : "unchanged");
                ImGui::TextDisabled("This is a session receipt, not a second project history.");
                ImGui::BeginDisabled();
                ImGui::SmallButton("Reopen parameters");
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("Unavailable: this receipt retains no editable parameter payload.");
                ImGui::EndChild();
            } else {
                ImGui::TextDisabled("Select an operation above to inspect its receipt.");
            }
            std::size_t displayed_operations = 0U;
            for (auto iterator = snapshot.operations.rbegin();
                 iterator != snapshot.operations.rend() && displayed_operations < 6U;
                 ++iterator, ++displayed_operations) {
                ImGui::Text("#%llu %s  rev %llu -> %llu",
                            static_cast<unsigned long long>(iterator->operation_id),
                            iterator->action.c_str(),
                            static_cast<unsigned long long>(iterator->revision_before.value()),
                            static_cast<unsigned long long>(iterator->revision_after.value()));
            }
        } else if (snapshot.bottom_panel == carto::ui::BottomPanel::problems) {
            for (const auto& problem : snapshot.problems) {
                ImGui::TextColored(color(snapshot.colors.semantic_error), "%s: %s",
                                   carto::core::error_code_name(problem.code), problem.message.c_str());
            }
            for (const auto& problem : snapshot.ui_problems) {
                ImGui::TextColored(color(snapshot.colors.semantic_warning), "UI: %s", problem.message.c_str());
            }
        } else {
            const std::size_t bottom_index = static_cast<std::size_t>(snapshot.bottom_panel);
            const char* bottom_label = bottom_index < tabs.size() ? tabs[bottom_index].second : "Unknown";
            ImGui::TextDisabled("%s panel is reserved for the corresponding future subsystem.",
                               bottom_label);
        }
        ImGui::EndChild();
    }

    void draw() {
        if (surface != ShellSurface::workspace) {
            native_viewport_hits.reset();
            work_surface_camera_panning = false;
        }
        poll_preparation_job();
        process_close_request();
        route_shortcuts();
        const auto snapshot = ui.snapshot();
        if (!previous_workspace_kind.has_value() ||
            *previous_workspace_kind != snapshot.workspace_kind) {
            if (snapshot.workspace_kind == carto::ui::Workspace::sculpt) {
                workbench.tools = true;
            } else if (snapshot.workspace_kind == carto::ui::Workspace::engineering) {
                workbench.scene = true;
                workbench.references = true;
                workbench.inspector = false;
                workbench.tools = false;
            } else if (snapshot.workspace_kind == carto::ui::Workspace::condition) {
                workbench.scene = true;
                workbench.references = true;
                workbench.inspector = false;
                workbench.tools = false;
            }
            previous_workspace_kind = snapshot.workspace_kind;
        }
        apply_theme(snapshot);
        apply_window_chrome(snapshot);
        draw_discard_prompt();
        draw_menu(snapshot);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        const ImGuiWindowFlags shell_flags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("Cartographer Shell", nullptr, shell_flags);
        draw_shell_header(snapshot);
        ImGui::BeginChild("ShellBody", ImVec2(0, 0), false);
        draw_navigation_rail(snapshot);
        ImGui::SameLine();
        ImGui::BeginChild("SurfaceHost", ImVec2(0, 0), false);
        switch (surface) {
        case ShellSurface::workspace:
            draw_workspace_surface(snapshot);
            break;
        case ShellSurface::project:
            draw_project_surface(snapshot);
            break;
        case ShellSurface::market:
            draw_market_surface(snapshot);
            break;
        case ShellSurface::diagnostics:
            draw_diagnostics_surface(snapshot);
            break;
        case ShellSurface::settings:
            draw_settings_surface(snapshot);
            break;
        }
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::End();
        if (surface == ShellSurface::workspace) draw_workbench_instruments(snapshot);
        draw_command_palette(snapshot);
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(l_param);
        limits->ptMinTrackSize.x = 1120;
        limits->ptMinTrackSize.y = 720;
        return 0;
    }
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && g_state != nullptr) {
        const auto native_key = [&]() -> std::optional<carto::ui::NativeKey> {
            switch (w_param) {
            case 'S': return carto::ui::NativeKey::s;
            case 'Z': return carto::ui::NativeKey::z;
            case 'Y': return carto::ui::NativeKey::y;
            case 'R': return carto::ui::NativeKey::r;
            case 'K': return carto::ui::NativeKey::k;
            case '1': return carto::ui::NativeKey::digit_1;
            case '2': return carto::ui::NativeKey::digit_2;
            case '3': return carto::ui::NativeKey::digit_3;
            case '4': return carto::ui::NativeKey::digit_4;
            case VK_ESCAPE: return carto::ui::NativeKey::escape;
            default: return std::nullopt;
            }
        }();
        if (native_key.has_value()) {
            std::uint8_t modifiers = 0U;
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                modifiers |= carto::ui::kNativeModifierControl;
            }
            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                modifiers |= carto::ui::kNativeModifierShift;
            }
            if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
                modifiers |= carto::ui::kNativeModifierAlt;
            }
            const bool repeat =
                (static_cast<std::uint64_t>(l_param) & (1ULL << 30U)) != 0U;
            if (g_state->route_native_key(
                    carto::ui::NativeKeyEvent{*native_key, modifiers, true, repeat})) {
                return 0;
            }
        }
    }
    if (message == WM_LBUTTONDOWN && g_state != nullptr) {
        std::uint8_t modifiers = 0U;
        if ((w_param & MK_CONTROL) != 0U) modifiers |= carto::ui::kNativeModifierControl;
        if ((w_param & MK_SHIFT) != 0U) modifiers |= carto::ui::kNativeModifierShift;
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= carto::ui::kNativeModifierAlt;
        const carto::ui::NativePointerEvent event{
            carto::ui::NativePointerButton::primary,
            static_cast<std::int32_t>(static_cast<SHORT>(LOWORD(l_param))),
            static_cast<std::int32_t>(static_cast<SHORT>(HIWORD(l_param))),
            modifiers,
            true};
        if (g_state->begin_native_sculpt_stroke(event)) return 0;
        if (g_state->route_native_pointer(event)) return 0;
    }
    if (message == WM_MOUSEMOVE && g_state != nullptr &&
        g_state->native_sculpt_stroke.has_value()) {
        const carto::ui::NativePointerEvent event{
            carto::ui::NativePointerButton::primary,
            static_cast<std::int32_t>(static_cast<SHORT>(LOWORD(l_param))),
            static_cast<std::int32_t>(static_cast<SHORT>(HIWORD(l_param))),
            0U,
            (w_param & MK_LBUTTON) != 0U};
        if (g_state->continue_native_sculpt_stroke(event)) return 0;
    }
    if (message == WM_MOUSEMOVE && g_state != nullptr &&
        g_state->native_condition_gizmo_drag.has_value()) {
        std::uint8_t modifiers = 0U;
        if ((w_param & MK_CONTROL) != 0U) modifiers |= carto::ui::kNativeModifierControl;
        if ((w_param & MK_SHIFT) != 0U) modifiers |= carto::ui::kNativeModifierShift;
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= carto::ui::kNativeModifierAlt;
        const carto::ui::NativePointerEvent event{
            carto::ui::NativePointerButton::primary,
            static_cast<std::int32_t>(static_cast<SHORT>(LOWORD(l_param))),
            static_cast<std::int32_t>(static_cast<SHORT>(HIWORD(l_param))),
            modifiers,
            (w_param & MK_LBUTTON) != 0U};
        if (g_state->route_native_condition_gizmo_drag(event)) return 0;
    }
    if (message == WM_MOUSEMOVE && g_state != nullptr &&
        g_state->native_transform_drag.has_value()) {
        std::uint8_t modifiers = 0U;
        if ((w_param & MK_CONTROL) != 0U) modifiers |= carto::ui::kNativeModifierControl;
        if ((w_param & MK_SHIFT) != 0U) modifiers |= carto::ui::kNativeModifierShift;
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= carto::ui::kNativeModifierAlt;
        const carto::ui::NativePointerEvent event{
            carto::ui::NativePointerButton::primary,
            static_cast<std::int32_t>(static_cast<SHORT>(LOWORD(l_param))),
            static_cast<std::int32_t>(static_cast<SHORT>(HIWORD(l_param))),
            modifiers,
            (w_param & MK_LBUTTON) != 0U};
        if (g_state->route_native_transform_drag(event)) return 0;
    }
    if (message == WM_LBUTTONUP && g_state != nullptr &&
        g_state->native_sculpt_stroke.has_value()) {
        const carto::ui::NativePointerEvent event{
            carto::ui::NativePointerButton::primary,
            static_cast<std::int32_t>(static_cast<SHORT>(LOWORD(l_param))),
            static_cast<std::int32_t>(static_cast<SHORT>(HIWORD(l_param))),
            0U,
            false};
        if (g_state->finish_native_sculpt_stroke(event)) return 0;
    }
    if (message == WM_LBUTTONUP && g_state != nullptr &&
        g_state->native_condition_gizmo_drag.has_value()) {
        std::uint8_t modifiers = 0U;
        if ((w_param & MK_CONTROL) != 0U) modifiers |= carto::ui::kNativeModifierControl;
        if ((w_param & MK_SHIFT) != 0U) modifiers |= carto::ui::kNativeModifierShift;
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= carto::ui::kNativeModifierAlt;
        const carto::ui::NativePointerEvent event{
            carto::ui::NativePointerButton::primary,
            static_cast<std::int32_t>(static_cast<SHORT>(LOWORD(l_param))),
            static_cast<std::int32_t>(static_cast<SHORT>(HIWORD(l_param))),
            modifiers,
            false};
        if (g_state->route_native_condition_gizmo_drag(event)) return 0;
    }
    if (message == WM_LBUTTONUP && g_state != nullptr &&
        g_state->native_transform_drag.has_value()) {
        std::uint8_t modifiers = 0U;
        if ((w_param & MK_CONTROL) != 0U) modifiers |= carto::ui::kNativeModifierControl;
        if ((w_param & MK_SHIFT) != 0U) modifiers |= carto::ui::kNativeModifierShift;
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= carto::ui::kNativeModifierAlt;
        const carto::ui::NativePointerEvent event{
            carto::ui::NativePointerButton::primary,
            static_cast<std::int32_t>(static_cast<SHORT>(LOWORD(l_param))),
            static_cast<std::int32_t>(static_cast<SHORT>(HIWORD(l_param))),
            modifiers,
            false};
        if (g_state->route_native_transform_drag(event)) return 0;
    }
    if (message == WM_CANCELMODE && g_state != nullptr &&
        g_state->native_sculpt_stroke.has_value()) {
        g_state->cancel_native_sculpt_stroke("Sculpt stroke cancelled by the window system.");
    }
    if (message == WM_CANCELMODE && g_state != nullptr &&
        g_state->native_transform_drag.has_value()) {
        g_state->cancel_native_transform_drag("Transform cancelled by the window system.");
    }
    if (message == WM_CANCELMODE && g_state != nullptr &&
        g_state->native_condition_gizmo_drag.has_value()) {
        g_state->cancel_native_condition_gizmo(
            "Acoustic gizmo cancelled by the window system.");
    }
    if (message == WM_CAPTURECHANGED && g_state != nullptr &&
        g_state->native_sculpt_stroke.has_value()) {
        g_state->cancel_native_sculpt_stroke(
            "Sculpt stroke cancelled because pointer capture changed.", false);
    }
    if (message == WM_CAPTURECHANGED && g_state != nullptr &&
        g_state->native_transform_drag.has_value()) {
        g_state->cancel_native_transform_drag(
            "Transform cancelled because pointer capture changed.", false);
    }
    if (message == WM_CAPTURECHANGED && g_state != nullptr &&
        g_state->native_condition_gizmo_drag.has_value()) {
        g_state->cancel_native_condition_gizmo(
            "Acoustic gizmo cancelled because pointer capture changed.", false);
    }
    if (ImGui_ImplWin32_WndProcHandler(window, message, w_param, l_param)) return 1;
    if (message == WM_SIZE && g_state != nullptr && w_param != SIZE_MINIMIZED) {
        g_state->renderer.request_resize();
    }
    if (message == WM_CLOSE && g_state != nullptr) {
        g_state->request_close();
        return 0;
    }
    if (message == WM_DESTROY) return 0;
    return DefWindowProcW(window, message, w_param, l_param);
}

HWND create_window(HINSTANCE instance) {
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = L"CartographerDesktopWindow";
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (RegisterClassW(&window_class) == 0) throw std::runtime_error("could not register Cartographer window class");
    HWND window = CreateWindowExW(
        0, window_class.lpszClassName,
        L"Cartographer | Spatial Intelligence by Design",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) throw std::runtime_error("could not create Cartographer window");
    return window;
}

int run(HINSTANCE instance) {
    DesktopState state;
    state.load_preferences();
    g_state = &state;
    state.window = create_window(instance);
    state.load_market_library();
    state.load_workbench_preferences();
    ImGui::CreateContext();
    // UI preferences are owned by UiController; do not let Dear ImGui create
    // an unmanaged imgui.ini beside the executable or in the checkout.
    ImGui::GetIO().IniFilename = nullptr;
    state.load_fonts();
    ImGui::StyleColorsDark();
        if (!ImGui_ImplWin32_Init(state.window)) throw std::runtime_error("Dear ImGui Win32 initialization failed");
        state.renderer.initialize(state.window);
        state.renderer_ready = true;

    MSG message{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            if (message.message == WM_QUIT) running = false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) break;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        state.draw();
        ImGui::Render();
        state.renderer.render(ImGui::GetDrawData());
    }

    state.save_preferences();
    state.save_workbench_preferences();
    state.renderer_ready = false;
    state.renderer.shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_state = nullptr;
    return 0;
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    try {
        const auto runtime = carto::vulkan::Runtime::probe();
        if (!runtime) {
            MessageBoxW(nullptr, wide(runtime.error().message).c_str(), L"Cartographer Vulkan unavailable", MB_OK | MB_ICONERROR);
            return 2;
        }
        return run(instance);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, wide(error.what()).c_str(), L"Cartographer desktop error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
