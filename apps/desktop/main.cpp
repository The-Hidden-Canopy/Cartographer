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

struct WorkbenchState {
    bool scene = true;
    bool assets = false;
    bool layers = false;
    bool tools = true;
    bool references = false;
    bool draft = false;
    bool inspector = true;
    bool material = false;
    bool constraint = false;
    bool modify = false;
    bool measure = false;
    bool transform = true;
    bool extrude = false;
    bool ledger_collapsed = false;
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

[[nodiscard]] Vec3d world_point(const carto::core::Transform& transform, Vec3d point) {
    return transform.translation +
           transform.rotation.rotate(carto::core::componentwise_multiply(transform.scale, point));
}

[[nodiscard]] std::optional<std::filesystem::path> choose_file(bool save) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Cartographer Project (*.carto)\0*.carto\0All Files\0*.*\0";
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
    WorkbenchState workbench;
    bool renderer_ready = false;
    std::optional<carto::ui::Theme> applied_window_theme;
    ImVec2 workbench_origin{};
    ImVec2 workbench_extent{};
    bool workbench_bounds_valid = false;
    carto::scene::ObjectId inspector_object{};
    carto::core::Transform inspector_transform = carto::core::Transform::identity();
    carto::geometry::VertexId inspector_vertex{};
    Vec3d inspector_vertex_position{};
    double extrude_distance = 0.25;
    std::optional<carto::application::ApplicationAction> pending_discard_action;
    std::uint64_t inspector_generation = std::numeric_limits<std::uint64_t>::max();
    bool pending_exit = false;
    bool discard_prompt_pending = false;
    bool close_requested = false;
    std::string ai_intent;
    std::optional<std::uint64_t> selected_operation_id;

