/*****************************************************************************
 * orient.c: image reorientation video conversion
 *****************************************************************************
 * Copyright (C) 2000-2006 VLC authors and VideoLAN
 * Copyright (C) 2010 Laurent Aimar
 * Copyright (C) 2012 Rémi Denis-Courmont
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
 *          Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
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
#   include "config.h"
#endif
#include <limits.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_mouse.h>
#include <vlc_picture.h>

static void vflip(void *restrict dst, ptrdiff_t dst_stride,
                  const void *restrict src, ptrdiff_t src_stride,
                  int width, int height, int order)
{
    const unsigned char *src_pixels = src;
    unsigned char *restrict dst_pixels = dst;
    size_t visible_pitch = width << order;

    dst_pixels += dst_stride * height;

    for (int y = 0; y < height; y++) {
        dst_pixels -= dst_stride;
        memcpy(dst_pixels, src_pixels, visible_pitch);
        src_pixels += src_stride;
    }
}

#define TRANSFORMS(bits) \
static void hflip_##bits(void *restrict dst, ptrdiff_t dst_stride, \
                         const void *restrict src, ptrdiff_t src_stride, \
                         int width, int height) \
{ \
    const uint##bits##_t *restrict src_pixels = src; \
    uint##bits##_t *restrict dst_pixels = dst; \
\
    dst_stride /= bits / 8; \
    src_stride /= bits / 8; \
    dst_pixels += width - 1; \
\
    for (int y = 0; y < height; y++) { \
        for (int x = 0; x < width; x++) \
            dst_pixels[-x] = src_pixels[x]; \
\
        src_pixels += src_stride; \
        dst_pixels += dst_stride; \
    } \
} \
\
static void vflip_##bits(void *restrict dst, ptrdiff_t dst_stride, \
                         const void *restrict src, ptrdiff_t src_stride, \
                         int width, int height) \
{ \
    vflip(dst, dst_stride, src, src_stride, width, height, ctz(bits / 8)); \
} \
\
static void r180_##bits(void *restrict dst, ptrdiff_t dst_stride, \
                        const void *restrict src, ptrdiff_t src_stride, \
                        int width, int height) \
{ \
    const unsigned char *src_pixels = src; \
\
    src_pixels += (height - 1) * src_stride; \
    src_stride *= -1; \
    hflip_##bits(dst, dst_stride, src_pixels, src_stride, width, height); \
} \
\
static void transpose_##bits(void *restrict dst, ptrdiff_t dst_stride, \
                             const void *restrict src, ptrdiff_t src_stride, \
                             int src_width, int src_height) \
{ \
    const uint##bits##_t *restrict src_pixels = src; \
    uint##bits##_t *restrict dst_pixels = dst; \
\
    dst_stride /= bits / 8; \
    src_stride /= bits / 8; \
\
    for (int y = 0; y < src_height; y++) { \
        for (int x = 0; x < src_width; x++) \
            dst_pixels[x * dst_stride] = src_pixels[x]; \
        src_pixels += src_stride; \
        dst_pixels++; \
    } \
} \
\
static void r270_##bits(void *restrict dst, ptrdiff_t dst_stride, \
                        const void *restrict src, ptrdiff_t src_stride, \
                        int src_width, int src_height) \
{ \
    unsigned char *dst_pixels = dst; \
\
    dst_pixels += (src_width - 1) * dst_stride; \
    dst_stride *= -1; \
    transpose_##bits(dst_pixels, dst_stride, src, src_stride, \
                     src_width, src_height); \
} \
\
static void r90_##bits(void *restrict dst, ptrdiff_t dst_stride, \
                       const void *restrict src, ptrdiff_t src_stride, \
                       int src_width, int src_height) \
{ \
    const unsigned char *src_pixels = src; \
\
    src_pixels += (src_height - 1) * src_stride; \
    src_stride *= -1; \
    transpose_##bits(dst, dst_stride, src_pixels, src_stride, \
                     src_width, src_height); \
} \
\
static void antitranspose_##bits(void *restrict dst, ptrdiff_t dst_stride, \
                                 const void *restrict src, \
                                 ptrdiff_t src_stride, \
                                 int src_width, int src_height) \
{ \
    const unsigned char *src_pixels = src; \
\
    src_pixels += (src_height - 1) * src_stride; \
    src_stride *= -1; \
    r270_##bits(dst, dst_stride, src_pixels, src_stride, \
                src_width, src_height);\
}

TRANSFORMS(8)
TRANSFORMS(16)
TRANSFORMS(32)

typedef void (*plane_transform_cb)(void *, ptrdiff_t, const void *, ptrdiff_t,
                                   int, int);

typedef struct {
    plane_transform_cb plane8;
    plane_transform_cb plane16;
    plane_transform_cb plane32;
} transform_description_t;

#define DESC(g) \
    { g##_8, g##_16, g##_32, }

static const transform_description_t descriptions[] = {
    [TRANSFORM_R90] =            DESC(r90),
    [TRANSFORM_R180] =           DESC(r180),
    [TRANSFORM_R270] =           DESC(r270),
    [TRANSFORM_HFLIP] =          DESC(hflip),
    [TRANSFORM_VFLIP] =          DESC(vflip),
    [TRANSFORM_TRANSPOSE] =      DESC(transpose),
    [TRANSFORM_ANTI_TRANSPOSE] = DESC(antitranspose),
};

typedef struct
{
    video_transform_t transform;
    plane_transform_cb plane[PICTURE_PLANE_MAX];
} filter_sys_t;

static picture_t *Filter(filter_t *filter, picture_t *src)
{
    filter_sys_t *sys = filter->p_sys;
    picture_t *dst = filter_NewPicture(filter);

    if (likely(dst != NULL)) {
        for (int i = 0; i < src->i_planes; i++)
            (sys->plane[i])(dst->p[i].p_pixels, dst->p[i].i_pitch,
                            src->p[i].p_pixels, src->p[i].i_pitch,
                            src->p[i].i_visible_pitch / src->p[i].i_pixel_pitch,
                            src->p[i].i_visible_lines);

        picture_CopyProperties(dst, src);
    }

    picture_Release(src);
    return dst;
}

static int Mouse(filter_t *filter, vlc_mouse_t *mouse,
                 const vlc_mouse_t *mold)
{
    VLC_UNUSED( mold );

    const video_format_t *fmt = &filter->fmt_out.video;
    const filter_sys_t   *sys = filter->p_sys;
    int dw = fmt->i_visible_width, dh = fmt->i_visible_height;
    int dx = mouse->i_x, dy = mouse->i_y;
    int sx, sy;

    switch (sys->transform) {
        case TRANSFORM_HFLIP:
        case TRANSFORM_R180:
            sx = dw - 1 - dx;
            break;
        //case TRANSFORM_IDENTITY:
        case TRANSFORM_VFLIP:
            sx = dx;
            break;
        case TRANSFORM_TRANSPOSE:
        case TRANSFORM_R90:
            sx = dy;
            break;
        case TRANSFORM_R270:
        case TRANSFORM_ANTI_TRANSPOSE:
            sx = dh - 1 - dy;
            break;
        default:
            vlc_assert_unreachable();
    }

    switch (sys->transform) {
        //case TRANSFORM_IDENTITY:
        case TRANSFORM_HFLIP:
            sy = dy;
            break;
        case TRANSFORM_VFLIP:
        case TRANSFORM_R180:
            sy = dh - 1 - dy;
            break;
        case TRANSFORM_TRANSPOSE:
        case TRANSFORM_R270:
            sy = dx;
            break;
        case TRANSFORM_R90:
        case TRANSFORM_ANTI_TRANSPOSE:
            sy = dw - 1 - dx;
            break;
        default:
            vlc_assert_unreachable();
    }

    mouse->i_x = sx;
    mouse->i_y = sy;
    return VLC_SUCCESS;
}

static int Open(filter_t *filter)
{
    const video_format_t *src = &filter->fmt_in.video;
    video_format_t       *dst = &filter->fmt_out.video;
    video_transform_t transform = video_format_GetTransform(src->orientation,
                                                            dst->orientation);

    if (transform == TRANSFORM_IDENTITY)
        return VLC_ENOTSUP; /* Nothing for this module to work at */

    video_format_t src_trans = *src;
    video_format_TransformBy(&src_trans, transform);

    if (dst->i_chroma         != src_trans.i_chroma ||
        dst->i_width          != src_trans.i_width ||
        dst->i_visible_width  != src_trans.i_visible_width ||
        dst->i_height         != src_trans.i_height ||
        dst->i_visible_height != src_trans.i_visible_height ||
        dst->i_x_offset       != src_trans.i_x_offset ||
        dst->i_y_offset       != src_trans.i_y_offset)
        return VLC_ENOTSUP; /* This module cannot rescale */

    const vlc_chroma_description_t *chroma =
        vlc_fourcc_GetChromaDescription(src->i_chroma);
    if (chroma == NULL)
        return VLC_ENOTSUP;

    filter_sys_t *sys = vlc_obj_malloc(VLC_OBJECT(filter), sizeof(*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;

    const transform_description_t *const dsc = &descriptions[transform];

    sys->transform = transform;

    switch (chroma->pixel_size) {
        case 1:
            sys->plane[0] = dsc->plane8;
            break;
        case 2:
            sys->plane[0] = dsc->plane16;
            break;
        case 4:
            sys->plane[0] = dsc->plane32;
            break;
        default:
            return VLC_ENOTSUP;
    }

    for (unsigned i = 1; i < PICTURE_PLANE_MAX; i++)
        sys->plane[i] = sys->plane[0];

    if (ORIENT_IS_SWAP(transform)) /* Cannot transform non-square samples */
        for (unsigned i = 0; i < chroma->plane_count; i++)
            if (chroma->p[i].w.num * chroma->p[i].h.den
             != chroma->p[i].h.num * chroma->p[i].w.den)
                return VLC_ENOTSUP;

    /* Deal with weird packed formats */
    switch (src->i_chroma) {
        case VLC_CODEC_NV12:
        case VLC_CODEC_NV21:
            sys->plane[1] = dsc->plane16;
            break;
    }

    static const struct vlc_filter_operations filter_ops =
    {
        .filter_video = Filter,
        .video_mouse = Mouse,
    };
    filter->ops = &filter_ops;
    filter->p_sys           = sys;
    return VLC_SUCCESS;
}

vlc_module_begin()
    set_description(N_("Video reorientation"))
    set_shortname(N_("Reorient"))
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_callback_video_converter(Open, 200)
vlc_module_end()
