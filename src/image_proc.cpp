/*
 * image_proc.cpp – image masking/blurring with optional GPU acceleration
 *
 * Backend priority (selected once at startup, logged to syslog):
 *
 *   1. ARM Compute Library (HAVE_ACL)  — direct Mali OpenCL via ACL.
 *      The expensive Gaussian blur is dispatched to the GPU; de-interleave,
 *      blend, and I/O remain on the CPU.  Requires ACL ≥ 22.x.
 *
 *   2. OpenCV UMat (HAVE_OPENCV)  — OpenCL through OpenCV's transparent
 *      GPU/CPU dispatch; both blur and blend run on-device.
 *
 *   3. CPU fallback  — always compiled in.  2-D integral-image box blur,
 *      three passes to approximate a Gaussian.
 *
 * Build flags injected by the Makefile:
 *   -DHAVE_ACL     + ACL include/link flags  (arm-compute-library pkg-config)
 *   -DHAVE_OPENCV  + OpenCV include/link flags (opencv4 / opencv pkg-config)
 *
 * I/O in all three paths uses stb_image / stb_image_write (already vendored).
 */

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

extern "C" {
#include "image_proc.h"
#include "../include/protocol.h"
}

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasecmp */
#include <errno.h>
#include <syslog.h>
#include <pthread.h>

/* ── optional ACL headers ─────────────────────────────────────────────── */
#ifdef HAVE_ACL
#include <arm_compute/runtime/CL/CLScheduler.h>
#include <arm_compute/runtime/CL/CLTensor.h>
#include <arm_compute/runtime/CL/functions/CLGaussian5x5.h>
#include <arm_compute/core/TensorInfo.h>
#include <arm_compute/core/Types.h>
#endif

/* ── optional OpenCV headers ──────────────────────────────────────────── */
#ifdef HAVE_OPENCV
#include <opencv2/opencv.hpp>
#include <opencv2/core/ocl.hpp>
#include <vector>
#endif

/* =========================================================================
 * Backend selection
 * ======================================================================= */

enum Backend { BACKEND_CPU, BACKEND_OPENCV, BACKEND_ACL };

static Backend        g_backend   = BACKEND_CPU;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;

static void detect_backend(void)
{
#ifdef HAVE_ACL
    try {
        arm_compute::CLScheduler::get().default_init();
        g_backend = BACKEND_ACL;
        syslog(LOG_INFO,
               "masking_service: ARM Compute Library GPU backend active");
        return;
    } catch (...) {
        syslog(LOG_INFO,
               "masking_service: ARM Compute Library unavailable, "
               "trying OpenCV");
    }
#endif

#ifdef HAVE_OPENCV
    cv::ocl::setUseOpenCL(true);
    if (cv::ocl::haveOpenCL() && cv::ocl::useOpenCL()) {
        cv::ocl::Context &ctx = cv::ocl::Context::getDefault();
        const char *name = (ctx.ndevices() > 0)
                           ? ctx.device(0).name().c_str() : "unknown device";
        syslog(LOG_INFO,
               "masking_service: OpenCV OpenCL backend active (%s)", name);
        g_backend = BACKEND_OPENCV;
        return;
    }
    syslog(LOG_INFO,
           "masking_service: OpenCV OpenCL unavailable, using CPU");
#endif

    syslog(LOG_INFO, "masking_service: CPU backend active");
}

/* =========================================================================
 * Helpers shared by all paths
 * ======================================================================= */

/* Nearest-neighbour mask resize.  Returns new buffer (caller frees) or NULL. */
static unsigned char *resize_mask_nn(const unsigned char *src,
                                     int mw, int mh, int tw, int th)
{
    unsigned char *dst = (unsigned char *)malloc((size_t)tw * th);
    if (!dst) return NULL;
    for (int y = 0; y < th; y++) {
        int sy = (y * mh) / th;
        for (int x = 0; x < tw; x++)
            dst[y * tw + x] = src[sy * mw + (x * mw) / tw];
    }
    return dst;
}

