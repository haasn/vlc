/*****************************************************************************
 * vk_instance.c: Vulkan instance abstraction
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

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>

#include <vlc_common.h>
#include <vlc_modules.h>

#include "vk_instance.h"

/**
 * Creates a Vulkan surface (and its underlying instance).
 *
 * @param wnd window to use as Vulkan surface
 * @param name module name (or NULL for auto)
 * @return a new context, or NULL on failure
 */
vlc_vk_t *vlc_vk_Create(struct vout_window_t *wnd, const char *name)
{
    vlc_object_t *parent = (vlc_object_t *) wnd;
    struct vlc_vk_t *vk;

    vk = vlc_object_create(parent, sizeof (*vk));
    if (unlikely(vk == NULL))
        return NULL;

    vk->ctx = NULL;
    vk->instance = NULL;
    vk->surface = (VkSurfaceKHR) NULL;

    vk->window = wnd;
    vk->module = module_need(vk, "vulkan", name, true);
    if (vk->module == NULL)
    {
        vlc_object_release(vk);
        return NULL;
    }
    atomic_init(&vk->ref_count, 1);

    return vk;
}

void vlc_vk_Hold(vlc_vk_t *vk)
{
    atomic_fetch_add(&vk->ref_count, 1);
}

void vlc_vk_Release(vlc_vk_t *vk)
{
    if (atomic_fetch_sub(&vk->ref_count, 1) != 1)
        return;
    module_unneed(vk, vk->module);
    vlc_object_release(vk);
}
