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
#include "../../vio_render_target.h"
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
    /* All per-image arrays below are calloc'd in create_swapchain, so a
     * partially-built swapchain (cleanup_swapchain called from a create_swapchain
     * failure path, M2) has VK_NULL_HANDLE in the not-yet-created slots; each
     * destroy is guarded on non-NULL and vkDestroy* on NULL is a safe no-op, so
     * this is correct whether the swapchain is fully or partially built. */
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
            if (vio_vk.framebuffers[i]) {
                vkDestroyFramebuffer(vio_vk.device, vio_vk.framebuffers[i], NULL);
            }
        }
        free(vio_vk.framebuffers);
        vio_vk.framebuffers = NULL;
    }

    if (vio_vk.swapchain_image_views) {
        for (uint32_t i = 0; i < vio_vk.swapchain_image_count; i++) {
            if (vio_vk.swapchain_image_views[i]) {
                vkDestroyImageView(vio_vk.device, vio_vk.swapchain_image_views[i], NULL);
            }
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

    /* All per-image arrays are freed and the count's slots no longer exist;
     * zero it so a later cleanup (e.g. shutdown after a failed create) does not
     * iterate a stale count over NULL/freed arrays. create_swapchain re-derives
     * the count via vkGetSwapchainImagesKHR. */
    vio_vk.swapchain_image_count = 0;
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
    /* COLOR_ATTACHMENT for rendering; TRANSFER_SRC so vio_read_pixels can
     * vkCmdCopyImageToBuffer the last-rendered swapchain image (Phase 5).
     * imageUsage must be a subset of caps.supportedUsageFlags (Vulkan spec
     * §"Swapchain"); TRANSFER_SRC is virtually universal on desktop, but guard
     * it so creation never fails on an exotic surface — read_pixels just
     * degrades to unsupported there (and warns at copy time). */
    sc_info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        sc_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
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

    /* Create image views. calloc so a partial loop leaves VK_NULL_HANDLE in the
     * not-yet-created slots, making cleanup_swapchain() safe on a failure path
     * (M2 — all-or-nothing creation). */
    vio_vk.swapchain_image_views = calloc(vio_vk.swapchain_image_count, sizeof(VkImageView));
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
            goto fail_cleanup;
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
        goto fail_cleanup;
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
        goto fail_cleanup;
    }

    /* Create framebuffers (calloc — see image-views note above). */
    vio_vk.framebuffers = calloc(vio_vk.swapchain_image_count, sizeof(VkFramebuffer));
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
            goto fail_cleanup;
        }
    }

    /* Create one render_finished semaphore PER SWAPCHAIN IMAGE (see the field
     * comment in vio_vulkan.h). Sized to swapchain_image_count, which may differ
     * across recreates — cleanup_swapchain() destroys these, so the count is
     * always re-derived here. */
    vio_vk.render_finished_per_image =
        calloc(vio_vk.swapchain_image_count, sizeof(VkSemaphore));
    for (uint32_t i = 0; i < vio_vk.swapchain_image_count; i++) {
        VkSemaphoreCreateInfo sem_info = {0};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(vio_vk.device, &sem_info, NULL,
                              &vio_vk.render_finished_per_image[i]) != VK_SUCCESS) {
            php_error_docref(NULL, E_WARNING, "Failed to create render_finished semaphore %u", i);
            goto fail_cleanup;
        }
    }

    return 0;

fail_cleanup:
    /* M2 — all-or-nothing: tear down everything this call created so far so a
     * partial swapchain is never left behind. cleanup_swapchain() guards every
     * handle on non-NULL and the per-image arrays are calloc'd, so it correctly
     * frees only what was created (including the just-created swapchain handle,
     * image-view/framebuffer/semaphore arrays at whatever fill level, and the
     * depth image/view). Callers (setup_context / recreate) see the -1 and do
     * not proceed with half-built state. */
    cleanup_swapchain();
    return -1;
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
    /* M1 — handle-based teardown, NOT gated on vio_vk.initialized.
     *
     * vio_vk.initialized is set ONLY at the very end of vio_vulkan_setup_context,
     * so a mid-setup failure (e.g. device/VMA/render-pass/swapchain creation
     * fails) returns -1 with initialized==0 while the instance/surface/device/
     * VMA/render-pass it DID create are still live. php_vio.c then calls
     * backend->shutdown() to unwind — if this early-returned on !initialized,
     * all of those would leak (and the validation layers would flag the leaked
     * VkInstance/VkDevice at process exit). Instead we destroy whatever handles
     * are non-NULL, in the correct reverse-dependency order. Every step below is
     * individually guarded, so this is correct for BOTH a fully-initialized
     * teardown and a partial-setup unwind, and is idempotent (memset at the end
     * + the setup_context memset at the start guarantee NULL handles on a second
     * call). If nothing was ever created this is a no-op. */
    if (!vio_vk.instance && !vio_vk.device) return;

    /* Only touch the device/queues if a device exists. */
    if (vio_vk.device) {
        vkDeviceWaitIdle(vio_vk.device);
    }

    /* Sweep any backend textures whose owning PHP object outlived vio_destroy()
     * (the Zend free handlers run during request shutdown, AFTER this). Without
     * this their VkImage/VkImageView/VkSampler would still be alive at
     * vkDestroyDevice — a VUID-vkDestroyDevice-device-05137 validation error.
     * The GPU is already idle, so pass wait=0. The free handlers null their own
     * backend_texture pointers afterwards, but since the device is gone by then
     * GPU objects are gone, so the later vulkan_destroy_texture only frees the
     * heap struct (its release step is a no-op on the now-NULL device).
     * (On a partial-setup unwind these lists are empty — no textures/RTs can
     * have been created before setup completed — but the sweeps are safe.) */
    while (vio_vk.live_textures) {
        vulkan_release_texture_gpu(vio_vk.live_textures, 0);
    }

    /* Sweep any offscreen render targets whose PHP object outlived vio_destroy()
     * for the same reason (see vio_vk.live_render_targets). The device is still
     * alive here, so vulkan_destroy_render_target frees their GPU objects;
     * swap-remove keeps shrinking the list, so destroy index 0 until empty. The
     * later RT free handler then no-ops (device gone) and just frees the cached
     * wrapper struct. */
    while (vio_vk.live_rt_count > 0) {
        vulkan_destroy_render_target(vio_vk.live_render_targets[0]);
    }
    if (vio_vk.live_render_targets) {
        free(vio_vk.live_render_targets);
        vio_vk.live_render_targets = NULL;
        vio_vk.live_rt_capacity = 0;
    }

    /* Frame resources, swapchain, render passes all require the device. On a
     * partial unwind where device creation failed, these were never created
     * (their handles are NULL) and these calls are guarded no-ops. */
    if (vio_vk.device) {
        destroy_frame_resources();
        cleanup_swapchain();

        if (vio_vk.swapchain_resume_render_pass) {
            vkDestroyRenderPass(vio_vk.device, vio_vk.swapchain_resume_render_pass, NULL);
            vio_vk.swapchain_resume_render_pass = VK_NULL_HANDLE;
        }
        if (vio_vk.render_pass) {
            vkDestroyRenderPass(vio_vk.device, vio_vk.render_pass, NULL);
            vio_vk.render_pass = VK_NULL_HANDLE;
        }
    }
    if (vio_vk.vma_allocator) { vio_vma_destroy(vio_vk.vma_allocator); vio_vk.vma_allocator = NULL; }
    if (vio_vk.device) { vkDestroyDevice(vio_vk.device, NULL); vio_vk.device = VK_NULL_HANDLE; }
    if (vio_vk.surface) { vkDestroySurfaceKHR(vio_vk.instance, vio_vk.surface, NULL); vio_vk.surface = VK_NULL_HANDLE; }

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

