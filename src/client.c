/*
 * client.c – command-line client for masking_service
 *
 * Usage:
 *   masking_client <target_image> <mask_image> <output_image> [blur_radius]
 *
 * Example:
 *   masking_client photo.jpg mask.png blurred_photo.png 20
 *
 * The service must be running and the Unix socket must exist at SOCKET_PATH
 * (defined in protocol.h, typically /run/masking_service/masking.sock).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "../include/protocol.h"

int main(int argc, char *argv[])
{
    if (argc < 4 || argc > 5) {
        fprintf(stderr,
                "Usage: %s <target_image> <mask_image> <output_image> [blur_radius]\n"
                "\n"
                "  target_image  – image to be partially blurred\n"
                "  mask_image    – grayscale mask (white = blur, black = keep)\n"
                "  output_image  – where to save the result (PNG/JPG/BMP)\n"
                "  blur_radius   – box-blur radius in pixels (default: %d)\n",
                argv[0], DEFAULT_BLUR_RADIUS);
        return EXIT_FAILURE;
    }

    /* Connect to the service socket */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect(%s): %s\n", SOCKET_PATH, strerror(errno));
        fprintf(stderr, "Is masking_service running?\n");
        close(fd);
        return EXIT_FAILURE;
    }

    /* Build and send the request */
    MaskRequest req = {0};
    strncpy(req.target_path, argv[1], sizeof(req.target_path) - 1);
    strncpy(req.mask_path,   argv[2], sizeof(req.mask_path)   - 1);
    strncpy(req.output_path, argv[3], sizeof(req.output_path) - 1);
    req.blur_radius = (argc == 5) ? atoi(argv[4]) : DEFAULT_BLUR_RADIUS;

    if (req.blur_radius <= 0) {
        fprintf(stderr, "blur_radius must be a positive integer\n");
        close(fd);
        return EXIT_FAILURE;
    }

    ssize_t sent = send(fd, &req, sizeof(req), 0);
    if (sent != (ssize_t)sizeof(req)) {
        fprintf(stderr, "send: short write (%zd of %zu bytes)\n",
                sent, sizeof(req));
        close(fd);
        return EXIT_FAILURE;
    }

    /* Receive and print the response */
    MaskResponse resp = {0};
    ssize_t n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
    close(fd);

    if (n != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "recv: short read (%zd bytes)\n", n);
        return EXIT_FAILURE;
    }

    if (resp.status == 0) {
        printf("%s\n", resp.message);
        return EXIT_SUCCESS;
    } else {
        fprintf(stderr, "Error (code %d): %s\n", resp.status, resp.message);
        return EXIT_FAILURE;
    }
}
