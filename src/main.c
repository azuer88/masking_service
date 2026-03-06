/*
 * main.c – masking_service daemon (worker-pool edition)
 *
 * The main thread accepts incoming connections and pushes client file
 * descriptors onto a bounded work queue.  A fixed pool of worker threads
 * drains the queue, each processing one request at a time.  When the queue
 * is full the main thread blocks, providing natural back-pressure.
 *
 * Tuning:
 *   MASKING_WORKERS env var – number of worker threads (default 4)
 *   QUEUE_SIZE #define      – max queued connections before back-pressure (default 64)
 *
 * Set MASKING_WORKERS in the systemd unit drop-in:
 *   [Service]
 *   Environment=MASKING_WORKERS=8
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

#define NUM_WORKERS_DEFAULT  4
#define NUM_WORKERS_MAX      64
#define QUEUE_SIZE           64

/* -------------------------------------------------------------------------
 * Bounded work queue
 * -----------------------------------------------------------------------*/

typedef struct {
    int  fds[QUEUE_SIZE];
    int  head, tail, count;
    int  shutdown;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} WorkQueue;

static WorkQueue wq;

static void wq_init(WorkQueue *q)
{
    q->head = q->tail = q->count = q->shutdown = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void wq_push(WorkQueue *q, int fd)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == QUEUE_SIZE && !q->shutdown)
        pthread_cond_wait(&q->not_full, &q->lock);
    if (!q->shutdown) {
        q->fds[q->tail] = fd;
        q->tail = (q->tail + 1) % QUEUE_SIZE;
        q->count++;
        pthread_cond_signal(&q->not_empty);
    } else {
        close(fd);   /* dropped during shutdown */
    }
    pthread_mutex_unlock(&q->lock);
}

/* Returns a client fd, or -1 when shutting down with an empty queue. */
static int wq_pop(WorkQueue *q)
{
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->shutdown)
        pthread_cond_wait(&q->not_empty, &q->lock);
    int fd = -1;
    if (q->count > 0) {
        fd = q->fds[q->head];
        q->head = (q->head + 1) % QUEUE_SIZE;
        q->count--;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->lock);
    return fd;
}

static void wq_shutdown(WorkQueue *q)
{
    pthread_mutex_lock(&q->lock);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}

static void wq_destroy(WorkQueue *q)
{
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/* -------------------------------------------------------------------------
 * Request processing (called by each worker)
 * -----------------------------------------------------------------------*/

static void process_client(int fd)
{
    MaskRequest req;
    ssize_t n = recv(fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        syslog(LOG_WARNING, "Short read from client (%zd bytes), dropping", n);
        close(fd);
        return;
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

    int result = apply_mask_blur(req.target_path, req.mask_path,
                                 req.output_path, req.blur_radius);

    MaskResponse resp = {0};
    resp.status = result;
    if (result == 0)
        snprintf(resp.message, sizeof(resp.message),
                 "OK: output written to %.400s", req.output_path);
    else
        snprintf(resp.message, sizeof(resp.message),
                 "Error processing image (code %d)", result);

    send(fd, &resp, sizeof(resp), 0);
    close(fd);

    syslog(LOG_INFO, "Request completed: status=%d", result);
}

/* -------------------------------------------------------------------------
 * Worker thread
 * -----------------------------------------------------------------------*/

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        int fd = wq_pop(&wq);
        if (fd < 0) break;   /* shutdown with empty queue */
        process_client(fd);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Signal handling
 * -----------------------------------------------------------------------*/

static volatile int running = 1;
static int server_fd = -1;

static void on_signal(int sig)
{
    syslog(LOG_INFO, "Caught signal %d, shutting down", sig);
    running = 0;
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
}

/* -------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/

int main(void)
{
    openlog("masking_service", LOG_PID | LOG_CONS, LOG_DAEMON);

    /* Resolve worker count from environment, falling back to the default */
    int num_workers = NUM_WORKERS_DEFAULT;
    const char *env = getenv("MASKING_WORKERS");
    if (env) {
        char *end;
        long v = strtol(env, &end, 10);
        if (*end != '\0' || v < 1 || v > NUM_WORKERS_MAX) {
            syslog(LOG_ERR,
                   "Invalid MASKING_WORKERS=%s (must be 1-%d), using default %d",
                   env, NUM_WORKERS_MAX, NUM_WORKERS_DEFAULT);
        } else {
            num_workers = (int)v;
        }
    }

    syslog(LOG_INFO, "masking_service starting (%d workers, queue depth %d)",
           num_workers, QUEUE_SIZE);

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    /* Initialise the work queue and start workers */
    wq_init(&wq);

    pthread_t *workers = malloc((size_t)num_workers * sizeof(pthread_t));
    if (!workers) {
        syslog(LOG_ERR, "malloc workers: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    for (int i = 0; i < num_workers; i++) {
        if (pthread_create(&workers[i], NULL, worker, NULL) != 0) {
            syslog(LOG_ERR, "pthread_create worker %d: %s", i, strerror(errno));
            free(workers);
            return EXIT_FAILURE;
        }
    }

    /* Set up the listening socket */
    if (mkdir(SOCKET_DIR, 0755) < 0 && errno != EEXIST)
        syslog(LOG_ERR, "mkdir(%s): %s", SOCKET_DIR, strerror(errno));

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        syslog(LOG_ERR, "socket: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    unlink(SOCKET_PATH);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "bind(%s): %s", SOCKET_PATH, strerror(errno));
        close(server_fd);
        return EXIT_FAILURE;
    }

    chmod(SOCKET_PATH, 0660);

    if (listen(server_fd, BACKLOG) < 0) {
        syslog(LOG_ERR, "listen: %s", strerror(errno));
        close(server_fd);
        return EXIT_FAILURE;
    }

    syslog(LOG_INFO, "Listening on %s", SOCKET_PATH);

    /* Accept loop — hand fds off to the worker pool */
    while (running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EBADF)
                break;
            syslog(LOG_ERR, "accept: %s", strerror(errno));
            continue;
        }
        wq_push(&wq, client_fd);
    }

    /* Graceful shutdown: drain the queue, then join workers */
    wq_shutdown(&wq);
    for (int i = 0; i < num_workers; i++)
        pthread_join(workers[i], NULL);
    wq_destroy(&wq);
    free(workers);

    if (server_fd >= 0)
        close(server_fd);
    unlink(SOCKET_PATH);

    syslog(LOG_INFO, "masking_service stopped");
    closelog();
    return EXIT_SUCCESS;
}