/* ── Transient one-time-submit command helpers ───────────────────────
 *
 * N1/N4 — factored out of vulkan_create_texture's two near-identical upload
 * blocks. vulkan_begin_transient_commands creates a TRANSIENT command pool +
 * primary command buffer and begins it ONE_TIME_SUBMIT; the caller records its
 * (block-specific) barriers/copies; vulkan_submit_transient_commands ends,
 * submits on a transfer fence, BLOCKS until complete, then destroys the fence
 * and pool (which frees the command buffer). Every VkResult is checked (N4) so
 * a host-OOM cannot silently submit a never-begun / never-ended command buffer.
 *
 * On a begin failure the pool (if any) is destroyed and *out_cmd is NULL. On a
 * submit failure the device is drained (best-effort) and the pool destroyed so
 * nothing is leaked. Both return 0 on success, -1 on failure.
 *
 * Used at vio_texture()/vio_font() time, which is OUTSIDE any frame (no
 * swapchain command buffer is recording), so a full queue stall on the fence is
 * acceptable and keeps lifetimes simple — the upload never touches the per-frame
 * command buffer, so there is no cross-frame hazard. */
static int vulkan_begin_transient_commands(VkCommandPool *out_pool, VkCommandBuffer *out_cmd)
{
    *out_pool = VK_NULL_HANDLE;
    *out_cmd  = VK_NULL_HANDLE;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info = {0};
    pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = vio_vk.graphics_family;
    if (vkCreateCommandPool(vio_vk.device, &pool_info, NULL, &pool) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan: failed to create transient command pool");
        return -1;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmd_alloc = {0};
    cmd_alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool        = pool;
    cmd_alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vio_vk.device, &cmd_alloc, &cmd) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan: failed to allocate transient command buffer");
        vkDestroyCommandPool(vio_vk.device, pool, NULL);
        return -1;
    }

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan: failed to begin transient command buffer");
        vkDestroyCommandPool(vio_vk.device, pool, NULL); /* frees cmd */
        return -1;
    }

    *out_pool = pool;
    *out_cmd  = cmd;
    return 0;
}

static int vulkan_submit_transient_commands(VkCommandPool pool, VkCommandBuffer cmd)
{
    int rc = 0;

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan: failed to end transient command buffer");
        vkDestroyCommandPool(vio_vk.device, pool, NULL);
        return -1;
    }

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(vio_vk.device, &fci, NULL, &fence) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan: failed to create transient fence");
        vkDestroyCommandPool(vio_vk.device, pool, NULL);
        return -1;
    }

    VkSubmitInfo submit = {0};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    if (vkQueueSubmit(vio_vk.graphics_queue, 1, &submit, fence) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan: failed to submit transient commands");
        /* The submit did not take; do NOT wait the (never-signalled) fence.
         * Best-effort drain so the cmd buffer is not in flight, then destroy. */
        vkDeviceWaitIdle(vio_vk.device);
        rc = -1;
    } else {
        vkWaitForFences(vio_vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
    }

    vkDestroyFence(vio_vk.device, fence, NULL);
    vkDestroyCommandPool(vio_vk.device, pool, NULL); /* frees cmd */
    return rc;
}

/* ── Texture creation ────────────────────────────────────────────────
 *
 * Uploads happen at vio_texture()/vio_font() time, which is OUTSIDE any frame
 * (no swapchain command buffer is recording). We therefore use a transient
 * one-time-submit command buffer (vulkan_begin/submit_transient_commands above)
 * to record the layout transitions + buffer-to-image copy, submit, and block on
 * a transfer fence before tearing the staging resources down. This keeps the
 * upload entirely off the per-frame command buffer so there is no cross-frame
 * hazard.
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
     *    transient one-time-submit command buffer. The pool/cmd/fence boilerplate
     *    and all VkResult checks (N4) live in vulkan_begin/submit_transient_commands
     *    (N1 — was two near-identical ~55-line blocks); only the barrier+copy
     *    recording, which genuinely differs between the data and no-data paths,
     *    stays inline here. Behavior (barriers, layouts, stages) is unchanged. */
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

        VkCommandPool   up_pool = VK_NULL_HANDLE;
        VkCommandBuffer up_cmd  = VK_NULL_HANDLE;
        if (vulkan_begin_transient_commands(&up_pool, &up_cmd) != 0) {
            vio_vma_destroy_buffer(vio_vk.vma_allocator, staging, staging_alloc);
            vio_vma_destroy_image(vio_vk.vma_allocator, tex->image, tex->allocation);
            free(tex);
            return NULL;
        }

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

        if (vulkan_submit_transient_commands(up_pool, up_cmd) != 0) {
            vio_vma_destroy_buffer(vio_vk.vma_allocator, staging, staging_alloc);
            vio_vma_destroy_image(vio_vk.vma_allocator, tex->image, tex->allocation);
            free(tex);
            return NULL;
        }
        vio_vma_destroy_buffer(vio_vk.vma_allocator, staging, staging_alloc);
    } else {
        /* No data: still transition UNDEFINED -> SHADER_READ_ONLY so the image
         * is in a samplable layout (sampling it yields garbage, but the layout
         * is valid and the layers stay quiet). */
        VkCommandPool   up_pool = VK_NULL_HANDLE;
        VkCommandBuffer up_cmd  = VK_NULL_HANDLE;
        if (vulkan_begin_transient_commands(&up_pool, &up_cmd) != 0) {
            vio_vma_destroy_image(vio_vk.vma_allocator, tex->image, tex->allocation);
            free(tex);
            return NULL;
        }

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

        if (vulkan_submit_transient_commands(up_pool, up_cmd) != 0) {
            vio_vma_destroy_image(vio_vk.vma_allocator, tex->image, tex->allocation);
            free(tex);
            return NULL;
        }
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

/* ── Offscreen render targets (Phase 3) ───────────────────────────────
 *
 * Each render target owns: a DEVICE_LOCAL color VkImage (B8G8R8A8_UNORM,
 * COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC) + view; an optional depth image+view;
 * a VkRenderPass that is RENDER-PASS-COMPATIBLE with vio_vk.render_pass (same
 * attachment formats and sample counts, same subpass references) so the Phase-1
 * 2D pipelines — built against vio_vk.render_pass — bind unchanged inside the
 * offscreen pass (Vulkan spec §8.2 Render Pass Compatibility: only formats,
 * samples, and references must match; loadOp/storeOp/initial/finalLayout do
 * not). The offscreen pass uses color loadOp=CLEAR, initialLayout=UNDEFINED,
 * finalLayout=SHADER_READ_ONLY_OPTIMAL — so vkCmdEndRenderPass leaves the color
 * image directly samplable with no extra barrier. A VkFramebuffer at the RT
 * extent + a VkSampler complete the set; sampling reuses the per-frame 2D
 * descriptor pool ring (vio_render_target_texture wraps the color view+sampler
 * in a vio_vulkan_texture so the existing vio_2d_flush textured path handles it).
 */

/* Render-pass-compatible offscreen pass. color_format/depth_format MUST equal
 * the swapchain pass's (B8G8R8A8_UNORM + find_depth_format()). */
static VkRenderPass vulkan_create_rt_render_pass(int with_depth)
{
    VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat depth_format = find_depth_format();

    VkAttachmentDescription attachments[2] = {0};
    /* Color: clear at load, store at end, end up SHADER_READ_ONLY so the
     * unbind needs no separate layout barrier before sampling. */
    attachments[0].format         = color_format;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    /* Depth (only referenced when with_depth). */
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
    subpass.pDepthStencilAttachment = with_depth ? &depth_ref : NULL;

    /* RENDER-PASS COMPATIBILITY: this validation layer compares the FULL subpass
     * dependency array (count + every field) under VUID-vkCmdDraw-renderPass-
     * 02684 — not just attachment formats/samples/references. So the offscreen
     * pass's dependency must be BYTE-IDENTICAL to vio_vk.render_pass's single
     * dependency for the shared 2D pipelines (built against render_pass) to bind
     * here. We therefore replicate exactly the swapchain pass's EXTERNAL->0
     * dependency (see create_render_pass). It also correctly orders this pass's
     * loadOp clear / attachment writes after any prior frame's attachment writes;
     * the render-to-texture write->sample visibility (Frame A color store ->
     * Frame B fragment-shader read) is provided by the SHADER_READ_ONLY_OPTIMAL
     * finalLayout transition plus the cross-submit semaphore/fence ordering. */
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
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                      | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info = {0};
    rp_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = with_depth ? 2 : 1;
    rp_info.pAttachments    = attachments;
    rp_info.subpassCount    = 1;
    rp_info.pSubpasses      = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies   = &dep;

    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(vio_vk.device, &rp_info, NULL, &rp) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan: failed to create offscreen render pass");
        return VK_NULL_HANDLE;
    }
    return rp;
}

