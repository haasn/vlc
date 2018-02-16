/**
 * @file xlib.c
 * @brief Vulkan Xlib extension module
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
#include <vlc_vulkan.h>
#include <vlc_vout_window.h>
#include <vlc_xlib.h>

#include "instance_helper.h"

static int Open (vlc_object_t *obj)
{
    vlc_vk_t *vk = (vlc_vk_t *) obj;
    VkInstance inst = (VkInstance) NULL;

    if (vk->window->type != VOUT_WINDOW_TYPE_XID || !vlc_xlib_init(obj))
        return VLC_EGENERIC;

    // Initialize X11 display
    Display *dpy = vk->sys = XOpenDisplay(vk->window->display.x11);
    if (dpy == NULL)
        return VLC_EGENERIC;

    // Initialize Vulkan instance
    int ret = vk_CreateInstance(vk, VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
    if (ret != VK_SUCCESS)
        goto error;

    VkXlibSurfaceCreateInfoKHR xinfo = {
         .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
         .dpy = dpy,
         .window = (Window) vk->window->handle.xid,
    };

    inst = vk->instance->instance;
    VkResult res = vkCreateXlibSurfaceKHR(inst, &xinfo, NULL, &vk->surface);
    if (res != VK_SUCCESS)
        goto error;

    return VLC_SUCCESS;

error:
    if (inst)
        vkDestroySurfaceKHR(inst, vk->surface, NULL);
    vk_DestroyInstance(vk);
    XCloseDisplay(dpy);
    return VLC_EGENERIC;
}

static void Close (vlc_object_t *obj)
{
    vlc_vk_t *vk = (vlc_vk_t *) obj;
    Display *dpy = vk->sys;

    vkDestroySurfaceKHR(vk->instance->instance, vk->surface, NULL);
    vk_DestroyInstance(vk);
    XCloseDisplay(dpy);
}

vlc_module_begin ()
    set_shortname (N_("VkXlib"))
    set_description (N_("Xlib extension for Vulkan"))
    set_category (CAT_VIDEO)
    set_subcategory (SUBCAT_VIDEO_VOUT)
    set_capability ("vulkan", 10)
    set_callbacks (Open, Close)
vlc_module_end ()
