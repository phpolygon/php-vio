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
#include "../../vio_texture.h"
#include "../../vio_font.h"
#include "../../../include/vio_types.h"
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
#ifdef __APPLE__
    /* MoltenVK requires the portability enumeration extension + flag */
    extra_exts[extra_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
#endif

    uint32_t total_ext_count = ext_count + extra_count;
    const char **all_extensions = malloc(total_ext_count * sizeof(const char *));
    for (uint32_t i = 0; i < glfw_ext_count; i++) all_extensions[i] = glfw_extensions[i];
    for (uint32_t i = 0; i < extra_count; i++) all_extensions[ext_count + i] = extra_exts[i];

    VkInstanceCreateInfo create_info = {0};
    create_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo        = &app_info;
    create_info.enabledExtensionCount   = total_ext_count;
    create_info.ppEnabledExtensionNames = all_extensions;
#ifdef __APPLE__
    create_info.flags                   = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

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
    /* render_finished semaphores are per swapchain image; tear them down with
     * the rest of the swapchain. Callers (vio_vulkan_recreate_swapchain and
     * vulkan_shutdown) vkDeviceWaitIdle first, so none is in use by a pending
     * submit or present here. */
    if (vio_vk.render_finished_per_image) {
        for (uint32_t i = 0; i < vio_vk.swapchain_image_count; i++) {
            if (vio_vk.render_finished_per_image[i]) {
                vkDestroySemaphore(vio_vk.device, vio_vk.render_finished_per_image[i], NULL);
            }
        }
        free(vio_vk.render_finished_per_image);
        vio_vk.render_finished_per_image = NULL;
    }

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

    /* Prefer B8G8R8A8_UNORM (linear) so vertex colors land in the swapchain
     * identically to the other backends (D3D12 uses an UNORM target). An sRGB
     * swapchain would gamma-encode the same colors and shift them brighter,
     * making golden-image parity impossible. */
    VkSurfaceFormatKHR chosen_format = formats[0];
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
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

    /* Create one render_finished semaphore PER SWAPCHAIN IMAGE (see the field
     * comment in vio_vulkan.h). Sized to swapchain_image_count, which may differ
     * across recreates — cleanup_swapchain() destroys these, so the count is
     * always re-derived here. */
    vio_vk.render_finished_per_image =
        malloc(vio_vk.swapchain_image_count * sizeof(VkSemaphore));
    for (uint32_t i = 0; i < vio_vk.swapchain_image_count; i++) {
        VkSemaphoreCreateInfo sem_info = {0};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(vio_vk.device, &sem_info, NULL,
                              &vio_vk.render_finished_per_image[i]) != VK_SUCCESS) {
            php_error_docref(NULL, E_WARNING, "Failed to create render_finished semaphore %u", i);
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

    /* External dependency. The source scope MUST cover the PRIOR frame's
     * attachment writes (color store at COLOR_ATTACHMENT_OUTPUT, depth store at
     * LATE_FRAGMENT_TESTS) so they complete before this frame's loadOp clears /
     * layout transitions write the same attachments. Omitting LATE_FRAGMENT_TESTS
     * + the WRITE access bits from the source leaves a depth WRITE_AFTER_WRITE
     * hazard across consecutive frames that synchronization validation flags.
     * Both EARLY and LATE fragment-test stages are listed for depth; color uses
     * COLOR_ATTACHMENT_OUTPUT for both load and store. */
    VkSubpassDependency dep = {0};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                      | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                      | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                      | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                      | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                      | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
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

        /* image_available stays per frame-in-flight: it is signalled by
         * vkAcquireNextImageKHR and waited by this frame's submit, so it is
         * never reused across an in-flight present (unlike render_finished,
         * which lives per swapchain image in vio_vk.render_finished_per_image). */
        VkSemaphoreCreateInfo sem_info = {0};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(vio_vk.device, &sem_info, NULL, &f->image_available);

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

    /* 6. Render pass. The color format MUST match the swapchain format chosen
     * in create_swapchain() (B8G8R8A8_UNORM preferred) so the framebuffers and
     * the 2D pipelines are render-pass-compatible. */
    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vio_vk.physical_device, vio_vk.surface, &fmt_count, NULL);
    VkSurfaceFormatKHR *formats = malloc(fmt_count * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(vio_vk.physical_device, vio_vk.surface, &fmt_count, formats);
    VkFormat color_format = formats[0].format;
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
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

/* Forward decl: defined with the texture code below, used by vulkan_shutdown's
 * live-texture sweep above it. */
static void vulkan_release_texture_gpu(vio_vulkan_texture *tex, int wait);

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

    /* Sweep any backend textures whose owning PHP object outlived vio_destroy()
     * (the Zend free handlers run during request shutdown, AFTER this). Without
     * this their VkImage/VkImageView/VkSampler would still be alive at
     * vkDestroyDevice — a VUID-vkDestroyDevice-device-05137 validation error.
     * The GPU is already idle, so pass wait=0. The free handlers null their own
     * backend_texture pointers afterwards, but since the device is gone by then
     * GPU objects are gone, so the later vulkan_destroy_texture only frees the
     * heap struct (its release step is a no-op on the now-NULL device). */
    while (vio_vk.live_textures) {
        vulkan_release_texture_gpu(vio_vk.live_textures, 0);
    }

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

/* ── Texture creation ────────────────────────────────────────────────
 *
 * Uploads happen at vio_texture()/vio_font() time, which is OUTSIDE any frame
 * (no swapchain command buffer is recording). We therefore allocate a transient
 * one-time-submit command buffer, record the layout transitions + buffer-to-
 * image copy, submit, and block on a transfer fence before tearing the staging
 * resources down. This keeps the upload entirely off the per-frame command
 * buffer so there is no cross-frame hazard.
 */
static void *vulkan_create_texture(vio_texture_desc *desc)
{
    if (!vio_vk.initialized || !vio_vk.device || desc->width <= 0 || desc->height <= 0) {
        return NULL;
    }

    vio_vulkan_texture *tex = calloc(1, sizeof(vio_vulkan_texture));
    if (!tex) return NULL;
    tex->width  = desc->width;
    tex->height = desc->height;

    const VkFormat   fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize img_bytes = (VkDeviceSize)desc->width * (VkDeviceSize)desc->height * 4u;

    /* 1. DEVICE_LOCAL sampled image. */
    VkImageCreateInfo img_info = {0};
    img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType     = VK_IMAGE_TYPE_2D;
    img_info.format        = fmt;
    img_info.extent.width  = (uint32_t)desc->width;
    img_info.extent.height = (uint32_t)desc->height;
    img_info.extent.depth  = 1;
    img_info.mipLevels     = 1;
    img_info.arrayLayers   = 1;
    img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    img_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vio_vma_create_image(vio_vk.vma_allocator, &img_info,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                             &tex->image, &tex->allocation) != 0) {
        php_error_docref(NULL, E_WARNING, "Vulkan: failed to create texture image (%dx%d)",
                          desc->width, desc->height);
        free(tex);
        return NULL;
    }

    /* 2. Upload pixel data (if any) via a HOST_VISIBLE staging buffer + a
     *    transient one-time-submit command buffer. */
    if (desc->data && img_bytes > 0) {
        VkBuffer staging = VK_NULL_HANDLE;
        void    *staging_alloc = NULL;
        if (vio_vma_create_buffer(vio_vk.vma_allocator, img_bytes,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &staging, &staging_alloc) != 0) {
            php_error_docref(NULL, E_WARNING, "Vulkan: failed to create texture staging buffer");
            vio_vma_destroy_image(vio_vk.vma_allocator, tex->image, tex->allocation);
            free(tex);
            return NULL;
        }

        void *mapped = vio_vma_map(vio_vk.vma_allocator, staging_alloc);
        if (mapped) {
            memcpy(mapped, desc->data, (size_t)img_bytes);
            vio_vma_unmap(vio_vk.vma_allocator, staging_alloc);
        }

        /* Transient command pool + one-time-submit buffer. */
        VkCommandPool up_pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pool_info = {0};
        pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = vio_vk.graphics_family;
        vkCreateCommandPool(vio_vk.device, &pool_info, NULL, &up_pool);

        VkCommandBuffer up_cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cmd_alloc = {0};
        cmd_alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_alloc.commandPool        = up_pool;
        cmd_alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(vio_vk.device, &cmd_alloc, &up_cmd);

        VkCommandBufferBeginInfo begin = {0};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(up_cmd, &begin);

        /* UNDEFINED -> TRANSFER_DST_OPTIMAL. */
        VkImageMemoryBarrier to_dst = {0};
        to_dst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image               = tex->image;
        to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_dst.subresourceRange.levelCount = 1;
        to_dst.subresourceRange.layerCount = 1;
        to_dst.srcAccessMask       = 0;
        to_dst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(up_cmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &to_dst);

        VkBufferImageCopy copy = {0};
        copy.bufferOffset      = 0;
        copy.bufferRowLength   = 0;   /* tightly packed */
        copy.bufferImageHeight = 0;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width  = (uint32_t)desc->width;
        copy.imageExtent.height = (uint32_t)desc->height;
        copy.imageExtent.depth  = 1;
        vkCmdCopyBufferToImage(up_cmd, staging, tex->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        /* TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL. */
        VkImageMemoryBarrier to_read = {0};
        to_read.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_read.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.image               = tex->image;
        to_read.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_read.subresourceRange.levelCount = 1;
        to_read.subresourceRange.layerCount = 1;
        to_read.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(up_cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &to_read);

        vkEndCommandBuffer(up_cmd);

        /* Submit + block on a transfer fence (upload is outside any frame, so a
         * full queue stall here is acceptable and keeps lifetimes simple). */
        VkFence up_fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fci = {0};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(vio_vk.device, &fci, NULL, &up_fence);

        VkSubmitInfo submit = {0};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &up_cmd;
        vkQueueSubmit(vio_vk.graphics_queue, 1, &submit, up_fence);
        vkWaitForFences(vio_vk.device, 1, &up_fence, VK_TRUE, UINT64_MAX);

        vkDestroyFence(vio_vk.device, up_fence, NULL);
        vkDestroyCommandPool(vio_vk.device, up_pool, NULL); /* frees up_cmd */
        vio_vma_destroy_buffer(vio_vk.vma_allocator, staging, staging_alloc);
    } else {
        /* No data: still transition UNDEFINED -> SHADER_READ_ONLY so the image
         * is in a samplable layout (sampling it yields garbage, but the layout
         * is valid and the layers stay quiet). */
        VkCommandPool up_pool = VK_NULL_HANDLE;
        VkCommandPoolCreateInfo pool_info = {0};
        pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = vio_vk.graphics_family;
        vkCreateCommandPool(vio_vk.device, &pool_info, NULL, &up_pool);

        VkCommandBuffer up_cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cmd_alloc = {0};
        cmd_alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_alloc.commandPool        = up_pool;
        cmd_alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(vio_vk.device, &cmd_alloc, &up_cmd);

        VkCommandBufferBeginInfo begin = {0};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(up_cmd, &begin);

        VkImageMemoryBarrier to_read = {0};
        to_read.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_read.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        to_read.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.image               = tex->image;
        to_read.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_read.subresourceRange.levelCount = 1;
        to_read.subresourceRange.layerCount = 1;
        to_read.srcAccessMask       = 0;
        to_read.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(up_cmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &to_read);

        vkEndCommandBuffer(up_cmd);

        VkFence up_fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fci = {0};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(vio_vk.device, &fci, NULL, &up_fence);

        VkSubmitInfo submit = {0};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &up_cmd;
        vkQueueSubmit(vio_vk.graphics_queue, 1, &submit, up_fence);
        vkWaitForFences(vio_vk.device, 1, &up_fence, VK_TRUE, UINT64_MAX);

        vkDestroyFence(vio_vk.device, up_fence, NULL);
        vkDestroyCommandPool(vio_vk.device, up_pool, NULL);
    }

    /* 3. Image view. */
    VkImageViewCreateInfo iv = {0};
    iv.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv.image    = tex->image;
    iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv.format   = fmt;
    iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    iv.subresourceRange.levelCount = 1;
    iv.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vio_vk.device, &iv, NULL, &tex->view) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan: failed to create texture image view");
        vio_vma_destroy_image(vio_vk.vma_allocator, tex->image, tex->allocation);
        free(tex);
        return NULL;
    }

    /* 4. Sampler from desc->filter / desc->wrap. */
    VkFilter             vk_filter = (desc->filter == VIO_FILTER_NEAREST)
                                     ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    VkSamplerAddressMode vk_wrap;
    switch (desc->wrap) {
        case VIO_WRAP_CLAMP:  vk_wrap = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case VIO_WRAP_MIRROR: vk_wrap = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        case VIO_WRAP_REPEAT:
        default:              vk_wrap = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
    }

    VkSamplerCreateInfo sci = {0};
    sci.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter     = vk_filter;
    sci.minFilter     = vk_filter;
    sci.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU  = vk_wrap;
    sci.addressModeV  = vk_wrap;
    sci.addressModeW  = vk_wrap;
    sci.maxLod        = 0.0f;
    sci.borderColor   = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    /* anisotropyEnable left VK_FALSE: the samplerAnisotropy feature is not
     * enabled on the device, so requesting it would be a validation error. */
    if (vkCreateSampler(vio_vk.device, &sci, NULL, &tex->sampler) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan: failed to create texture sampler");
        vkDestroyImageView(vio_vk.device, tex->view, NULL);
        vio_vma_destroy_image(vio_vk.vma_allocator, tex->image, tex->allocation);
        free(tex);
        return NULL;
    }

    /* Register in the live-texture list so vulkan_shutdown can sweep it if its
     * owning PHP object outlives vio_destroy(). */
    tex->prev = NULL;
    tex->next = vio_vk.live_textures;
    if (vio_vk.live_textures) vio_vk.live_textures->prev = tex;
    vio_vk.live_textures = tex;

    return tex;
}