/* =========================================================================
 * ACL backend
 * ======================================================================= */
#ifdef HAVE_ACL

/*
 * Copy a contiguous plane (w×h bytes) into a CLTensor, accounting for any
 * row padding ACL may have introduced.
 */
static void plane_to_tensor(arm_compute::CLTensor &t,
                             const uint8_t *plane, int w, int h)
{
    t.map(true);
    uint8_t       *buf    = reinterpret_cast<uint8_t *>(t.buffer());
    const size_t   stride = t.info()->strides_in_bytes()[1];
    for (int y = 0; y < h; ++y)
        memcpy(buf + y * stride, plane + y * w, w);
    t.unmap();
}

static void tensor_to_plane(const arm_compute::CLTensor &t,
                             uint8_t *plane, int w, int h)
{
    const_cast<arm_compute::CLTensor &>(t).map(true);
    const uint8_t *buf    = reinterpret_cast<const uint8_t *>(t.buffer());
    const size_t   stride = t.info()->strides_in_bytes()[1];
    for (int y = 0; y < h; ++y)
        memcpy(plane + y * w, buf + y * stride, w);
    const_cast<arm_compute::CLTensor &>(t).unmap();
}

/*
 * Blur a single-channel U8 plane on the GPU using CLGaussian5x5.
 * Applied blur_passes times (three passes ≈ the original three-pass box blur).
 * Returns false on any ACL error.
 */
static bool acl_blur_plane(const uint8_t *src, uint8_t *dst,
                            int w, int h, int blur_passes)
{
    using namespace arm_compute;

    TensorInfo plane_info(TensorShape((unsigned int)w, (unsigned int)h),
                          1, DataType::U8);

    CLTensor ping, pong;
    ping.allocator()->init(plane_info);
    pong.allocator()->init(plane_info);

    /* Configure three CLGaussian5x5 instances to ping-pong across the same
     * pair of tensors.  ACL functions must be configured before allocation. */
    CLGaussian5x5 pass1, pass2, pass3;
    pass1.configure(&ping, &pong, BorderMode::REFLECT);
    pass2.configure(&pong, &ping, BorderMode::REFLECT);
    pass3.configure(&ping, &pong, BorderMode::REFLECT);

    ping.allocator()->allocate();
    pong.allocator()->allocate();

    plane_to_tensor(ping, src, w, h);

    for (int i = 0; i < blur_passes; ++i) {
        pass1.run();          /* ping → pong */
        if (i + 1 < blur_passes) {
            pass2.run();      /* pong → ping */
            if (i + 2 < blur_passes)
                pass3.run();  /* ping → pong */
        }
    }

    /* Result lands in pong for odd pass counts, ping for even. */
    CLTensor &result = (blur_passes % 2 == 1) ? pong : ping;
    tensor_to_plane(result, dst, w, h);
    return true;
}

