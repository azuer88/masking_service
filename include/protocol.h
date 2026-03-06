#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <limits.h>

/* Unix socket path — created under systemd's RuntimeDirectory */
#define SOCKET_DIR  "/run/masking_service"
#define SOCKET_PATH "/run/masking_service/masking.sock"

/* Default blur radius when none is specified */
#define DEFAULT_BLUR_RADIUS 15

/* Maximum number of pending connections */
#define BACKLOG 10

/*
 * Request sent by the client to the service.
 *
 * target_path  – absolute path to the image to process
 * mask_path    – absolute path to the mask image (white = blur, black = keep)
 * output_path  – absolute path where the result should be written
 * blur_radius  – box-blur radius in pixels; applied 3× for Gaussian approximation
 */
typedef struct {
    char target_path[PATH_MAX];
    char mask_path[PATH_MAX];
    char output_path[PATH_MAX];
    int  blur_radius;
} MaskRequest;

/*
 * Response sent by the service back to the client.
 *
 * status  – 0 on success, negative error code otherwise
 * message – human-readable status string
 */
typedef struct {
    int  status;
    char message[512];
} MaskResponse;

/* Error codes */
#define ERR_LOAD_TARGET  -1   /* failed to load target image  */
#define ERR_LOAD_MASK    -2   /* failed to load mask image    */
#define ERR_WRITE_OUTPUT -3   /* failed to write output image */
#define ERR_OOM          -4   /* out of memory                */

#endif /* PROTOCOL_H */