    DesktopState() : ui(session) {}

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
        for (std::size_t index = 0U; index < carto::ui::kWorkbenchInstrumentCount; ++index) {
            const auto instrument = static_cast<carto::ui::WorkbenchInstrument>(index);
            workbench_visible(workbench, instrument) = workbench.persisted.instruments[index].visible;
        }
    }

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

    void reset_workbench() {
        workbench = WorkbenchState{};
    }

    bool dispatch(const carto::application::ApplicationAction& action) {
        return static_cast<bool>(ui.dispatch(action));
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

    [[nodiscard]] static bool workspace_available(carto::ui::Workspace workspace) noexcept {
        return workspace == carto::ui::Workspace::model || workspace == carto::ui::Workspace::ai;
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
        style.Colors[ImGuiCol_Separator] = color(snapshot.colors.border_soft);
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
        style.WindowRounding = 2.0F;
        style.ChildRounding = 2.0F;
        style.FrameRounding = 3.0F;
        style.PopupRounding = 3.0F;
        style.ScrollbarRounding = 2.0F;
        style.WindowBorderSize = 0.0F;
        style.ChildBorderSize = 1.0F;
        style.FrameBorderSize = 1.0F;
        style.ItemSpacing = ImVec2(8.0F, 7.0F);
        style.WindowPadding = snapshot.density == carto::ui::Density::compact
            ? ImVec2(6, 5) : snapshot.density == carto::ui::Density::touch
            ? ImVec2(12, 10) : ImVec2(8, 7);
        style.FramePadding = snapshot.density == carto::ui::Density::compact
            ? ImVec2(5, 3) : snapshot.density == carto::ui::Density::touch
            ? ImVec2(9, 7) : ImVec2(7, 5);
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
        ImGui::BeginChild("ShellHeader", ImVec2(0, 72), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::BeginGroup();
        ImGui::TextColored(color(snapshot.colors.accent_primary), "CARTOGRAPHER");
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
        ImGui::TextUnformatted(surface_title());
        ImGui::Text("%s", snapshot.project_name.c_str());
        ImGui::TextDisabled("%s", project_location(snapshot).c_str());
        ImGui::EndGroup();

        ImGui::SameLine(std::max(240.0F, ImGui::GetWindowWidth() - 555.0F));
        ImGui::BeginGroup();
        ImGui::TextColored(status_color(snapshot), "%s", snapshot.project_status_text.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("THEME");
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
        if (ImGui::Button("Ctrl+K  Command")) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::toggle_command_palette));
        }
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
        ImGui::TextUnformatted("SPATIAL AUTHORING");
        ImGui::TextDisabled("A truthful workspace for geometry, history, and evidence.");
        ImGui::Separator();

        const auto nav_item = [this, &snapshot](const char* label, const char* detail,
                                                 ShellSurface target) {
            const bool selected = surface == target;
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Header, color(snapshot.colors.panel_selected));
                ImGui::PushStyleColor(ImGuiCol_Text, color(snapshot.colors.accent_primary));
            }
            if (ImGui::Selectable(label, selected, 0, ImVec2(-1, 32))) surface = target;
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
        ImGui::TextDisabled("SESSION");
        ImGui::Text("Revision %llu", static_cast<unsigned long long>(snapshot.project_revision.value()));
        ImGui::Text("%zu object%s", snapshot.objects.size(), snapshot.objects.size() == 1U ? "" : "s");
        ImGui::TextColored(status_color(snapshot), "%s", snapshot.project_status_text.c_str());
        if (snapshot.dirty) ImGui::TextColored(color(snapshot.colors.modified), "Unsaved changes");
        ImGui::Spacing();
        ImGui::TextDisabled("NATIVE SHELL");
        ImGui::TextColored(renderer_ready ? color(snapshot.colors.semantic_success)
                                          : color(snapshot.colors.semantic_error),
                          renderer_ready ? "ACTIVE" : "UNAVAILABLE");
        ImGui::TextDisabled("Mutations remain behind the application session.");
        ImGui::EndChild();
    }

    void rack_button(const UiSnapshot& snapshot, const char* label, bool& open) {
        if (open) {
            ImGui::PushStyleColor(ImGuiCol_Button, color(snapshot.colors.panel_selected));
            ImGui::PushStyleColor(ImGuiCol_Text, color(snapshot.colors.accent_primary));
        }
        if (ImGui::Button(label, ImVec2(-1, 26))) open = !open;
        if (open) ImGui::PopStyleColor(2);
    }

    void draw_tool_rack(const UiSnapshot& snapshot) {
        ImGui::BeginChild("ToolRack", ImVec2(108, 0), true);
        ImGui::TextColored(color(snapshot.colors.accent_primary), "TOOL");
        ImGui::TextUnformatted("RACK");
        ImGui::Separator();
        rack_button(snapshot, "SCENE", workbench.scene);
        rack_button(snapshot, "ASSETS", workbench.assets);
        rack_button(snapshot, "LAYERS", workbench.layers);
        rack_button(snapshot, "TOOLS", workbench.tools);
        rack_button(snapshot, "REFS", workbench.references);
        rack_button(snapshot, "DRAFT", workbench.draft);
        ImGui::Spacing();
        ImGui::TextDisabled("Grab an instrument\nby its title bar.");
        ImGui::EndChild();
    }

    void draw_bay_rack(const UiSnapshot& snapshot) {
        ImGui::BeginChild("InstrumentBay", ImVec2(108, 0), true);
        ImGui::TextColored(color(snapshot.colors.accent_secondary), "INSTRUMENT");
        ImGui::TextUnformatted("BAY");
        ImGui::Separator();
        rack_button(snapshot, "INSPECT", workbench.inspector);
        rack_button(snapshot, "TRANSFORM", workbench.transform);
        rack_button(snapshot, "MATERIAL", workbench.material);
        rack_button(snapshot, "CONSTRAINT", workbench.constraint);
        rack_button(snapshot, "MODIFY", workbench.modify);
        rack_button(snapshot, "MEASURE", workbench.measure);
        rack_button(snapshot, "EXTRUDE", workbench.extrude);
        ImGui::Spacing();
        ImGui::TextDisabled("Stored instruments\nslide out here.");
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
            const ImVec2 position = layout.positioned
                ? viewport->WorkPos + ImVec2(static_cast<float>(layout.x), static_cast<float>(layout.y))
                : instrument_origin(offset, size);
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
        ImGui::GetWindowDrawList()->AddRectFilled(
            grip_origin, grip_origin + ImVec2(grip_extent.x, 7.0F),
            ImGui::ColorConvertFloat4ToU32(color(accent)));
        ImGui::EndChild();
        ImGui::TextDisabled("GRIP  /  MOVE · RESIZE · MAGNET · DROP TO RACK");
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

    void magnetize_instrument(carto::ui::WorkbenchInstrument instrument,
                              ImVec2 position, ImVec2 size) {
        constexpr float snap_distance = 24.0F;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        std::optional<ImVec2> best_position;
        float best_distance = std::numeric_limits<float>::max();
        const auto consider = [&](ImVec2 candidate) {
            const float dx = std::abs(position.x - candidate.x);
            const float dy = std::abs(position.y - candidate.y);
            if (dx > snap_distance || dy > snap_distance) return;
            const float distance = dx * dx + dy * dy;
            if (distance < best_distance) {
                best_distance = distance;
                best_position = candidate;
            }
        };

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
        }
        if (best_position.has_value()) ImGui::SetWindowPos(*best_position);
    }

    void settle_instrument(carto::ui::WorkbenchInstrument instrument, bool& open) {
        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
            !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
        const ImVec2 position = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const ImVec2 rack_gap(ImGui::GetStyle().ItemSpacing.x, ImGui::GetStyle().ItemSpacing.y);
        const ImRect window_rect(position, position + size);
        const ImRect left_rack(
            ImVec2(workbench_origin.x - 108.0F - rack_gap.x, workbench_origin.y),
            ImVec2(workbench_origin.x - rack_gap.x,
                   workbench_origin.y + workbench_extent.y));
        const ImRect right_rack(
            ImVec2(workbench_origin.x + workbench_extent.x + rack_gap.x, workbench_origin.y),
            ImVec2(workbench_origin.x + workbench_extent.x + rack_gap.x + 108.0F,
                   workbench_origin.y + workbench_extent.y));
        const bool overlaps_rack = workbench_bounds_valid &&
            (window_rect.Overlaps(left_rack) || window_rect.Overlaps(right_rack));
        if (overlaps_rack) {
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
        ImGui::TextColored(color(snapshot.colors.semantic_warning), "STORED / NOT CONNECTED");
        ImGui::TextDisabled("This instrument is an explicit capability boundary, not fabricated live state.");
        end_instrument(instrument, open);
    }

    void route_shortcuts() {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) return;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::save));
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::undo));
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::redo));
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K)) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::toggle_command_palette));
        } else if (ImGui::IsKeyPressed(ImGuiKey_1)) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::object_mode));
        } else if (ImGui::IsKeyPressed(ImGuiKey_2)) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::vertex_mode));
        } else if (ImGui::IsKeyPressed(ImGuiKey_3)) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::edge_mode));
        } else if (ImGui::IsKeyPressed(ImGuiKey_4)) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::face_mode));
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            static_cast<void>(ui.handle_shortcut(carto::ui::Shortcut::close_overlay));
        }
    }

    void draw_workspace_bar(const UiSnapshot& snapshot) {
        ImGui::BeginChild("WorkspaceBar", ImVec2(0, 36), true);
        static constexpr std::array<std::pair<carto::ui::Workspace, const char*>, 8> workspaces{{
            {carto::ui::Workspace::model, "MODEL"},
            {carto::ui::Workspace::sculpt, "SCULPT"},
            {carto::ui::Workspace::cad, "CAD"},
            {carto::ui::Workspace::build, "BUILD"},
            {carto::ui::Workspace::material, "MATERIAL"},
            {carto::ui::Workspace::animate, "ANIMATE"},
            {carto::ui::Workspace::review, "REVIEW"},
            {carto::ui::Workspace::ai, "AI"},
        }};
        for (const auto& [workspace, label] : workspaces) {
            const bool available = workspace_available(workspace);
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
                              ? "PERSON-FIRST"
                              : "AI-FIRST")) {
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
            if (ImGui::MenuItem("Exit")) request_close();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) dispatch(carto::application::UndoAction{});
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) dispatch(carto::application::RedoAction{});
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Scene instrument", nullptr, workbench.scene)) workbench.scene = !workbench.scene;
            if (ImGui::MenuItem("Tools instrument", nullptr, workbench.tools)) workbench.tools = !workbench.tools;
            if (ImGui::MenuItem("Inspector instrument", nullptr, workbench.inspector)) workbench.inspector = !workbench.inspector;
            if (ImGui::MenuItem("Transform instrument", nullptr, workbench.transform)) workbench.transform = !workbench.transform;
            if (ImGui::MenuItem("Operation ledger", nullptr, !workbench.ledger_collapsed)) {
                workbench.ledger_collapsed = !workbench.ledger_collapsed;
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
        ImGui::BeginChild("Toolbar", ImVec2(0, 42), true);
        ImGui::TextDisabled("WORK SURFACE");
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
            ImGui::TextColored(color(snapshot.colors.semantic_error), "RENDER UNAVAILABLE");
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
        ImGui::TextUnformatted("SCENE");
        ImGui::TextDisabled("Objects currently on the work surface");
        ImGui::Separator();
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
        ImGui::BeginChild("WorkSurfaceViewport", size, true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextUnformatted("WORK SURFACE / COMPILED SNAPSHOT");
        ImGui::Separator();
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
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const auto rgba = [](carto::ui::UiColor token, float alpha) {
            token.alpha *= alpha;
            return ImGui::ColorConvertFloat4ToU32(color(token));
        };
        draw->AddRectFilled(origin, origin + extent, rgba(snapshot.colors.canvas_0, 1.0F));
        const auto instances = snapshot.viewport.scene.instances();
        Vec3d minimum{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::infinity()};
        Vec3d maximum{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                      -std::numeric_limits<double>::infinity()};
        for (const auto& instance : instances) {
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

        struct FaceHit { carto::scene::ObjectId object; carto::geometry::FaceId face; std::array<ImVec2, 3> points; };
        struct VertexHit { carto::scene::ObjectId object; carto::geometry::VertexId vertex; ImVec2 point; };
        struct EdgeHit {
            carto::scene::ObjectId object;
            carto::geometry::EdgeId edge;
            ImVec2 first;
            ImVec2 second;
        };
        std::vector<FaceHit> faces;
        std::vector<VertexHit> vertices;
        std::vector<EdgeHit> edges;
        std::vector<std::pair<carto::scene::ObjectId, ImRect>> objects;
        for (const auto& instance : instances) {
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
                const auto a = projected.at(instance.mesh->indices[index]);
                const auto b = projected.at(instance.mesh->indices[index + 1U]);
                const auto c = projected.at(instance.mesh->indices[index + 2U]);
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
                const auto& triangle_edges = instance.mesh->triangle_edges.at(index / 3U);
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
        if (instances.empty()) ImGui::TextDisabled("No compiled geometry");

        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetMousePos();
            if (snapshot.selection.mode == carto::editor::SelectionMode::edge) {
                float best = 12.0F * 12.0F;
                std::optional<EdgeHit> hit;
                for (const auto& candidate : edges) {
                    const float distance = point_segment_distance_squared(
                        mouse, candidate.first, candidate.second);
                    if (distance < best) {
                        best = distance;
                        hit = candidate;
                    }
                }
                if (hit.has_value()) dispatch(SelectEdgeAction{hit->object, hit->edge});
            } else if (snapshot.selection.mode == carto::editor::SelectionMode::face) {
                for (auto iterator = faces.rbegin(); iterator != faces.rend(); ++iterator) {
                    if (point_in_triangle(mouse, iterator->points[0], iterator->points[1], iterator->points[2])) {
                        dispatch(SelectFaceAction{iterator->object, iterator->face});
                        break;
                    }
                }
            } else if (snapshot.selection.mode == carto::editor::SelectionMode::vertex) {
                float best = 12.0F * 12.0F;
                std::optional<VertexHit> hit;
                for (const auto& candidate : vertices) {
                    const float dx = candidate.point.x - mouse.x;
                    const float dy = candidate.point.y - mouse.y;
                    const float distance = dx * dx + dy * dy;
                    if (distance < best) { best = distance; hit = candidate; }
                }
                if (hit.has_value()) dispatch(SelectVertexAction{hit->object, hit->vertex});
            } else {
                for (auto iterator = objects.rbegin(); iterator != objects.rend(); ++iterator) {
                    if (iterator->second.Contains(mouse)) {
                        dispatch(SelectObjectAction{iterator->first});
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
        ImGui::TextUnformatted("INSPECTOR");
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
            ImGui::TextDisabled("Edge authoring tools are not available yet.");
        }
        if (snapshot.selection.mode == carto::editor::SelectionMode::face && !snapshot.selection.faces.empty()) {
            ImGui::Separator();
            ImGui::Text("Face %llu", static_cast<unsigned long long>(snapshot.selection.faces.front().value));
            ImGui::TextDisabled("Face operation available as a movable instrument.");
            if (ImGui::Button("Open Extrude Instrument")) workbench.extrude = true;
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
        ImGui::TextDisabled("AUTHORING INSTRUMENTS");
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
        if (face_available) {
            if (ImGui::Button("Extrude Instrument", ImVec2(-1, 30))) workbench.extrude = true;
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
        ImGui::TextDisabled("AI STUDY / NON-AUTHORITATIVE");
        std::array<char, 4097> intent{};
        const std::size_t copy_bytes = std::min(ai_intent.size(), intent.size() - 1U);
        std::copy_n(ai_intent.data(), copy_bytes, intent.data());
        if (ImGui::InputTextMultiline("##draft-intent", intent.data(), intent.size(), ImVec2(-1, 90))) {
            ai_intent.assign(intent.data());
        }
        ImGui::TextDisabled("CONSTRAINTS");
        ImGui::BulletText("Existing geometry remains authoritative");
        ImGui::BulletText("Proposals require explicit review");
        ImGui::BeginDisabled(!snapshot.ai_available);
        ImGui::Button("Generate Study", ImVec2(-1, 30));
        ImGui::EndDisabled();
        if (!snapshot.ai_available) ImGui::TextColored(
            color(snapshot.colors.semantic_warning), "Unavailable: no planner connected");
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
            "ASSET RACK", "Content-addressed assets will appear here when an asset browser is connected.");
        if (workbench.layers) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::layers, "Layers", workbench.layers,
            ImVec2(135, 330), ImVec2(280, 190),
            "LAYER RACK", "Layer visibility and ordering are reserved for a future governed workspace layer.");
        if (workbench.references) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::references, "References", workbench.references,
            ImVec2(250, 430), ImVec2(300, 185),
            "REFERENCE RACK", "Reference drawings remain an explicit future input boundary.");
        if (workbench.material) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::material, "Material", workbench.material,
            ImVec2(-8, 390), ImVec2(285, 190),
            "MATERIAL BAY", "Material authoring is not connected to the current render snapshot.");
        if (workbench.constraint) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::constraint, "Constraint", workbench.constraint,
            ImVec2(-8, 210), ImVec2(285, 170),
            "CONSTRAINT BAY", "Constraint solving is not available in this capability slice.");
        if (workbench.modify) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::modify, "Modify", workbench.modify,
            ImVec2(-8, 575), ImVec2(285, 170),
            "MODIFIER BAY", "Modifier evaluation remains a deferred, revision-bound subsystem.");
        if (workbench.measure) draw_placeholder_instrument(
            snapshot, carto::ui::WorkbenchInstrument::measure, "Measure", workbench.measure,
            ImVec2(540, 430), ImVec2(250, 170),
            "MEASURE BAY", "Measurement tools are not yet connected to a governed geometry query.");
    }

    void draw_project_surface(const UiSnapshot& snapshot) {
        ImGui::BeginChild("ProjectSurface", ImVec2(0, 0), false);
        ImGui::TextColored(color(snapshot.colors.accent_primary), "PROJECT");
        ImGui::TextDisabled("The durable boundary for this workspace.");
        ImGui::Separator();

        ImGui::BeginChild("ProjectOverview", ImVec2(0, 122), true);
        ImGui::TextDisabled("CURRENT PROJECT");
        ImGui::Text("%s", snapshot.project_name.c_str());
        ImGui::TextDisabled("%s", project_location(snapshot).c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 275.0F);
        ImGui::TextColored(status_color(snapshot), "%s", snapshot.project_status_text.c_str());
        ImGui::Text("Revision %llu", static_cast<unsigned long long>(snapshot.project_revision.value()));
        ImGui::SameLine();
        ImGui::TextDisabled("·");
        ImGui::SameLine();
        ImGui::Text("%zu object%s", snapshot.objects.size(), snapshot.objects.size() == 1U ? "" : "s");
        ImGui::EndChild();

        ImGui::BeginChild("ProjectMetrics", ImVec2(0, 112), true);
        ImGui::TextDisabled("PROJECT SIGNALS");
        ImGui::Columns(4, "ProjectMetricsColumns", false);
        ImGui::Text("%llu", static_cast<unsigned long long>(snapshot.project_revision.value()));
        ImGui::TextDisabled("REVISION");
        ImGui::NextColumn();
        ImGui::Text("%zu", snapshot.operations.size());
        ImGui::TextDisabled("OPERATIONS");
        ImGui::NextColumn();
        ImGui::Text("%zu", snapshot.undo_count);
        ImGui::TextDisabled("UNDO AVAILABLE");
        ImGui::NextColumn();
        ImGui::Text("%zu", snapshot.redo_count);
        ImGui::TextDisabled("REDO AVAILABLE");
        ImGui::Columns(1);
        ImGui::EndChild();

        ImGui::BeginChild("ProjectLineage", ImVec2(0, 0), true);
        ImGui::TextDisabled("LINEAGE AND PERSISTENCE");
        if (snapshot.project_path.has_value()) {
            ImGui::TextColored(color(snapshot.colors.semantic_success), "PROJECT PATH BOUND");
            ImGui::TextWrapped("%s", snapshot.project_path->string().c_str());
            ImGui::TextDisabled("Authoring changes remain routed through the session and its journal boundary.");
        } else {
            ImGui::TextColored(color(snapshot.colors.semantic_warning), "NOT SAVED TO DISK");
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
        ImGui::TextColored(color(snapshot.colors.accent_primary), "DIAGNOSTICS");
        ImGui::TextDisabled("Evidence about the current shell and application snapshot.");
        ImGui::Separator();

        ImGui::BeginChild("CapabilityCards", ImVec2(0, 118), true);
        ImGui::TextDisabled("CAPABILITY STATE");
        ImGui::Columns(3, "CapabilityColumns", false);
        ImGui::TextColored(renderer_ready ? color(snapshot.colors.semantic_success)
                                          : color(snapshot.colors.semantic_error),
                          renderer_ready ? "ACTIVE" : "UNAVAILABLE");
        ImGui::TextDisabled("NATIVE SHELL");
        ImGui::NextColumn();
        if (snapshot.viewport.error.has_value()) {
            ImGui::TextColored(color(snapshot.colors.semantic_warning), "UNAVAILABLE");
            ImGui::TextDisabled("VIEWPORT SNAPSHOT");
        } else {
            ImGui::TextColored(color(snapshot.colors.semantic_success), "READY");
            ImGui::TextDisabled("VIEWPORT SNAPSHOT");
        }
        ImGui::NextColumn();
        ImGui::TextColored(snapshot.project_path.has_value()
                               ? color(snapshot.colors.semantic_success)
                               : color(snapshot.colors.semantic_warning),
                          snapshot.project_path.has_value() ? "BOUND" : "LOCAL ONLY");
        ImGui::TextDisabled("PERSISTENCE");
        ImGui::Columns(1);
        ImGui::EndChild();

        ImGui::BeginChild("DiagnosticDetails", ImVec2(0, 0), true);
        ImGui::TextDisabled("CURRENT REPORT");
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
        ImGui::TextColored(color(snapshot.colors.accent_primary), "SETTINGS");
        ImGui::TextDisabled("Presentation preferences are local UI state; project truth remains in the application session.");
        ImGui::Separator();

        ImGui::BeginChild("AppearanceSettings", ImVec2(0, 180), true);
        ImGui::TextDisabled("APPEARANCE");
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

        ImGui::BeginChild("OperatorSettings", ImVec2(0, 142), true);
        ImGui::TextDisabled("OPERATOR BOUNDARY");
        ImGui::Text("Current mode: %s", carto::ui::operator_mode_name(snapshot.operator_mode));
        if (ImGui::Button("Person-first", ImVec2(130, 32))) {
            static_cast<void>(ui.set_operator_mode(carto::ui::OperatorMode::person_first));
        }
        ImGui::SameLine();
        if (ImGui::Button("AI-first", ImVec2(130, 32))) {
            static_cast<void>(ui.set_operator_mode(carto::ui::OperatorMode::ai_first));
        }
        ImGui::TextDisabled("AI proposals remain non-authoritative and no planner is connected.");
        ImGui::EndChild();

        ImGui::BeginChild("PanelSettings", ImVec2(0, 0), true);
        ImGui::TextDisabled("INSTRUMENT RACK STATE");
        const auto instrument_toggle = [](const char* label, bool& visible) {
            static_cast<void>(ImGui::Checkbox(label, &visible));
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
        const float rack_width = 108.0F;
        const float bay_gap = ImGui::GetStyle().ItemSpacing.x;
        const float center_width = std::max(
            320.0F, ImGui::GetContentRegionAvail().x - rack_width * 2.0F - bay_gap * 2.0F);
        draw_tool_rack(snapshot);
        ImGui::SameLine();
        ImGui::BeginChild("WorkSurfaceFrame", ImVec2(center_width, 0), false);
        workbench_origin = ImGui::GetWindowPos();
        workbench_extent = ImGui::GetWindowSize();
        workbench_bounds_valid = true;
        const float ledger_height = workbench.ledger_collapsed ? 34.0F : 148.0F;
        draw_viewport(snapshot, ImVec2(0, -ledger_height - ImGui::GetStyle().ItemSpacing.y));
        draw_bottom(snapshot, ledger_height);
        ImGui::EndChild();
        ImGui::SameLine();
        draw_bay_rack(snapshot);
        ImGui::EndChild();
    }

    void draw_ai_context(const UiSnapshot& snapshot) {
        ImGui::BeginChild("AI Context", ImVec2(260, 0), true);
        ImGui::TextUnformatted("AI-FIRST");
        ImGui::Separator();
        ImGui::TextDisabled("Intent");
        std::array<char, 8193> intent{};
        const std::size_t copy_bytes = std::min(ai_intent.size(), intent.size() - 1U);
        std::copy_n(ai_intent.data(), copy_bytes, intent.data());
        if (ImGui::InputTextMultiline("##intent", intent.data(), intent.size(), ImVec2(-1, 100))) {
            ai_intent.assign(intent.data());
        }
        ImGui::TextDisabled("Constraints");
        ImGui::BulletText("No planner connected");
        ImGui::BulletText("No proposal is authoritative");
        ImGui::Separator();
        ImGui::TextWrapped("%s", snapshot.ai_status.c_str());
        ImGui::TextDisabled("AI proposals require a future bounded planner and transaction adapter.");
        ImGui::EndChild();
    }

    void draw_bottom(const UiSnapshot& snapshot, float height) {
        ImGui::BeginChild("OperationLedger", ImVec2(0, height), true);
        ImGui::TextColored(color(snapshot.colors.accent_primary), "OPERATION LEDGER");
        ImGui::SameLine();
        ImGui::TextDisabled("revision-bound activity");
        ImGui::SameLine(ImGui::GetWindowWidth() - 90.0F);
        if (ImGui::SmallButton(workbench.ledger_collapsed ? "EXPAND" : "COLLAPSE")) {
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
                if (ImGui::SmallButton("DETAILS")) workbench.ledger_collapsed = false;
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
                    ImGui::TextDisabled("────");
                }
            }
            ImGui::EndChild();
        }
        static constexpr std::array<std::pair<carto::ui::BottomPanel, const char*>, 6> tabs{{
            {carto::ui::BottomPanel::operations, "OPERATIONS"},
            {carto::ui::BottomPanel::graph, "GRAPH"},
            {carto::ui::BottomPanel::assets, "ASSETS"},
            {carto::ui::BottomPanel::timeline, "TIMELINE"},
            {carto::ui::BottomPanel::problems, "PROBLEMS"},
            {carto::ui::BottomPanel::console, "CONSOLE"},
        }};
        for (const auto& [panel, label] : tabs) {
            if (panel == snapshot.bottom_panel) ImGui::PushStyleColor(ImGuiCol_Button, color(snapshot.colors.panel_selected));
            if (ImGui::Button(label)) static_cast<void>(ui.set_bottom_panel(panel));
            if (panel == snapshot.bottom_panel) ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::Separator();
        if (snapshot.bottom_panel == carto::ui::BottomPanel::operations) {
            ImGui::Text("OPERATIONS: %llu  |  PROBLEMS: %llu  |  UNDO: %llu  |  REDO: %llu",
                    static_cast<unsigned long long>(snapshot.operations.size()),
                    static_cast<unsigned long long>(snapshot.problems.size()),
                    static_cast<unsigned long long>(snapshot.undo_count),
                    static_cast<unsigned long long>(snapshot.redo_count));
            if (selected_operation != nullptr) {
                ImGui::BeginChild("LedgerReceiptDetail", ImVec2(0, 86), true);
                ImGui::Text("RECEIPT #%llu  /  %s",
                            static_cast<unsigned long long>(selected_operation->operation_id),
                            selected_operation->action.c_str());
                ImGui::Text("Revision %llu -> %llu  |  document %s",
                            static_cast<unsigned long long>(selected_operation->revision_before.value()),
                            static_cast<unsigned long long>(selected_operation->revision_after.value()),
                            selected_operation->document_changed ? "changed" : "unchanged");
                ImGui::TextDisabled("This is a session receipt, not a second project history.");
                ImGui::BeginDisabled();
                ImGui::SmallButton("REOPEN PARAMETERS");
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
            ImGui::TextDisabled("%s panel is reserved for the corresponding future subsystem.",
                               tabs.at(static_cast<std::size_t>(snapshot.bottom_panel)).second);
        }
        ImGui::EndChild();
    }

    void draw() {
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