static int apply_mask_blur_acl(const char *target_path,
                                const char *mask_path,
                                const char *output_path,
                                int         blur_radius)
{
    int tw, th, tc;
    unsigned char *target = stbi_load(target_path, &tw, &th, &tc, 0);
    if (!target) {
        syslog(LOG_ERR, "ACL: failed to load target: %s (%s)",
               target_path, stbi_failure_reason());
        return ERR_LOAD_TARGET;
    }

    int mw, mh, mc;
    unsigned char *mask_raw = stbi_load(mask_path, &mw, &mh, &mc, 1);
    if (!mask_raw) {
        syslog(LOG_ERR, "ACL: failed to load mask: %s (%s)",
               mask_path, stbi_failure_reason());
        stbi_image_free(target);
        return ERR_LOAD_MASK;
    }

    unsigned char *mask;
    if (mw == tw && mh == th) {
        mask = mask_raw;
    } else {
        mask = resize_mask_nn(mask_raw, mw, mh, tw, th);
        stbi_image_free(mask_raw);
        if (!mask) { stbi_image_free(target); return ERR_OOM; }
    }

    const size_t plane_sz  = (size_t)tw * th;
    const size_t img_sz    = plane_sz * tc;

    unsigned char *src_plane  = (unsigned char *)malloc(plane_sz);
    unsigned char *blur_plane = (unsigned char *)malloc(plane_sz);
    unsigned char *result     = (unsigned char *)malloc(img_sz);
    if (!src_plane || !blur_plane || !result) {
        free(src_plane); free(blur_plane); free(result);
        if (mask != mask_raw) free(mask); else stbi_image_free(mask_raw);
        stbi_image_free(target);
        return ERR_OOM;
    }
    memcpy(result, target, img_sz);

    /*
     * Map blur_radius to a number of 5×5 Gaussian passes.
     * Three passes of a 5×5 kernel approximates the original three-pass
     * box blur for typical radii; scale up for larger radii.
     * Minimum one pass; no hard upper bound (caller controls radius).
     */
    int passes = (blur_radius < 3) ? 1 : (blur_radius < 7) ? 3 : blur_radius / 2;

    for (int c = 0; c < tc; c++) {
        /* De-interleave channel c */
        for (int i = 0; i < tw * th; i++)
            src_plane[i] = target[i * tc + c];

        if (!acl_blur_plane(src_plane, blur_plane, tw, th, passes)) {
            syslog(LOG_ERR, "ACL: blur failed on channel %d", c);
            free(src_plane); free(blur_plane); free(result);
            if (mask != mask_raw) free(mask); else stbi_image_free(mask_raw);
            stbi_image_free(target);
            return ERR_OOM;
        }

        /* Blend: result[i] = orig[i] + (blur[i] − orig[i]) × mask[i]/255 */
        for (int i = 0; i < tw * th; i++) {
            float alpha = mask[i] / 255.0f;
            float orig  = src_plane [i];
            float blur  = blur_plane[i];
            result[i * tc + c] = (unsigned char)(orig + (blur - orig) * alpha);
        }
    }

    free(src_plane);
    free(blur_plane);

    int ret = 0;
    const char *ext = strrchr(output_path, '.');
    if (ext && strcasecmp(ext, ".jpg") == 0)
        ret = stbi_write_jpg(output_path, tw, th, tc, result, 92) ? 0 : ERR_WRITE_OUTPUT;
    else if (ext && strcasecmp(ext, ".jpeg") == 0)
        ret = stbi_write_jpg(output_path, tw, th, tc, result, 92) ? 0 : ERR_WRITE_OUTPUT;
    else if (ext && strcasecmp(ext, ".bmp") == 0)
        ret = stbi_write_bmp(output_path, tw, th, tc, result)     ? 0 : ERR_WRITE_OUTPUT;
    else
        ret = stbi_write_png(output_path, tw, th, tc, result, tw * tc) ? 0 : ERR_WRITE_OUTPUT;

    if (ret != 0)
        syslog(LOG_ERR, "ACL: failed to write output: %s (%s)",
               output_path, strerror(errno));

    free(result);
    if (mask != mask_raw) free(mask); else stbi_image_free(mask_raw);
    stbi_image_free(target);
    return ret;
}
#endif /* HAVE_ACL */

/* =========================================================================
 * OpenCV backend
 * ======================================================================= */
#ifdef HAVE_OPENCV

