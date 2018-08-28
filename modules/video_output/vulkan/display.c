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
#include <vlc_plugin.h>
#include <vlc_vout_display.h>

#include "../placebo_utils.h"
#include "vk_instance.h"

#include <libplacebo/renderer.h>
#include <libplacebo/utils/upload.h>
#include <libplacebo/swapchain.h>
#include <libplacebo/vulkan.h>

#define VLCVK_MAX_BUFFERS 128

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

    // Storage for rendering parameters
    struct pl_deband_params deband;
    struct pl_sigmoid_params sigmoid;
    struct pl_color_adjustment color_adjust;
    struct pl_color_map_params color_map;
    struct pl_dither_params dither;
    struct pl_render_params params;
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
static void PollBuffers(vout_display_t *);

// Update the renderer settings based on the current configuration.
//
// XXX: This could be called every time the parameters change, but currently
// VLC does not allow that - so we're stuck with doing it once on Open().
// Should be changed as soon as it's possible!
static void UpdateParams(vout_display_t *vd)
{
    vout_display_sys_t *sys = vd->sys;

    sys->deband = pl_deband_default_params;
    sys->deband.iterations = var_InheritInteger(vd, "iterations");
    sys->deband.threshold = var_InheritFloat(vd, "threshold");
    sys->deband.radius = var_InheritFloat(vd, "radius");
    sys->deband.grain = var_InheritFloat(vd, "grain");
    bool use_deband = sys->deband.iterations > 0 || sys->deband.grain > 0;

    sys->sigmoid = pl_sigmoid_default_params;
    sys->sigmoid.center = var_InheritFloat(vd, "sigmoid-center");
    sys->sigmoid.slope = var_InheritFloat(vd, "sigmoid-slope");
    bool use_sigmoid = var_InheritBool(vd, "sigmoid");

    sys->color_adjust = pl_color_adjustment_neutral;
    sys->color_adjust.brightness = var_InheritFloat(vd, "vkbrightness");
    sys->color_adjust.contrast = var_InheritFloat(vd, "vkcontrast");
    sys->color_adjust.saturation = var_InheritFloat(vd, "vksaturation");
    sys->color_adjust.hue = var_InheritFloat(vd, "vkhue");
    sys->color_adjust.gamma = var_InheritFloat(vd, "vkgamma");

    sys->color_map = pl_color_map_default_params;
    sys->color_map.intent = var_InheritInteger(vd, "intent");
    sys->color_map.tone_mapping_algo = var_InheritInteger(vd, "tone-mapping");
    sys->color_map.tone_mapping_param = var_InheritFloat(vd, "tone-mapping-param");
    sys->color_map.tone_mapping_desaturate = var_InheritFloat(vd, "tone-mapping-desat");
    sys->color_map.gamut_warning = var_InheritBool(vd, "gamut-warning");
    sys->color_map.peak_detect_frames = var_InheritInteger(vd, "peak-frames");
    sys->color_map.scene_threshold = var_InheritFloat(vd, "scene-threshold");

    sys->dither = pl_dither_default_params;
    int method = var_InheritInteger(vd, "dither");
    bool use_dither = method >= 0;
    sys->dither.method = use_dither ? method : 0;
    sys->dither.lut_size = var_InheritInteger(vd, "dither-size");
    sys->dither.temporal = var_InheritBool(vd, "temporal-dither");

    sys->params = pl_render_default_params;
    sys->params.deband_params = use_deband ? &sys->deband : NULL;
    sys->params.sigmoid_params = use_sigmoid ? &sys->sigmoid : NULL;
    sys->params.color_adjustment = &sys->color_adjust;
    sys->params.color_map_params = &sys->color_map;
    sys->params.dither_params = use_dither ? &sys->dither : NULL;
    sys->params.skip_anti_aliasing = var_InheritBool(vd, "skip-aa");
    sys->params.polar_cutoff = var_InheritFloat(vd, "polar-cutoff");
    sys->params.disable_linear_scaling = var_InheritBool(vd, "disable-linear");
    sys->params.disable_builtin_scalers = var_InheritBool(vd, "force-general");
}

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

    UpdateParams(vd);
    return VLC_SUCCESS;

error:
    pl_renderer_destroy(&sys->renderer);
    pl_swapchain_destroy(&sys->swapchain);
    pl_vulkan_destroy(&sys->pl_vk);

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

    PollBuffers(vd);
    if (sys->pool)
        picture_pool_Release(sys->pool);

    pl_vulkan_destroy(&sys->pl_vk);
    vlc_vk_Release(sys->vk);
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

    // Upload the image data for each subpicture region
    // TODO

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

    // Dispatch the actual image rendering with the pre-configured parameters
    if (!pl_render_image(sys->renderer, &img, &target, &sys->params)) {
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
    if (subpicture)
        subpicture_Delete(subpicture);

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

// Options

#define VK_TEXT N_("Vulkan surface extension")
#define PROVIDER_LONGTEXT N_( \
    "Extension which provides the Vulkan surface to use.")

