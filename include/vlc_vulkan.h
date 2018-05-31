/*****************************************************************************
 * vlc_vulkan.h: VLC Vulkan API
 *****************************************************************************
 * Copyright (C) 2018 Niklas Haas
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

#ifndef VLC_VK_H
#define VLC_VK_H 1

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

// FIXME: parametrize!
#define VK_USE_PLATFORM_XLIB_KHR

#include <vulkan/vulkan.h>
#include <libplacebo/vulkan.h>

/**
 * \file
 * This file defines Vulkan structures and functions.
 */

struct vout_window_t;
struct vout_window_cfg_t;

/**
 * A VLC Vulkan surface (and its underlying instance)
 */
typedef struct vlc_vk_t vlc_vk_t;

struct vlc_vk_t
{
    struct vlc_common_members obj;

    struct vout_window_t *window;
    module_t *module;
    void *sys;

    struct pl_context *ctx;
    const struct pl_vk_inst *instance;
    const struct pl_vulkan *vulkan;
    VkSurfaceKHR surface;

    // whether to enable standard validation layers
    bool use_debug;
};

VLC_API vlc_vk_t *vlc_vk_Create(struct vout_window_t *, bool, const char *) VLC_USED;
VLC_API void vlc_vk_Release(vlc_vk_t *);
VLC_API void vlc_vk_Hold(vlc_vk_t *);

#endif /* VLC_VK_H */