static int apply_mask_blur_opencv(const char *target_path,
                                   const char *mask_path,
                                   const char *output_path,
                                   int         blur_radius)
{
    cv::Mat target_mat = cv::imread(target_path, cv::IMREAD_UNCHANGED);
    if (target_mat.empty()) {
        syslog(LOG_ERR, "OpenCV: failed to load target: %s", target_path);
        return ERR_LOAD_TARGET;
    }

    cv::Mat mask_mat = cv::imread(mask_path, cv::IMREAD_GRAYSCALE);
    if (mask_mat.empty()) {
        syslog(LOG_ERR, "OpenCV: failed to load mask: %s", mask_path);
        return ERR_LOAD_MASK;
    }

    if (mask_mat.size() != target_mat.size())
        cv::resize(mask_mat, mask_mat, target_mat.size(),
                   0, 0, cv::INTER_NEAREST);

    /* Upload to GPU (UMat) — transparent CPU fallback if OpenCL is off */
    cv::UMat u_target, u_mask;
    target_mat.copyTo(u_target);
    mask_mat.copyTo(u_mask);

    /* Three-pass Gaussian blur, matching the original three-pass box blur */
    int ksize = 2 * blur_radius + 1;
    cv::UMat u_blurred;
    cv::GaussianBlur(u_target,  u_blurred, cv::Size(ksize, ksize), 0);
    cv::GaussianBlur(u_blurred, u_blurred, cv::Size(ksize, ksize), 0);
    cv::GaussianBlur(u_blurred, u_blurred, cv::Size(ksize, ksize), 0);

    /* Per-pixel blend: result = orig + (blur − orig) × mask/255 */
    cv::UMat u_orig_f, u_blur_f, u_mask_f;
    u_target.convertTo(u_orig_f, CV_32F);
    u_blurred.convertTo(u_blur_f, CV_32F);
    u_mask.convertTo(u_mask_f, CV_32F, 1.0 / 255.0);

    int nc = target_mat.channels();
    std::vector<cv::UMat> mask_planes(nc, u_mask_f);
    cv::UMat u_mask_ch;
    cv::merge(mask_planes, u_mask_ch);

    cv::UMat diff, u_result_f, u_result;
    cv::subtract(u_blur_f, u_orig_f, diff);
    cv::multiply(diff, u_mask_ch, diff);
    cv::add(u_orig_f, diff, u_result_f);
    u_result_f.convertTo(u_result, CV_8U);

    cv::Mat result;
    u_result.copyTo(result);

    const char *ext = strrchr(output_path, '.');
    std::vector<int> params;
    if (ext && (strcasecmp(ext, ".jpg")  == 0 ||
                strcasecmp(ext, ".jpeg") == 0))
        params = { cv::IMWRITE_JPEG_QUALITY, 92 };

    if (!cv::imwrite(output_path, result, params)) {
        syslog(LOG_ERR, "OpenCV: failed to write output: %s", output_path);
        return ERR_WRITE_OUTPUT;
    }

    return 0;
}
#endif /* HAVE_OPENCV */

/* =========================================================================
 * CPU backend  (original O(w×h) integral-image box blur)
 * ======================================================================= */

static void box_blur_once(const unsigned char *src, unsigned char *dst,
                          int w, int h, int ch, int r)
{
    int64_t *ii = (int64_t *)calloc((size_t)w * h, sizeof(int64_t));
    if (!ii) return;

    for (int c = 0; c < ch; c++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int64_t v = src[(y * w + x) * ch + c];
                if (y > 0) v += ii[(y - 1) * w + x];
                if (x > 0) v += ii[y * w + (x - 1)];
                if (y > 0 && x > 0) v -= ii[(y - 1) * w + (x - 1)];
                ii[y * w + x] = v;
            }
        }
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