#define DEBAND_ITER_TEXT "Debanding iterations"
#define DEBAND_ITER_LONGTEXT "The number of debanding steps to perform per sample. Each step reduces a bit more banding, but takes time to compute. Note that the strength of each step falls off very quickly, so high numbers (>4) are practically useless. Setting this to 0 performs no debanding."

#define DEBAND_THRESH_TEXT "Gradient threshold"
#define DEBAND_THRESH_LONGTEXT "The debanding filter's cut-off threshold. Higher numbers increase the debanding strength dramatically, but progressively diminish image details."

#define DEBAND_RADIUS_TEXT "Search radius"
#define DEBAND_RADIUS_LONGTEXT "The debanding filter's initial radius. The radius increases linearly for each iteration. A higher radius will find more gradients, but a lower radius will smooth more aggressively."

#define DEBAND_GRAIN_TEXT "Grain strength"
#define DEBAND_GRAIN_LONGTEXT "Add some extra noise to the image. This significantly helps cover up remaining quantization artifacts. Higher numbers add more noise."

#define SIGMOID_TEXT "Use sigmoidization"
#define SIGMOID_LONGTEXT "If true, sigmoidizes the signal before upscaling. This helps prevent ringing artifacts. Not always in effect, even if enabled."

#define SIGMOID_CENTER_TEXT "Sigmoid center"
#define SIGMOID_CENTER_LONGTEXT "The center (bias) of the sigmoid curve."

#define SIGMOID_SLOPE_TEXT "Sigmoid slope"
#define SIGMOID_SLOPE_LONGTEXT "The slope (steepness) of the sigmoid curve."

#define BRIGHTNESS_TEXT "Brightness boost"
#define BRIGHTNESS_LONGTEXT "Raises the black level of the video signal."

#define CONTRAST_TEXT "Contrast scale"
#define CONTRAST_LONGTEXT "Scales the output intensity of the video signal."

#define SATURATION_TEXT "Saturation gain"
#define SATURATION_LONGTEXT "Scales the saturation (chromaticity) of the video signal."

#define GAMMA_TEXT "Gamma factor"
#define GAMMA_LONGTEXT "Makes the video signal's gamma curve steeper or shallower."

#define HUE_TEXT "Hue shift"
#define HUE_LONGTEXT "Rotates the hue vector of the video signal, specified in radians. Not effective for all sources."

// XXX: code duplication with opengl/vout_helper.h
#define INTENT_TEXT "Rendering intent for color conversion"
#define INTENT_LONGTEXT "The mapping type used to convert between color spaces."

static const int intent_values[] = {
    PL_INTENT_PERCEPTUAL,
    PL_INTENT_RELATIVE_COLORIMETRIC,
    PL_INTENT_SATURATION,
    PL_INTENT_ABSOLUTE_COLORIMETRIC,
};

static const char * const intent_text[] = {
    "Perceptual",
    "Relative colorimetric",
    "Absolute colorimetric",
    "Saturation",
};

#define TONEMAPPING_TEXT N_("Tone-mapping algorithm")
#define TONEMAPPING_LONGTEXT N_("Algorithm to use when converting from wide gamut to standard gamut, or from HDR to SDR.")

static const int tone_values[] = {
    PL_TONE_MAPPING_HABLE,
    PL_TONE_MAPPING_MOBIUS,
    PL_TONE_MAPPING_REINHARD,
    PL_TONE_MAPPING_GAMMA,
    PL_TONE_MAPPING_LINEAR,
    PL_TONE_MAPPING_CLIP,
};

static const char * const tone_text[] = {
    "Hable (filmic mapping, recommended)",
    "Mobius (linear + knee)",
    "Reinhard (simple non-linear)",
    "Gamma-Power law",
    "Linear stretch (peak to peak)",
    "Hard clip out-of-gamut",
};

#define TONEMAP_PARAM_TEXT "Tone-mapping parameter"
#define TONEMAP_PARAM_LONGTEXT "This parameter can be used to tune the tone-mapping curve. Specifics depend on the curve used. If left as 0, the curve's preferred default is used."

#define TONEMAP_DESAT_TEXT "Tone-mapping desaturation coefficient"
#define TONEMAP_DESAT_LONGTEXT "How strongly to desaturate bright spectral colors towards white. 0.0 disables this behavior."

