#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#define IMGUI_DEFINE_MATH_OPERATORS

#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <vulkan/vulkan.h>

#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>
#include <imgui_internal.h>

// Dear ImGui intentionally keeps the Win32 message handler declaration behind
// a disabled block in its public backend header to avoid pulling in windows.h.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window,
    UINT message,
    WPARAM w_param,
    LPARAM l_param);

#include <carto/application/application.hpp>
#include <carto/io/builtin_provider.hpp>
#include <carto/io/gltf_export.hpp>
#include <carto/io/obj.hpp>
#include <carto/io/ply.hpp>
#include <carto/io/stl.hpp>
#include <carto/providers/registry.hpp>
#include <carto/ui/ui.hpp>
#include <carto/vulkan/runtime.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
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
        create_render_pass();
        create_command_objects();
        create_descriptor_pool();
        create_swapchain();

        ImGui_ImplVulkan_InitInfo init_info{};
        init_info.ApiVersion = VK_API_VERSION_1_0;
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

        check_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");

        std::uint32_t image_index = 0;
        const VkResult acquired = vkAcquireNextImageKHR(
            device_, swapchain_, UINT64_MAX, image_available_, VK_NULL_HANDLE, &image_index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_SUBOPTIMAL_KHR) {
            resize_pending_ = true;
            return;
        }
        check_vk(acquired, "vkAcquireNextImageKHR");
        check_vk(vkResetFences(device_, 1, &fence_), "vkResetFences");

        check_vk(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        check_vk(vkBeginCommandBuffer(command_buffer_, &begin), "vkBeginCommandBuffer");

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
        vkCmdBeginRenderPass(command_buffer_, &pass, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(draw_data, command_buffer_);
        vkCmdEndRenderPass(command_buffer_);
        check_vk(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer");

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &image_available_;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer_;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_finished_;
        check_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit");

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished_;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image_index;
        const VkResult presented = vkQueuePresentKHR(queue_, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
            resize_pending_ = true;
            return;
        }
        check_vk(presented, "vkQueuePresentKHR");
    }

private:
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
        app.apiVersion = VK_API_VERSION_1_0;
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
        allocate.commandBufferCount = 1;
        check_vk(vkAllocateCommandBuffers(device_, &allocate, &command_buffer_), "vkAllocateCommandBuffers");

        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        check_vk(vkCreateSemaphore(device_, &semaphore, nullptr, &image_available_), "vkCreateSemaphore");
        check_vk(vkCreateSemaphore(device_, &semaphore, nullptr, &render_finished_), "vkCreateSemaphore");
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check_vk(vkCreateFence(device_, &fence, nullptr, &fence_), "vkCreateFence");
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

    void create_swapchain() {
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
        VkSurfaceFormatKHR selected_format{};
        bool found_preferred_format = false;
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM) {
                selected_format = format;
                found_preferred_format = true;
                break;
            }
        }
        if (!found_preferred_format) {
            throw std::runtime_error("desktop shell requires a BGRA8 UNORM swapchain format");
        }
        swapchain_format_ = selected_format.format;
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

        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface_;
        info.minImageCount = image_count_;
        info.imageFormat = swapchain_format_;
        info.imageColorSpace = selected_format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        check_vk(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR");

        std::uint32_t actual_count = 0;
        check_vk(vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, nullptr), "vkGetSwapchainImagesKHR");
        images_.resize(actual_count);
        check_vk(vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, images_.data()), "vkGetSwapchainImagesKHR");
        image_views_.resize(images_.size());
        framebuffers_.resize(images_.size());
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
            if (fence_ != VK_NULL_HANDLE) vkDestroyFence(device_, fence_, nullptr);
            if (image_available_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, image_available_, nullptr);
            if (render_finished_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, render_finished_, nullptr);
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
    std::uint32_t swapchain_width_ = 0;
    std::uint32_t swapchain_height_ = 0;
    std::uint32_t image_count_ = 2;
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkSemaphore image_available_ = VK_NULL_HANDLE;
    VkSemaphore render_finished_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool imgui_initialized_ = false;
    bool resize_pending_ = false;
};