/* Release a texture's GPU objects, unlink it from the live list, and zero the
 * handles so this is idempotent. Does NOT free the struct — the owning PHP
 * object's free handler is always responsible for that, which keeps the two
 * teardown paths (shutdown sweep vs. PHP GC, in either order) from double-
 * freeing the heap struct. `wait` drains the GPU first (the image may be in an
 * in-flight command buffer via a descriptor set); the shutdown sweep passes 0
 * because it already did a single vkDeviceWaitIdle. No-op on a NULL device. */
static void vulkan_release_texture_gpu(vio_vulkan_texture *tex, int wait)
{
    if (!tex) return;

    /* Unlink from the live list (idempotent: NULL links + head check). */
    if (tex->prev) tex->prev->next = tex->next;
    else if (vio_vk.live_textures == tex) vio_vk.live_textures = tex->next;
    if (tex->next) tex->next->prev = tex->prev;
    tex->prev = tex->next = NULL;

    if (vio_vk.device) {
        if (wait) vkDeviceWaitIdle(vio_vk.device);
        if (tex->sampler) vkDestroySampler(vio_vk.device, tex->sampler, NULL);
        if (tex->view)    vkDestroyImageView(vio_vk.device, tex->view, NULL);
        if (tex->image)   vio_vma_destroy_image(vio_vk.vma_allocator, tex->image, tex->allocation);
    }
    tex->sampler = VK_NULL_HANDLE;
    tex->view    = VK_NULL_HANDLE;
    tex->image   = VK_NULL_HANDLE;
    tex->allocation = NULL;
}

