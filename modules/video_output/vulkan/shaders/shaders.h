/*****************************************************************************
 * shaders.h: Built in GLSL shaders
 *****************************************************************************
 * Copyright (C) 2020 Niklas Haas
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

#ifndef VLC_VULKAN_SHADERS_H
#define VLC_VULKAN_SHADERS_H

extern const char fsrcnnx_8_0_4_1[];
extern const size_t fsrcnnx_8_0_4_1_len;

extern const char krig_bilateral[];
extern const size_t krig_bilateral_len;

extern const char ravu_r3_compute[];
extern const size_t ravu_r3_compute_len;

extern const char ssim_downscaler[];
extern const size_t ssim_downscaler_len;

extern const char ssim_super_res[];
extern const size_t ssim_super_res_len;

#endif // VLC_VULKAN_SHADERS_H
