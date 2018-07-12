/*****************************************************************************
 * vk_instance.h: Vulkan instance abstraction
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

#ifndef VLC_VULKAN_INSTANCE_H
#define VLC_VULKAN_INSTANCE_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <vulkan/vulkan.h>
#include <libplacebo/vulkan.h>

struct vout_window_t;
struct vout_window_cfg_t;

// Shared struct for vulkan instance / surface / device state
typedef struct vlc_vk_t
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
} vlc_vk_t;

vlc_vk_t *vlc_vk_Create(struct vout_window_t *, bool, const char *) VLC_USED;
void vlc_vk_Release(vlc_vk_t *);
void vlc_vk_Hold(vlc_vk_t *);

#endif // VLC_VULKAN_INSTANCE_H