static void vulkan_destroy_texture(void *texture_ptr)
{
    vio_vulkan_texture *tex = (vio_vulkan_texture *)texture_ptr;
    if (!tex) return;
    /* Wait: the texture may still be referenced by an in-flight command buffer.
     * destroy is rare (texture/font GC or shutdown) so the stall is harmless.
     * If the shutdown sweep already released the GPU objects this is a no-op. */
    vulkan_release_texture_gpu(tex, 1);
    free(tex);
}

/* Object destructors invoked from the Zend free_object handlers (vio_texture.c
 * vio_texture_free_object -> destroy_texture_obj; vio_font.c
 * vio_font_free_object -> destroy_font_atlas). Both unwrap the vio_vulkan_texture
 * stored on the object and route to vulkan_destroy_texture so per-texture VMA
 * allocations / image views / samplers are freed rather than leaked (a leak the
 * validation layers report at vkDestroyDevice). */

static void vulkan_destroy_texture_obj(void *tex_obj_ptr)
{
    vio_texture_object *tex_obj = (vio_texture_object *)tex_obj_ptr;
    if (tex_obj->backend_texture) {
        vulkan_destroy_texture(tex_obj->backend_texture);
        tex_obj->backend_texture = NULL;
    }
}

static void vulkan_destroy_font_atlas(void *font_ptr)
{
    vio_font_object *font = (vio_font_object *)font_ptr;
    if (font->atlas_backend_texture) {
        vulkan_destroy_texture(font->atlas_backend_texture);
        font->atlas_backend_texture = NULL;
    }
}

