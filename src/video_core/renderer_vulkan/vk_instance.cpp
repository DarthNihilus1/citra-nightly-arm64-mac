// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <span>
#include "common/assert.h"
#include "core/frontend/emu_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

vk::Format ToVkFormat(VideoCore::PixelFormat format) {
    switch (format) {
    case VideoCore::PixelFormat::RGBA8:
        return vk::Format::eR8G8B8A8Unorm;
    case VideoCore::PixelFormat::RGB8:
        return vk::Format::eB8G8R8Unorm;
    case VideoCore::PixelFormat::RGB5A1:
        return vk::Format::eR5G5B5A1UnormPack16;
    case VideoCore::PixelFormat::RGB565:
        return vk::Format::eR5G6B5UnormPack16;
    case VideoCore::PixelFormat::RGBA4:
        return vk::Format::eR4G4B4A4UnormPack16;
    case VideoCore::PixelFormat::D16:
        return vk::Format::eD16Unorm;
    case VideoCore::PixelFormat::D24:
        return vk::Format::eX8D24UnormPack32;
    case VideoCore::PixelFormat::D24S8:
        return vk::Format::eD24UnormS8Uint;
    case VideoCore::PixelFormat::Invalid:
        LOG_ERROR(Render_Vulkan, "Unknown texture format {}!", format);
        return vk::Format::eUndefined;
    default:
        // Use default case for the texture formats
        return vk::Format::eR8G8B8A8Unorm;
    }
}

