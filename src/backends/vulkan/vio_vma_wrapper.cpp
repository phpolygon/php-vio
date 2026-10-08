/*
 * php-vio - VMA wrapper functions (C++ translation unit)
 * Provides C-callable interface to Vulkan Memory Allocator
 *
 * This file is compiled separately via Makefile.frag
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_VULKAN

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"

extern "C" {

int vio_vma_create(VkInstance instance, VkPhysicalDevice phys, VkDevice device, void **out_allocator)
{
    VmaVulkanFunctions funcs = {};
    funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    funcs.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ci = {};
    ci.instance        = instance;
    ci.physicalDevice  = phys;
    ci.device          = device;
    ci.vulkanApiVersion = VK_API_VERSION_1_0;
    ci.pVulkanFunctions = &funcs;

    VmaAllocator alloc;
    VkResult res = vmaCreateAllocator(&ci, &alloc);
    if (res != VK_SUCCESS) return -1;

    *out_allocator = (void *)alloc;
    return 0;
}

void vio_vma_destroy(void *allocator)
{
    if (allocator) {
        vmaDestroyAllocator((VmaAllocator)allocator);
    }
}

int vio_vma_create_buffer(void *allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags mem_props,
                           VkBuffer *out_buffer, void **out_allocation)
{
    VkBufferCreateInfo buf_info = {};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size  = size;
    buf_info.usage = usage;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.requiredFlags = mem_props;
    if (mem_props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        /* A buffer that is only ever a copy destination is a readback buffer:
         * the CPU reads it, so it must be cached host memory. With
         * SEQUENTIAL_WRITE VMA picks write-combined memory (on NVIDIA with
         * ReBAR the device-local BAR), where CPU reads are uncached - a
         * 256x256 vio_read_pixels took 20 ms instead of well under one. */
        alloc_info.flags = usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT
                         ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                         : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else {
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    VmaAllocation alloc;
    VkResult res = vmaCreateBuffer((VmaAllocator)allocator, &buf_info, &alloc_info, out_buffer, &alloc, nullptr);
    if (res != VK_SUCCESS) return -1;

    *out_allocation = (void *)alloc;
    return 0;
}

void vio_vma_destroy_buffer(void *allocator, VkBuffer buffer, void *allocation)
{
    vmaDestroyBuffer((VmaAllocator)allocator, buffer, (VmaAllocation)allocation);
}

void *vio_vma_map(void *allocator, void *allocation)
{
    void *data = nullptr;
    vmaMapMemory((VmaAllocator)allocator, (VmaAllocation)allocation, &data);
    return data;
}

void vio_vma_unmap(void *allocator, void *allocation)
{
    vmaUnmapMemory((VmaAllocator)allocator, (VmaAllocation)allocation);
}

int vio_vma_create_image(void *allocator, const VkImageCreateInfo *info,
                          VkMemoryPropertyFlags mem_props,
                          VkImage *out_image, void **out_allocation)
{
    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    alloc_info.requiredFlags = mem_props;

    VmaAllocation alloc;
    VkResult res = vmaCreateImage((VmaAllocator)allocator, info, &alloc_info, out_image, &alloc, nullptr);
    if (res != VK_SUCCESS) return -1;

    *out_allocation = (void *)alloc;
    return 0;
}

void vio_vma_destroy_image(void *allocator, VkImage image, void *allocation)
{
    vmaDestroyImage((VmaAllocator)allocator, image, (VmaAllocation)allocation);
}

} /* extern "C" */

#endif /* HAVE_VULKAN */
