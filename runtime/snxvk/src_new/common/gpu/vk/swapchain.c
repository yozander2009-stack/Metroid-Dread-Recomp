/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/swapchain.h"
#include <stdlib.h>

bool snx_vk_surface_format(VkPhysicalDevice phys, VkSurfaceKHR surface, const VkFormat* wanted, uint32_t count,
                           VkSurfaceFormatKHR* out) {
    *out = (VkSurfaceFormatKHR){0};
    uint32_t n = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, NULL) != VK_SUCCESS || !n) return false;
    VkSurfaceFormatKHR* formats = calloc(n, sizeof *formats);
    if (!formats) return false;
    bool found = false;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, formats) >= 0 && n) {
        *out = formats[0];
        for (uint32_t i = 0; i < n && !found; ++i)
            for (uint32_t w = 0; w < count && !found; ++w)
                if (formats[i].format == wanted[w] && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    *out = formats[i];
                    found = true;
                }
    }
    free(formats);
    return found;
}

VkExtent2D snx_vk_swap_extent(const VkSurfaceCapabilitiesKHR* caps, VkExtent2D window) {
    VkExtent2D e = caps->currentExtent.width == UINT32_MAX ? window : caps->currentExtent;
    if (e.width < caps->minImageExtent.width) e.width = caps->minImageExtent.width;
    if (e.height < caps->minImageExtent.height) e.height = caps->minImageExtent.height;
    if (e.width > caps->maxImageExtent.width) e.width = caps->maxImageExtent.width;
    if (e.height > caps->maxImageExtent.height) e.height = caps->maxImageExtent.height;
    return e;
}

uint32_t snx_vk_swap_image_count(const VkSurfaceCapabilitiesKHR* caps, uint32_t extra, uint32_t most) {
    uint32_t count = caps->minImageCount + extra;
    if (caps->maxImageCount && count > caps->maxImageCount) count = caps->maxImageCount;
    /* never fewer than the surface takes, whatever most says */
    if (count > most) count = most > caps->minImageCount ? most : caps->minImageCount;
    return count;
}

void snx_vk_swapchain_free(VkDevice device, SnxVkSwapchain* sc) {
    if (sc->swapchain) vkDestroySwapchainKHR(device, sc->swapchain, NULL);
    *sc = (SnxVkSwapchain){0};
}

bool snx_vk_swapchain_make(VkPhysicalDevice phys, VkDevice device, const SnxVkSwapchainInfo* info,
                           SnxVkSwapchain* sc) {
    VkSwapchainKHR old = sc->swapchain;
    /* the old one's images may still be in use by work in flight */
    if (old) vkDeviceWaitIdle(device);
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR format = {0};
    bool ok = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, info->surface, &caps) == VK_SUCCESS;
    if (ok) {
        snx_vk_surface_format(phys, info->surface, info->formats, info->format_count, &format);
        ok = format.format != VK_FORMAT_UNDEFINED;
    }
    VkExtent2D extent = ok ? snx_vk_swap_extent(&caps, info->window) : (VkExtent2D){0, 0};
    VkSwapchainKHR made = VK_NULL_HANDLE;
    if (ok && extent.width && extent.height) {
        VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                       .surface = info->surface,
                                       .minImageCount = snx_vk_swap_image_count(&caps, 1, SNX_VK_SWAP_IMAGES),
                                       .imageFormat = format.format,
                                       .imageColorSpace = format.colorSpace,
                                       .imageExtent = extent,
                                       .imageArrayLayers = 1,
                                       .imageUsage = info->usage,
                                       .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                       .preTransform = caps.currentTransform,
                                       .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                       .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                       .clipped = VK_TRUE,
                                       .oldSwapchain = old};
        if (vkCreateSwapchainKHR(device, &ci, NULL, &made) != VK_SUCCESS) made = VK_NULL_HANDLE;
    }
    /* retired by the create, made or not */
    snx_vk_swapchain_free(device, sc);
    if (!made) return false;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, made, &count, NULL);
    if (count > SNX_VK_SWAP_IMAGES) count = SNX_VK_SWAP_IMAGES;
    if (vkGetSwapchainImagesKHR(device, made, &count, sc->images) < 0 || !count) {
        vkDestroySwapchainKHR(device, made, NULL);
        *sc = (SnxVkSwapchain){0};
        return false;
    }
    sc->swapchain = made;
    sc->format = format;
    sc->extent = extent;
    sc->count = count;
    return true;
}
