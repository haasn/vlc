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

#define VLCVK_MAX_BUFFERS 128

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
                VK_TEXT, PROVIDER_LONGTEXT)
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

    // Mapped buffers
    picture_t *pics[VLCVK_MAX_BUFFERS];
    unsigned long long list; // bitset of available pictures
};

struct picture_sys
{
    vlc_vk_t *vk;
    unsigned index;
    const struct pl_buf *buf;
};

// Display callbacks
static picture_pool_t *Pool(vout_display_t *, unsigned);
static void PictureRender(vout_display_t *, picture_t *, subpicture_t *, mtime_t);
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

    vout_window_t *window = vd->cfg->window;
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

    // TODO: Move this to context creation
    sys->vk->vulkan = sys->pl_vk;

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

    if (sys->vk != NULL)
        vlc_vk_Release(sys->vk);
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

    vlc_vk_Release(sys->vk);

    if (sys->pool)
        picture_pool_Release(sys->pool);
    free (sys);
}

static void DestroyPicture(picture_t *pic)
{
    struct picture_sys *picsys = pic->p_sys;
    const struct pl_gpu *gpu = picsys->vk->vulkan->gpu;

    pl_buf_destroy(gpu, &picsys->buf);
    vlc_vk_Release(picsys->vk);
}

static picture_t *CreatePicture(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;
    const struct pl_gpu *gpu = sys->pl_vk->gpu;

    struct picture_sys *picsys = calloc(1, sizeof(*picsys));
    if (unlikely(picsys == NULL))
        return NULL;

    picture_t *pic = picture_NewFromResource(&vd->fmt, &(picture_resource_t) {
        .p_sys = picsys,
        .pf_destroy = DestroyPicture,
    });

    if (!pic) {
        free(picsys);
        return NULL;
    }

    picsys->vk = sys->vk;
    vlc_vk_Hold(picsys->vk);

    // XXX: needed since picture_NewFromResource override pic planes
    // cf. opengl display.c
    if (picture_Setup(pic, &vd->fmt) != VLC_SUCCESS) {
        picture_Release(pic);
        return NULL;
    }

    size_t buf_size = 0;
    size_t offsets[PICTURE_PLANE_MAX];
    for (int i = 0; i < pic->i_planes; i++)
    {
        const plane_t *p = &pic->p[i];

        if (p->i_pitch < 0 || p->i_lines <= 0 ||
            (size_t) p->i_pitch > SIZE_MAX/p->i_lines)
        {
            picture_Release(pic);
            return NULL;
        }
        offsets[i] = buf_size;
        buf_size += p->i_pitch * p->i_lines;
    }

    // Round up for alignment
    buf_size = buf_size + 15 / 16 * 16;

    picsys->buf = pl_buf_create(gpu, &(struct pl_buf_params) {
        .type = PL_BUF_TEX_TRANSFER,
        .size = buf_size,
        .host_mapped = true,
    });

    if (!picsys->buf) {
        picture_Release(pic);
        return NULL;
    }

    for (int i = 0; i < pic->i_planes; ++i)
        pic->p[i].p_pixels = (void *) &picsys->buf->data[offsets[i]];

    return pic;
}

static picture_pool_t *Pool(vout_display_t *vd, unsigned requested_count)
{
    assert(requested_count <= VLCVK_MAX_BUFFERS);
    vout_display_sys_t *sys = vd->sys;
    if (sys->pool)
        return sys->pool;

    unsigned count;
    picture_t *pictures[requested_count];
    for (count = 0; count < requested_count; count++)
    {
        pictures[count] = CreatePicture(vd);
        if (!pictures[count])
            break;

        struct picture_sys *picsys = pictures[count]->p_sys;
        picsys->index = count;
    }

    if (count <= 1)
        goto error;

    sys->pool = picture_pool_New(count, pictures);
    if (!sys->pool)
        goto error;

    return sys->pool;

error:
    for (unsigned i = 0; i < count; i++) {
        picture_Release(pictures[i]);
        sys->pics[i] = NULL;
    }

    // Fallback to a regular memory pool
    sys->pool = picture_pool_NewFromFormat(&vd->fmt, requested_count);
    return sys->pool;
}

// Garbage collect all buffers that can be re-used
static void PollBuffers(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;
    const struct pl_gpu *gpu = sys->pl_vk->gpu;
    unsigned long long list = sys->list;

    // Release all pictures that are not used by the GPU anymore
    while (list != 0) {
        int i = ctz(list);
        picture_t *pic = sys->pics[i];
        assert(pic);
        struct picture_sys *picsys = pic->p_sys;
        assert(picsys);

        if (!pl_buf_poll(gpu, picsys->buf, 0)) {
            sys->list &= ~(1ULL << i);
            sys->pics[i] = NULL;
            picture_Release(pic);
        }

        list &= ~(1ULL << i);
    }
}

static void PictureRender(vout_display_t *vd, picture_t *pic,
                          subpicture_t *subpicture, mtime_t date)
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
        .width      = pic->format.i_visible_width,
        .height     = pic->format.i_visible_height,
        .color      = vlc_placebo_ColorSpace(&pic->format),
        .repr       = vlc_placebo_ColorRepr(&pic->format),
        .src_rect = {
            .x0 = pic->format.i_x_offset,
            .y0 = pic->format.i_y_offset,
            .x1 = pic->format.i_x_offset + pic->format.i_visible_width,
            .y1 = pic->format.i_y_offset + pic->format.i_visible_height,
        },
    };

    // Upload the image data for each plane
    struct pl_plane_data data[4];
    struct picture_sys *picsys = pic->p_sys;
    if (!vlc_placebo_PlaneData(pic, data, picsys ? picsys->buf : NULL)) {
        // This should never happen, in theory
        assert(!"Failed processing the picture_t into pl_plane_data!?");
    }

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

    // If this was a mapped buffer, mark it as in use by the GPU
    if (picsys) {
        unsigned index = picsys->index;
        if (sys->pics[index] == NULL) {
            sys->list |= 1ULL << index;
            sys->pics[index] = pic;
            picture_Hold(pic);
        }
    }

    // Garbage collect all previously used mapped buffers
    PollBuffers(vd);

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
        return;
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
        abort();

    case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
    case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
    case VOUT_DISPLAY_CHANGE_ZOOM: {
        vout_display_cfg_t cfg = *va_arg (ap, const vout_display_cfg_t *);
        vout_display_PlacePicture(&sys->place, &vd->source, &cfg, false);
        return VLC_SUCCESS;
    }

    case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
    case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
        vout_display_PlacePicture(&sys->place, &vd->source, vd->cfg, false);
        return VLC_SUCCESS;

    default:
        msg_Err (vd, "Unknown request %d", query);
    }

    return VLC_EGENERIC;
}