static void *vulkan_compile_shader(vio_shader_desc *desc) { (void)desc; return NULL; }
static void vulkan_destroy_shader(void *shader) { (void)shader; }

static void vulkan_begin_frame(void)
{
    if (!vio_vk.initialized) return;

    vio_vk_frame *f = &vio_vk.frames[vio_vk.current_frame];

    /* Wait for this frame's previous work to finish */
    vkWaitForFences(vio_vk.device, 1, &f->in_flight, VK_TRUE, UINT64_MAX);

    /* Reset this frame's 2D descriptor pool. SAFE only because the fence wait
     * above guarantees the GPU has finished consuming the descriptor sets this
     * pool handed out two frames ago — resetting an in-flight pool would be a
     * use-after-free the sync-validation layer flags. No-op until the 2D Vulkan
     * state is initialised. */
    vio_2d_vulkan_reset_frame_descriptors(vio_vk.current_frame);

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

    vio_vk.in_frame = 1;
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
    /* Signal the render_finished tied to the swapchain IMAGE being rendered, not
     * the frame-in-flight (avoids VUID-vkQueueSubmit-pSignalSemaphores-00067). */
    submit.pSignalSemaphores    = &vio_vk.render_finished_per_image[vio_vk.current_image_index];

    vkQueueSubmit(vio_vk.graphics_queue, 1, &submit, f->in_flight);

    vio_vk.in_frame = 0;
}

