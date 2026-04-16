/*
 * php-vio - Vulkan Backend implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_VULKAN

#include <vulkan/vulkan.h>

#ifdef HAVE_GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

#include "vio_vulkan.h"
#include <string.h>
#include <stdlib.h>

vio_vulkan_state vio_vk = {0};

/* ── Debug messenger ─────────────────────────────────────────────── */

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user_data)
{
    (void)type; (void)user_data;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        php_error_docref(NULL, E_NOTICE, "Vulkan: %s", data->pMessage);
    }
    return VK_FALSE;
}

/* ── Helper: find memory type ────────────────────────────────────── */

static uint32_t find_memory_type(uint32_t filter, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vio_vk.physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((filter & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* ── Helper: find depth format ───────────────────────────────────── */

static VkFormat find_depth_format(void)
{
    VkFormat candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT };
    for (int i = 0; i < 3; i++) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(vio_vk.physical_device, candidates[i], &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return candidates[i];
        }
    }
    return VK_FORMAT_D32_SFLOAT;
}

/* ── Instance creation ───────────────────────────────────────────── */

static int create_instance(int debug)
{
    VkApplicationInfo app_info = {0};
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName   = "php-vio";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName        = "php-vio";
    app_info.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion         = VK_API_VERSION_1_0;

    /* Required extensions from GLFW + portability */
    uint32_t glfw_ext_count = 0;
    const char **glfw_extensions = NULL;
#ifdef HAVE_GLFW
    glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
#endif

    /* Build extension list */
    uint32_t ext_count = glfw_ext_count;
    const char *extra_exts[4];
    uint32_t extra_count = 0;

    if (debug) {
        extra_exts[extra_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }
    /* MoltenVK portability */
    extra_exts[extra_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;

    uint32_t total_ext_count = ext_count + extra_count;
    const char **all_extensions = malloc(total_ext_count * sizeof(const char *));
    for (uint32_t i = 0; i < glfw_ext_count; i++) all_extensions[i] = glfw_extensions[i];
    for (uint32_t i = 0; i < extra_count; i++) all_extensions[ext_count + i] = extra_exts[i];

    VkInstanceCreateInfo create_info = {0};
    create_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo        = &app_info;
    create_info.enabledExtensionCount   = total_ext_count;
    create_info.ppEnabledExtensionNames = all_extensions;
    create_info.flags                   = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

    const char *validation_layer = "VK_LAYER_KHRONOS_validation";
    if (debug) {
        create_info.enabledLayerCount   = 1;
        create_info.ppEnabledLayerNames = &validation_layer;
    }

    VkResult result = vkCreateInstance(&create_info, NULL, &vio_vk.instance);
    free(all_extensions);

    if (result != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Failed to create Vulkan instance (VkResult %d)", result);
        return -1;
    }

    /* Set up debug messenger */
    if (debug) {
        VkDebugUtilsMessengerCreateInfoEXT dbg_info = {0};
        dbg_info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dbg_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbg_info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbg_info.pfnUserCallback = vk_debug_callback;

        PFN_vkCreateDebugUtilsMessengerEXT func =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vio_vk.instance, "vkCreateDebugUtilsMessengerEXT");
        if (func) {
            func(vio_vk.instance, &dbg_info, NULL, &vio_vk.debug_messenger);
        }
        vio_vk.debug_enabled = 1;
    }

    return 0;
}

/* ── Physical device selection ───────────────────────────────────── */

static int select_physical_device(void)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(vio_vk.instance, &count, NULL);
    if (count == 0) {
        php_error_docref(NULL, E_WARNING, "No Vulkan-capable GPU found");
        return -1;
    }

    VkPhysicalDevice *devices = malloc(count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(vio_vk.instance, &count, devices);

    /* Pick first device with graphics + present queue support */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qf_count, NULL);
        VkQueueFamilyProperties *qf_props = malloc(qf_count * sizeof(VkQueueFamilyProperties));
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qf_count, qf_props);

        int gfx_found = 0, present_found = 0;
        for (uint32_t j = 0; j < qf_count; j++) {
            if (qf_props[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                vio_vk.graphics_family = j;
                gfx_found = 1;
            }

            VkBool32 present_support = VK_FALSE;
            if (vio_vk.surface) {
                vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], j, vio_vk.surface, &present_support);
            }
            if (present_support) {
                vio_vk.present_family = j;
                present_found = 1;
            }

            if (gfx_found && present_found) break;
        }
        free(qf_props);

        if (gfx_found && present_found) {
            vio_vk.physical_device = devices[i];
            free(devices);
            return 0;
        }
    }

    free(devices);
    php_error_docref(NULL, E_WARNING, "No suitable Vulkan GPU found (need graphics + present queue)");
    return -1;
}

