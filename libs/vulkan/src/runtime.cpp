#include <carto/vulkan/runtime.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto_vulkan_shaders.hpp>

#include <vulkan/vulkan.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace carto::vulkan {

namespace {

core::Diagnostic failure(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

constexpr std::uint32_t kRequiredVulkanApiVersion = VK_API_VERSION_1_1;

bool uuid_is_zero(const std::uint8_t* uuid) {
    return std::all_of(uuid, uuid + 16U, [](const std::uint8_t value) {
        return value == 0U;
    });
}

bool supports_format(
    VkPhysicalDevice device,
    VkFormat format,
    VkFormatFeatureFlags required) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(device, format, &properties);
    return (properties.optimalTilingFeatures & required) == required;
}

std::optional<std::uint32_t> find_memory_type(
    VkPhysicalDevice device,
    std::uint32_t type_bits,
    VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(device, &properties);
    for (std::uint32_t index = 0U; index < properties.memoryTypeCount; ++index) {
        if ((type_bits & (1U << index)) != 0U &&
            (properties.memoryTypes[index].propertyFlags & required) == required) {
            return index;
        }
    }
    return std::nullopt;
}

bool create_host_buffer(
    VkPhysicalDevice physical_device,
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    const void* initial_data,
    VkBuffer& buffer,
    VkDeviceMemory& memory) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    const auto memory_type = find_memory_type(
        physical_device,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!memory_type.has_value()) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *memory_type;
    if (vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(device, buffer, memory, 0U) != VK_SUCCESS) {
        if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        memory = VK_NULL_HANDLE;
        buffer = VK_NULL_HANDLE;
        return false;
    }

    if (initial_data != nullptr) {
        void* mapped = nullptr;
        if (vkMapMemory(device, memory, 0U, size, 0U, &mapped) != VK_SUCCESS ||
            mapped == nullptr) {
            vkFreeMemory(device, memory, nullptr);
            vkDestroyBuffer(device, buffer, nullptr);
            memory = VK_NULL_HANDLE;
            buffer = VK_NULL_HANDLE;
            return false;
        }
        std::memcpy(mapped, initial_data, static_cast<std::size_t>(size));
        vkUnmapMemory(device, memory);
    }
    return true;
}

bool create_shader_module(
    VkDevice device,
    const std::uint32_t* code,
    std::size_t word_count,
    VkShaderModule& shader) {
    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = word_count * sizeof(std::uint32_t);
    shader_info.pCode = code;
    return vkCreateShaderModule(device, &shader_info, nullptr, &shader) == VK_SUCCESS;
}

struct AcceptanceResources {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceMemory index_memory = VK_NULL_HANDLE;
    VkImage color_image = VK_NULL_HANDLE;
    VkImageView color_view = VK_NULL_HANDLE;
    VkDeviceMemory color_memory = VK_NULL_HANDLE;
    VkImage depth_image = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule fragment_shader = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    ~AcceptanceResources() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
            if (pipeline_layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            }
            if (fragment_shader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device, fragment_shader, nullptr);
            }
            if (vertex_shader != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device, vertex_shader, nullptr);
            }
            if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer, nullptr);
            if (render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(device, render_pass, nullptr);
            if (readback_buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, readback_buffer, nullptr);
            if (readback_memory != VK_NULL_HANDLE) vkFreeMemory(device, readback_memory, nullptr);
            if (vertex_buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, vertex_buffer, nullptr);
            if (vertex_memory != VK_NULL_HANDLE) vkFreeMemory(device, vertex_memory, nullptr);
            if (index_buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, index_buffer, nullptr);
            if (index_memory != VK_NULL_HANDLE) vkFreeMemory(device, index_memory, nullptr);
            if (color_view != VK_NULL_HANDLE) vkDestroyImageView(device, color_view, nullptr);
            if (color_image != VK_NULL_HANDLE) vkDestroyImage(device, color_image, nullptr);
            if (color_memory != VK_NULL_HANDLE) vkFreeMemory(device, color_memory, nullptr);
            if (depth_view != VK_NULL_HANDLE) vkDestroyImageView(device, depth_view, nullptr);
            if (depth_image != VK_NULL_HANDLE) vkDestroyImage(device, depth_image, nullptr);
            if (depth_memory != VK_NULL_HANDLE) vkFreeMemory(device, depth_memory, nullptr);
            if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }
};

} // namespace