struct DesktopState;
DesktopState* g_state = nullptr;

enum class ShellSurface {
    workspace,
    project,
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

struct NativeViewportHitCache {
    carto::core::Revision project_revision;
    ImVec2 screen_origin{};
    ImVec2 extent{};
    bool valid = false;
    bool interactive = false;
    std::vector<ViewportFaceHit> faces;
    std::vector<ViewportVertexHit> vertices;
    std::vector<ViewportEdgeHit> edges;
    std::vector<std::pair<carto::scene::ObjectId, ImRect>> objects;

    void reset() {
        project_revision = {};
        screen_origin = {};
        extent = {};
        valid = false;
        interactive = false;
        faces.clear();
        vertices.clear();
        edges.clear();
        objects.clear();
    }
};

[[nodiscard]] Vec3d world_point(const carto::core::Transform& transform, Vec3d point) {
    return transform.translation +
           transform.rotation.rotate(carto::core::componentwise_multiply(transform.scale, point));
}

enum class FileDialogKind {
    project,
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
    }
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
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

struct DesktopState {
    ApplicationSession session;
    carto::ui::UiController ui;
    VulkanShell renderer;
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
    carto::scene::ObjectId inspector_object{};
    carto::core::Transform inspector_transform = carto::core::Transform::identity();
    carto::geometry::VertexId inspector_vertex{};
    Vec3d inspector_vertex_position{};
    double extrude_distance = 0.25;
    double inset_distance = 0.15;
    double edge_split_factor = 0.5;
    std::optional<carto::application::ApplicationAction> pending_discard_action;
    std::uint64_t inspector_generation = std::numeric_limits<std::uint64_t>::max();
    bool pending_exit = false;
    bool discard_prompt_pending = false;
    bool close_requested = false;
    std::string ai_intent;
    std::optional<carto::ai::Proposal> ai_proposal;
    std::optional<carto::editor::AuthoringPreview> ai_preview;
    std::string ai_feedback;
    std::optional<std::uint64_t> selected_operation_id;

    static constexpr float kWorkbenchRackWidth = 128.0F;
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