/* ── Logical device creation ─────────────────────────────────────── */

static int create_logical_device(void)
{
    float queue_priority = 1.0f;

    /* Unique queue families */
    uint32_t unique_families[2] = { vio_vk.graphics_family, vio_vk.present_family };
    uint32_t unique_count = (vio_vk.graphics_family == vio_vk.present_family) ? 1 : 2;

    VkDeviceQueueCreateInfo queue_infos[2] = {0};
    for (uint32_t i = 0; i < unique_count; i++) {
        queue_infos[i].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_infos[i].queueFamilyIndex = unique_families[i];
        queue_infos[i].queueCount       = 1;
        queue_infos[i].pQueuePriorities = &queue_priority;
    }

    /* Required device extensions */
    const char *device_extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        "VK_KHR_portability_subset", /* MoltenVK */
    };

    /* Check if portability_subset is available */
    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(vio_vk.physical_device, NULL, &ext_count, NULL);
    VkExtensionProperties *ext_props = malloc(ext_count * sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(vio_vk.physical_device, NULL, &ext_count, ext_props);

    int has_portability = 0;
    for (uint32_t i = 0; i < ext_count; i++) {
        if (strcmp(ext_props[i].extensionName, "VK_KHR_portability_subset") == 0) {
            has_portability = 1;
            break;
        }
    }
    free(ext_props);

    uint32_t device_ext_count = has_portability ? 2 : 1;

    VkPhysicalDeviceFeatures features = {0};

    VkDeviceCreateInfo create_info = {0};
    create_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount    = unique_count;
    create_info.pQueueCreateInfos       = queue_infos;
    create_info.enabledExtensionCount   = device_ext_count;
    create_info.ppEnabledExtensionNames = device_extensions;
    create_info.pEnabledFeatures        = &features;

    VkResult result = vkCreateDevice(vio_vk.physical_device, &create_info, NULL, &vio_vk.device);
    if (result != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Failed to create Vulkan logical device (VkResult %d)", result);
        return -1;
    }

    vkGetDeviceQueue(vio_vk.device, vio_vk.graphics_family, 0, &vio_vk.graphics_queue);
    vkGetDeviceQueue(vio_vk.device, vio_vk.present_family, 0, &vio_vk.present_queue);

    return 0;
}

/* ── Swapchain creation ──────────────────────────────────────────── */

