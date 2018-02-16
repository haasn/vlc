/*****************************************************************************
 * vout_helper.h: Vulkan instance creation helpers
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

#ifndef VLC_VULKAN_INSTANCE_HELPER_H
#define VLC_VULKAN_INSTANCE_HELPER_H

#include <vlc_vulkan.h>

int vk_CreateInstance(vlc_vk_t *, const char *);
void vk_DestroyInstance(vlc_vk_t *);

#endif // VLC_VULKAN_INSTANCE_HELPER_H
