#include <carto/vulkan/runtime.hpp>

#include <vulkan/vulkan.h>

#include <string>
#include <utility>
#include <vector>

namespace carto::vulkan {

namespace {

core::Diagnostic failure(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

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

    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME};
    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Cartographer Runtime Probe";
    application_info.applicationVersion = 1;
    application_info.pEngineName = "Cartographer";
    application_info.engineVersion = 1;
    application_info.apiVersion = api_version;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;
    instance_info.enabledExtensionCount = 1;
    instance_info.ppEnabledExtensionNames = extensions;

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

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(devices.front(), &properties);
    RuntimeInfo info{api_version, properties.deviceName};
    vkDestroyInstance(instance, nullptr);
    return core::Result<RuntimeInfo>::success(std::move(info));
}

} // namespace carto::vulkan