static void cleanup_swapchain(void)
{
    if (vio_vk.depth_view) { vkDestroyImageView(vio_vk.device, vio_vk.depth_view, NULL); vio_vk.depth_view = VK_NULL_HANDLE; }
    if (vio_vk.depth_image) {
        if (vio_vk.vma_allocator) {
            /* depth_memory is used as VMA allocation handle */
            vio_vma_destroy_image(vio_vk.vma_allocator, vio_vk.depth_image, vio_vk.depth_memory);
        }
        vio_vk.depth_image = VK_NULL_HANDLE;
        vio_vk.depth_memory = VK_NULL_HANDLE;
    }

    if (vio_vk.framebuffers) {
        for (uint32_t i = 0; i < vio_vk.swapchain_image_count; i++) {
            vkDestroyFramebuffer(vio_vk.device, vio_vk.framebuffers[i], NULL);
        }
        free(vio_vk.framebuffers);
        vio_vk.framebuffers = NULL;
    }

    if (vio_vk.swapchain_image_views) {
        for (uint32_t i = 0; i < vio_vk.swapchain_image_count; i++) {
            vkDestroyImageView(vio_vk.device, vio_vk.swapchain_image_views[i], NULL);
        }
        free(vio_vk.swapchain_image_views);
        vio_vk.swapchain_image_views = NULL;
    }

    free(vio_vk.swapchain_images);
    vio_vk.swapchain_images = NULL;

    if (vio_vk.swapchain) {
        vkDestroySwapchainKHR(vio_vk.device, vio_vk.swapchain, NULL);
        vio_vk.swapchain = VK_NULL_HANDLE;
    }
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vio_vk.physical_device, vio_vk.surface, &caps);

    /* Choose format */
    uint32_t fmt_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vio_vk.physical_device, vio_vk.surface, &fmt_count, NULL);
    VkSurfaceFormatKHR *formats = malloc(fmt_count * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(vio_vk.physical_device, vio_vk.surface, &fmt_count, formats);

    VkSurfaceFormatKHR chosen_format = formats[0];
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen_format = formats[i];
            break;
        }
    }
    free(formats);

    /* Choose present mode */
    uint32_t pm_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vio_vk.physical_device, vio_vk.surface, &pm_count, NULL);
    VkPresentModeKHR *modes = malloc(pm_count * sizeof(VkPresentModeKHR));
    vkGetPhysicalDeviceSurfacePresentModesKHR(vio_vk.physical_device, vio_vk.surface, &pm_count, modes);

    VkPresentModeKHR chosen_mode = VK_PRESENT_MODE_FIFO_KHR; /* vsync, always available */
    for (uint32_t i = 0; i < pm_count; i++) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            chosen_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            break;
        }
    }
    free(modes);

    /* Choose extent */
    VkExtent2D extent;
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width  = (uint32_t)vio_vk.framebuffer_width;
        extent.height = (uint32_t)vio_vk.framebuffer_height;
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR sc_info = {0};
    sc_info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sc_info.surface          = vio_vk.surface;
    sc_info.minImageCount    = image_count;
    sc_info.imageFormat      = chosen_format.format;
    sc_info.imageColorSpace  = chosen_format.colorSpace;
    sc_info.imageExtent      = extent;
    sc_info.imageArrayLayers = 1;
    sc_info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc_info.preTransform     = caps.currentTransform;
    sc_info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc_info.presentMode      = chosen_mode;
    sc_info.clipped          = VK_TRUE;

    uint32_t queue_families[] = { vio_vk.graphics_family, vio_vk.present_family };
    if (vio_vk.graphics_family != vio_vk.present_family) {
        sc_info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        sc_info.queueFamilyIndexCount = 2;
        sc_info.pQueueFamilyIndices   = queue_families;
    } else {
        sc_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkResult result = vkCreateSwapchainKHR(vio_vk.device, &sc_info, NULL, &vio_vk.swapchain);
    if (result != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Failed to create Vulkan swapchain (VkResult %d)", result);
        return -1;
    }

    vio_vk.swapchain_format = chosen_format.format;
    vio_vk.swapchain_extent = extent;

    /* Get swapchain images */
    vkGetSwapchainImagesKHR(vio_vk.device, vio_vk.swapchain, &vio_vk.swapchain_image_count, NULL);
    vio_vk.swapchain_images = malloc(vio_vk.swapchain_image_count * sizeof(VkImage));
    vkGetSwapchainImagesKHR(vio_vk.device, vio_vk.swapchain, &vio_vk.swapchain_image_count, vio_vk.swapchain_images);

    /* Create image views */
    vio_vk.swapchain_image_views = malloc(vio_vk.swapchain_image_count * sizeof(VkImageView));
    for (uint32_t i = 0; i < vio_vk.swapchain_image_count; i++) {
        VkImageViewCreateInfo iv_info = {0};
        iv_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv_info.image    = vio_vk.swapchain_images[i];
        iv_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv_info.format   = vio_vk.swapchain_format;
        iv_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        iv_info.subresourceRange.baseMipLevel   = 0;
        iv_info.subresourceRange.levelCount     = 1;
        iv_info.subresourceRange.baseArrayLayer = 0;
        iv_info.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(vio_vk.device, &iv_info, NULL, &vio_vk.swapchain_image_views[i]) != VK_SUCCESS) {
            php_error_docref(NULL, E_WARNING, "Failed to create swapchain image view %u", i);
            return -1;
        }
    }

    /* Create depth buffer */
    VkFormat depth_format = find_depth_format();
    VkImageCreateInfo depth_img_info = {0};
    depth_img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_img_info.imageType     = VK_IMAGE_TYPE_2D;
    depth_img_info.format        = depth_format;
    depth_img_info.extent.width  = extent.width;
    depth_img_info.extent.height = extent.height;
    depth_img_info.extent.depth  = 1;
    depth_img_info.mipLevels     = 1;
    depth_img_info.arrayLayers   = 1;
    depth_img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    depth_img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    depth_img_info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    void *depth_alloc = NULL;
    if (vio_vma_create_image(vio_vk.vma_allocator, &depth_img_info,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              &vio_vk.depth_image, &depth_alloc) != 0) {
        php_error_docref(NULL, E_WARNING, "Failed to create depth image");
        return -1;
    }
    vio_vk.depth_memory = (VkDeviceMemory)depth_alloc; /* Stores VMA allocation handle */

    VkImageViewCreateInfo depth_view_info = {0};
    depth_view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_info.image    = vio_vk.depth_image;
    depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_info.format   = depth_format;
    depth_view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_view_info.subresourceRange.baseMipLevel   = 0;
    depth_view_info.subresourceRange.levelCount     = 1;
    depth_view_info.subresourceRange.baseArrayLayer = 0;
    depth_view_info.subresourceRange.layerCount     = 1;

    if (vkCreateImageView(vio_vk.device, &depth_view_info, NULL, &vio_vk.depth_view) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Failed to create depth image view");
        return -1;
    }

    /* Create framebuffers */
    vio_vk.framebuffers = malloc(vio_vk.swapchain_image_count * sizeof(VkFramebuffer));
    for (uint32_t i = 0; i < vio_vk.swapchain_image_count; i++) {
        VkImageView attachments[] = { vio_vk.swapchain_image_views[i], vio_vk.depth_view };

        VkFramebufferCreateInfo fb_info = {0};
        fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass      = vio_vk.render_pass;
        fb_info.attachmentCount = 2;
        fb_info.pAttachments    = attachments;
        fb_info.width           = extent.width;
        fb_info.height          = extent.height;
        fb_info.layers          = 1;

        if (vkCreateFramebuffer(vio_vk.device, &fb_info, NULL, &vio_vk.framebuffers[i]) != VK_SUCCESS) {
            php_error_docref(NULL, E_WARNING, "Failed to create framebuffer %u", i);
            return -1;
        }
    }

    return 0;
}