core::Result<RuntimeInfo> Runtime::probe() {
    std::uint32_t api_version = VK_API_VERSION_1_0;
    auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerate_version != nullptr) {
        if (enumerate_version(&api_version) != VK_SUCCESS) {
            return core::Result<RuntimeInfo>::failure(
                failure("Vulkan instance version query failed"));
        }
    }
    if (api_version < kRequiredVulkanApiVersion) {
        return core::Result<RuntimeInfo>::failure(
            failure("Cartographer native Vulkan runtime requires Vulkan 1.1"));
    }

    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Cartographer Runtime Probe";
    application_info.applicationVersion = 1;
    application_info.pEngineName = "Cartographer";
    application_info.engineVersion = 1;
    application_info.apiVersion = kRequiredVulkanApiVersion;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        return core::Result<RuntimeInfo>::failure(
            failure("Vulkan instance creation failed"));
    }

    std::uint32_t device_count = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (result != VK_SUCCESS || device_count == 0U) {
        vkDestroyInstance(instance, nullptr);
        return core::Result<RuntimeInfo>::failure(
            failure("Vulkan reported no physical device"));
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    result = vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
    if (result != VK_SUCCESS || devices.empty()) {
        vkDestroyInstance(instance, nullptr);
        return core::Result<RuntimeInfo>::failure(
            failure("Vulkan physical-device enumeration failed"));
    }

    VkPhysicalDevice selected = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties selected_properties{};
    std::uint32_t selected_graphics_family = 0U;
    gpu::DeviceCapabilities selected_capabilities;
    std::array<std::uint8_t, 16U> selected_device_uuid{};
    for (const VkPhysicalDevice device : devices) {
        VkPhysicalDeviceIDProperties device_id_properties{};
        device_id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &device_id_properties;
        vkGetPhysicalDeviceProperties2(device, &properties2);
        const VkPhysicalDeviceProperties& properties = properties2.properties;
        if (properties.apiVersion < kRequiredVulkanApiVersion ||
            uuid_is_zero(device_id_properties.deviceUUID)) {
            continue;
        }
        std::uint32_t queue_count = 0U;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, queues.data());

        std::optional<std::uint32_t> graphics_family;
        bool compute_queue = false;
        bool copy_queue = false;
        for (std::uint32_t index = 0U; index < queue_count; ++index) {
            const VkQueueFlags flags = queues[index].queueFlags;
            if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0U && !graphics_family.has_value()) {
                graphics_family = index;
            }
            compute_queue = compute_queue || ((flags & VK_QUEUE_COMPUTE_BIT) != 0U);
            copy_queue = copy_queue || ((flags & VK_QUEUE_TRANSFER_BIT) != 0U);
        }
        if (!graphics_family.has_value()) {
            continue;
        }

        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(device, &features);
        gpu::DeviceCapabilities capabilities;
        capabilities.graphics_queue = true;
        capabilities.compute_queue = compute_queue;
        capabilities.copy_queue = copy_queue;
        capabilities.anisotropy = features.samplerAnisotropy == VK_TRUE;
        capabilities.sampler_compare = true;
        capabilities.timestamp_queries = properties.limits.timestampComputeAndGraphics == VK_TRUE;
        capabilities.fp16_color_attachment = supports_format(
            device,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        capabilities.fp16_storage = supports_format(
            device,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
        capabilities.max_texture_dimension_2d = properties.limits.maxImageDimension2D;
        capabilities.max_sampler_anisotropy = properties.limits.maxSamplerAnisotropy;
        capabilities.max_push_constant_bytes = properties.limits.maxPushConstantsSize;
        capabilities.max_color_attachments = properties.limits.maxColorAttachments;

        const bool required_formats =
            supports_format(device, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) &&
            supports_format(device, VK_FORMAT_D32_SFLOAT,
                            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
        if (!required_formats || capabilities.max_texture_dimension_2d == 0U) {
            continue;
        }
        if (selected == VK_NULL_HANDLE ||
            (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
             selected_properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) {
            selected = device;
            selected_properties = properties;
            selected_graphics_family = *graphics_family;
            selected_capabilities = capabilities;
            std::copy(
                std::begin(device_id_properties.deviceUUID),
                std::end(device_id_properties.deviceUUID),
                selected_device_uuid.begin());
        }
    }

    if (selected == VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        return core::Result<RuntimeInfo>::failure(
            failure("Vulkan reported no adapter meeting Cartographer headless requirements"));
    }

    constexpr float queue_priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = selected_graphics_family;
    queue_info.queueCount = 1U;
    queue_info.pQueuePriorities = &queue_priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1U;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice logical_device = VK_NULL_HANDLE;
    if (vkCreateDevice(selected, &device_info, nullptr, &logical_device) != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return core::Result<RuntimeInfo>::failure(
            failure("Vulkan logical headless device creation failed"));
    }
    VkQueue graphics_queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(logical_device, selected_graphics_family, 0U, &graphics_queue);
    const bool queue_obtained = graphics_queue != VK_NULL_HANDLE;
    vkDeviceWaitIdle(logical_device);
    vkDestroyDevice(logical_device, nullptr);

    if (!queue_obtained) {
        vkDestroyInstance(instance, nullptr);
        return core::Result<RuntimeInfo>::failure(
            failure("Vulkan headless device did not expose a graphics queue"));
    }

    RuntimeInfo info;
    info.api_version = selected_properties.apiVersion;
    info.device_name = selected_properties.deviceName;
    info.vendor_id = selected_properties.vendorID;
    info.device_id = selected_properties.deviceID;
    info.graphics_queue_family = selected_graphics_family;
    info.logical_device_created = true;
    info.capabilities = selected_capabilities;
    info.device_uuid = selected_device_uuid;
    vkDestroyInstance(instance, nullptr);
    return core::Result<RuntimeInfo>::success(std::move(info));
}

core::Result<HeadlessTargetReceipt> Runtime::accept_headless_targets() {
    const auto source_mesh = geometry::make_plane(1.3, 1.3);
    if (!source_mesh) {
        return core::Result<HeadlessTargetReceipt>::failure(
            source_mesh.error().with_context("headless acceptance source mesh"));
    }
    const auto compiled_result = source_mesh.value().compile();
    if (!compiled_result) {
        return core::Result<HeadlessTargetReceipt>::failure(
            compiled_result.error().with_context("headless acceptance source mesh compilation"));
    }
    return accept_headless_targets(
        compiled_result.value(), compiled_result.value().source_revision);
}

core::Result<HeadlessTargetReceipt> Runtime::accept_headless_targets(
    const geometry::CompiledMesh& compiled_mesh,
    core::Revision expected_source_revision,
    std::uint32_t width,
    std::uint32_t height) {
    if (width == 0U || height == 0U) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance target extent must be non-zero"));
    }
    if (compiled_mesh.source_revision != expected_source_revision) {
        return core::Result<HeadlessTargetReceipt>::failure(stale(
            "headless acceptance compiled mesh revision does not match the expected source"));
    }
    if (!compiled_mesh.valid() || compiled_mesh.positions.empty() ||
        compiled_mesh.indices.empty() || compiled_mesh.indices.size() % 3U != 0U ||
        compiled_mesh.positions.size() > std::numeric_limits<std::uint32_t>::max() ||
        compiled_mesh.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance compiled mesh is invalid"));
    }

    const auto runtime = probe();
    if (!runtime) {
        return core::Result<HeadlessTargetReceipt>::failure(runtime.error());
    }
    if (width > runtime.value().capabilities.max_texture_dimension_2d ||
        height > runtime.value().capabilities.max_texture_dimension_2d) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance target extent exceeds adapter limits"));
    }
    const auto width64 = static_cast<VkDeviceSize>(width);
    const auto height64 = static_cast<VkDeviceSize>(height);
    if (width64 > std::numeric_limits<VkDeviceSize>::max() / height64 / 8U) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance readback extent overflows its byte size"));
    }
    const VkDeviceSize readback_bytes = width64 * height64 * 8U;

    struct Vertex {
        float position[3];
        float color[3];
    };
    std::vector<Vertex> vertices;
    vertices.reserve(compiled_mesh.positions.size());
    for (std::size_t index = 0U; index < compiled_mesh.positions.size(); ++index) {
        const auto position = compiled_mesh.positions[index];
        const auto normal = compiled_mesh.normals[index];
        vertices.push_back(Vertex{
            {
                static_cast<float>(position.x),
                static_cast<float>(position.y),
                static_cast<float>(position.z),
            },
            {
                0.2F + 0.6F * static_cast<float>(std::abs(normal.x)),
                0.2F + 0.6F * static_cast<float>(std::abs(normal.y)),
                0.2F + 0.6F * static_cast<float>(std::abs(normal.z)),
            },
        });
    }
    const auto& indices = compiled_mesh.indices;

    AcceptanceResources resources;
    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Cartographer Headless Acceptance";
    application_info.applicationVersion = 1U;
    application_info.pEngineName = "Cartographer";
    application_info.engineVersion = 1U;
    application_info.apiVersion = kRequiredVulkanApiVersion;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;
    if (vkCreateInstance(&instance_info, nullptr, &resources.instance) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance instance creation failed"));
    }

    std::uint32_t device_count = 0U;
    if (vkEnumeratePhysicalDevices(resources.instance, &device_count, nullptr) != VK_SUCCESS ||
        device_count == 0U) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance found no physical devices"));
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    if (vkEnumeratePhysicalDevices(resources.instance, &device_count, devices.data()) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance physical-device enumeration failed"));
    }

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    for (const VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceIDProperties device_id_properties{};
        device_id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &device_id_properties;
        vkGetPhysicalDeviceProperties2(candidate, &properties2);
        const bool same_uuid = std::equal(
            runtime.value().device_uuid.begin(),
            runtime.value().device_uuid.end(),
            std::begin(device_id_properties.deviceUUID));
        if (same_uuid) {
            physical_device = candidate;
            break;
        }
    }
    if (physical_device == VK_NULL_HANDLE) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance could not reacquire the probed adapter"));
    }

    std::uint32_t queue_count = 0U;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, queues.data());
    std::optional<std::uint32_t> graphics_family;
    for (std::uint32_t index = 0U; index < queue_count; ++index) {
        if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
            graphics_family = index;
            break;
        }
    }
    if (!graphics_family.has_value()) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance adapter has no graphics queue"));
    }

    constexpr float queue_priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = *graphics_family;
    queue_info.queueCount = 1U;
    queue_info.pQueuePriorities = &queue_priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1U;
    device_info.pQueueCreateInfos = &queue_info;
    if (vkCreateDevice(physical_device, &device_info, nullptr, &resources.device) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance logical-device creation failed"));
    }

    VkCommandPoolCreateInfo command_pool_info{};
    command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_pool_info.queueFamilyIndex = *graphics_family;
    command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(resources.device, &command_pool_info, nullptr,
                            &resources.command_pool) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance command-pool creation failed"));
    }

    const auto create_image = [&](VkFormat format, VkImageUsageFlags usage,
                                  VkImageAspectFlags aspect, VkImage& image,
                                  VkDeviceMemory& memory, VkImageView& view) -> bool {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format;
        image_info.extent = {width, height, 1U};
        image_info.mipLevels = 1U;
        image_info.arrayLayers = 1U;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = usage;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(resources.device, &image_info, nullptr, &image) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(resources.device, image, &requirements);
        const auto memory_type = find_memory_type(
            physical_device, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!memory_type.has_value()) return false;
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *memory_type;
        if (vkAllocateMemory(resources.device, &allocation, nullptr, &memory) != VK_SUCCESS ||
            vkBindImageMemory(resources.device, image, memory, 0U) != VK_SUCCESS) {
            return false;
        }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {aspect, 0U, 1U, 0U, 1U};
        return vkCreateImageView(resources.device, &view_info, nullptr, &view) == VK_SUCCESS;
    };

    if (!create_image(
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            resources.color_image,
            resources.color_memory,
            resources.color_view)) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance RGBA16F target creation failed"));
    }
    if (!create_image(
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            resources.depth_image,
            resources.depth_memory,
            resources.depth_view)) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance D32 target creation failed"));
    }

    if (!create_host_buffer(
            physical_device,
            resources.device,
            readback_bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            nullptr,
            resources.readback_buffer,
            resources.readback_memory)) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance readback buffer creation failed"));
    }

    if (!create_host_buffer(
            physical_device,
            resources.device,
            static_cast<VkDeviceSize>(vertices.size() * sizeof(Vertex)),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            vertices.data(),
            resources.vertex_buffer,
            resources.vertex_memory)) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance vertex buffer creation failed"));
    }
    if (!create_host_buffer(
            physical_device,
            resources.device,
            static_cast<VkDeviceSize>(indices.size() * sizeof(std::uint32_t)),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            indices.data(),
            resources.index_buffer,
            resources.index_memory)) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance index buffer creation failed"));
    }

    VkCommandBufferAllocateInfo command_buffer_info{};
    command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_info.commandPool = resources.command_pool;
    command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_info.commandBufferCount = 1U;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(resources.device, &command_buffer_info, &command_buffer) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance command-buffer allocation failed"));
    }

    const std::array<VkAttachmentDescription, 2U> attachments = {
        VkAttachmentDescription{
            0U,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_CLEAR,
            VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        },
        VkAttachmentDescription{
            0U,
            VK_FORMAT_D32_SFLOAT,
            VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_CLEAR,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };
    VkAttachmentReference color_attachment{};
    color_attachment.attachment = 0U;
    color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depth_attachment{};
    depth_attachment.attachment = 1U;
    depth_attachment.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1U;
    subpass.pColorAttachments = &color_attachment;
    subpass.pDepthStencilAttachment = &depth_attachment;
    const std::array<VkSubpassDependency, 2U> dependencies = {
        VkSubpassDependency{
            VK_SUBPASS_EXTERNAL,
            0U,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            0U,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_DEPENDENCY_BY_REGION_BIT,
        },
        VkSubpassDependency{
            0U,
            VK_SUBPASS_EXTERNAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_DEPENDENCY_BY_REGION_BIT,
        },
    };
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    render_pass_info.pAttachments = attachments.data();
    render_pass_info.subpassCount = 1U;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    render_pass_info.pDependencies = dependencies.data();
    if (vkCreateRenderPass(resources.device, &render_pass_info, nullptr,
                           &resources.render_pass) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance render-pass creation failed"));
    }

    const std::array<VkImageView, 2U> framebuffer_attachments = {
        resources.color_view,
        resources.depth_view,
    };
    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = resources.render_pass;
    framebuffer_info.attachmentCount = static_cast<std::uint32_t>(framebuffer_attachments.size());
    framebuffer_info.pAttachments = framebuffer_attachments.data();
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = 1U;
    if (vkCreateFramebuffer(resources.device, &framebuffer_info, nullptr,
                            &resources.framebuffer) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance framebuffer creation failed"));
    }

    if (!create_shader_module(
            resources.device,
            shaders::triangle_vertex.data(),
            shaders::triangle_vertex.size(),
            resources.vertex_shader) ||
        !create_shader_module(
            resources.device,
            shaders::triangle_fragment.data(),
            shaders::triangle_fragment.size(),
            resources.fragment_shader)) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance shader-module creation failed"));
    }

    const std::array<VkPipelineShaderStageCreateInfo, 2U> shader_stages = {
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0U,
            VK_SHADER_STAGE_VERTEX_BIT,
            resources.vertex_shader,
            "main",
            nullptr,
        },
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0U,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            resources.fragment_shader,
            "main",
            nullptr,
        },
    };
    const VkVertexInputBindingDescription vertex_binding{
        0U,
        static_cast<std::uint32_t>(sizeof(Vertex)),
        VK_VERTEX_INPUT_RATE_VERTEX,
    };
    const std::array<VkVertexInputAttributeDescription, 2U> vertex_attributes = {
        VkVertexInputAttributeDescription{0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U},
        VkVertexInputAttributeDescription{
            1U,
            0U,
            VK_FORMAT_R32G32B32_SFLOAT,
            static_cast<std::uint32_t>(sizeof(float) * 3U),
        },
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1U;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount =
        static_cast<std::uint32_t>(vertex_attributes.size());
    vertex_input.pVertexAttributeDescriptions = vertex_attributes.data();
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{
        0.0F,
        0.0F,
        static_cast<float>(width),
        static_cast<float>(height),
        0.0F,
        1.0F,
    };
    VkRect2D scissor{{0, 0}, {width, height}};
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1U;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1U;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.depthClampEnable = VK_FALSE;
    rasterization.rasterizerDiscardEnable = VK_FALSE;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState color_blend_attachment{};
    color_blend_attachment.blendEnable = VK_FALSE;
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1U;
    color_blend.pAttachments = &color_blend_attachment;
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(resources.device, &pipeline_layout_info, nullptr,
                               &resources.pipeline_layout) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance pipeline-layout creation failed"));
    }
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = static_cast<std::uint32_t>(shader_stages.size());
    pipeline_info.pStages = shader_stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.layout = resources.pipeline_layout;
    pipeline_info.renderPass = resources.render_pass;
    pipeline_info.subpass = 0U;
    if (vkCreateGraphicsPipelines(resources.device, VK_NULL_HANDLE, 1U, &pipeline_info,
                                  nullptr, &resources.pipeline) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance graphics-pipeline creation failed"));
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance command-buffer begin failed"));
    }

    std::array<VkClearValue, 2U> clear_values{};
    clear_values[0].color.float32[0] = 0.0F;
    clear_values[0].color.float32[1] = 0.0F;
    clear_values[0].color.float32[2] = 0.0F;
    clear_values[0].color.float32[3] = 1.0F;
    clear_values[1].depthStencil.depth = 1.0F;
    clear_values[1].depthStencil.stencil = 0U;
    VkRenderPassBeginInfo render_pass_begin{};
    render_pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin.renderPass = resources.render_pass;
    render_pass_begin.framebuffer = resources.framebuffer;
    render_pass_begin.renderArea = VkRect2D{{0, 0}, {width, height}};
    render_pass_begin.clearValueCount = static_cast<std::uint32_t>(clear_values.size());
    render_pass_begin.pClearValues = clear_values.data();
    vkCmdBeginRenderPass(command_buffer, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, resources.pipeline);
    const VkDeviceSize vertex_offset = 0U;
    vkCmdBindVertexBuffers(command_buffer, 0U, 1U, &resources.vertex_buffer, &vertex_offset);
    vkCmdBindIndexBuffer(command_buffer, resources.index_buffer, 0U, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(
        command_buffer,
        static_cast<std::uint32_t>(indices.size()),
        1U,
        0U,
        0,
        0U);
    vkCmdEndRenderPass(command_buffer);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 0U, 1U};
    copy.imageExtent = {width, height, 1U};
    vkCmdCopyImageToBuffer(command_buffer, resources.color_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           resources.readback_buffer, 1U, &copy);
    if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance command-buffer end failed"));
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(resources.device, &fence_info, nullptr, &resources.fence) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance fence creation failed"));
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1U;
    submit.pCommandBuffers = &command_buffer;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(resources.device, *graphics_family, 0U, &graphics_queue);
    if (graphics_queue == VK_NULL_HANDLE ||
        vkQueueSubmit(graphics_queue, 1U, &submit, resources.fence) != VK_SUCCESS ||
        vkWaitForFences(resources.device, 1U, &resources.fence, VK_TRUE,
                        std::numeric_limits<std::uint64_t>::max()) != VK_SUCCESS) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance submission or wait failed"));
    }

    void* mapped = nullptr;
    if (vkMapMemory(resources.device, resources.readback_memory, 0U, readback_bytes, 0U, &mapped) != VK_SUCCESS ||
        mapped == nullptr) {
        return core::Result<HeadlessTargetReceipt>::failure(
            failure("headless acceptance readback map failed"));
    }
    HeadlessTargetReceipt receipt;
    receipt.width = width;
    receipt.height = height;
    receipt.rgba16f_target_created = true;
    receipt.depth32_target_created = true;
    receipt.compiled_mesh_uploaded = true;
    receipt.compiled_mesh_vertex_count = static_cast<std::uint32_t>(compiled_mesh.positions.size());
    receipt.compiled_mesh_index_count = static_cast<std::uint32_t>(compiled_mesh.indices.size());
    receipt.indexed_draw_submitted = true;
    receipt.readback_completed = true;
    receipt.source_revision = compiled_mesh.source_revision;
    std::memcpy(receipt.first_rgba16f_pixel.data(), mapped, receipt.first_rgba16f_pixel.size());
    const VkDeviceSize center_offset =
        (static_cast<VkDeviceSize>(height / 2U) * static_cast<VkDeviceSize>(width) +
         static_cast<VkDeviceSize>(width / 2U)) * 8U;
    std::memcpy(
        receipt.center_rgba16f_pixel.data(),
        static_cast<const std::uint8_t*>(mapped) + static_cast<std::size_t>(center_offset),
        receipt.center_rgba16f_pixel.size());
    vkUnmapMemory(resources.device, resources.readback_memory);
    return core::Result<HeadlessTargetReceipt>::success(receipt);
}

} // namespace carto::vulkan