static int apply_mask_blur_cpu(const char *target_path,
                                const char *mask_path,
                                const char *output_path,
                                int         blur_radius)
{
    int tw, th, tc;
    unsigned char *target = stbi_load(target_path, &tw, &th, &tc, 0);
    if (!target) {
        syslog(LOG_ERR, "Failed to load target image: %s (%s)",
               target_path, stbi_failure_reason());
        return ERR_LOAD_TARGET;
    }

    int mw, mh, mc;
    unsigned char *mask_raw = stbi_load(mask_path, &mw, &mh, &mc, 1);
    if (!mask_raw) {
        syslog(LOG_ERR, "Failed to load mask image: %s (%s)",
               mask_path, stbi_failure_reason());
        stbi_image_free(target);
        return ERR_LOAD_MASK;
    }

    unsigned char *mask;
    int mask_needs_free;
    if (mw == tw && mh == th) {
        mask = mask_raw;
        mask_needs_free = 0;
    } else {
        mask = resize_mask_nn(mask_raw, mw, mh, tw, th);
        stbi_image_free(mask_raw);
        if (!mask) { stbi_image_free(target); return ERR_OOM; }
        mask_needs_free = 1;
    }

    size_t img_sz = (size_t)tw * th * tc;
    unsigned char *blurred = (unsigned char *)malloc(img_sz);
    if (!blurred) {
        if (mask_needs_free) free(mask); else stbi_image_free(mask);
        stbi_image_free(target);
        return ERR_OOM;
    }
    memcpy(blurred, target, img_sz);

    unsigned char *tmp = (unsigned char *)malloc(img_sz);
    if (!tmp) {
        free(blurred);
        if (mask_needs_free) free(mask); else stbi_image_free(mask);
        stbi_image_free(target);
        return ERR_OOM;
    }
    box_blur_once(blurred, tmp, tw, th, tc, blur_radius);
    box_blur_once(tmp, blurred, tw, th, tc, blur_radius);
    box_blur_once(blurred, tmp, tw, th, tc, blur_radius);
    memcpy(blurred, tmp, img_sz);
    free(tmp);

    for (int i = 0; i < tw * th; i++) {
        float alpha = mask[i] / 255.0f;
        for (int c = 0; c < tc; c++) {
            float orig = target [i * tc + c];
            float blur = blurred[i * tc + c];
            target[i * tc + c] = (unsigned char)(orig * (1.0f - alpha) + blur * alpha);
        }
    }

    int ret = 0;
    const char *ext = strrchr(output_path, '.');
    if (ext && strcasecmp(ext, ".jpg") == 0)
        ret = stbi_write_jpg(output_path, tw, th, tc, target, 92) ? 0 : ERR_WRITE_OUTPUT;
    else if (ext && strcasecmp(ext, ".jpeg") == 0)
        ret = stbi_write_jpg(output_path, tw, th, tc, target, 92) ? 0 : ERR_WRITE_OUTPUT;
    else if (ext && strcasecmp(ext, ".bmp") == 0)
        ret = stbi_write_bmp(output_path, tw, th, tc, target)     ? 0 : ERR_WRITE_OUTPUT;
    else
        ret = stbi_write_png(output_path, tw, th, tc, target, tw * tc) ? 0 : ERR_WRITE_OUTPUT;

    if (ret != 0)
        syslog(LOG_ERR, "Failed to write output image: %s (%s)",
               output_path, strerror(errno));

    free(blurred);
    if (mask_needs_free) free(mask); else stbi_image_free(mask);
    stbi_image_free(target);
    return ret;
}

/* =========================================================================
 * Public API — dispatches to the selected backend
 * ======================================================================= */

int apply_mask_blur(const char *target_path,
                    const char *mask_path,
                    const char *output_path,
                    int         blur_radius)
{
    pthread_once(&g_init_once, detect_backend);

    switch (g_backend) {
#ifdef HAVE_ACL
    case BACKEND_ACL:
        return apply_mask_blur_acl(target_path, mask_path,
                                   output_path, blur_radius);
#endif
#ifdef HAVE_OPENCV
    case BACKEND_OPENCV:
        return apply_mask_blur_opencv(target_path, mask_path,
                                      output_path, blur_radius);
#endif
    default:
        return apply_mask_blur_cpu(target_path, mask_path,
                                   output_path, blur_radius);
    }
}
