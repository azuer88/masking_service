/*
 * main.c – masking_service daemon
 *
 * Listens on a Unix stream socket, receives MaskRequest structs, processes
 * them with apply_mask_blur(), and returns a MaskResponse.  Each connection
 * is handled in a detached POSIX thread so multiple requests can be
 * processed concurrently.
 *
 * Socket path and runtime directory are defined in include/protocol.h and
 * are created by systemd's RuntimeDirectory= directive in the unit file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>

#include "../include/protocol.h"
#include "image_proc.h"

static volatile int running = 1;
static int server_fd = -1;

/* -------------------------------------------------------------------------
 * Signal handling
 * -----------------------------------------------------------------------*/

static void on_signal(int sig)
{
    syslog(LOG_INFO, "Caught signal %d, shutting down", sig);
    running = 0;

    /* Unblock accept() by closing the listening socket */
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
}

/* -------------------------------------------------------------------------
 * Client thread
 * -----------------------------------------------------------------------*/

static void *handle_client(void *arg)
{
    int fd = *(int *)arg;
    free(arg);

    MaskRequest req;
    ssize_t n = recv(fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        syslog(LOG_WARNING, "Short read from client (%zd bytes), dropping", n);
        close(fd);
        return NULL;
    }

    /* Null-terminate all paths defensively */
    req.target_path[PATH_MAX - 1] = '\0';
    req.mask_path  [PATH_MAX - 1] = '\0';
    req.output_path[PATH_MAX - 1] = '\0';

    if (req.blur_radius <= 0)
        req.blur_radius = DEFAULT_BLUR_RADIUS;

    syslog(LOG_INFO,
           "Request: target=%s mask=%s output=%s radius=%d",
           req.target_path, req.mask_path, req.output_path, req.blur_radius);

    int result = apply_mask_blur(req.target_path,
                                 req.mask_path,
                                 req.output_path,
                                 req.blur_radius);

    MaskResponse resp = {0};
    resp.status = result;

    if (result == 0) {
        snprintf(resp.message, sizeof(resp.message),
                 "OK: output written to %.400s", req.output_path);
    } else {
        snprintf(resp.message, sizeof(resp.message),
                 "Error processing image (code %d)", result);
    }

    send(fd, &resp, sizeof(resp), 0);
    close(fd);

    syslog(LOG_INFO, "Request completed: status=%d", result);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/

int main(void)
{
    openlog("masking_service", LOG_PID | LOG_CONS, LOG_DAEMON);
    syslog(LOG_INFO, "masking_service starting");

    /* Signal handlers */
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    /* Ensure the socket directory exists */
    if (mkdir(SOCKET_DIR, 0755) < 0 && errno != EEXIST) {
        syslog(LOG_ERR, "mkdir(%s): %s", SOCKET_DIR, strerror(errno));
        /* Non-fatal: systemd's RuntimeDirectory= may have created it already */
    }

    /* Create the listening socket */
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        syslog(LOG_ERR, "socket: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    /* Remove stale socket file if present */
    unlink(SOCKET_PATH);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "bind(%s): %s", SOCKET_PATH, strerror(errno));
        close(server_fd);
        return EXIT_FAILURE;
    }

    /* Allow group write so a dedicated group can send requests */
    chmod(SOCKET_PATH, 0660);

    if (listen(server_fd, BACKLOG) < 0) {
        syslog(LOG_ERR, "listen: %s", strerror(errno));
        close(server_fd);
        return EXIT_FAILURE;
    }

    syslog(LOG_INFO, "Listening on %s", SOCKET_PATH);

    /* Accept loop */
    while (running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EBADF)
                break;   /* shutdown requested */
            syslog(LOG_ERR, "accept: %s", strerror(errno));
            continue;
        }

        int *pfd = malloc(sizeof(int));
        if (!pfd) {
            syslog(LOG_ERR, "OOM allocating client fd");
            close(client_fd);
            continue;
        }
        *pfd = client_fd;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&tid, &attr, handle_client, pfd) != 0) {
            syslog(LOG_ERR, "pthread_create: %s", strerror(errno));
            free(pfd);
            close(client_fd);
        }

        pthread_attr_destroy(&attr);
    }

    /* Cleanup */
    if (server_fd >= 0)
        close(server_fd);
    unlink(SOCKET_PATH);

    syslog(LOG_INFO, "masking_service stopped");
    closelog();
    return EXIT_SUCCESS;
}