#define GAMUT_WARN_TEXT "Highlight clipped pixels"
#define GAMUT_WARN_LONGTEXT "Debugging tool to indicate which pixels were clipped as part of the tone mapping process."

#define PEAK_FRAMES_TEXT "HDR peak detection buffer size"
#define PEAK_FRAMES_LONGTEXT "How many input frames to consider when determining the brightness of HDR signals. Higher values result in a slower/smoother response to brightness level changes. Setting this to 0 disables peak detection entirely."

#define SCENE_THRESHOLD_TEXT "HDR peak scene change threshold"
#define SCENE_THRESHOLD_LONGTEXT "When using HDR peak detection, this sets a threshold for sudden brightness changes that should be considered as scene changes. This will result in the detected peak being immediately updated to the new value, rather than gradually being adjusted. Setting this to 0 disables this feature."

#define DITHER_TEXT N_("Dithering algorithm")
#define DITHER_LONGTEXT N_("The algorithm to use when dithering to a lower bit depth.")

static const int dither_values[] = {
    -1, // no dithering
    PL_DITHER_BLUE_NOISE,
    PL_DITHER_ORDERED_FIXED,
    PL_DITHER_ORDERED_LUT,
    PL_DITHER_WHITE_NOISE,
};

static const char * const dither_text[] = {
    "Disabled",
    "Blue noise (high quality)",
    "Bayer matrix (ordered dither), 16x16 fixed size (fast)",
    "Bayer matrix (ordered dither), any size",
    "White noise (fast but low quality)",
};

#define DITHER_SIZE_TEXT "Dither LUT size (log 2)"
#define DITHER_SIZE_LONGTEXT "Controls the size of the dither matrix, as a power of two (e.g. the default of 6 corresponds to a 64x64 matrix). Does not affect all algorithms."

#define TEMPORAL_DITHER_TEXT "Temporal dithering"
#define TEMPORAL_DITHER_LONGTEXT "Enables perturbing the dither matrix across frames. This reduces the persistence of dithering artifacts, but can cause flickering on some (cheap) LCD screens."

#define POLAR_CUTOFF_TEXT "Cut-off value for polar samplers"
#define POLAR_CUTOFF_LONGTEXT "As a micro-optimization, all samples with a weight below this value will be ignored. This reduces the need to perform unnecessary work that doesn't noticeably change the resulting image. Setting it to a value of 0.0 disables this optimization."

#define SKIP_AA_TEXT "Disable anti-aliasing when downscaling"
#define SKIP_AA_LONGTEXT "This will result in moiré artifacts and nasty, jagged pixels when downscaling, except for some very limited special cases (e.g. bilinear downsampling to exactly 0.5x). Significantly speeds up downscaling with high downscaling ratios."

#define DISABLE_LINEAR_TEXT "Don't linearize before scaling"
#define DISABLE_LINEAR_LONGTEXT "Normally, the image is converted to linear light before scaling (under certain conditions). Enabling this option disables this behavior."

#define FORCE_GENERAL_TEXT "Force the use of general-purpose scalers"
#define FORCE_GENERAL_LONGTEXT "Normally, certain special scalers will be replaced by faster versions instead of going through the general scaler architecture. Enabling this option disables these optimizations."