/* ── Live render-target tracking (mirrors the live-texture sweep) ────── */

void vulkan_rt_track(void *rt)
{
    if (!rt) return;
    /* Avoid double-registration. */
    for (uint32_t i = 0; i < vio_vk.live_rt_count; i++) {
        if (vio_vk.live_render_targets[i] == rt) return;
    }
    if (vio_vk.live_rt_count == vio_vk.live_rt_capacity) {
        uint32_t cap = vio_vk.live_rt_capacity ? vio_vk.live_rt_capacity * 2 : 8;
        void **grown = realloc(vio_vk.live_render_targets, cap * sizeof(void *));
        if (!grown) return; /* tracking is best-effort; OOM here just risks the leak msg */
        vio_vk.live_render_targets = grown;
        vio_vk.live_rt_capacity = cap;
    }
    vio_vk.live_render_targets[vio_vk.live_rt_count++] = rt;
}

void vulkan_rt_untrack(void *rt)
{
    if (!rt || !vio_vk.live_render_targets) return;
    for (uint32_t i = 0; i < vio_vk.live_rt_count; i++) {
        if (vio_vk.live_render_targets[i] == rt) {
            /* Swap-remove. */
            vio_vk.live_render_targets[i] = vio_vk.live_render_targets[vio_vk.live_rt_count - 1];
            vio_vk.live_rt_count--;
            return;
        }
    }
}

int vulkan_create_render_target(void *rt_ptr, int width, int height, int hdr, int depth_only)
{
    (void)hdr; /* HDR offscreen (R16G16B16A16_SFLOAT) is Phase 5; keep UNORM for now. */
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!vio_vk.initialized || !vio_vk.device || width <= 0 || height <= 0) {
        return -1;
    }

    const VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    const int with_depth = !depth_only;

    VkImage      color_image = VK_NULL_HANDLE;
    void        *color_alloc = NULL;
    VkImageView  color_view  = VK_NULL_HANDLE;
    VkImage      depth_image = VK_NULL_HANDLE;
    void        *depth_alloc = NULL;
    VkImageView  depth_view  = VK_NULL_HANDLE;
    VkRenderPass rp          = VK_NULL_HANDLE;
    VkFramebuffer fb         = VK_NULL_HANDLE;
    VkSampler    sampler     = VK_NULL_HANDLE;

    /* 1. Color image: COLOR_ATTACHMENT (render into) | SAMPLED (read back) |
     *    TRANSFER_SRC (readback / blit later). depth_only skips this. */
    if (!depth_only) {
        VkImageCreateInfo ci = {0};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = color_format;
        ci.extent.width  = (uint32_t)width;
        ci.extent.height = (uint32_t)height;
        ci.extent.depth  = 1;
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT
                         | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vio_vma_create_image(vio_vk.vma_allocator, &ci,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 &color_image, &color_alloc) != 0) {
            php_error_docref(NULL, E_WARNING, "Vulkan: RT color image create failed (%dx%d)", width, height);
            goto fail;
        }

        VkImageViewCreateInfo iv = {0};
        iv.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image    = color_image;
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format   = color_format;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv.subresourceRange.levelCount = 1;
        iv.subresourceRange.layerCount = 1;
        if (vkCreateImageView(vio_vk.device, &iv, NULL, &color_view) != VK_SUCCESS) {
            php_error_docref(NULL, E_WARNING, "Vulkan: RT color view create failed");
            goto fail;
        }
    }

    /* 2. Depth image (only when !depth_only — depth_only color-less targets are
     *    a shadow-map case not exercised by the 2D path, but supported here). */
    if (with_depth) {
        VkFormat depth_format = find_depth_format();
        VkImageCreateInfo ci = {0};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = depth_format;
        ci.extent.width  = (uint32_t)width;
        ci.extent.height = (uint32_t)height;
        ci.extent.depth  = 1;
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vio_vma_create_image(vio_vk.vma_allocator, &ci,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                 &depth_image, &depth_alloc) != 0) {
            php_error_docref(NULL, E_WARNING, "Vulkan: RT depth image create failed");
            goto fail;
        }

        VkImageViewCreateInfo iv = {0};
        iv.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image    = depth_image;
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format   = depth_format;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        iv.subresourceRange.levelCount = 1;
        iv.subresourceRange.layerCount = 1;
        if (vkCreateImageView(vio_vk.device, &iv, NULL, &depth_view) != VK_SUCCESS) {
            php_error_docref(NULL, E_WARNING, "Vulkan: RT depth view create failed");
            goto fail;
        }
    }

    /* 3. Render pass (compatible with the swapchain 2D pipelines). */
    rp = vulkan_create_rt_render_pass(with_depth);
    if (rp == VK_NULL_HANDLE) goto fail;

    /* 4. Framebuffer at the RT extent. */
    {
        VkImageView attachments[2];
        uint32_t att_count = 0;
        if (!depth_only) attachments[att_count++] = color_view;
        if (with_depth)  attachments[att_count++] = depth_view;

        VkFramebufferCreateInfo fb_info = {0};
        fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass      = rp;
        fb_info.attachmentCount = att_count;
        fb_info.pAttachments    = attachments;
        fb_info.width           = (uint32_t)width;
        fb_info.height          = (uint32_t)height;
        fb_info.layers          = 1;
        if (vkCreateFramebuffer(vio_vk.device, &fb_info, NULL, &fb) != VK_SUCCESS) {
            php_error_docref(NULL, E_WARNING, "Vulkan: RT framebuffer create failed");
            goto fail;
        }
    }

    /* 5. Sampler for sampling the result (linear/clamp; matches the d3d11/d3d12
     *    color RT sampler choice). Only meaningful for color targets. */
    if (!depth_only) {
        VkSamplerCreateInfo sci = {0};
        sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod       = 0.0f;
        sci.borderColor  = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        if (vkCreateSampler(vio_vk.device, &sci, NULL, &sampler) != VK_SUCCESS) {
            php_error_docref(NULL, E_WARNING, "Vulkan: RT sampler create failed");
            goto fail;
        }
    }

    /* Commit handles onto the RT object. */
    rt->vulkan_color_image = color_image;
    rt->vulkan_color_alloc = color_alloc;
    rt->vulkan_color_view  = color_view;
    rt->vulkan_depth_image = depth_image;
    rt->vulkan_depth_alloc = depth_alloc;
    rt->vulkan_depth_view  = depth_view;
    rt->vulkan_render_pass = rp;
    rt->vulkan_framebuffer = fb;
    rt->vulkan_sampler     = sampler;
    rt->vulkan_color_backend_texture = NULL; /* built lazily by vio_render_target_texture */

    /* Track for the shutdown sweep so the RT's GPU objects are freed before
     * vkDestroyDevice even if the PHP object outlives vio_destroy(). */
    vulkan_rt_track(rt);
    return 0;

