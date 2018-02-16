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
#include <vlc_es.h>

#include <libplacebo/colorspace.h>

// Create a libplacebo context, hooked up to the log system; or NULL on OOM
VLC_API struct pl_context *vlc_placebo_Create(vlc_object_t *obj);

// Turn a video_format_t into the equivalent libplacebo values
VLC_API struct pl_color_space vlc_placebo_ColorSpace(const video_format_t *fmt);
VLC_API struct pl_color_repr vlc_placebo_ColorRepr(const video_format_t *fmt);
VLC_API enum pl_chroma_location vlc_placebo_ChromaLoc(const video_format_t *fmt);

#endif // VLC_PLACEBO_H
