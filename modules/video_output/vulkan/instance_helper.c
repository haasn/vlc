/*****************************************************************************
 * instance_helper.c: Vulkan instance creation helpers
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

#include <assert.h>

#include <vlc_common.h>
#include <vlc_placebo.h>
#include <vlc_vulkan.h>

#include "instance_helper.h"

/**
 * Creates a vulkan instance with the required extension enabled
 *
 * @param vk the vulkan context object to create the instance in
 * @param surf_extension the windowing extension to enable
 * @return error value
 */
int vk_CreateInstance(vlc_vk_t *vk, const char *surf_extension)
{
    vk->ctx = vlc_placebo_Create(VLC_OBJECT(vk));
    if (!vk->ctx)
        return VLC_ENOMEM;

    vk->instance = pl_vk_inst_create(vk->ctx, &(struct pl_vk_inst_params) {
        .debug = vk->use_debug,
        .extensions = (const char *[]) {
            VK_KHR_SURFACE_EXTENSION_NAME,
            surf_extension,
        },
        .num_extensions = 2,
    });

    if (!vk->instance)
        return VLC_EGENERIC;

    return VLC_SUCCESS;
}

void vk_DestroyInstance(vlc_vk_t *vk)
{
    if (!vk)
        return;

    pl_vk_inst_destroy(&vk->instance);
    pl_context_destroy(&vk->ctx);
}
