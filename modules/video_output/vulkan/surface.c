/**
 * @file surface.c
 * @brief Vulkan platform-specific surface extension module
 */
/*****************************************************************************
 * Copyright © 2018 Niklas Haas
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
#endif

    // Initialize Vulkan instance
    vk->ctx = vlc_placebo_Create(VLC_OBJECT(vk));
    if (!vk->ctx)
        goto error;

    vk->instance = pl_vk_inst_create(vk->ctx, &(struct pl_vk_inst_params) {
        .debug = vk->use_debug,
        .extensions = (const char *[]) {
            VK_KHR_SURFACE_EXTENSION_NAME,
            surf_extension,
        },
        .num_extensions = 2,
    });

    // Create the platform-specific surface object
    const VkInstance *vkinst = &vk->instance->instance;
#ifdef VK_USE_PLATFORM_XLIB_KHR
    VkXlibSurfaceCreateInfoKHR xinfo = {
         .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
         .dpy = dpy,
         .window = (Window) vk->window->handle.xid,
    };

    VkResult res = vkCreateXlibSurfaceKHR(*vkinst, &xinfo, NULL, &vk->surface);
#endif

    if (res != VK_SUCCESS)
        goto error;

    return VLC_SUCCESS;

error:
    if (vk->surface)
        vkDestroySurfaceKHR(*vkinst, vk->surface, NULL);

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

vlc_module_begin ()
    set_shortname (MODULE_NAME)
    set_description (MODULE_DESC)
    set_category (CAT_VIDEO)
    set_subcategory (SUBCAT_VIDEO_VOUT)
    set_capability ("vulkan", 10)
    set_callbacks (Open, Close)
vlc_module_end ()