Instance::Instance() {
    // Fetch instance independant function pointers
    auto vkGetInstanceProcAddr =
        dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    const vk::ApplicationInfo application_info = {.pApplicationName = "Citra",
                                                  .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                                                  .pEngineName = "Citra Vulkan",
                                                  .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                                                  .apiVersion = VK_API_VERSION_1_0};

    const vk::InstanceCreateInfo instance_info = {.pApplicationInfo = &application_info};

    instance = vk::createInstance(instance_info);

    // Load required function pointers for querying the physical device
    VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumeratePhysicalDevices = PFN_vkEnumeratePhysicalDevices(
        vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties = PFN_vkGetPhysicalDeviceProperties(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyInstance =
        PFN_vkDestroyInstance(vkGetInstanceProcAddr(instance, "vkDestroyInstance"));

    physical_devices = instance.enumeratePhysicalDevices();
}

Instance::Instance(Frontend::EmuWindow& window, u32 physical_device_index, bool enable_validation) {
    auto window_info = window.GetWindowInfo();

    // Fetch instance independant function pointers
    auto vkGetInstanceProcAddr =
        dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    // Enable the instance extensions the backend uses
    auto extensions = GetInstanceExtensions(window_info.type, true);

    // We require a Vulkan 1.1 driver
    const u32 available_version = vk::enumerateInstanceVersion();
    if (available_version < VK_API_VERSION_1_1) {
        LOG_CRITICAL(Render_Vulkan, "Vulkan 1.0 is not supported, 1.1 is required!");
        return;
    }

    const vk::ApplicationInfo application_info = {.pApplicationName = "Citra",
                                                  .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                                                  .pEngineName = "Citra Vulkan",
                                                  .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                                                  .apiVersion = available_version};

    const std::array layers = {"VK_LAYER_KHRONOS_validation"};
    const u32 layer_count = enable_validation ? 1u : 0u;
    const vk::InstanceCreateInfo instance_info = {.pApplicationInfo = &application_info,
                                                  .enabledLayerCount = layer_count,
                                                  .ppEnabledLayerNames = layers.data(),
                                                  .enabledExtensionCount =
                                                      static_cast<u32>(extensions.size()),
                                                  .ppEnabledExtensionNames = extensions.data()};

    instance = vk::createInstance(instance_info);
    surface = CreateSurface(instance, window);

    // Pick physical device
    physical_devices = instance.enumeratePhysicalDevices();
    if (const u16 physical_device_count = static_cast<u16>(physical_devices.size());
        physical_device_index >= physical_devices.size()) {
        LOG_CRITICAL(Render_Vulkan,
                     "Invalid physical device index {} provided when only {} devices exist",
                     physical_device_index, physical_device_count);
        UNREACHABLE();
    }

    physical_device = physical_devices[physical_device_index];
    device_properties = physical_device.getProperties();

    CreateDevice();
    CreateFormatTable();
}

Instance::~Instance() {
    if (device) {
        vmaDestroyAllocator(allocator);
        device.destroy();
        instance.destroySurfaceKHR(surface);
    }

    instance.destroy();
}

FormatTraits Instance::GetTraits(VideoCore::PixelFormat pixel_format) const {
    if (pixel_format == VideoCore::PixelFormat::Invalid) [[unlikely]] {
        return FormatTraits{};
    }

    const u32 index = static_cast<u32>(pixel_format);
    return format_table[index];
}

void Instance::CreateFormatTable() {
    constexpr std::array pixel_formats = {
        VideoCore::PixelFormat::RGBA8,  VideoCore::PixelFormat::RGB8,
        VideoCore::PixelFormat::RGB5A1, VideoCore::PixelFormat::RGB565,
        VideoCore::PixelFormat::RGBA4,  VideoCore::PixelFormat::IA8,
        VideoCore::PixelFormat::RG8,    VideoCore::PixelFormat::I8,
        VideoCore::PixelFormat::A8,     VideoCore::PixelFormat::IA4,
        VideoCore::PixelFormat::I4,     VideoCore::PixelFormat::A4,
        VideoCore::PixelFormat::ETC1,   VideoCore::PixelFormat::ETC1A4,
        VideoCore::PixelFormat::D16,    VideoCore::PixelFormat::D24,
        VideoCore::PixelFormat::D24S8};

    const vk::FormatFeatureFlags storage_usage = vk::FormatFeatureFlagBits::eStorageImage;
    const vk::FormatFeatureFlags blit_usage =
        vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferDst |
        vk::FormatFeatureFlagBits::eTransferSrc | vk::FormatFeatureFlagBits::eBlitSrc |
        vk::FormatFeatureFlagBits::eBlitDst;

    for (const auto& pixel_format : pixel_formats) {
        const vk::Format format = ToVkFormat(pixel_format);
        const vk::FormatProperties properties = physical_device.getFormatProperties(format);
        const vk::ImageAspectFlags aspect = GetImageAspect(format);

        const vk::FormatFeatureFlagBits attachment_usage =
            (aspect & vk::ImageAspectFlagBits::eDepth)
                ? vk::FormatFeatureFlagBits::eDepthStencilAttachment
                : vk::FormatFeatureFlagBits::eColorAttachment;

        const bool supports_blit = (properties.optimalTilingFeatures & blit_usage) == blit_usage;
        const bool supports_attachment =
            (properties.optimalTilingFeatures & attachment_usage) == attachment_usage;
        const bool supports_storage =
            (properties.optimalTilingFeatures & storage_usage) == storage_usage;

        // Find the most inclusive usage flags for this format
        vk::ImageUsageFlags best_usage;
        if (supports_blit) {
            best_usage |= vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                          vk::ImageUsageFlagBits::eTransferSrc;
        }
        if (supports_attachment) {
            best_usage |= (aspect & vk::ImageAspectFlagBits::eDepth)
                              ? vk::ImageUsageFlagBits::eDepthStencilAttachment
                              : vk::ImageUsageFlagBits::eColorAttachment;
        }
        if (supports_storage) {
            best_usage |= vk::ImageUsageFlagBits::eStorage;
        }

        // Always fallback to RGBA8 or D32(S8) for convenience
        vk::Format fallback = vk::Format::eR8G8B8A8Unorm;
        if (aspect & vk::ImageAspectFlagBits::eDepth) {
            fallback = vk::Format::eD32Sfloat;
            if (aspect & vk::ImageAspectFlagBits::eStencil) {
                fallback = vk::Format::eD32SfloatS8Uint;
            }
        }

        // Report completely unsupported formats
        if (!supports_blit && !supports_attachment && !supports_storage) {
            LOG_WARNING(Render_Vulkan, "Format {} unsupported, falling back unconditionally to {}",
                        vk::to_string(format), vk::to_string(fallback));
        }

        const u32 index = static_cast<u32>(pixel_format);
        format_table[index] = FormatTraits{.blit_support = supports_blit,
                                           .attachment_support = supports_attachment,
                                           .storage_support = supports_storage,
                                           .usage = best_usage,
                                           .native = format,
                                           .fallback = fallback};
    }
}

bool Instance::CreateDevice() {
    auto feature_chain =
        physical_device.getFeatures2<vk::PhysicalDeviceFeatures2,
                                     vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
                                     vk::PhysicalDeviceTimelineSemaphoreFeaturesKHR>();

    // Not having geometry shaders will cause issues with accelerated rendering.
    const vk::PhysicalDeviceFeatures available = feature_chain.get().features;
    if (!available.geometryShader) {
        LOG_WARNING(Render_Vulkan,
                    "Geometry shaders not availabe! Accelerated rendering not possible!");
    }

    auto extension_list = physical_device.enumerateDeviceExtensionProperties();
    if (extension_list.empty()) {
        LOG_CRITICAL(Render_Vulkan, "No extensions supported by device.");
        return false;
    }

    // Helper lambda for adding extensions
    std::array<const char*, 6> enabled_extensions;
    u32 enabled_extension_count = 0;

    auto AddExtension = [&](std::string_view name) -> bool {
        auto result =
            std::find_if(extension_list.begin(), extension_list.end(),
                         [&](const auto& prop) { return name.compare(prop.extensionName.data()); });

        if (result != extension_list.end()) {
            LOG_INFO(Render_Vulkan, "Enabling extension: {}", name);
            enabled_extensions[enabled_extension_count++] = name.data();
            return true;
        }

        LOG_WARNING(Render_Vulkan, "Extension {} unavailable.", name);
        return false;
    };

    AddExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    AddExtension(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    timeline_semaphores = AddExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    extended_dynamic_state = AddExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    push_descriptors = AddExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);

    // Search queue families for graphics and present queues
    auto family_properties = physical_device.getQueueFamilyProperties();
    if (family_properties.empty()) {
        LOG_CRITICAL(Render_Vulkan, "Vulkan physical device reported no queues.");
        return false;
    }

    bool graphics_queue_found = false;
    bool present_queue_found = false;
    for (std::size_t i = 0; i < family_properties.size(); i++) {
        // Check if queue supports graphics
        const u32 index = static_cast<u32>(i);
        if (family_properties[i].queueFlags & vk::QueueFlagBits::eGraphics) {
            graphics_queue_family_index = index;
            graphics_queue_found = true;

            // If this queue also supports presentation we are finished
            if (physical_device.getSurfaceSupportKHR(static_cast<u32>(i), surface)) {
                present_queue_family_index = index;
                present_queue_found = true;
                break;
            }
        }

        // Check if queue supports presentation
        if (physical_device.getSurfaceSupportKHR(index, surface)) {
            present_queue_family_index = index;
            present_queue_found = true;
        }
    }

    if (!graphics_queue_found || !present_queue_found) {
        LOG_CRITICAL(Render_Vulkan, "Unable to find graphics and/or present queues.");
        return false;
    }

    static constexpr float queue_priorities[] = {1.0f};

    const std::array queue_infos = {
        vk::DeviceQueueCreateInfo{.queueFamilyIndex = graphics_queue_family_index,
                                  .queueCount = 1,
                                  .pQueuePriorities = queue_priorities},
        vk::DeviceQueueCreateInfo{.queueFamilyIndex = present_queue_family_index,
                                  .queueCount = 1,
                                  .pQueuePriorities = queue_priorities}};

    const u32 queue_count = graphics_queue_family_index != present_queue_family_index ? 2u : 1u;
    const vk::StructureChain device_chain = {
        vk::DeviceCreateInfo{
            .queueCreateInfoCount = queue_count,
            .pQueueCreateInfos = queue_infos.data(),
            .enabledExtensionCount = enabled_extension_count,
            .ppEnabledExtensionNames = enabled_extensions.data(),
        },
        vk::PhysicalDeviceFeatures2{
            .features = {.robustBufferAccess = available.robustBufferAccess,
                         .geometryShader = available.geometryShader,
                         .dualSrcBlend = available.dualSrcBlend,
                         .logicOp = available.logicOp,
                         .depthClamp = available.depthClamp,
                         .largePoints = available.largePoints,
                         .samplerAnisotropy = available.samplerAnisotropy,
                         .fragmentStoresAndAtomics = available.fragmentStoresAndAtomics,
                         .shaderStorageImageMultisample = available.shaderStorageImageMultisample,
                         .shaderClipDistance = available.shaderClipDistance}},
        vk::PhysicalDeviceDepthClipControlFeaturesEXT{.depthClipControl = true},
        feature_chain.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>(),
        feature_chain.get<vk::PhysicalDeviceTimelineSemaphoreFeaturesKHR>()};

    // Create logical device
    device = physical_device.createDevice(device_chain.get());
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

    // Grab the graphics and present queues.
    graphics_queue = device.getQueue(graphics_queue_family_index, 0);
    present_queue = device.getQueue(present_queue_family_index, 0);

    CreateAllocator();
    return true;
}

void Instance::CreateAllocator() {
    const VmaVulkanFunctions functions = {
        .vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr};

    const VmaAllocatorCreateInfo allocator_info = {.physicalDevice = physical_device,
                                                   .device = device,
                                                   .pVulkanFunctions = &functions,
                                                   .instance = instance,
                                                   .vulkanApiVersion = VK_API_VERSION_1_1};

    if (VkResult result = vmaCreateAllocator(&allocator_info, &allocator); result != VK_SUCCESS) {
        LOG_CRITICAL(Render_Vulkan, "Failed to initialize VMA with error {}", result);
        UNREACHABLE();
    }
}

} // namespace Vulkan