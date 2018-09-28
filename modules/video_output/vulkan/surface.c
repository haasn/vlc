/**
 * @file surface.c
 * @brief Vulkan platform-specific surface extension module
 */
/*****************************************************************************
 * Copyright © 2018 Niklas Haas, Marvin Scholz
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_window.h>

#ifdef VK_USE_PLATFORM_XLIB_KHR

#include <vlc_xlib.h>
#define MODULE_NAME N_("VkXlib")
#define MODULE_DESC N_("Xlib extension for Vulkan")

#elif VK_USE_PLATFORM_WIN32_KHR

#define MODULE_NAME N_("VkWin32")
#define MODULE_DESC N_("Win32 extension for Vulkan")

#else
#error Trying to build vulkan/surface.c without any platform defined!
#endif

#include "../placebo_utils.h"
#include "vk_instance.h"

static int Open (vlc_object_t *obj)
{
    vlc_vk_t *vk = (vlc_vk_t *) obj;
    const char *surf_extension;

#ifdef VK_USE_PLATFORM_XLIB_KHR

    if (vk->window->type != VOUT_WINDOW_TYPE_XID || !vlc_xlib_init(obj))
        return VLC_EGENERIC;

    // Initialize X11 display
    Display *dpy = vk->sys = XOpenDisplay(vk->window->display.x11);
    if (dpy == NULL)
        return VLC_EGENERIC;

    surf_extension = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;

#elif VK_USE_PLATFORM_WIN32_KHR

    if (vk->window->type != VOUT_WINDOW_TYPE_HWND)
        return VLC_EGENERIC;

    surf_extension = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;

#endif

    // Initialize Vulkan instance
    vk->ctx = vlc_placebo_Create(VLC_OBJECT(vk));
    if (!vk->ctx)
        goto error;

    vk->instance = pl_vk_inst_create(vk->ctx, &(struct pl_vk_inst_params) {
        .debug = var_InheritBool(vk, "vk-debug"),
        .extensions = (const char *[]) {
            VK_KHR_SURFACE_EXTENSION_NAME,
            surf_extension,
        },
        .num_extensions = 2,
    });
    if (!vk->instance)
        goto error;

    // Create the platform-specific surface object
    const VkInstance vkinst = vk->instance->instance;
#ifdef VK_USE_PLATFORM_XLIB_KHR

    VkXlibSurfaceCreateInfoKHR xinfo = {
         .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
         .dpy = dpy,
         .window = (Window) vk->window->handle.xid,
    };

    VkResult res = vkCreateXlibSurfaceKHR(vkinst, &xinfo, NULL, &vk->surface);

#elif VK_USE_PLATFORM_WIN32_KHR

    // Get current win32 HINSTANCE
    HINSTANCE hInst = GetModuleHandle(NULL);

    VkWin32SurfaceCreateInfoKHR winfo = {
         .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
         .hinstance = hInst,
         .hwnd = (HWND) vk->window->handle.hwnd,
    };

    VkResult res = vkCreateWin32SurfaceKHR(vkinst, &winfo, NULL, &vk->surface);

#endif

    if (res != VK_SUCCESS)
        goto error;

    // Create vulkan device
    vk->vulkan = pl_vulkan_create(vk->ctx, &(struct pl_vulkan_params) {
        .instance = vkinst,
        .surface = vk->surface,
        .device_name = var_InheritString(vk, "vk-device"),
        .allow_software = var_InheritBool(vk, "allow-sw"),
        .async_transfer = var_InheritBool(vk, "async-xfer"),
        .async_compute = var_InheritBool(vk, "async-comp"),
        .queue_count = var_InheritInteger(vk, "queue-count"),
    });
    if (!vk->vulkan)
        goto error;

    // Create swapchain for this surface
    struct pl_vulkan_swapchain_params swap_params = {
        .surface = vk->surface,
        .present_mode = var_InheritInteger(vk, "present-mode"),
        .swapchain_depth = var_InheritInteger(vk, "queue-depth"),
    };

    vk->swapchain = pl_vulkan_create_swapchain(vk->vulkan, &swap_params);
    if (!vk->swapchain)
        goto error;

    return VLC_SUCCESS;

error:
    pl_swapchain_destroy(&vk->swapchain);
    if (vk->surface)
        vkDestroySurfaceKHR(vk->instance->instance, vk->surface, NULL);

    pl_vulkan_destroy(&vk->vulkan);
    pl_vk_inst_destroy(&vk->instance);
    pl_context_destroy(&vk->ctx);

#ifdef VK_USE_PLATFORM_XLIB_KHR
    if (dpy)
        XCloseDisplay(dpy);
#endif

    return VLC_EGENERIC;
}

static void Close (vlc_object_t *obj)
{
    vlc_vk_t *vk = (vlc_vk_t *) obj;

    vkDestroySurfaceKHR(vk->instance->instance, vk->surface, NULL);
    pl_vk_inst_destroy(&vk->instance);
    pl_context_destroy(&vk->ctx);

#ifdef VK_USE_PLATFORM_XLIB_KHR
    Display *dpy = vk->sys;
    XCloseDisplay(dpy);
#endif
}

#define DEBUG_TEXT "Enable API debugging"
#define DEBUG_LONGTEXT "This loads the vulkan standard validation layers, which can help catch API usage errors. Comes at a small performance penalty."

#define DEVICE_TEXT "Device name override"
#define DEVICE_LONGTEXT "If set to something non-empty, only a device with this exact name will be used. To see a list of devices and their names, run vlc -v with this module active."

#define ALLOWSW_TEXT "Allow software devices"
#define ALLOWSW_LONGTEXT "If enabled, allow the use of software emulation devices, which are not real devices and therefore typically very slow. (This option has no effect if forcing a specific device name)"

#define ASYNC_XFER_TEXT "Allow asynchronous transfer"
#define ASYNC_XFER_LONGTEXT "Allows the use of an asynchronous transfer queue if the device has one. Typically this maps to a DMA engine, which can perform texture uploads/downloads without blocking the GPU's compute units. Highly recommended for 4K and above."

#define ASYNC_COMP_TEXT "Allow asynchronous compute"
#define ASYNC_COMP_LONGTEXT "Allows the use of dedicated compute queue families if the device has one. Sometimes these will schedule concurrent compute work better than the main graphics queue. Turn this off if you have any issues."

#define QUEUE_COUNT_TEXT "Queue count"
#define QUEUE_COUNT_LONGTEXT "How many queues to use on the device. Increasing this might improve rendering throughput for GPUs capable of concurrent scheduling. Increasing this past the driver's limit has no effect."

#define QUEUE_DEPTH_TEXT "Maximum frame latency"
#define QUEUE_DEPTH_LONGTEXT "Affects how many frames to render/present in advance. Increasing this can improve performance at the cost of latency, by allowing better pipelining between frames. May have no effect, depending on the VLC clock settings."

static const int present_values[] = {
    VK_PRESENT_MODE_IMMEDIATE_KHR,
    VK_PRESENT_MODE_MAILBOX_KHR,
    VK_PRESENT_MODE_FIFO_KHR,
    VK_PRESENT_MODE_FIFO_RELAXED_KHR,
};

static const char * const present_text[] = {
    "Immediate (non-blocking, tearing)",
    "Mailbox (non-blocking, non-tearing)",
    "FIFO (blocking, non-tearing)",
    "Relaxed FIFO (blocking, tearing)",
};

#define PRESENT_MODE_TEXT "Preferred present mode"
#define PRESENT_MODE_LONGTEXT "Which present mode to use when creating the swapchain. If the chosen mode is not supported, VLC will fall back to using FIFO."

vlc_module_begin ()
    set_shortname (MODULE_NAME)
    set_description (MODULE_DESC)
    set_category (CAT_VIDEO)
    set_subcategory (SUBCAT_VIDEO_VOUT)
    set_capability ("vulkan", 10)
    set_callbacks (Open, Close)

    set_section(N_("Device selection"), NULL)
    add_bool("vk-debug", false, DEBUG_TEXT, DEBUG_LONGTEXT, false)
    add_string("vk-device", "", DEVICE_TEXT, DEVICE_LONGTEXT, false)
    add_bool("allow-sw", pl_vulkan_default_params.allow_software,
            ALLOWSW_TEXT, ALLOWSW_LONGTEXT, false)

    set_section(N_("Performance tuning"), NULL)
    add_bool("async-xfer", pl_vulkan_default_params.async_transfer,
            ASYNC_XFER_TEXT, ASYNC_XFER_LONGTEXT, false)
    add_bool("async-comp", pl_vulkan_default_params.async_compute,
            ASYNC_COMP_TEXT, ASYNC_COMP_LONGTEXT, false)
    add_integer_with_range("queue-count", pl_vulkan_default_params.queue_count,
            1, 8, QUEUE_COUNT_TEXT, QUEUE_COUNT_LONGTEXT, false)
    add_integer_with_range("queue-depth", 3,
            1, 8, QUEUE_DEPTH_TEXT, QUEUE_DEPTH_LONGTEXT, false)
    add_integer("present-mode", VK_PRESENT_MODE_FIFO_KHR,
            PRESENT_MODE_TEXT, PRESENT_MODE_LONGTEXT, false)
            change_integer_list(present_values, present_text)

vlc_module_end ()