static void vulkan_present(void)
{
    if (!vio_vk.initialized) return;

    /* No vio_vk_frame needed here: present waits on the per-image
     * render_finished (indexed by current_image_index), and image rotation /
     * current_frame advance use vio_vk fields directly. */
    VkPresentInfoKHR present_info = {0};
    present_info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    /* Wait on the render_finished tied to the image being presented (matches the
     * submit in vulkan_end_frame), indexed by current_image_index. */
    present_info.pWaitSemaphores    = &vio_vk.render_finished_per_image[vio_vk.current_image_index];
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
        case VIO_FEATURE_3D_PIPELINE:  return 1;
        case VIO_FEATURE_RAYTRACING:   return 0; /* VK_KHR_ray_tracing not wired */
        case VIO_FEATURE_MULTIVIEW:    return 0; /* VK_KHR_multiview not wired */
        case VIO_FEATURE_READ_PIXELS:  return 0; /* vkCmdCopyImageToBuffer path not wired */
        case VIO_FEATURE_INSTANCED_DRAW: return 1;
        case VIO_FEATURE_RENDER_TARGET:       return 0; /* offscreen pass not wired */
        case VIO_FEATURE_RENDER_TARGET_HDR:   return 0;
        case VIO_FEATURE_RENDER_TARGET_DEPTH: return 0;
        case VIO_FEATURE_RENDER_TARGET_MSAA:  return 0;
        case VIO_FEATURE_CUBEMAP:      return 0;
        case VIO_FEATURE_DEPTH_BIAS:   return 1; /* pipeline rasterization state */
        case VIO_FEATURE_SCISSOR:      return 1;
        case VIO_FEATURE_TEXTURE_SWIZZLE: return 1; /* VkComponentMapping */
        case VIO_FEATURE_NATIVE_2D_BATCH: return 1; /* Vulkan 2D path (shapes/sprites/text) */
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
    .destroy_texture_obj = vulkan_destroy_texture_obj,
    .destroy_font_atlas  = vulkan_destroy_font_atlas,
};

void vio_backend_vulkan_register(void)
{
    vio_register_backend(&vulkan_backend);
}

#endif /* HAVE_VULKAN */