fail:
    if (sampler)     vkDestroySampler(vio_vk.device, sampler, NULL);
    if (fb)          vkDestroyFramebuffer(vio_vk.device, fb, NULL);
    if (rp)          vkDestroyRenderPass(vio_vk.device, rp, NULL);
    if (depth_view)  vkDestroyImageView(vio_vk.device, depth_view, NULL);
    if (depth_image) vio_vma_destroy_image(vio_vk.vma_allocator, depth_image, depth_alloc);
    if (color_view)  vkDestroyImageView(vio_vk.device, color_view, NULL);
    if (color_image) vio_vma_destroy_image(vio_vk.vma_allocator, color_image, color_alloc);
    return -1;
}

void vulkan_destroy_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_VULKAN) return;

    /* Remove from the live-RT sweep list (idempotent if already gone, e.g. the
     * shutdown sweep released it and the free handler is now finishing up). */
    vulkan_rt_untrack(rt);

    /* Drop any tracking references first so a later bind/unbind/begin can't
     * dereference freed memory. */
    if (vio_vk.current_bound_rt == rt) vio_vk.current_bound_rt = NULL;
    if (vio_vk.pending_bound_rt == rt) vio_vk.pending_bound_rt = NULL;

    if (!vio_vk.device) {
        /* Device already gone (shutdown raced ahead). Just null the cached
         * wrapper struct so the RT free handler doesn't leak heap memory. */
        if (rt->vulkan_color_backend_texture) {
            free(rt->vulkan_color_backend_texture);
            rt->vulkan_color_backend_texture = NULL;
        }
        rt->vulkan_color_image = rt->vulkan_color_view = NULL;
        rt->vulkan_color_alloc = NULL;
        rt->vulkan_depth_image = rt->vulkan_depth_view = NULL;
        rt->vulkan_depth_alloc = NULL;
        rt->vulkan_render_pass = rt->vulkan_framebuffer = rt->vulkan_sampler = NULL;
        return;
    }

    /* An offscreen frame may still be in flight: a present-skipped warm frame
     * (Phase 4) has no vkQueuePresentKHR to implicitly throttle it, and even a
     * normal frame's just-submitted cmd buffer references this RT's framebuffer/
     * images. Drain the GPU before destroying — releasing in-flight resources is a
     * use-after-free. (Bug #3 of the warm-render class; mirrors d3d12.) */
    vkDeviceWaitIdle(vio_vk.device);

    /* The cached sampling wrapper borrows color_view+sampler (RT-owned), so only
     * the struct itself is freed here. */
    if (rt->vulkan_color_backend_texture) {
        free(rt->vulkan_color_backend_texture);
        rt->vulkan_color_backend_texture = NULL;
    }
    if (rt->vulkan_sampler) {
        vkDestroySampler(vio_vk.device, (VkSampler)rt->vulkan_sampler, NULL);
        rt->vulkan_sampler = NULL;
    }
    if (rt->vulkan_framebuffer) {
        vkDestroyFramebuffer(vio_vk.device, (VkFramebuffer)rt->vulkan_framebuffer, NULL);
        rt->vulkan_framebuffer = NULL;
    }
    if (rt->vulkan_render_pass) {
        vkDestroyRenderPass(vio_vk.device, (VkRenderPass)rt->vulkan_render_pass, NULL);
        rt->vulkan_render_pass = NULL;
    }
    if (rt->vulkan_color_view) {
        vkDestroyImageView(vio_vk.device, (VkImageView)rt->vulkan_color_view, NULL);
        rt->vulkan_color_view = NULL;
    }
    if (rt->vulkan_color_image) {
        vio_vma_destroy_image(vio_vk.vma_allocator, (VkImage)rt->vulkan_color_image, rt->vulkan_color_alloc);
        rt->vulkan_color_image = NULL;
        rt->vulkan_color_alloc = NULL;
    }
    if (rt->vulkan_depth_view) {
        vkDestroyImageView(vio_vk.device, (VkImageView)rt->vulkan_depth_view, NULL);
        rt->vulkan_depth_view = NULL;
    }
    if (rt->vulkan_depth_image) {
        vio_vma_destroy_image(vio_vk.vma_allocator, (VkImage)rt->vulkan_depth_image, rt->vulkan_depth_alloc);
        rt->vulkan_depth_image = NULL;
        rt->vulkan_depth_alloc = NULL;
    }
}

/* Begin the offscreen RT pass (loadOp=CLEAR) on the open frame command buffer and
 * set the RT-extent viewport/scissor + vio_vk.current_bound_rt. NO vkCmdEndRenderPass
 * here — the caller is responsible for ensuring no pass is currently open (the
 * offscreen-only frame, which never began the swapchain pass) OR for having ended
 * the prior pass itself (the mid-frame switch in vulkan_record_bind_render_target).
 * Caller has already validated rt + vio_vk.in_frame. */
static void vulkan_record_begin_offscreen_pass(VkCommandBuffer cmd,
                                                vio_render_target_object *rt)
{
    /* Begin the offscreen pass (loadOp=CLEAR clears the color/depth). */
    VkClearValue clears[2];
    clears[0].color.float32[0] = vio_vk.clear_r;
    clears[0].color.float32[1] = vio_vk.clear_g;
    clears[0].color.float32[2] = vio_vk.clear_b;
    clears[0].color.float32[3] = vio_vk.clear_a;
    clears[1].depthStencil.depth   = 1.0f;
    clears[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rp_begin = {0};
    rp_begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass        = (VkRenderPass)rt->vulkan_render_pass;
    rp_begin.framebuffer       = (VkFramebuffer)rt->vulkan_framebuffer;
    rp_begin.renderArea.offset = (VkOffset2D){0, 0};
    rp_begin.renderArea.extent = (VkExtent2D){ (uint32_t)rt->width, (uint32_t)rt->height };
    /* depth_only RTs have a single (depth) clear; color RTs clear color+depth. */
    rp_begin.clearValueCount   = rt->depth_only ? 1 : 2;
    rp_begin.pClearValues      = clears;
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    /* Viewport + scissor to the RT extent. */
    VkViewport vp = {0};
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width  = (float)rt->width;
    vp.height = (float)rt->height;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc = {0};
    sc.offset = (VkOffset2D){0, 0};
    sc.extent = (VkExtent2D){ (uint32_t)rt->width, (uint32_t)rt->height };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vio_vk.current_bound_rt = rt;
}

void vulkan_record_bind_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_VULKAN) return;
    if (!vio_vk.in_frame || !rt->vulkan_render_pass || !rt->vulkan_framebuffer) return;

    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;

    /* End whatever pass is currently open (the swapchain pass from begin_frame,
     * or — if a prior bind already switched — that offscreen pass). Vulkan
     * cannot switch render passes without ending the active one first. */
    vkCmdEndRenderPass(cmd);

    vulkan_record_begin_offscreen_pass(cmd, rt);
}