        if (const auto path = system_font_path(L"segoeui.ttf"); path.has_value()) {
            body_font = io.Fonts->AddFontFromFileTTF(
                path->c_str(), 14.5F, &font_config, glyph_ranges);
        }
        if (const auto path = system_font_path(L"segoeuib.ttf"); path.has_value()) {
            heading_font = io.Fonts->AddFontFromFileTTF(
                path->c_str(), 17.0F, &font_config, glyph_ranges);
        }
        if (const auto path = system_font_path(L"CascadiaMono.ttf"); path.has_value()) {
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
        const bool draft = flow == WorkbenchFlow::draft;
        workbench.scene = scene;
        workbench.tools = tools;
        workbench.inspector = inspector;
        workbench.transform = transform;
        workbench.draft = draft;
        workbench.assets = false;
        workbench.layers = false;
        workbench.references = false;
        workbench.material = false;
        workbench.constraint = false;
        workbench.modify = false;
        workbench.measure = false;
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

    bool dispatch(const carto::application::ApplicationAction& action) {
        return static_cast<bool>(ui.dispatch(action));
    }

    bool route_native_key(const carto::ui::NativeKeyEvent& event) {
        if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantTextInput) {
            return false;
        }
        const auto shortcut = carto::ui::shortcut_for_native_key(event);
        if (!shortcut.has_value()) return false;
        if (!event.repeat) static_cast<void>(ui.handle_shortcut(*shortcut));
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
        if (document.value().meshes().empty()) {
            MessageBoxW(window, L"The project has no mesh assets to export.",
                        L"Cartographer export unavailable", MB_OK | MB_ICONWARNING);
            return;
        }
        const auto export_mesh = [&]() -> carto::core::Result<carto::io::IoReport> {
            const auto& mesh = document.value().meshes().begin()->second;
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
        // HAVEN-inspired geometry: cards and controls should feel calm and
        // current while the workbench remains physically rearrangeable.
        style.WindowRounding = 12.0F;
        style.ChildRounding = 12.0F;
        style.FrameRounding = 8.0F;
        style.PopupRounding = 10.0F;
        style.ScrollbarRounding = 8.0F;
        style.WindowBorderSize = 0.0F;
        style.ChildBorderSize = 1.0F;
        style.FrameBorderSize = 1.0F;
        style.ScrollbarSize = 10.0F;
        style.GrabMinSize = 16.0F;
        style.ItemSpacing = ImVec2(10.0F, 8.0F);
        style.ItemInnerSpacing = ImVec2(8.0F, 6.0F);
        style.SelectableTextAlign = ImVec2(0.0F, 0.5F);
        style.WindowPadding = snapshot.density == carto::ui::Density::compact
            ? ImVec2(10, 8) : snapshot.density == carto::ui::Density::touch
            ? ImVec2(14, 12) : ImVec2(12, 10);
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
        ImGui::BeginChild("ShellHeader", ImVec2(0, 84), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::BeginGroup();
        heading_text_colored(snapshot.colors.accent_primary, "CARTOGRAPHER");
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
        heading_text(surface_title());
        ImGui::Text("%s", snapshot.project_name.c_str());
        mono_text_disabled(project_location(snapshot).c_str());
        ImGui::EndGroup();

        ImGui::SameLine(std::max(240.0F, ImGui::GetWindowWidth() - 555.0F));
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
        if (ImGui::Button("Command palette  Ctrl+K")) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::toggle_command_palette));
        }
        ImGui::SameLine();
        if (ImGui::Button("Compose")) ImGui::OpenPopup("Compose workbench");
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
            if (snapshot.project_path.has_value()) {
                static_cast<void>(dispatch(carto::application::SaveProjectAction{std::nullopt}));
            } else if (const auto path = choose_file(true); path.has_value()) {
                static_cast<void>(dispatch(carto::application::SaveProjectAction{*path}));
            }
        }
        ImGui::EndGroup();
        ImGui::EndChild();
    }

    void draw_navigation_rail(const UiSnapshot& snapshot) {
        ImGui::BeginChild("ProductRail", ImVec2(232, 0), true);
        ImGui::TextColored(color(snapshot.colors.accent_primary), "C");
        ImGui::SameLine();
        heading_text("Spatial authoring");
        ImGui::TextDisabled("A truthful workspace for geometry, history, and evidence.");
        ImGui::Dummy(ImVec2(0, 6));

        const auto nav_item = [this, &snapshot](const char* label, const char* detail,
                                                 ShellSurface target) {
            const bool selected = surface == target;
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Header, color(snapshot.colors.panel_selected));
                ImGui::PushStyleColor(ImGuiCol_Text, color(snapshot.colors.accent_primary));
            }
            if (ImGui::Selectable(label, selected, 0, ImVec2(0, 32))) surface = target;
            if (selected) ImGui::PopStyleColor(2);
            ImGui::TextDisabled("  %s", detail);
        };
        nav_item("Workspace", "Author geometry", ShellSurface::workspace);
        nav_item("Project", "Persistence and revision", ShellSurface::project);
        nav_item("Diagnostics", "Problems and capability", ShellSurface::diagnostics);
        nav_item("Settings", "Density, theme, operator", ShellSurface::settings);

        ImGui::Spacing();
        if (ImGui::Button("+  New Project", ImVec2(-1, 34))) {
            carto::application::NewProjectAction action{"Untitled"};
            if (snapshot.dirty) {
                action.discard_dirty = true;
                request_discard(std::move(action));
            } else {
                static_cast<void>(dispatch(action));
            }
        }
        if (ImGui::Button("Open Project", ImVec2(-1, 30))) {
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
        ImGui::TextDisabled("Session");
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
        rack_button(snapshot, "Measure", workbench.measure);
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
                if (ImGui::Button(label)) static_cast<void>(ui.set_workspace(workspace));
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
            if (ImGui::MenuItem("Save")) {
                if (snapshot.project_path.has_value()) {
                    dispatch(carto::application::SaveProjectAction{std::nullopt});
                } else if (const auto path = choose_file(true); path.has_value()) {
                    dispatch(carto::application::SaveProjectAction{*path});
                }
            }
            if (ImGui::MenuItem("Recover latest journal snapshot")) {
                recover_latest_snapshot(snapshot);
            }
            const bool can_export = snapshot.project_path.has_value() && !snapshot.dirty;
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
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Scene instrument", nullptr, workbench.scene)) {
                workbench.scene = !workbench.scene;
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
                if (command.id == "project.save") static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::save));
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
            if (ImGui::Selectable(object.object.name.c_str(), selected)) {
                dispatch(SelectObjectAction{object.object.id});
            }
            ImGui::SameLine();
            ImGui::TextDisabled("#%llu", static_cast<unsigned long long>(object.object.id.value));
        }
        if (snapshot.objects.empty()) ImGui::TextDisabled("No project objects");
    }

    void draw_viewport(const UiSnapshot& snapshot, ImVec2 size = ImVec2(0, 0)) {
        native_viewport_hits.reset();
        ImGui::BeginChild("WorkSurfaceViewport", size, true, ImGuiWindowFlags_NoScrollbar);
        heading_text("Work surface · compiled snapshot");
        ImGui::Dummy(ImVec2(0, 4));
        if (snapshot.viewport.error.has_value()) {
            ImGui::TextColored(
                color(snapshot.colors.semantic_error),
                "Render unavailable: %s",
                snapshot.viewport.error->message.c_str());
            ImGui::EndChild();
            return;
        }
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 extent = ImGui::GetContentRegionAvail();
        native_viewport_hits.project_revision = snapshot.project_revision;
        native_viewport_hits.screen_origin = origin;
        native_viewport_hits.extent = extent;
        native_viewport_hits.valid = extent.x > 0.0F && extent.y > 0.0F;
        native_viewport_hits.interactive = ImGui::IsWindowHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const auto rgba = [](carto::ui::UiColor token, float alpha) {
            token.alpha *= alpha;
            return ImGui::ColorConvertFloat4ToU32(color(token));
        };
        draw->AddRectFilled(origin, origin + extent, rgba(snapshot.colors.canvas_0, 1.0F));
        const auto instances = snapshot.viewport.scene.instances();
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
        const double width = std::max(1e-6, maximum.x - minimum.x);
        const double height = std::max(1e-6, maximum.z - minimum.z);
        const double scale = std::max(width, height);
        const auto project = [&](Vec3d point) {
            const auto world = world_point(carto::core::Transform::identity(), point);
            return ImVec2(
                origin.x + static_cast<float>((world.x - minimum.x) / scale * std::max(1.0F, extent.x - 24.0F)) + 12.0F,
                origin.y + static_cast<float>((maximum.z - world.z) / scale * std::max(1.0F, extent.y - 24.0F)) + 12.0F);
        };

        auto& faces = native_viewport_hits.faces;
        auto& vertices = native_viewport_hits.vertices;
        auto& edges = native_viewport_hits.edges;
        auto& objects = native_viewport_hits.objects;
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
        if (invalid_render_snapshot) {
            ImGui::TextColored(
                color(snapshot.colors.semantic_warning),
                "Render snapshot contains invalid geometry; selection disabled.");
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

    void draw_tools_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::tools,
                              "Tools", workbench.tools, ImVec2(118, 390),
                              ImVec2(286, 265), snapshot.colors.accent_primary)) return;
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
            static_cast<void>(dispatch(SetObjectTransformAction{selected_id, inspector_transform}));
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

    void draw_draft_instrument(const UiSnapshot& snapshot) {
        if (!begin_instrument(snapshot, carto::ui::WorkbenchInstrument::draft,
                              "Drafting Board", workbench.draft, ImVec2(180, 150),
                              ImVec2(320, 260), snapshot.colors.provisional)) return;
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
        end_instrument(carto::ui::WorkbenchInstrument::draft, workbench.draft);
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
        if (workbench.assets) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::assets, "Assets", workbench.assets,
            ImVec2(135, 100), ImVec2(280, 210),
            "Asset rack", "Content-addressed assets will appear here when an asset browser is connected.");
        if (workbench.layers) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::layers, "Layers", workbench.layers,
            ImVec2(135, 330), ImVec2(280, 190),
            "Layer rack", "Layer visibility and ordering are reserved for a future governed workspace layer.");
        if (workbench.references) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::references, "References", workbench.references,
            ImVec2(250, 430), ImVec2(300, 185),
            "Reference rack", "Reference drawings remain an explicit future input boundary.");
        if (workbench.material) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::material, "Material", workbench.material,
            ImVec2(-8, 390), ImVec2(285, 190),
            "Material bay", "Material authoring is not connected to the current render snapshot.");
        if (workbench.constraint) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::constraint, "Constraint", workbench.constraint,
            ImVec2(-8, 210), ImVec2(285, 170),
            "Constraint bay", "Constraint solving is not available in this capability slice.");
        if (workbench.modify) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::modify, "Modify", workbench.modify,
            ImVec2(-8, 575), ImVec2(285, 170),
            "Modifier bay", "Modifier evaluation remains a deferred, revision-bound subsystem.");
        if (workbench.measure) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::measure, "Measure", workbench.measure,
            ImVec2(540, 430), ImVec2(250, 170),
            "Measure bay", "Measurement tools are not yet connected to a governed geometry query.");
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
            if (snapshot.project_path.has_value()) {
                static_cast<void>(dispatch(carto::application::SaveProjectAction{std::nullopt}));
            } else if (const auto path = choose_file(true); path.has_value()) {
                static_cast<void>(dispatch(carto::application::SaveProjectAction{*path}));
            }
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
        if (snapshot.problems.empty() && snapshot.ui_problems.empty() &&
            !snapshot.viewport.error.has_value()) {
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

        ImGui::BeginChild("WorkflowShortcuts", ImVec2(0, 192), true);
        heading_text("Workflow shortcuts");
        ImGui::TextDisabled("Compose flows without leaving the work surface.");
        ImGui::Text("Ctrl+Shift+1   Focus");
        ImGui::Text("Ctrl+Shift+2   Author");
        ImGui::Text("Ctrl+Shift+3   Inspect");
        ImGui::Text("Ctrl+Shift+4   Draft");
        ImGui::Text("Ctrl+Shift+R   Resume learned flow");
        ImGui::Text("Ctrl+Shift+L / I / T   Ledger / Inspector / Transform");
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
        instrument_toggle("Tools", workbench.tools);
        instrument_toggle("Inspector", workbench.inspector);
        instrument_toggle("Transform", workbench.transform);
        instrument_toggle("Material", workbench.material);
        instrument_toggle("Constraint", workbench.constraint);
        instrument_toggle("Modify", workbench.modify);
        instrument_toggle("Measure", workbench.measure);
        ImGui::TextDisabled("The viewport and machine frame remain fixed.\nRack instruments can be reopened at any time.");
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void draw_workspace_surface(const UiSnapshot& snapshot) {
        draw_workspace_bar(snapshot);
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
        if (surface != ShellSurface::workspace) native_viewport_hits.reset();
        process_close_request();
        route_shortcuts();
        const auto snapshot = ui.snapshot();
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
        if (g_state->route_native_pointer(event)) return 0;
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
        0, window_class.lpszClassName, L"Cartographer", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) throw std::runtime_error("could not create Cartographer window");
    return window;
}

int run(HINSTANCE instance) {
    DesktopState state;
    state.load_preferences();
    g_state = &state;
    state.window = create_window(instance);
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
