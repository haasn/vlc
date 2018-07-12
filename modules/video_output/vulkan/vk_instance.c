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

struct vlc_vk_priv_t
{
    vlc_vk_t vk;
    atomic_uint ref_count;
};

/**
 * Creates a Vulkan surface (and its underlying instance).
 *
 * @param wnd window to use as Vulkan surface
 * @param debug if true, load the standard validation layers
 * @param name module name (or NULL for auto)
 * @return a new context, or NULL on failure
 */
vlc_vk_t *vlc_vk_Create(struct vout_window_t *wnd, bool debug, const char *name)
{
    vlc_object_t *parent = (vlc_object_t *) wnd;
    struct vlc_vk_priv_t *vkpriv;

    vkpriv = vlc_object_create(parent, sizeof (*vkpriv));
    if (unlikely(vkpriv == NULL))
        return NULL;

    vkpriv->vk.ctx = NULL;
    vkpriv->vk.instance = NULL;
    vkpriv->vk.surface = (VkSurfaceKHR) NULL;

    vkpriv->vk.use_debug = debug;
    vkpriv->vk.window = wnd;
    vkpriv->vk.module = module_need(&vkpriv->vk, "vulkan", name, true);
    if (vkpriv->vk.module == NULL)
    {
        vlc_object_release(&vkpriv->vk);
        return NULL;
    }
    atomic_init(&vkpriv->ref_count, 1);

    return &vkpriv->vk;
}

void vlc_vk_Hold(vlc_vk_t *vk)
{
    struct vlc_vk_priv_t *vkpriv = (struct vlc_vk_priv_t *) vk;
    atomic_fetch_add(&vkpriv->ref_count, 1);
}

void vlc_vk_Release(vlc_vk_t *vk)
{
    struct vlc_vk_priv_t *vkpriv = (struct vlc_vk_priv_t *) vk;
    if (atomic_fetch_sub(&vkpriv->ref_count, 1) != 1)
        return;
    module_unneed(vk, vk->module);
    vlc_object_release(vk);
}
