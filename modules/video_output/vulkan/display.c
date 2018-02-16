/**
 * @file display.c
 * @brief Vulkan video output module
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


#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>

#include <vlc_common.h>
#include <vlc_placebo.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>
#include <vlc_vulkan.h>

#include <libplacebo/renderer.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/swapchain.h>
#include <libplacebo/vulkan.h>

// Plugin callbacks
static int Open (vlc_object_t *);
static void Close (vlc_object_t *);

#define VK_TEXT N_("Vulkan surface extension")
#define PROVIDER_LONGTEXT N_( \
    "Extension which provides the Vulkan surface to use.")

vlc_module_begin ()
    set_shortname (N_("Vulkan"))
    set_description (N_("Vulkan video output"))
    set_category (CAT_VIDEO)
    set_subcategory (SUBCAT_VIDEO_VOUT)
    set_capability ("vout display", 300)
    set_callbacks (Open, Close)
    add_shortcut ("vulkan", "vk")
    add_module ("vk", "vulkan", NULL,
                VK_TEXT, PROVIDER_LONGTEXT, true)
vlc_module_end ()

struct vout_display_sys_t
{
    vlc_vk_t *vk;
    const struct pl_vulkan *pl_vk;
    const struct pl_swapchain *swapchain;
    const struct pl_tex *plane_tex[4];
    struct pl_renderer *renderer;
    picture_pool_t *pool;

    // Dynamic during rendering
    vout_display_place_t place;
    uint64_t counter;
};

// Display callbacks
static picture_pool_t *Pool(vout_display_t *, unsigned);
static void PictureRender(vout_display_t *, picture_t *, subpicture_t *);
static void PictureDisplay(vout_display_t *, picture_t *, subpicture_t *);
static int Control(vout_display_t *, int, va_list);

// Allocates a Vulkan surface and instance for video output.
static int Open(vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *) obj;
    vout_display_sys_t *sys = vd->sys = malloc(sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;
    *sys = (struct vout_display_sys_t) {0};

    vout_window_t *window = vout_display_NewWindow(vd, VOUT_WINDOW_TYPE_INVALID);
    if (window == NULL)
    {
        msg_Err(vd, "parent window not available");
        goto error;
    }

    sys->vk = vlc_vk_Create(window, false, NULL); // TODO parametrize
    if (sys->vk == NULL)
        goto error;

    struct pl_context *ctx = sys->vk->ctx;
    struct pl_vulkan_params vk_params = pl_vulkan_default_params;
    vk_params.instance = sys->vk->instance->instance;
    vk_params.surface = sys->vk->surface;
    // TODO: allow influencing device selection

    sys->pl_vk = pl_vulkan_create(ctx, &vk_params);
    if (!sys->pl_vk)
        goto error;

    struct pl_vulkan_swapchain_params swap_params = {
        .surface = sys->vk->surface,
        .present_mode = VK_PRESENT_MODE_FIFO_KHR,
        // TODO: allow influencing the other settings?
    };

    sys->swapchain = pl_vulkan_create_swapchain(sys->pl_vk, &swap_params);
    if (!sys->swapchain)
        goto error;

    const struct pl_gpu *gpu = sys->pl_vk->gpu;
    sys->renderer = pl_renderer_create(ctx, gpu);
    if (!sys->renderer)
        goto error;

    vd->info.has_pictures_invalid = true;

    // Attempt using the input format as the display format
    if (vlc_placebo_FormatSupported(gpu, vd->source.i_chroma)) {
        vd->fmt.i_chroma = vd->source.i_chroma;
    } else {
        const vlc_fourcc_t *fcc;
        for (fcc = vlc_fourcc_GetFallback(vd->source.i_chroma); *fcc; fcc++) {
            if (vlc_placebo_FormatSupported(gpu, *fcc)) {
                vd->fmt.i_chroma = *fcc;
                break;
            }
        }

        if (!vd->fmt.i_chroma) {
            vd->fmt.i_chroma = VLC_CODEC_RGBA;
            msg_Warn(vd, "Failed picking any suitable input format, falling "
                     "back to RGBA for sanity!");
        }
    }

    vd->pool = Pool;
    vd->prepare = PictureRender;
    vd->display = PictureDisplay;
    vd->control = Control;
    return VLC_SUCCESS;

error:
    pl_renderer_destroy(&sys->renderer);
    pl_swapchain_destroy(&sys->swapchain);
    pl_vulkan_destroy(&sys->pl_vk);

    if (sys->vk != NULL)
        vlc_vk_Release(sys->vk);
    if (window != NULL)
        vout_display_DeleteWindow(vd, window);
    free(sys);
    return VLC_EGENERIC;
}

static void Close(vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = vd->sys;
    const struct pl_gpu *gpu = sys->pl_vk->gpu;

    for (int i = 0; i < 4; i++)
        pl_tex_destroy(gpu, &sys->plane_tex[i]);

    pl_renderer_destroy(&sys->renderer);
    pl_swapchain_destroy(&sys->swapchain);
    pl_vulkan_destroy(&sys->pl_vk);

    vlc_vk_Release(sys->vk);
    vout_display_DeleteWindow(vd, sys->vk->window);

    if (sys->pool)
        picture_pool_Release(sys->pool);
    free (sys);
}

static picture_pool_t *Pool(vout_display_t *vd, unsigned count)
{
    // TODO: use mapped buffers
    vout_display_sys_t *sys = vd->sys;
    if (!sys->pool)
        sys->pool = picture_pool_NewFromFormat(&vd->fmt, count);
    return sys->pool;
}

static void PictureRender(vout_display_t *vd, picture_t *pic, subpicture_t *subpicture)
{
    vout_display_sys_t *sys = vd->sys;
    const struct pl_gpu *gpu = sys->pl_vk->gpu;
    bool failed = false;

    struct pl_swapchain_frame frame;
    if (!pl_swapchain_start_frame(sys->swapchain, &frame))
        return; // Probably benign error, ignore it

    struct pl_image img = {
        .signature  = sys->counter++,
        .num_planes = pic->i_planes,
        .width      = pic->format.i_width,
        .height     = pic->format.i_height,
        .color      = vlc_placebo_ColorSpace(&pic->format),
        .repr       = vlc_placebo_ColorRepr(&pic->format),
        .src_rect = {
            .x0 = pic->format.i_x_offset,
            .y0 = pic->format.i_y_offset,
            .x1 = pic->format.i_x_offset + pic->format.i_visible_width,
            .y1 = pic->format.i_y_offset + pic->format.i_visible_height,
        },
    };

    printf("width: %d, height: %d, visw: %d, vish: %d\n",
           pic->format.i_width, pic->format.i_height,
           pic->format.i_visible_width, pic->format.i_visible_height);

    // Upload the image data for each plane
    struct pl_plane_data data[4];
    if (!vlc_placebo_PlaneData(pic, data))
        abort(); // XXX

    for (int i = 0; i < pic->i_planes; i++) {
        struct pl_plane *plane = &img.planes[i];
        if (!pl_upload_plane(gpu, plane, &sys->plane_tex[i], &data[i])) {
            msg_Err(vd, "Failed uploading image data!");
            failed = true;
            goto done;
        }

        // Matches only the chroma planes, never luma or alpha
        if (vlc_fourcc_IsYUV(pic->format.i_chroma) && i != 0 && i != 3) {
            enum pl_chroma_location loc = vlc_placebo_ChromaLoc(&pic->format);
            pl_chroma_location_offset(loc, &plane->shift_x, &plane->shift_y);
        }
    }

    struct pl_render_target target;
    pl_render_target_from_swapchain(&target, &frame);
    // TODO: set overlays based on the subpictures
    target.dst_rect = (struct pl_rect2d) {
        .x0 = sys->place.x,
        .y0 = sys->place.y,
        .x1 = sys->place.x + sys->place.width,
        .y1 = sys->place.y + sys->place.height,
    };

    // If we don't cover the entire output, clear it first
    struct pl_rect2d full = {0, 0, frame.fbo->params.w, frame.fbo->params.h };
    if (!pl_rect2d_eq(target.dst_rect, full)) {
        // TODO: make background color configurable?
        pl_tex_clear(gpu, frame.fbo, (float[4]){ 0.0, 0.0, 0.0, 0.0 });
    }

    struct pl_render_params params = pl_render_default_params;
    params.deband_params = NULL; // XXX: work-around
    // TODO: allow changing renderer settings

    if (!pl_render_image(sys->renderer, &img, &target, &params)) {
        msg_Err(vd, "Failed rendering frame!");
        failed = true;
        goto done;
    }

done:

    if (failed)
        pl_tex_clear(gpu, frame.fbo, (float[4]){ 1.0, 0.0, 0.0, 1.0 });

    if (!pl_swapchain_submit_frame(sys->swapchain)) {
        msg_Err(vd, "Failed rendering frame!");
        return; // XXX: This is probably a serious failure. Can we request
                // reinit or something?
    }
}

static void PictureDisplay(vout_display_t *vd, picture_t *pic, subpicture_t *subpicture)
{
    picture_Release(pic);

    vout_display_sys_t *sys = vd->sys;
    pl_swapchain_swap_buffers(sys->swapchain);
}

static int Control(vout_display_t *vd, int query, va_list ap)
{
    vout_display_sys_t *sys = vd->sys;

    switch (query)
    {
    case VOUT_DISPLAY_RESET_PICTURES:
        // XXX: do we also have to re-probe formats?
        pl_renderer_flush_cache(sys->renderer);
        sys->counter = 0;
        return VLC_SUCCESS;

    case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
    case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
    case VOUT_DISPLAY_CHANGE_ZOOM: {
        vout_display_cfg_t cfg = *va_arg (ap, const vout_display_cfg_t *);
        vout_display_PlacePicture (&sys->place, &vd->source, &cfg, false);
        return VLC_SUCCESS;
    }

    case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
    case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
        return VLC_SUCCESS;

    default:
        msg_Err (vd, "Unknown request %d", query);
    }

    return VLC_EGENERIC;
}