/* ── Render pass creation ────────────────────────────────────────── */

static int create_render_pass(VkFormat color_format)
{
    VkFormat depth_format = find_depth_format();

    VkAttachmentDescription attachments[2] = {0};
    /* Color attachment */
    attachments[0].format         = color_format;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    /* Depth attachment */
    attachments[1].format         = depth_format;
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency dep = {0};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info = {0};
    rp_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 2;
    rp_info.pAttachments    = attachments;
    rp_info.subpassCount    = 1;
    rp_info.pSubpasses      = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies   = &dep;

    if (vkCreateRenderPass(vio_vk.device, &rp_info, NULL, &vio_vk.render_pass) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Failed to create Vulkan render pass");
        return -1;
    }

    return 0;
}

/* ── Per-frame resources ─────────────────────────────────────────── */

static int create_frame_resources(void)
{
    for (int i = 0; i < VIO_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        vio_vk_frame *f = &vio_vk.frames[i];

        VkCommandPoolCreateInfo pool_info = {0};
        pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = vio_vk.graphics_family;

        if (vkCreateCommandPool(vio_vk.device, &pool_info, NULL, &f->cmd_pool) != VK_SUCCESS) {
            return -1;
        }

        VkCommandBufferAllocateInfo alloc_info = {0};
        alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool        = f->cmd_pool;
        alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(vio_vk.device, &alloc_info, &f->cmd_buf) != VK_SUCCESS) {
            return -1;
        }

        VkSemaphoreCreateInfo sem_info = {0};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(vio_vk.device, &sem_info, NULL, &f->image_available);
        vkCreateSemaphore(vio_vk.device, &sem_info, NULL, &f->render_finished);

        VkFenceCreateInfo fence_info = {0};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(vio_vk.device, &fence_info, NULL, &f->in_flight);
    }

    return 0;
}

