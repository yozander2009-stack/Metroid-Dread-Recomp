/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_COMMON_VK_SWAPCHAIN_H
#define SNX_COMMON_VK_SWAPCHAIN_H
#include <stdbool.h>
#include <stdint.h>
#include <volk.h>
/* A window's swapchain, and the choices every frontend makes for one (vk_common.h) */
#define SNX_VK_SWAP_IMAGES 16

/* The first of the surface's formats that is one of wanted, in sRGB's color space: true when one is. Otherwise false,
   and *out the surface's first format (all zero when the surface has none) */
bool snx_vk_surface_format(VkPhysicalDevice phys, VkSurfaceKHR surface, const VkFormat* wanted, uint32_t count,
                           VkSurfaceFormatKHR* out);
/* The images' extent: the surface's, or the window's (its size in pixels) where the surface leaves it to the
   swapchain (Wayland), kept within what the surface takes. Zero wide or high for a window that shows nothing */
VkExtent2D snx_vk_swap_extent(const VkSurfaceCapabilitiesKHR* caps, VkExtent2D window);
/* the images to ask for: extra more than the least the surface takes, no more than it has nor than most */
uint32_t snx_vk_swap_image_count(const VkSurfaceCapabilitiesKHR* caps, uint32_t extra, uint32_t most);

typedef struct {
    VkSwapchainKHR swapchain;
    VkSurfaceFormatKHR format;
    VkExtent2D extent;
    VkImage images[SNX_VK_SWAP_IMAGES];
    uint32_t count;
} SnxVkSwapchain;
/* what a swapchain is made of: the formats wanted, in order (the surface's first one when it has none of them), its
   images' usage, the window's size in pixels and FIFO presentation, an image more than the least */
typedef struct {
    VkSurfaceKHR surface;
    const VkFormat* formats;
    uint32_t format_count;
    VkImageUsageFlags usage;
    VkExtent2D window;
} SnxVkSwapchainInfo;
/* The swapchain made anew for the surface's size, in place of the one sc holds (which goes whether or not the new one
   is made, after the device is idle). False with none when it can't be */
bool snx_vk_swapchain_make(VkPhysicalDevice phys, VkDevice device, const SnxVkSwapchainInfo* info,
                           SnxVkSwapchain* sc);
/* the swapchain destroyed: nothing may use its images any more */
void snx_vk_swapchain_free(VkDevice device, SnxVkSwapchain* sc);
#endif
