#ifndef IMAGE_PROC_H
#define IMAGE_PROC_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * apply_mask_blur
 *
 * Loads `target_path` and `mask_path`, blurs the target image in regions
 * where the mask is white (or bright), and saves the result to `output_path`.
 *
 * The mask is loaded as grayscale. Each output pixel is the lerp:
 *   output = original * (1 - mask/255) + blurred * (mask/255)
 *
 * `blur_radius` is the box-blur radius; three passes are applied for a
 * Gaussian approximation.  Output format is inferred from `output_path`'s
 * extension (.png, .jpg/.jpeg); defaults to PNG.
 *
 * Returns 0 on success, or a negative ERR_* code on failure.
 */
int apply_mask_blur(const char *target_path,
                    const char *mask_path,
                    const char *output_path,
                    int         blur_radius);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_PROC_H */