/* Phase 4 — offscreen-only frame: begin the offscreen pass with NO preceding
 * vkCmdEndRenderPass, because vulkan_begin_frame opened the command buffer but did
 * NOT begin the swapchain pass (frame_is_offscreen==1, the warm-render bind-then-
 * begin order). Driven from the deferred-bind block in vio_begin(). */
void vulkan_begin_offscreen_render_pass(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_VULKAN) return;
    if (!vio_vk.in_frame || !rt->vulkan_render_pass || !rt->vulkan_framebuffer) return;

    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    vulkan_record_begin_offscreen_pass(cmd, rt);
}

/* Lazily create the loadOp=LOAD swapchain resume pass. Compatible with
 * vio_vk.render_pass (same formats/samples), so the framebuffers built for
 * render_pass are usable with it (framebuffer/render-pass compatibility follows
 * the same §8.2 rule). */
static VkRenderPass vulkan_get_swapchain_resume_pass(void)
{
    if (vio_vk.swapchain_resume_render_pass) return vio_vk.swapchain_resume_render_pass;

    VkFormat depth_format = find_depth_format();

    VkAttachmentDescription attachments[2] = {0};
    /* Color: LOAD existing contents, keep them, end up PRESENT_SRC_KHR. The
     * primary pass left the image in PRESENT_SRC_KHR (its finalLayout), so this
     * pass's initialLayout matches — no implicit transition wipes the contents. */
    attachments[0].format         = vio_vk.swapchain_format;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    /* Depth: the primary pass left it DEPTH_STENCIL_ATTACHMENT_OPTIMAL. LOAD it
     * (contents are don't-care for the 2D path, which has depth test off, but
     * LOAD + matching initialLayout avoids a clear and keeps the layout valid). */
    attachments[1].format         = depth_format;
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    /* RENDER-PASS COMPATIBILITY: like the offscreen pass, the resume pass binds
     * the same 2D pipelines (built against vio_vk.render_pass), and this layer
     * compares the full dependency array for compatibility. Replicate the
     * swapchain pass's EXTERNAL->0 dependency BYTE-IDENTICALLY (see
     * create_render_pass). It also orders this resume pass's color writes after
     * the just-ended offscreen pass's attachment writes. */
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
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                      | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info = {0};
    rp_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 2;
    rp_info.pAttachments    = attachments;
    rp_info.subpassCount    = 1;
    rp_info.pSubpasses      = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies   = &dep;

    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(vio_vk.device, &rp_info, NULL, &rp) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan: failed to create swapchain resume render pass");
        return VK_NULL_HANDLE;
    }
    vio_vk.swapchain_resume_render_pass = rp;
    return rp;
}

