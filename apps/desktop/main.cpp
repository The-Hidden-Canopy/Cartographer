#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#define IMGUI_DEFINE_MATH_OPERATORS

#include <windows.h>
#include <commdlg.h>
#include <vulkan/vulkan.h>

#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <carto/application/application.hpp>
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
#include <utility>
#include <vector>

namespace {

using carto::application::ApplicationSession;
using carto::application::ApplicationSnapshot;
using carto::application::SelectFaceAction;
using carto::application::SelectObjectAction;
using carto::application::SelectVertexAction;
using carto::application::SetObjectTransformAction;
using carto::application::SetSelectionModeAction;
using carto::application::UndoAction;
using carto::application::RedoAction;
using carto::core::Vec3d;

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

[[nodiscard]] bool point_in_triangle(ImVec2 point, ImVec2 a, ImVec2 b, ImVec2 c) {
    const float ab = (point.x - b.x) * (a.y - b.y) - (point.y - b.y) * (a.x - b.x);
    const float bc = (point.x - c.x) * (b.y - c.y) - (point.y - c.y) * (b.x - c.x);
    const float ca = (point.x - a.x) * (c.y - a.y) - (point.y - a.y) * (c.x - a.x);
    return (ab >= 0.0F && bc >= 0.0F && ca >= 0.0F) ||
           (ab <= 0.0F && bc <= 0.0F && ca <= 0.0F);
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

struct DesktopState {
    ApplicationSession session;
    VulkanShell renderer;
    HWND window = nullptr;
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

    bool dispatch(const carto::application::ApplicationAction& action) {
        return static_cast<bool>(session.dispatch(action));
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
        if (session.can_close()) {
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

    void toggle_pane(const ApplicationSnapshot& snapshot, carto::application::Pane pane) {
        if (pane == carto::application::Pane::viewport) return;
        auto workspace = snapshot.workspace;
        const auto found = std::find(
            workspace.visible_panes.begin(), workspace.visible_panes.end(), pane);
        if (found == workspace.visible_panes.end()) {
            workspace.visible_panes.push_back(pane);
        } else {
            workspace.visible_panes.erase(found);
        }
        dispatch(carto::application::SetWorkspaceAction{std::move(workspace)});
    }

    void draw_menu(const ApplicationSnapshot& snapshot) {
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
                auto snapshot = session.snapshot();
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
            const auto pane_item = [this, &snapshot](
                                       const char* label,
                                       carto::application::Pane pane) {
                if (ImGui::MenuItem(label, nullptr, has_pane(snapshot, pane))) {
                    toggle_pane(snapshot, pane);
                }
            };
            pane_item("Outliner", carto::application::Pane::outliner);
            pane_item("Inspector", carto::application::Pane::inspector);
            pane_item("Problems", carto::application::Pane::problems);
            pane_item("History", carto::application::Pane::history);
            ImGui::Separator();
            ImGui::TextDisabled("Viewport is required");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    void draw_toolbar(const ApplicationSnapshot& snapshot) {
        ImGui::BeginChild("Toolbar", ImVec2(0, 42), true);
        if (ImGui::Button("New Box")) dispatch(carto::application::CreateBoxAction{"Box", {2.0, 2.0, 2.0}});
        ImGui::SameLine();
        if (ImGui::Button("New Plane")) dispatch(carto::application::CreatePlaneAction{"Plane", 2.0, 2.0});
        ImGui::SameLine();
        if (ImGui::Button("Undo")) dispatch(carto::application::UndoAction{});
        ImGui::SameLine();
        if (ImGui::Button("Redo")) dispatch(carto::application::RedoAction{});
        ImGui::SameLine();
        int mode = static_cast<int>(snapshot.selection.mode);
        const char* modes[] = {"Object", "Vertex", "Face"};
        if (ImGui::Combo("Mode", &mode, modes, 3)) {
            dispatch(SetSelectionModeAction{static_cast<carto::editor::SelectionMode>(mode)});
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(snapshot.dirty ? "DIRTY" : "SAVED");
        if (snapshot.viewport.error.has_value()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "RENDER UNAVAILABLE");
        }
        ImGui::EndChild();
    }

    void draw_outliner(const ApplicationSnapshot& snapshot) {
        ImGui::BeginChild("Outliner", ImVec2(260, 0), true);
        ImGui::TextUnformatted("OUTLINER");
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
        ImGui::EndChild();
    }

    void draw_viewport(const ApplicationSnapshot& snapshot) {
        ImGui::BeginChild("Viewport", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextUnformatted("VIEWPORT / COMPILED SNAPSHOT");
        ImGui::Separator();
        if (snapshot.viewport.error.has_value()) {
            ImGui::TextColored(
                ImVec4(1.0F, 0.45F, 0.35F, 1.0F),
                "Render unavailable: %s",
                snapshot.viewport.error->message.c_str());
            ImGui::EndChild();
            return;
        }
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 extent = ImGui::GetContentRegionAvail();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, origin + extent, IM_COL32(14, 18, 24, 255));
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
        std::vector<FaceHit> faces;
        std::vector<VertexHit> vertices;
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
                const ImU32 fill = face_selected ? IM_COL32(210, 170, 70, 130)
                                                  : (highlighted ? IM_COL32(100, 170, 220, 80)
                                                                 : IM_COL32(80, 100, 130, 45));
                draw->AddTriangleFilled(a, b, c, fill);
                draw->AddTriangle(a, b, c, IM_COL32(150, 180, 210, 210), 1.0F);
                faces.push_back({instance.object, instance.mesh->triangle_faces[index / 3U], {a, b, c}});
            }
        }
        if (instances.empty()) ImGui::TextDisabled("No compiled geometry");

        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetMousePos();
            if (snapshot.selection.mode == carto::editor::SelectionMode::face) {
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
        ImGui::BeginChild("Inspector", ImVec2(300, 0), true);
        ImGui::TextUnformatted("INSPECTOR");
        ImGui::Separator();
        if (!snapshot.selection.component_object.has_value() && snapshot.selection.objects.empty()) {
            ImGui::TextDisabled("Select an object or mesh element");
            ImGui::EndChild();
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
            ImGui::InputDouble("Translate X", &inspector_transform.translation.x);
            ImGui::InputDouble("Translate Y", &inspector_transform.translation.y);
            ImGui::InputDouble("Translate Z", &inspector_transform.translation.z);
            if (ImGui::Button("Apply Transform")) {
                dispatch(SetObjectTransformAction{selected_id, inspector_transform});
            }
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
        if (snapshot.selection.mode == carto::editor::SelectionMode::face && !snapshot.selection.faces.empty()) {
            ImGui::Separator();
            ImGui::Text("Face %llu", static_cast<unsigned long long>(snapshot.selection.faces.front().value));
            ImGui::InputDouble("Extrude distance", &extrude_distance);
            if (ImGui::Button("Extrude Selected Face")) {
                carto::editor::ToolArguments arguments;
                arguments.distance = extrude_distance;
                dispatch(carto::application::InvokeToolAction{"mesh.extrude-face", arguments});
            }
        }
        ImGui::EndChild();
    }

    void draw_bottom(const ApplicationSnapshot& snapshot) {
        ImGui::BeginChild("Bottom", ImVec2(0, 130), true);
        ImGui::Text("PROBLEMS: %llu  |  UNDO: %llu  |  REDO: %llu",
                    static_cast<unsigned long long>(snapshot.problems.size()),
                    static_cast<unsigned long long>(snapshot.undo_count),
                    static_cast<unsigned long long>(snapshot.redo_count));
        for (const auto& problem : snapshot.problems) {
            ImGui::TextColored(ImVec4(1.0F, 0.5F, 0.4F, 1.0F), "%s: %s",
                               carto::core::error_code_name(problem.code), problem.message.c_str());
        }
        ImGui::EndChild();
    }

    void draw() {
        process_close_request();
        draw_discard_prompt();
        const auto snapshot = session.snapshot();
        draw_menu(snapshot);
        draw_toolbar(snapshot);
        ImGui::Begin("Cartographer Workspace", nullptr, ImGuiWindowFlags_NoCollapse);
        const bool show_outliner = has_pane(snapshot, carto::application::Pane::outliner);
        const bool show_inspector = has_pane(snapshot, carto::application::Pane::inspector);
        const bool show_bottom = has_pane(snapshot, carto::application::Pane::problems) ||
            has_pane(snapshot, carto::application::Pane::history);
        if (show_outliner) {
            draw_outliner(snapshot);
            ImGui::SameLine();
        }
        ImGui::BeginGroup();
        draw_viewport(snapshot);
        if (show_bottom) draw_bottom(snapshot);
        ImGui::EndGroup();
        if (show_inspector) {
            ImGui::SameLine();
            draw_inspector(snapshot);
        }
        ImGui::End();
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
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (RegisterClassW(&window_class) == 0) throw std::runtime_error("could not register Cartographer window class");
    HWND window = CreateWindowExW(
        0, window_class.lpszClassName, L"Cartographer", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) throw std::runtime_error("could not create Cartographer window");
    return window;
}

int run(HINSTANCE instance) {
    DesktopState state;
    g_state = &state;
    state.window = create_window(instance);
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(state.window)) throw std::runtime_error("Dear ImGui Win32 initialization failed");
    state.renderer.initialize(state.window);

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

    state.renderer.shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_state = nullptr;
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
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
