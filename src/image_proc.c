/*
 * image_proc.c – image masking/blurring implementation
 *
 * Dependencies: stb_image.h and stb_image_write.h (vendor/)
 * Build with -lm
 */

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "stb_image.h"
#include "stb_image_write.h"

#include "image_proc.h"
#include "../include/protocol.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <syslog.h>

/* -------------------------------------------------------------------------
 * Box blur using a 2-D integral image — O(w*h) regardless of radius.
 * -----------------------------------------------------------------------*/

/*
 * box_blur_once: apply one pass of a separable box blur to `src`, writing
 * the result to `dst`.  Both buffers are w*h*ch bytes, interleaved.
 */
static void box_blur_once(const unsigned char *src, unsigned char *dst,
                          int w, int h, int ch, int r)
{
    int64_t *ii = calloc((size_t)w * h, sizeof(int64_t));
    if (!ii) return;

    for (int c = 0; c < ch; c++) {

        /* Build integral image for this channel */
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int64_t v = src[(y * w + x) * ch + c];
                if (y > 0) v += ii[(y - 1) * w + x];
                if (x > 0) v += ii[y * w + (x - 1)];
                if (y > 0 && x > 0) v -= ii[(y - 1) * w + (x - 1)];
                ii[y * w + x] = v;
            }
        }

        /* Query the integral image for each output pixel */
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int ax1 = x - r; if (ax1 < 0) ax1 = 0;
                int ay1 = y - r; if (ay1 < 0) ay1 = 0;
                int ax2 = x + r; if (ax2 >= w) ax2 = w - 1;
                int ay2 = y + r; if (ay2 >= h) ay2 = h - 1;

                int64_t sum = ii[ay2 * w + ax2];
                if (ax1 > 0) sum -= ii[ay2 * w + (ax1 - 1)];
                if (ay1 > 0) sum -= ii[(ay1 - 1) * w + ax2];
                if (ax1 > 0 && ay1 > 0) sum += ii[(ay1 - 1) * w + (ax1 - 1)];

                int area = (ax2 - ax1 + 1) * (ay2 - ay1 + 1);
                dst[(y * w + x) * ch + c] = (unsigned char)(sum / area);
            }
        }
    }

    free(ii);
}

/*
 * gaussian_blur: approximate Gaussian blur via 3 passes of box blur.
 * Modifies `img` in-place.
 */
static int gaussian_blur(unsigned char *img, int w, int h, int ch, int r)
{
    size_t sz = (size_t)w * h * ch;
    unsigned char *tmp = malloc(sz);
    if (!tmp) return -1;

    box_blur_once(img, tmp, w, h, ch, r);   /* pass 1: img  → tmp */
    box_blur_once(tmp, img, w, h, ch, r);   /* pass 2: tmp  → img */
    box_blur_once(img, tmp, w, h, ch, r);   /* pass 3: img  → tmp */
    memcpy(img, tmp, sz);

    free(tmp);
    return 0;
}

/* -------------------------------------------------------------------------
 * Nearest-neighbour mask resize
 * -----------------------------------------------------------------------*/

/*
 * resize_mask_nn: resample `src` (mw×mh, 1 channel) into a new buffer of
 * tw×th using nearest-neighbour sampling.  Returns the new buffer (caller
 * must free) or NULL on OOM.
 */
static unsigned char *resize_mask_nn(const unsigned char *src,
                                     int mw, int mh,
                                     int tw, int th)
{
    unsigned char *dst = malloc((size_t)tw * th);
    if (!dst) return NULL;

    for (int y = 0; y < th; y++) {
        int sy = (int)((y * mh) / th);
        for (int x = 0; x < tw; x++) {
            int sx = (int)((x * mw) / tw);
            dst[y * tw + x] = src[sy * mw + sx];
        }
    }
    return dst;
}

/* -------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int apply_mask_blur(const char *target_path,
                    const char *mask_path,
                    const char *output_path,
                    int         blur_radius)
{
    int tw, th, tc;
    unsigned char *target = stbi_load(target_path, &tw, &th, &tc, 0);
    if (!target) {
        syslog(LOG_ERR, "Failed to load target image: %s", target_path);
        return ERR_LOAD_TARGET;
    }

    int mw, mh, mc;
    unsigned char *mask_raw = stbi_load(mask_path, &mw, &mh, &mc, 1);
    if (!mask_raw) {
        syslog(LOG_ERR, "Failed to load mask image: %s", mask_path);
        stbi_image_free(target);
        return ERR_LOAD_MASK;
    }

    /* Resize mask to target dimensions if needed */
    unsigned char *mask;
    int mask_needs_free;

    if (mw == tw && mh == th) {
        mask = mask_raw;
        mask_needs_free = 0;
    } else {
        mask = resize_mask_nn(mask_raw, mw, mh, tw, th);
        stbi_image_free(mask_raw);
        if (!mask) {
            stbi_image_free(target);
            return ERR_OOM;
        }
        mask_needs_free = 1;
    }

    /* Create a blurred copy of the target */
    size_t img_sz = (size_t)tw * th * tc;
    unsigned char *blurred = malloc(img_sz);
    if (!blurred) {
        if (mask_needs_free) free(mask);
        else stbi_image_free(mask);
        stbi_image_free(target);
        return ERR_OOM;
    }
    memcpy(blurred, target, img_sz);

    if (gaussian_blur(blurred, tw, th, tc, blur_radius) != 0) {
        free(blurred);
        if (mask_needs_free) free(mask);
        else stbi_image_free(mask);
        stbi_image_free(target);
        return ERR_OOM;
    }

    /*
     * Blend: output[p] = original[p] * (1 - α) + blurred[p] * α
     * where α = mask[p] / 255
     */
    for (int i = 0; i < tw * th; i++) {
        float alpha = mask[i] / 255.0f;
        for (int c = 0; c < tc; c++) {
            float orig  = target [i * tc + c];
            float blur  = blurred[i * tc + c];
            target[i * tc + c] = (unsigned char)(orig * (1.0f - alpha) + blur * alpha);
        }
    }

    /* Write output — format inferred from extension */
    int ret = 0;
    const char *ext = strrchr(output_path, '.');

    if (ext && strcasecmp(ext, ".jpg") == 0) {
        ret = stbi_write_jpg(output_path, tw, th, tc, target, 92)
              ? 0 : ERR_WRITE_OUTPUT;
    } else if (ext && strcasecmp(ext, ".jpeg") == 0) {
        ret = stbi_write_jpg(output_path, tw, th, tc, target, 92)
              ? 0 : ERR_WRITE_OUTPUT;
    } else if (ext && strcasecmp(ext, ".bmp") == 0) {
        ret = stbi_write_bmp(output_path, tw, th, tc, target)
              ? 0 : ERR_WRITE_OUTPUT;
    } else {
        /* Default: PNG */
        ret = stbi_write_png(output_path, tw, th, tc, target, tw * tc)
              ? 0 : ERR_WRITE_OUTPUT;
    }

    if (ret != 0)
        syslog(LOG_ERR, "Failed to write output image: %s", output_path);

    free(blurred);
    if (mask_needs_free) free(mask);
    else stbi_image_free(mask);
    stbi_image_free(target);

    return ret;
}