void vulkan_record_unbind_render_target(void)
{
    if (!vio_vk.in_frame) return;
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;

    /* End the offscreen pass. Its color finalLayout=SHADER_READ_ONLY_OPTIMAL is
     * applied here, so the color image is immediately samplable — no extra
     * barrier needed before vio_render_target_texture binds it. */
    vkCmdEndRenderPass(cmd);
    vio_vk.current_bound_rt = NULL;

    /* Re-open the swapchain pass with loadOp=LOAD so prior swapchain draws (if
     * any) survive and subsequent draws composite on top. */
    VkRenderPass resume = vulkan_get_swapchain_resume_pass();
    if (resume == VK_NULL_HANDLE) return;

    VkRenderPassBeginInfo rp_begin = {0};
    rp_begin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass        = resume;
    rp_begin.framebuffer       = vio_vk.framebuffers[vio_vk.current_image_index];
    rp_begin.renderArea.offset = (VkOffset2D){0, 0};
    rp_begin.renderArea.extent = vio_vk.swapchain_extent;
    rp_begin.clearValueCount   = 0;   /* loadOp=LOAD: no clear values consumed */
    rp_begin.pClearValues      = NULL;
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    /* Restore swapchain viewport/scissor. */
    VkViewport vp = {0};
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width  = (float)vio_vk.swapchain_extent.width;
    vp.height = (float)vio_vk.swapchain_extent.height;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc = {0};
    sc.offset = (VkOffset2D){0, 0};
    sc.extent = vio_vk.swapchain_extent;
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

static void *vulkan_compile_shader(vio_shader_desc *desc) { (void)desc; return NULL; }
static void vulkan_destroy_shader(void *shader) { (void)shader; }

static void vulkan_begin_frame(void)
{
    if (!vio_vk.initialized) return;

    vio_vk_frame *f = &vio_vk.frames[vio_vk.current_frame];

    /* Phase 4 — OFFSCREEN-ONLY frame detection. vio_vk.pending_bound_rt is set ONLY
     * by an out-of-frame vio_bind_render_target (the warm-render "bind BEFORE
     * vio_begin" order), and vio_begin calls begin_frame() before applying that
     * pending bind. So a non-NULL pending_bound_rt here means this whole frame
     * renders to an offscreen target and is never presented.
     *
     * For such a frame we MUST skip vkAcquireNextImageKHR: present is also skipped
     * (vulkan_present), and acquiring without ever presenting would, after a few
     * frames, exhaust the swapchain's images and make vkAcquireNextImageKHR(...,
     * UINT64_MAX) block forever. We therefore acquire NOTHING, begin NO swapchain
     * pass, and let the deferred-bind block in vio_begin open the offscreen pass.
     *
     * The normal path below (frame_is_offscreen==0) is byte-for-byte the
     * pre-Phase-4 flow. */
    int offscreen = (vio_vk.pending_bound_rt != NULL);
    vio_vk.frame_is_offscreen = offscreen;
    /* B1 — assume not presentable until we successfully open the normal
     * swapchain path below. An offscreen-only frame, or an aborted acquire,
     * leaves this 0 so vulkan_present skips. */
    vio_vk.frame_presentable = 0;

    /* Wait for this frame's previous work to finish */
    vkWaitForFences(vio_vk.device, 1, &f->in_flight, VK_TRUE, UINT64_MAX);

    /* Reset this frame's 2D descriptor pool. SAFE only because the fence wait
     * above guarantees the GPU has finished consuming the descriptor sets this
     * pool handed out two frames ago — resetting an in-flight pool would be a
     * use-after-free the sync-validation layer flags. No-op until the 2D Vulkan
     * state is initialised. */
    vio_2d_vulkan_reset_frame_descriptors(vio_vk.current_frame);

    if (offscreen) {
        /* OFFSCREEN-ONLY: no acquire, no swapchain pass. Reset+begin the command
         * buffer (same as the normal path) so the offscreen pass + 2D draws can
         * record onto it, and reset the in_flight fence (the end-of-frame submit
         * still signals it). current_image_index is intentionally NOT touched —
         * no swapchain image participates in this frame. */
        vkResetFences(vio_vk.device, 1, &f->in_flight);
        vkResetCommandBuffer(f->cmd_buf, 0);

        VkCommandBufferBeginInfo begin_info = {0};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(f->cmd_buf, &begin_info);

        /* No render pass is begun here; vio_begin's deferred-bind block calls
         * vulkan_begin_offscreen_render_pass() to open the offscreen pass (which
         * also sets the RT-extent viewport/scissor). */
        vio_vk.in_frame = 1;
        return;
    }

    /* Acquire next swapchain image */
    VkResult result = vkAcquireNextImageKHR(vio_vk.device, vio_vk.swapchain, UINT64_MAX,
                                             f->image_available, VK_NULL_HANDLE,
                                             &vio_vk.current_image_index);

    /* B1 — handle ALL non-SUCCESS acquire results, not just OUT_OF_DATE.
     *
     * OUT_OF_DATE / error: the swapchain is unusable and (for OUT_OF_DATE) no
     * image was acquired and image_available was NOT signalled. We must NOT
     * fall through to begin a command buffer / submit a wait on an unsignalled
     * semaphore — that is the deadlock B1 describes. Recreate (best-effort) and
     * leave a CLEAN not-in-frame state: in_frame stays 0, frame_is_offscreen is
     * reset to 0 (it was latched above off pending_bound_rt and must not leak
     * into the next frame's offscreen detection), frame_presentable stays 0.
     * vio_end's end_frame() + present() then both early-out on these guards and
     * the frame is cleanly skipped (the recreate already happened).
     *
     * SUBOPTIMAL_KHR: an image WAS acquired and image_available WAS signalled —
     * the swapchain is just not optimally configured. It is legal to keep
     * rendering with it this frame; flag it for recreation after present so the
     * mismatch is corrected without dropping a frame mid-flight.
     *
     * Note vkAcquireNextImageKHR never returns VK_TIMEOUT/VK_NOT_READY here
     * because the timeout is UINT64_MAX (a blocking wait); any other value is a
     * hard error and is treated like OUT_OF_DATE (skip cleanly). */
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        vio_vk.frame_is_offscreen = 0;
        /* M2 — recreate is fallible; on failure flag a retry so we don't leave
         * half-built state and so the next frame attempts recreation again. */
        if (vio_vulkan_recreate_swapchain() != 0) {
            php_error_docref(NULL, E_WARNING,
                "Vulkan: swapchain recreation failed at acquire; will retry next frame");
            vio_vk.swapchain_needs_recreate = 1;
        }
        return;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        /* Hard error from acquire (e.g. device lost, surface lost, OOM). The
         * image_available semaphore is in an unknown/unsignalled state, so the
         * frame cannot be safely submitted. Skip cleanly; let the next frame
         * retry via the recreate flag. */
        php_error_docref(NULL, E_WARNING,
            "Vulkan: vkAcquireNextImageKHR failed (VkResult %d); skipping frame", (int)result);
        vio_vk.frame_is_offscreen = 0;
        vio_vk.swapchain_needs_recreate = 1;
        return;
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        /* Image acquired and image_available signalled — proceed with this
         * frame, but recreate afterwards (present consumes the flag). */
        vio_vk.swapchain_needs_recreate = 1;
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
    /* B1 — a normal swapchain frame is now fully opened (image acquired, command
     * buffer begun, swapchain pass started): it is presentable. */
    vio_vk.frame_presentable = 1;
}

static void vulkan_end_frame(void)
{
    if (!vio_vk.initialized) return;

    /* B1 — vio_end (php_vio.c) calls end_frame() then present() guarded only on
     * the PHP-side ctx->in_frame, which vio_begin sets to 1 regardless of what
     * begin_frame actually did. If begin_frame aborted (e.g. acquire returned
     * OUT_OF_DATE and it recreated the swapchain without opening a pass), no
     * command buffer was begun and no swapchain image was acquired this frame.
     * Running the normal end_frame path here would vkCmdEndRenderPass /
     * vkEndCommandBuffer a buffer that was never begun and submit a wait on an
     * un-signalled image_available semaphore — a guaranteed deadlock / device
     * loss. Mirror the same in_frame guard the PHP layer relies on: if this
     * frame was never opened, there is nothing to end. */
    if (!vio_vk.in_frame) return;

    vio_vk_frame *f = &vio_vk.frames[vio_vk.current_frame];

    if (vio_vk.frame_is_offscreen) {
        /* Phase 4 — OFFSCREEN-ONLY frame: no swapchain image was acquired and the
         * frame will not be presented, so NEITHER the per-frame image_available
         * NOR the per-image render_finished semaphore participates. Submitting with
         * waitSemaphoreCount=0 / signalSemaphoreCount=0 keeps both swapchain
         * semaphores out of this frame entirely — there is no acquire to wait on
         * and no present to signal for. (Signalling render_finished here without a
         * matching present, or waiting image_available without an acquire, is
         * exactly the binary-semaphore-desync hazard the sync layer flags.)
         *
         * The only open pass is the offscreen pass (begun in vio_begin's deferred
         * block; the warm unbind happens AFTER vio_end so it never ran yet). End
         * it only if it was actually opened (current_bound_rt set) — a deferred
         * bind that no-op'd on an invalid RT would leave no pass open. */
        if (vio_vk.current_bound_rt) {
            vkCmdEndRenderPass(f->cmd_buf);
        }
        vkEndCommandBuffer(f->cmd_buf);

        VkSubmitInfo submit = {0};
        submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount   = 0;
        submit.pWaitSemaphores      = NULL;
        submit.pWaitDstStageMask    = NULL;
        submit.commandBufferCount   = 1;
        submit.pCommandBuffers      = &f->cmd_buf;
        submit.signalSemaphoreCount = 0;
        submit.pSignalSemaphores    = NULL;

        /* Submit on the in_flight fence: vulkan_begin_frame waits it before reusing
         * this frame's command buffer / descriptor pool / VBO slice, and
         * vulkan_destroy_render_target's vkDeviceWaitIdle (4c) also gates on it. */
        vkQueueSubmit(vio_vk.graphics_queue, 1, &submit, f->in_flight);

        vio_vk.in_frame = 0;
        return;
    }

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

    /* B1 — vulkan_end_frame clears in_frame before present runs, so present
     * cannot key off in_frame; it keys off frame_presentable instead. An
     * aborted begin_frame (acquire returned OUT_OF_DATE/error and the swapchain
     * was recreated without opening a pass) leaves frame_presentable=0 and
     * frame_is_offscreen=0 — and end_frame early-returned, so NO submit and NO
     * fence reset happened this frame. Present must therefore do NOTHING: no
     * vkQueuePresentKHR (current_image_index is stale / no semaphore was
     * signalled) and NO current_frame advance (the in_flight fence was not
     * submitted, so the ring must not rotate onto an out-of-sync slot). The
     * recreate already happened in begin_frame; the next frame retries cleanly. */
    if (!vio_vk.frame_presentable && !vio_vk.frame_is_offscreen) {
        return;
    }

    if (vio_vk.frame_is_offscreen) {
        /* Phase 4 — OFFSCREEN-ONLY frame: nothing was acquired and the swapchain
         * image was never drawn, so presenting it would flip an undrawn/cleared
         * backbuffer (the splash "flash"). Skip vkQueuePresentKHR and the swapchain
         * entirely. We still advance current_frame so the frame-in-flight ring
         * rotates (the in_flight fence was submitted in vulkan_end_frame), and clear
         * frame_is_offscreen so a subsequent NORMAL frame takes the present path. */
        vio_vk.frame_is_offscreen = 0;
        vio_vk.current_frame = (vio_vk.current_frame + 1) % VIO_VK_MAX_FRAMES_IN_FLIGHT;
        return;
    }

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

    /* This frame is done; clear presentable so a subsequent aborted frame's
     * present() (which early-outs above) is not mistaken for presentable. */
    vio_vk.frame_presentable = 0;

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || vio_vk.swapchain_needs_recreate) {
        vio_vk.swapchain_needs_recreate = 0;
        /* M2 — recreate is fallible; on failure leave swapchain_needs_recreate=1
         * so the next frame retries rather than rendering against half-built
         * state. create_swapchain() is all-or-nothing (cleans up on any partial
         * failure), so a failed recreate leaves NO swapchain at all and the next
         * begin_frame's acquire returns OUT_OF_DATE -> recreate again. */
        if (vio_vulkan_recreate_swapchain() != 0) {
            php_error_docref(NULL, E_WARNING,
                "Vulkan: swapchain recreation failed after present; will retry next frame");
            vio_vk.swapchain_needs_recreate = 1;
        }
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

/* Phase 5 — CPU readback of swapchain content. See the header comment for the
 * full contract. NOTE: unlike D3D12 (which reads its last_presented buffer
 * directly), this RE-ACQUIRES a swapchain image via vkAcquireNextImageKHR and
 * reads that — required for sync correctness (the just-presented image is owned
 * by the presentation engine, which vkDeviceWaitIdle does NOT synchronize; the
 * re-acquire establishes the present->acquire->copy dependency the sync layer
 * needs). The re-acquired buffer holds the most-recent render of that image
 * (the swapchain never clears presented images); under FIFO/vsync with a stable
 * scene that is the just-presented content. It then copies into a HOST_VISIBLE
 * buffer honoring row stride and writes TOP-DOWN RGBA8. The output byte order
 * matches D3D12's R8G8B8A8_UNORM readback (golden-compare parity); since the
 * swapchain is B8G8R8A8_UNORM we swap the B/R bytes per pixel. */
int vulkan_read_pixels(int width, int height, void *out_rgba)
{
    if (!vio_vk.initialized || !vio_vk.device || !out_rgba) return -1;
    if (vio_vk.swapchain_image_count == 0 || !vio_vk.swapchain_images) return -1;

    /* The swapchain must have been created with TRANSFER_SRC (added in
     * create_swapchain when caps allow it); without it vkCmdCopyImageToBuffer
     * is invalid. Bail cleanly rather than tripping the validation layer. */
    if (!(vio_vk.swapchain_format == VK_FORMAT_B8G8R8A8_UNORM ||
          vio_vk.swapchain_format == VK_FORMAT_R8G8B8A8_UNORM ||
          vio_vk.swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ||
          vio_vk.swapchain_format == VK_FORMAT_R8G8B8A8_SRGB)) {
        php_error_docref(NULL, E_WARNING,
            "vio_read_pixels: unsupported swapchain format %d for readback",
            (int)vio_vk.swapchain_format);
        return -1;
    }

    /* Ensure all rendering (the just-submitted frame) has completed on the GPU
     * queue so the source image holds the final, fully-rendered pixels. */
    vkDeviceWaitIdle(vio_vk.device);

    /* Re-acquire a swapchain image to read from. This is REQUIRED for sync
     * correctness, not just convenience: after vio_end the just-rendered image
     * is in PRESENT_SRC_KHR and was last touched by vkQueuePresentKHR. The
     * presentation engine's read of that image is NOT synchronized by
     * vkDeviceWaitIdle (which only drains the device QUEUES, not the present
     * engine), so transitioning it for a copy directly would be a
     * WRITE_AFTER_PRESENT hazard (flagged by synchronization validation).
     * vkAcquireNextImageKHR signals a semaphore once the presentation engine has
     * released an image; waiting that semaphore in the readback submit
     * establishes the present -> acquire -> copy dependency. The acquired image
     * holds the most recent render of that buffer (the swapchain never clears
     * presented images); with vsync/FIFO and a stable scene this is the same
     * content as the just-presented frame — the Vulkan analog of D3D12's
     * last_presented_frame_idx read. We MUST re-present the acquired image
     * afterwards (an acquire without a matching present leaks acquired images
     * and eventually hangs future acquires — the Phase 4 lesson). */
    const uint32_t sw = vio_vk.swapchain_extent.width;
    const uint32_t sh = vio_vk.swapchain_extent.height;
    if (sw == 0 || sh == 0) return -1;

    VkSemaphore acq_sem = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo sem_info = {0};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(vio_vk.device, &sem_info, NULL, &acq_sem);

    VkFence acq_fence = VK_NULL_HANDLE;
    VkFenceCreateInfo afci = {0};
    afci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(vio_vk.device, &afci, NULL, &acq_fence);

    uint32_t acq_idx = 0;
    VkResult ar = vkAcquireNextImageKHR(vio_vk.device, vio_vk.swapchain, UINT64_MAX,
                                        acq_sem, acq_fence, &acq_idx);
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        /* OUT_OF_DATE (e.g. mid-resize) or error: nothing safe to read. */
        vkDestroyFence(vio_vk.device, acq_fence, NULL);
        vkDestroySemaphore(vio_vk.device, acq_sem, NULL);
        php_error_docref(NULL, E_WARNING,
            "vio_read_pixels: vkAcquireNextImageKHR failed (VkResult %d)", (int)ar);
        return -1;
    }
    /* Wait the acquire fence so the presentation engine has demonstrably
     * released this image before we touch it on the host/device side. */
    vkWaitForFences(vio_vk.device, 1, &acq_fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(vio_vk.device, acq_fence, NULL);

    if (acq_idx >= vio_vk.swapchain_image_count) acq_idx = 0;
    VkImage src_image = vio_vk.swapchain_images[acq_idx];
    /* Keep current_image_index consistent with the image we now own (acquired
     * but not yet re-presented); the re-present at the end uses it. */
    vio_vk.current_image_index = acq_idx;

    /* Tightly-packed readback buffer: vkCmdCopyImageToBuffer with
     * bufferRowLength=0 packs rows at width*4 with no padding (unlike D3D12's
     * 256-byte-aligned footprint), so the row stride here is exactly sw*4. */
    const VkDeviceSize row_bytes  = (VkDeviceSize)sw * 4;
    const VkDeviceSize buf_bytes  = row_bytes * sh;

    VkBuffer       rb_buf   = VK_NULL_HANDLE;
    void          *rb_alloc = NULL;
    if (vio_vma_create_buffer(vio_vk.vma_allocator, buf_bytes,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &rb_buf, &rb_alloc) != 0 || !rb_buf) {
        php_error_docref(NULL, E_WARNING,
            "vio_read_pixels: failed to create Vulkan readback buffer");
        return -1;
    }

    /* Transient one-time-submit command buffer (mirrors vulkan_create_texture).
     * Does not touch the frame command buffer. */
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info = {0};
    pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = vio_vk.graphics_family;
    if (vkCreateCommandPool(vio_vk.device, &pool_info, NULL, &pool) != VK_SUCCESS) {
        vio_vma_destroy_buffer(vio_vk.vma_allocator, rb_buf, rb_alloc);
        return -1;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmd_alloc = {0};
    cmd_alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool        = pool;
    cmd_alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(vio_vk.device, &cmd_alloc, &cmd);

    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    /* PRESENT_SRC_KHR -> TRANSFER_SRC_OPTIMAL. The image was left in
     * PRESENT_SRC_KHR by the swapchain render pass's finalLayout. */
    VkImageMemoryBarrier to_src = {0};
    to_src.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image               = src_image;
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;
    to_src.srcAccessMask       = 0;
    to_src.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_src);

    VkBufferImageCopy copy = {0};
    copy.bufferOffset      = 0;
    copy.bufferRowLength   = 0;  /* tightly packed -> row stride = width*4 */
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width  = sw;
    copy.imageExtent.height = sh;
    copy.imageExtent.depth  = 1;
    vkCmdCopyImageToBuffer(cmd, src_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rb_buf, 1, &copy);

    /* TRANSFER_SRC_OPTIMAL -> PRESENT_SRC_KHR so the image is back in a
     * presentable layout (the next acquire+pass would otherwise transition from
     * an unexpected layout). */
    VkImageMemoryBarrier to_present = {0};
    to_present.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_present.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_present.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image               = src_image;
    to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_present.subresourceRange.levelCount = 1;
    to_present.subresourceRange.layerCount = 1;
    to_present.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    to_present.dstAccessMask       = 0;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &to_present);

    vkEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(vio_vk.device, &fci, NULL, &fence);

    /* Semaphore signalled by the readback submit and waited by the re-present,
     * so the presentation engine does not read the image until the copy + the
     * transition back to PRESENT_SRC_KHR have completed. */
    VkSemaphore done_sem = VK_NULL_HANDLE;
    vkCreateSemaphore(vio_vk.device, &sem_info, NULL, &done_sem);

    /* Wait the acquire semaphore at TRANSFER (the stage of our first barrier +
     * copy): the present -> acquire -> copy dependency that resolves the
     * WRITE_AFTER_PRESENT hazard. */
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {0};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = &acq_sem;
    submit.pWaitDstStageMask    = &wait_stage;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &done_sem;
    vkQueueSubmit(vio_vk.graphics_queue, 1, &submit, fence);
    vkWaitForFences(vio_vk.device, 1, &fence, VK_TRUE, UINT64_MAX);

    /* Map + copy out. The swapchain is B8G8R8A8_UNORM, so the buffer holds
     * B,G,R,A per pixel; D3D12's readback is R8G8B8A8_UNORM (R,G,B,A). Swap the
     * B/R bytes (index 0 <-> 2) so the output is R,G,B,A — identical to D3D12.
     * If the surface fell back to an R8G8B8A8 format the order already matches,
     * so we copy straight through. The copy is already top-down: image row 0 is
     * the top of the framebuffer and the buffer received rows in order. */
    void *mapped = vio_vma_map(vio_vk.vma_allocator, rb_alloc);
    int rc = 0;
    if (!mapped) {
        php_error_docref(NULL, E_WARNING, "vio_read_pixels: failed to map Vulkan readback buffer");
        rc = -1;
    } else {
        const int swizzle_br = (vio_vk.swapchain_format == VK_FORMAT_B8G8R8A8_UNORM ||
                                vio_vk.swapchain_format == VK_FORMAT_B8G8R8A8_SRGB);
        const unsigned char *srcp = (const unsigned char *)mapped;
        unsigned char *dst = (unsigned char *)out_rgba;

        /* Honor the caller's expected dimensions: only write the overlapping
         * region so a width/height mismatch with the actual swapchain extent
         * never overruns out_rgba (sized width*height*4 by the caller). */
        const uint32_t copy_w = ((uint32_t)width  < sw) ? (uint32_t)width  : sw;
        const uint32_t copy_h = ((uint32_t)height < sh) ? (uint32_t)height : sh;
        const size_t   dst_stride = (size_t)width * 4;

        for (uint32_t y = 0; y < copy_h; y++) {
            const unsigned char *srow = srcp + (size_t)y * row_bytes;
            unsigned char       *drow = dst  + (size_t)y * dst_stride;
            if (swizzle_br) {
                for (uint32_t x = 0; x < copy_w; x++) {
                    const unsigned char *sp = srow + (size_t)x * 4;
                    unsigned char       *dp = drow + (size_t)x * 4;
                    dp[0] = sp[2]; /* R <- B */
                    dp[1] = sp[1]; /* G       */
                    dp[2] = sp[0]; /* B <- R */
                    dp[3] = sp[3]; /* A       */
                }
            } else {
                memcpy(drow, srow, (size_t)copy_w * 4);
            }
        }
        vio_vma_unmap(vio_vk.vma_allocator, rb_alloc);
    }

    /* Re-present the acquired image to keep the acquire/present balance (every
     * vkAcquireNextImageKHR must be matched by a present or the swapchain
     * eventually has no acquirable images and the next acquire blocks forever).
     * The image was transitioned back to PRESENT_SRC_KHR by the second barrier;
     * the present waits on done_sem so it does not race the copy/transition.
     * The image content is unchanged by the copy, so re-presenting it shows the
     * same frame again — visually a no-op. */
    VkPresentInfoKHR present_info = {0};
    present_info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores    = &done_sem;
    present_info.swapchainCount     = 1;
    present_info.pSwapchains        = &vio_vk.swapchain;
    present_info.pImageIndices      = &acq_idx;
    VkResult pr = vkQueuePresentKHR(vio_vk.present_queue, &present_info);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        vio_vk.swapchain_needs_recreate = 1;
    }
    /* Make sure the present has been consumed before destroying done_sem /
     * acq_sem (a semaphore destroyed while a pending present still references it
     * is a use-after-free). vkDeviceWaitIdle does not wait on the present
     * engine, but vkQueueWaitIdle on the present queue drains the queued present
     * operation's semaphore wait. */
    vkQueueWaitIdle(vio_vk.present_queue);

    vkDestroySemaphore(vio_vk.device, done_sem, NULL);
    vkDestroySemaphore(vio_vk.device, acq_sem, NULL);
    vkDestroyFence(vio_vk.device, fence, NULL);
    vkDestroyCommandPool(vio_vk.device, pool, NULL); /* frees cmd */
    vio_vma_destroy_buffer(vio_vk.vma_allocator, rb_buf, rb_alloc);

    return rc;
}

