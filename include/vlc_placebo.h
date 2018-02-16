/*****************************************************************************
 * vlc_fourcc.h: Definition of various libplacebo helpers
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

#ifndef VLC_PLACEBO_H
#define VLC_PLACEBO_H 1

#include <vlc_common.h>
#include <vlc_picture.h>

#include <libplacebo/colorspace.h>
#include <libplacebo/utils/upload.h>

// Create a libplacebo context, hooked up to the log system; or NULL on OOM
VLC_API struct pl_context *vlc_placebo_Create(vlc_object_t *);

// Turn a video_format_t into the equivalent libplacebo values
VLC_API struct pl_color_space vlc_placebo_ColorSpace(const video_format_t *);
VLC_API struct pl_color_repr vlc_placebo_ColorRepr(const video_format_t *);
VLC_API enum pl_chroma_location vlc_placebo_ChromaLoc(const video_format_t *);

// Fill a pl_plane_data array with various data. Returns the number of planes,
// or 0 if the format is unsupported by the libplacebo API
VLC_API int vlc_placebo_PlaneFormat(const video_format_t *, struct pl_plane_data[4]);
VLC_API int vlc_placebo_PlaneData(const picture_t *, struct pl_plane_data[4]);

// See if a given FourCC is physically supported by a given GPU
VLC_API bool vlc_placebo_FormatSupported(const struct pl_gpu *, vlc_fourcc_t);

#endif // VLC_PLACEBO_H