vlc_module_begin () set_shortname (N_("Vulkan"))
    set_description (N_("Vulkan video output"))
    set_category (CAT_VIDEO)
    set_subcategory (SUBCAT_VIDEO_VOUT)
    set_capability ("vout display", 300)
    set_callbacks (Open, Close)
    add_shortcut ("vulkan", "vk")
    add_module ("vk", "vulkan", NULL,
                VK_TEXT, PROVIDER_LONGTEXT)

    set_section(N_("Upscaling"), NULL)
    // TODO: upscaler
    add_bool("sigmoid", !!pl_render_default_params.sigmoid_params,
            SIGMOID_TEXT, SIGMOID_LONGTEXT, true)
    add_float_with_range("sigmoid-center", pl_sigmoid_default_params.center,
            0., 1., SIGMOID_CENTER_TEXT, SIGMOID_CENTER_LONGTEXT, true)
    add_float_with_range("sigmoid-slope", pl_sigmoid_default_params.slope,
            1., 20., SIGMOID_SLOPE_TEXT, SIGMOID_SLOPE_LONGTEXT, true)

    set_section(N_("Downscaling"), NULL)
    // TODO: downscaler

    set_section(N_("Debanding"), NULL)
    add_integer("iterations", pl_deband_default_params.iterations,
            DEBAND_ITER_TEXT, DEBAND_ITER_LONGTEXT, false)
    add_float("threshold", pl_deband_default_params.threshold,
            DEBAND_THRESH_TEXT, DEBAND_THRESH_LONGTEXT, false)
    add_float("radius", pl_deband_default_params.radius,
            DEBAND_RADIUS_TEXT, DEBAND_RADIUS_LONGTEXT, false)
    add_float("grain", pl_deband_default_params.grain,
            DEBAND_GRAIN_TEXT, DEBAND_GRAIN_LONGTEXT, false)

    // XXX: This may not really make sense as a libplacebo-specified option.
    // Does VLC expose some generalized/shared color mixer settings somewhere?
    // TODO: turns out it does, need to fix
    set_section(N_("Color adjustment"), NULL)
    add_float_with_range("vkbrightness", pl_color_adjustment_neutral.brightness,
            -1., 1., BRIGHTNESS_TEXT, BRIGHTNESS_LONGTEXT, false)
    add_float_with_range("vksaturation", pl_color_adjustment_neutral.saturation,
            0., 10., SATURATION_TEXT, SATURATION_LONGTEXT, false)
    add_float_with_range("vkcontrast", pl_color_adjustment_neutral.contrast,
            0., 10., CONTRAST_TEXT, CONTRAST_TEXT, false)
    add_float_with_range("vkgamma", pl_color_adjustment_neutral.gamma,
            0., 10., GAMMA_TEXT, GAMMA_LONGTEXT, false)
    add_float_with_range("vkhue", pl_color_adjustment_neutral.hue,
            -M_PI, M_PI, HUE_TEXT, HUE_LONGTEXT, false)

    // XXX: code duplication with opengl/vout_helper.h
    set_section(N_("Colorspace conversion"), NULL)
    add_integer("intent", pl_color_map_default_params.intent,
            INTENT_TEXT, INTENT_LONGTEXT, false)
            change_integer_list(intent_values, intent_text)
    add_integer("tone-mapping", pl_color_map_default_params.tone_mapping_algo,
            TONEMAPPING_TEXT, TONEMAPPING_LONGTEXT, false)
            change_integer_list(tone_values, tone_text)
    add_float("tone-mapping-param", pl_color_map_default_params.tone_mapping_param,
            TONEMAP_PARAM_TEXT, TONEMAP_PARAM_LONGTEXT, true)
    add_float("tone-mapping-desat", pl_color_map_default_params.tone_mapping_desaturate,
            TONEMAP_DESAT_TEXT, TONEMAP_DESAT_LONGTEXT, false)
    add_bool("gamut-warning", false, GAMUT_WARN_TEXT, GAMUT_WARN_LONGTEXT, true)
    add_integer_with_range("peak-frames", pl_color_map_default_params.peak_detect_frames,
            0, 255, PEAK_FRAMES_TEXT, PEAK_FRAMES_LONGTEXT, false)
    add_float_with_range("scene-threshold", pl_color_map_default_params.scene_threshold,
            0., 10., SCENE_THRESHOLD_TEXT, SCENE_THRESHOLD_LONGTEXT, false)

    set_section(N_("Dithering"), NULL)
    add_integer("dither", pl_dither_default_params.method,
            DITHER_TEXT, DITHER_LONGTEXT, false)
            change_integer_list(dither_values, dither_text)
    add_integer_with_range("dither-size", pl_dither_default_params.lut_size,
            1, 8, DITHER_SIZE_TEXT, DITHER_SIZE_LONGTEXT, false)
    add_bool("temporal-dither", pl_dither_default_params.temporal,
            TEMPORAL_DITHER_TEXT, TEMPORAL_DITHER_LONGTEXT, false)

    // TODO: support for ICC profiles / 3DLUTs.. we will need some way of loading
    // this from the operating system / user

    set_section(N_("Performance tweaks / debugging"), NULL)
    add_bool("skip-aa", false, SKIP_AA_TEXT, SKIP_AA_LONGTEXT, false)
    add_float_with_range("polar-cutoff", 0.001,
            0., 1., POLAR_CUTOFF_TEXT, POLAR_CUTOFF_LONGTEXT, false)
    //add_bool("overlay-direct", false, OVERLAY_DIRECT_TEXT, OVERLAY_DIRECT_LONGTEXT, false) // TODO: implement overlay first
    add_bool("disable-linear", false, DISABLE_LINEAR_TEXT, DISABLE_LINEAR_LONGTEXT, false)
    add_bool("force-general", false, FORCE_GENERAL_TEXT, FORCE_GENERAL_LONGTEXT, false)

vlc_module_end ()