static void destroy_frame_resources(void)
{
    for (int i = 0; i < VIO_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        vio_vk_frame *f = &vio_vk.frames[i];
        if (f->in_flight) vkDestroyFence(vio_vk.device, f->in_flight, NULL);
        if (f->render_finished) vkDestroySemaphore(vio_vk.device, f->render_finished, NULL);
        if (f->image_available) vkDestroySemaphore(vio_vk.device, f->image_available, NULL);
        if (f->cmd_pool) vkDestroyCommandPool(vio_vk.device, f->cmd_pool, NULL);
    }
}

/* ── Swapchain recreation ────────────────────────────────────────── */

int vio_vulkan_recreate_swapchain(void)
{
#ifdef HAVE_GLFW
    int w = 0, h = 0;
    glfwGetFramebufferSize((GLFWwindow *)vio_vk.glfw_window, &w, &h);
    while (w == 0 || h == 0) {
        glfwGetFramebufferSize((GLFWwindow *)vio_vk.glfw_window, &w, &h);
        glfwWaitEvents();
    }
    vio_vk.framebuffer_width  = w;
    vio_vk.framebuffer_height = h;
#endif

    vkDeviceWaitIdle(vio_vk.device);
    cleanup_swapchain();
    return create_swapchain();
}

/* ── Full Vulkan setup ───────────────────────────────────────────── */

int vio_vulkan_setup_context(void *glfw_window, vio_config *cfg)
{
    memset(&vio_vk, 0, sizeof(vio_vk));
    vio_vk.glfw_window = glfw_window;
    vio_vk.clear_r = 0.1f;
    vio_vk.clear_g = 0.1f;
    vio_vk.clear_b = 0.1f;
    vio_vk.clear_a = 1.0f;

    /* 1. Instance */
    if (create_instance(cfg->debug) != 0) return -1;

    /* 2. Surface (via GLFW) */
#ifdef HAVE_GLFW
    if (glfwCreateWindowSurface(vio_vk.instance, (GLFWwindow *)glfw_window, NULL, &vio_vk.surface) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Failed to create Vulkan window surface");
        return -1;
    }

    glfwGetFramebufferSize((GLFWwindow *)glfw_window, &vio_vk.framebuffer_width, &vio_vk.framebuffer_height);
#endif

    /* 3. Physical device */
    if (select_physical_device() != 0) return -1;

    /* 4. Logical device */
    if (create_logical_device() != 0) return -1;

    /* 5. VMA allocator */
    if (vio_vma_create(vio_vk.instance, vio_vk.physical_device, vio_vk.device, &vio_vk.vma_allocator) != 0) {
        php_error_docref(NULL, E_WARNING, "Failed to create VMA allocator");
        return -1;
    }

    /* 6. Render pass (need to know swapchain format first — pick B8G8R8A8_SRGB as default) */
    /* We'll determine the actual format during swapchain creation, create render pass with it */
    /* For now, query surface format to create render pass before swapchain */
    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vio_vk.physical_device, vio_vk.surface, &fmt_count, NULL);
    VkSurfaceFormatKHR *formats = malloc(fmt_count * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(vio_vk.physical_device, vio_vk.surface, &fmt_count, formats);
    VkFormat color_format = formats[0].format;
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB) {
            color_format = formats[i].format;
            break;
        }
    }
    free(formats);

    if (create_render_pass(color_format) != 0) return -1;

    /* 7. Swapchain + framebuffers */
    if (create_swapchain() != 0) return -1;

    /* 8. Per-frame resources */
    if (create_frame_resources() != 0) return -1;

    vio_vk.initialized = 1;
    return 0;
}

/* ── Backend vtable implementation ───────────────────────────────── */

static int vulkan_init(vio_config *cfg)
{
    (void)cfg;
    /* Actual init happens in vio_vulkan_setup_context after window creation */
    return 0;
}