static int vulkan_supports_feature(vio_feature feature)
{
    switch (feature) {
        case VIO_FEATURE_COMPUTE:      return 1;
        case VIO_FEATURE_TESSELLATION: return 1;
        case VIO_FEATURE_GEOMETRY:     return 1;
        case VIO_FEATURE_3D_PIPELINE:  return 1;
        case VIO_FEATURE_RAYTRACING:   return 0; /* VK_KHR_ray_tracing not wired */
        case VIO_FEATURE_MULTIVIEW:    return 0; /* VK_KHR_multiview not wired */
        case VIO_FEATURE_READ_PIXELS:  return 1; /* vkCmdCopyImageToBuffer readback of a RE-ACQUIRED swapchain image (see vulkan_read_pixels); requires the swapchain's TRANSFER_SRC usage added in create_swapchain */
        case VIO_FEATURE_INSTANCED_DRAW: return 1;
        case VIO_FEATURE_RENDER_TARGET:       return 1; /* offscreen RT + render-to-texture (Phase 3) */
        case VIO_FEATURE_RENDER_TARGET_HDR:   return 0; /* R16G16B16A16_SFLOAT offscreen not wired (HDR deferred) */
        case VIO_FEATURE_RENDER_TARGET_DEPTH: return 0; /* depth-RT sampling descriptor not wired */
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
    /* Offscreen render targets (Phase 3). create_render_target is invoked
     * directly from the HAVE_VULKAN branch of ZEND_FUNCTION(vio_render_target)
     * (the create dispatch gates the vtable call by backend name, like d3d12);
     * destroy_render_target is the one the RT free handler calls via
     * rt->backend->destroy_render_target. The mid-frame bind/unbind recording is
     * driven from php_vio.c via vulkan_record_bind/unbind_render_target() (the
     * in_frame-vs-pending decision lives there), so the bind/unbind vtable slots
     * are intentionally left NULL — same as d3d11/d3d12. */
    .create_render_target  = vulkan_create_render_target,
    .destroy_render_target = vulkan_destroy_render_target,
};

void vio_backend_vulkan_register(void)
{
    vio_register_backend(&vulkan_backend);
}

#endif /* HAVE_VULKAN */