static void vulkan_shutdown(void)
{
    if (!vio_vk.initialized) return;

    vkDeviceWaitIdle(vio_vk.device);

    destroy_frame_resources();
    cleanup_swapchain();

    if (vio_vk.render_pass) vkDestroyRenderPass(vio_vk.device, vio_vk.render_pass, NULL);
    if (vio_vk.vma_allocator) vio_vma_destroy(vio_vk.vma_allocator);
    if (vio_vk.device) vkDestroyDevice(vio_vk.device, NULL);
    if (vio_vk.surface) vkDestroySurfaceKHR(vio_vk.instance, vio_vk.surface, NULL);

    if (vio_vk.debug_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT func =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(vio_vk.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func) func(vio_vk.instance, vio_vk.debug_messenger, NULL);
    }

    if (vio_vk.instance) vkDestroyInstance(vio_vk.instance, NULL);

    memset(&vio_vk, 0, sizeof(vio_vk));
}

static void *vulkan_create_surface(vio_config *cfg) { (void)cfg; return NULL; }
static void vulkan_destroy_surface(void *s) { (void)s; }

static void vulkan_resize(int width, int height)
{
    (void)width; (void)height;
    vio_vk.swapchain_needs_recreate = 1;
}

static void *vulkan_create_pipeline(vio_pipeline_desc *desc) { (void)desc; return NULL; }
static void vulkan_destroy_pipeline(void *p) { (void)p; }
static void vulkan_bind_pipeline(void *p) { (void)p; }

static void *vulkan_create_buffer(vio_buffer_desc *desc) { (void)desc; return NULL; }
static void vulkan_update_buffer(void *buf, const void *data, size_t size) { (void)buf; (void)data; (void)size; }
static void vulkan_destroy_buffer(void *buf) { (void)buf; }

static void *vulkan_create_texture(vio_texture_desc *desc) { (void)desc; return NULL; }
static void vulkan_destroy_texture(void *tex) { (void)tex; }

static void *vulkan_compile_shader(vio_shader_desc *desc) { (void)desc; return NULL; }
static void vulkan_destroy_shader(void *shader) { (void)shader; }

static void vulkan_begin_frame(void)
{
    if (!vio_vk.initialized) return;

    vio_vk_frame *f = &vio_vk.frames[vio_vk.current_frame];

    /* Wait for this frame's previous work to finish */
    vkWaitForFences(vio_vk.device, 1, &f->in_flight, VK_TRUE, UINT64_MAX);

    /* Acquire next swapchain image */
    VkResult result = vkAcquireNextImageKHR(vio_vk.device, vio_vk.swapchain, UINT64_MAX,
                                             f->image_available, VK_NULL_HANDLE,
                                             &vio_vk.current_image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        vio_vulkan_recreate_swapchain();
        return;
    }

    vkResetFences(vio_vk.device, 1, &f->in_flight);
    vkResetCommandBuffer(f->cmd_buf, 0);

    /* Begin command buffer */
    VkCommandBufferBeginInfo begin_info = {0};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(f->cmd_buf, &begin_info);

    /* Begin render pass */
    VkClearValue clear_values[2];
    clear_values[0].color.float32[0] = vio_vk.clear_r;
    clear_values[0].color.float32[1] = vio_vk.clear_g;
    clear_values[0].color.float32[2] = vio_vk.clear_b;
    clear_values[0].color.float32[3] = vio_vk.clear_a;
    clear_values[1].depthStencil.depth   = 1.0f;
    clear_values[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rp_begin = {0};
    rp_begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass        = vio_vk.render_pass;
    rp_begin.framebuffer       = vio_vk.framebuffers[vio_vk.current_image_index];
    rp_begin.renderArea.offset = (VkOffset2D){0, 0};
    rp_begin.renderArea.extent = vio_vk.swapchain_extent;
    rp_begin.clearValueCount   = 2;
    rp_begin.pClearValues      = clear_values;

    vkCmdBeginRenderPass(f->cmd_buf, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    /* Set dynamic viewport and scissor */
    VkViewport viewport = {0};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = (float)vio_vk.swapchain_extent.width;
    viewport.height   = (float)vio_vk.swapchain_extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(f->cmd_buf, 0, 1, &viewport);

    VkRect2D scissor = {0};
    scissor.offset = (VkOffset2D){0, 0};
    scissor.extent = vio_vk.swapchain_extent;
    vkCmdSetScissor(f->cmd_buf, 0, 1, &scissor);
}

static void vulkan_end_frame(void)
{
    if (!vio_vk.initialized) return;

    vio_vk_frame *f = &vio_vk.frames[vio_vk.current_frame];

    /* End render pass and command buffer */
    vkCmdEndRenderPass(f->cmd_buf);
    vkEndCommandBuffer(f->cmd_buf);

    /* Submit */
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit = {0};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = &f->image_available;
    submit.pWaitDstStageMask    = &wait_stage;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &f->cmd_buf;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &f->render_finished;

    vkQueueSubmit(vio_vk.graphics_queue, 1, &submit, f->in_flight);
}

static void vulkan_present(void)
{
    if (!vio_vk.initialized) return;

    vio_vk_frame *f = &vio_vk.frames[vio_vk.current_frame];

    VkPresentInfoKHR present_info = {0};
    present_info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores    = &f->render_finished;
    present_info.swapchainCount     = 1;
    present_info.pSwapchains        = &vio_vk.swapchain;
    present_info.pImageIndices      = &vio_vk.current_image_index;

    VkResult result = vkQueuePresentKHR(vio_vk.present_queue, &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || vio_vk.swapchain_needs_recreate) {
        vio_vk.swapchain_needs_recreate = 0;
        vio_vulkan_recreate_swapchain();
    }

    vio_vk.current_frame = (vio_vk.current_frame + 1) % VIO_VK_MAX_FRAMES_IN_FLIGHT;
}

static void vulkan_draw(vio_draw_cmd *cmd) { (void)cmd; }
static void vulkan_draw_indexed(vio_draw_indexed_cmd *cmd) { (void)cmd; }

static void vulkan_clear(float r, float g, float b, float a)
{
    vio_vk.clear_r = r;
    vio_vk.clear_g = g;
    vio_vk.clear_b = b;
    vio_vk.clear_a = a;
}

static void vulkan_dispatch_compute(vio_compute_cmd *cmd) { (void)cmd; }

static int vulkan_supports_feature(vio_feature feature)
{
    switch (feature) {
        case VIO_FEATURE_COMPUTE:      return 1;
        case VIO_FEATURE_TESSELLATION: return 1;
        case VIO_FEATURE_GEOMETRY:     return 1;
        default: return 0;
    }
}

static const vio_backend vulkan_backend = {
    .name              = "vulkan",
    .api_version       = VIO_BACKEND_API_VERSION,
    .init              = vulkan_init,
    .shutdown          = vulkan_shutdown,
    .create_surface    = vulkan_create_surface,
    .destroy_surface   = vulkan_destroy_surface,
    .resize            = vulkan_resize,
    .create_pipeline   = vulkan_create_pipeline,
    .destroy_pipeline  = vulkan_destroy_pipeline,
    .bind_pipeline     = vulkan_bind_pipeline,
    .create_buffer     = vulkan_create_buffer,
    .update_buffer     = vulkan_update_buffer,
    .destroy_buffer    = vulkan_destroy_buffer,
    .create_texture    = vulkan_create_texture,
    .destroy_texture   = vulkan_destroy_texture,
    .compile_shader    = vulkan_compile_shader,
    .destroy_shader    = vulkan_destroy_shader,
    .begin_frame       = vulkan_begin_frame,
    .end_frame         = vulkan_end_frame,
    .draw              = vulkan_draw,
    .draw_indexed      = vulkan_draw_indexed,
    .present           = vulkan_present,
    .clear             = vulkan_clear,
    .gpu_flush         = NULL,
    .dispatch_compute  = vulkan_dispatch_compute,
    .supports_feature  = vulkan_supports_feature,
};

void vio_backend_vulkan_register(void)
{
    vio_register_backend(&vulkan_backend);
}

#endif /* HAVE_VULKAN */
