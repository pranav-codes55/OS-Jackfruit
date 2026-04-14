/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Two modes:
 *   supervisor  - long-running daemon: manages containers, owns the logging
 *                 pipeline, accepts CLI commands over a UNIX domain socket.
 *   client      - any other sub-command (start/run/ps/logs/stop). Each is a
 *                 short-lived process that connects to the supervisor, sends
 *                 a control_request_t, and prints the response.
 *
 * IPC paths:
 *   Path A (logging)  : pipe per container → producer thread → bounded buffer
 *                       → consumer thread → per-container log files.
 *   Path B (control)  : UNIX domain socket at /tmp/mini_runtime.sock.
 *                       Bidirectional, connection-per-request.
 *
 * Synchronisation:
 *   bounded_buffer   : mutex + two condition variables (not_full / not_empty).
 *   container list   : metadata_lock mutex (separate from buffer lock).
 *
 * Termination classification:
 *   Each container record has stop_requested. supervisor sets it before
 *   sending SIGTERM in the stop path. SIGCHLD handler checks it:
 *     stop_requested set       → state = CONTAINER_STOPPED
 *     SIGKILL without request  → state = CONTAINER_KILLED  (hard limit)
 *     clean exit               → state = CONTAINER_EXITED
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define STACK_SIZE            (1024 * 1024)
#define CONTAINER_ID_LEN      32
#define CONTROL_PATH          "/tmp/mini_runtime.sock"
#define LOG_DIR               "logs"
#define CONTROL_MESSAGE_LEN   512
#define CHILD_COMMAND_LEN     256
#define LOG_CHUNK_SIZE        4096
#define LOG_BUFFER_CAPACITY   32
#define DEFAULT_SOFT_LIMIT    (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT    (64UL << 20)   /* 64 MiB */

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char                    id[CONTAINER_ID_LEN];
    pid_t                   host_pid;
    char                    rootfs[PATH_MAX];
    time_t                  started_at;
    container_state_t       state;
    unsigned long           soft_limit_bytes;
    unsigned long           hard_limit_bytes;
    int                     exit_code;
    int                     exit_signal;
    int                     stop_requested;   /* set before SIGTERM in stop */
    char                    log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;   /* write-end of the logging pipe */
} child_config_t;

typedef struct {
    int             server_fd;
    int             monitor_fd;
    volatile int    should_stop;
    pthread_t       logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    pthread_cond_t   metadata_cv;
    pthread_cond_t   requests_cv;
    int              active_requests;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    supervisor_ctx_t *ctx;
    int conn_fd;
} request_thread_args_t;

/* ------------------------------------------------------------------ */
/* Globals (supervisor side only)                                      */
/* ------------------------------------------------------------------ */
static supervisor_ctx_t *g_ctx = NULL;  /* set once in run_supervisor */

/* ------------------------------------------------------------------ */
/* Usage                                                               */
/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/* Argument parsing helpers                                            */
/* ------------------------------------------------------------------ */
static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc, char *argv[],
                                int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1],
                               &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1],
                               &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static int is_known_flag(const char *arg)
{
    return (strcmp(arg, "--soft-mib") == 0 ||
            strcmp(arg, "--hard-mib") == 0 ||
            strcmp(arg, "--nice") == 0);
}

/*
 * Accepts:
 *   <command> [arg1 ... argN] [--soft-mib N] [--hard-mib N] [--nice N]
 *
 * The command (and its args) are encoded as a single command string.
 */
static int parse_command_and_flags(control_request_t *req,
                                   int argc, char *argv[],
                                   int command_start_index)
{
    int i = command_start_index;
    size_t used = 0;

    req->command[0] = '\0';

    while (i < argc && !is_known_flag(argv[i])) {
        int written;
        const char *sep = (used == 0) ? "" : " ";

        written = snprintf(req->command + used,
                           sizeof(req->command) - used,
                           "%s%s", sep, argv[i]);
        if (written < 0 || (size_t)written >= sizeof(req->command) - used) {
            fprintf(stderr, "Command is too long\n");
            return -1;
        }
        used += (size_t)written;
        i++;
    }

    if (used == 0) {
        fprintf(stderr, "Missing <command>\n");
        return -1;
    }

    return parse_optional_flags(req, argc, argv, i);
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded Buffer                                                      */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));

    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;

    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/*
 * bounded_buffer_push: producer side.
 *
 * Blocks when the buffer is full, until space opens or shutdown begins.
 * Returns 0 on success, -1 if we should stop (shutdown).
 *
 * Without the mutex + not_full condvar: two producers could both see
 * `count < capacity` simultaneously and write to the same slot →
 * data corruption. With mutex we prevent that race.
 *
 * Without not_empty broadcast after push: a waiting consumer would
 * never wake up → deadlock.
 */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * bounded_buffer_pop: consumer side.
 *
 * Blocks when buffer is empty. On shutdown, drains remaining items
 * before returning -1 so no log lines are dropped.
 *
 * Returns 0 on success (item filled in), -1 when drained and shutdown.
 */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == 0) {
        if (buf->shutting_down) {
            pthread_mutex_unlock(&buf->mutex);
            return -1;
        }
        pthread_cond_wait(&buf->not_empty, &buf->mutex);
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Logging consumer thread                                             */
/* ------------------------------------------------------------------ */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        /* Find the container record to get log_path */
        char path[PATH_MAX];
        path[0] = '\0';

        pthread_mutex_lock(&ctx->metadata_lock);
        {
            container_record_t *c = ctx->containers;
            while (c) {
                if (strncmp(c->id, item.container_id, CONTAINER_ID_LEN) == 0) {
                    strncpy(path, c->log_path, PATH_MAX - 1);
                    path[PATH_MAX - 1] = '\0';
                    break;
                }
                c = c->next;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (path[0] == '\0') {
            /* Fallback: write to generic log file */
            snprintf(path, sizeof(path), "%s/%s.log",
                     LOG_DIR, item.container_id);
        }

        int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t n = write(fd, item.data, item.length);
            (void)n;
            close(fd);
        }
    }

    /* Drain remaining items after shutdown signal */
    {
        bounded_buffer_t *buf = &ctx->log_buffer;
        pthread_mutex_lock(&buf->mutex);
        while (buf->count > 0) {
            log_item_t leftover = buf->items[buf->head];
            buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
            buf->count--;
            pthread_mutex_unlock(&buf->mutex);

            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s.log",
                     LOG_DIR, leftover.container_id);
            int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
            if (fd >= 0) {
                ssize_t n = write(fd, leftover.data, leftover.length);
                (void)n;
                close(fd);
            }

            pthread_mutex_lock(&buf->mutex);
        }
        pthread_mutex_unlock(&buf->mutex);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Producer thread – reads one container's pipe and pushes to buffer  */
/* ------------------------------------------------------------------ */

typedef struct {
    int              read_fd;
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_args_t;

static void *producer_thread(void *arg)
{
    producer_args_t *pa = (producer_args_t *)arg;
    char tmp[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(pa->read_fd, tmp, sizeof(tmp) - 1)) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, pa->container_id, CONTAINER_ID_LEN - 1);
        item.length = (size_t)n;
        memcpy(item.data, tmp, (size_t)n);
        bounded_buffer_push(pa->buffer, &item);
    }

    close(pa->read_fd);
    free(pa);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container child entrypoint (runs after clone())                    */
/* ------------------------------------------------------------------ */

int child_fn(void *arg)
{
#ifdef __linux__
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the logging pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Set nice value */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) == -1 && errno != 0)
            perror("nice");   /* non-fatal */
    }

    /* Set hostname (UTS namespace) */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("sethostname");   /* non-fatal */

    /* chroot into container rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    /* Mount /proc so 'ps' and other tools work inside the container.
     * Linux mount(2) takes 5 arguments: source, target, fstype, flags, data.
     */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");   /* non-fatal if already mounted */

    /* Execute through /bin/sh so command arguments work naturally. */
    {
        char *argv_exec[] = { "/bin/sh", "-c", cfg->command, NULL };
        execv("/bin/sh", argv_exec);
    }

    perror("exec /bin/sh");
    return 127;
#else
    (void)arg;
    fprintf(stderr, "child_fn: Linux-only (clone/namespaces not available)\n");
    return 1;
#endif
}

/* ------------------------------------------------------------------ */
/* Monitor device helpers                                              */
/* ------------------------------------------------------------------ */

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid              = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                            const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

static volatile sig_atomic_t g_run_stop_forward_requested = 0;

static void run_client_signal_handler(int sig)
{
    (void)sig;
    g_run_stop_forward_requested = 1;
}

static int send_stop_request_for_run_client(const char *container_id)
{
    int sock;
    struct sockaddr_un addr;
    control_request_t req;
    control_response_t resp;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (send(sock, &req, sizeof(req), 0) != (ssize_t)sizeof(req)) {
        close(sock);
        return -1;
    }

    (void)recv(sock, &resp, sizeof(resp), 0);
    close(sock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Container metadata helpers                                          */
/* ------------------------------------------------------------------ */

static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

static void add_container(supervisor_ctx_t *ctx, container_record_t *rec)
{
    rec->next = ctx->containers;
    ctx->containers = rec;
}

/* ------------------------------------------------------------------ */
/* SIGCHLD handler and signal infrastructure                           */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_sigchld_pending = 0;
static volatile sig_atomic_t g_shutdown        = 0;

static void sigchld_handler(int sig)
{
    (void)sig;
    g_sigchld_pending = 1;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code   = WEXITSTATUS(status);
                    c->exit_signal = 0;
                    c->state       = c->stop_requested
                                     ? CONTAINER_STOPPED
                                     : CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    c->exit_code   = 128 + c->exit_signal;
                    if (c->stop_requested)
                        c->state = CONTAINER_STOPPED;
                    else if (c->exit_signal == SIGKILL)
                        c->state = CONTAINER_KILLED;
                    else
                        c->state = CONTAINER_EXITED;
                }
                /* Unregister from kernel monitor */
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, c->id, pid);
                pthread_cond_broadcast(&ctx->metadata_cv);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ------------------------------------------------------------------ */
/* Launch a container (used by both start and run commands)            */
/* ------------------------------------------------------------------ */

static int launch_container(supervisor_ctx_t *ctx,
                            const control_request_t *req,
                            control_response_t *resp)
{
    int pipefd[2];
    container_record_t *rec;
    child_config_t *cfg;
    char *stack;
    pid_t pid;

    /* Check ID uniqueness */
    pthread_mutex_lock(&ctx->metadata_lock);
    {
        container_record_t *c = ctx->containers;
        if (find_container(ctx, req->container_id)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            snprintf(resp->message, sizeof(resp->message),
                     "container '%s' already exists", req->container_id);
            resp->status = -1;
            return -1;
        }

        while (c) {
            if ((c->state == CONTAINER_STARTING || c->state == CONTAINER_RUNNING) &&
                strncmp(c->rootfs, req->rootfs, PATH_MAX) == 0) {
                pthread_mutex_unlock(&ctx->metadata_lock);
                snprintf(resp->message, sizeof(resp->message),
                         "rootfs '%s' already used by running container '%s'",
                         req->rootfs, c->id);
                resp->status = -1;
                return -1;
            }
            c = c->next;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create the logging pipe */
    if (pipe(pipefd) < 0) {
        perror("pipe");
        snprintf(resp->message, sizeof(resp->message), "pipe: %s", strerror(errno));
        resp->status = -1;
        return -1;
    }

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    /* Allocate container record */
    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "OOM");
        return -1;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(rec->rootfs, req->rootfs, PATH_MAX - 1);
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_STARTING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    /* Allocate child config */
    cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        free(rec);
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "OOM");
        return -1;
    }
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* Allocate clone stack */
    stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg); free(rec);
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message), "OOM");
        return -1;
    }

    /* Clone with isolated namespaces (Linux-only) */
#ifdef __linux__
    pid = clone(child_fn, stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg);
    if (pid < 0) {
        perror("clone");
        free(stack); free(cfg); free(rec);
        close(pipefd[0]); close(pipefd[1]);
        snprintf(resp->message, sizeof(resp->message),
                 "clone failed: %s", strerror(errno));
        resp->status = -1;
        return -1;
    }
#else
    (void)child_fn;
    free(stack); free(cfg); free(rec);
    close(pipefd[0]); close(pipefd[1]);
    snprintf(resp->message, sizeof(resp->message),
             "clone/namespaces not available on this platform");
    resp->status = -1;
    return -1;
    pid = -1; /* unreachable, suppress compiler warning */
#endif

    /* Parent: close write end; start producer thread on read end */
    close(pipefd[1]);

    producer_args_t *pa = malloc(sizeof(*pa));
    if (pa) {
        pa->read_fd = pipefd[0];
        strncpy(pa->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        pa->container_id[CONTAINER_ID_LEN - 1] = '\0';
        pa->buffer = &ctx->log_buffer;
        pthread_t prod_tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&prod_tid, &attr, producer_thread, pa);
        pthread_attr_destroy(&attr);
    } else {
        close(pipefd[0]);
    }

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);
    }

    /* Update metadata */
    rec->host_pid = pid;
    rec->state    = CONTAINER_RUNNING;
    free(stack);
    free(cfg);

    pthread_mutex_lock(&ctx->metadata_lock);
    add_container(ctx, rec);
    pthread_mutex_unlock(&ctx->metadata_lock);

    snprintf(resp->message, sizeof(resp->message),
             "started container '%s' pid=%d", req->container_id, pid);
    resp->status = 0;
    return pid;
}

/* ------------------------------------------------------------------ */
/* Handle one control request from a connected CLI client             */
/* ------------------------------------------------------------------ */

static void handle_request(supervisor_ctx_t *ctx, int conn_fd)
{
    control_request_t  req;
    control_response_t resp;

    memset(&resp, 0, sizeof(resp));

    ssize_t n = recv(conn_fd, &req, sizeof(req), 0);
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "protocol error");
        send(conn_fd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    case CMD_START: {
        launch_container(ctx, &req, &resp);
        break;
    }

    case CMD_RUN: {
        int pid = launch_container(ctx, &req, &resp);
        if (pid > 0) {
            int exit_code = -1;

            pthread_mutex_lock(&ctx->metadata_lock);
            {
                container_record_t *c = find_container(ctx, req.container_id);
                while (c &&
                       (c->state == CONTAINER_STARTING || c->state == CONTAINER_RUNNING)) {
                    pthread_cond_wait(&ctx->metadata_cv, &ctx->metadata_lock);
                }

                if (c)
                    exit_code = c->exit_code;
            }
            pthread_mutex_unlock(&ctx->metadata_lock);

            if (exit_code < 0) {
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message),
                         "container '%s' finished but exit status unavailable",
                         req.container_id);
            } else {
                snprintf(resp.message, sizeof(resp.message),
                         "container '%s' exited with status %d",
                         req.container_id, exit_code);
                resp.status = exit_code;
            }
        }
        break;
    }

    case CMD_PS: {
        char buf[CONTROL_MESSAGE_LEN * 8];
        int  off = 0;

        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-16s %-8s %-10s %-12s %-12s\n",
                        "ID", "PID", "STATE", "SOFT-MiB", "HARD-MiB");

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c && off < (int)sizeof(buf) - 80) {
            off += snprintf(buf + off, sizeof(buf) - off,
                            "%-16s %-8d %-10s %-12lu %-12lu\n",
                            c->id, c->host_pid,
                            state_to_string(c->state),
                            c->soft_limit_bytes >> 20,
                            c->hard_limit_bytes >> 20);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "%s", buf);
        break;
    }

    case CMD_LOGS: {
        char log_path[PATH_MAX];
        log_path[0] = '\0';

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (c)
            strncpy(log_path, c->log_path, PATH_MAX - 1);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0') {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' not found", req.container_id);
        } else {
            /* Read and send log file contents */
            int fd = open(log_path, O_RDONLY);
            if (fd < 0) {
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message),
                         "cannot open log: %s", strerror(errno));
            } else {
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message),
                         "log: %s", log_path);
                send(conn_fd, &resp, sizeof(resp), 0);

                /* Stream log content */
                char chunk[4096];
                ssize_t r;
                while ((r = read(fd, chunk, sizeof(chunk))) > 0)
                    send(conn_fd, chunk, (size_t)r, 0);
                close(fd);
                return;
            }
        }
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (!c) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' not found", req.container_id);
        } else if (c->state != CONTAINER_RUNNING) {
            pid_t pid = c->host_pid;
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' (pid=%d) is not running", req.container_id, pid);
        } else {
            c->stop_requested = 1;
            pid_t pid = c->host_pid;
            pthread_mutex_unlock(&ctx->metadata_lock);

            kill(pid, SIGTERM);
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "sent SIGTERM to container '%s' pid=%d",
                     req.container_id, pid);
        }
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "unknown command %d", req.kind);
        break;
    }

    send(conn_fd, &resp, sizeof(resp), 0);
}

static void *request_thread_main(void *arg)
{
    request_thread_args_t *rt = (request_thread_args_t *)arg;
    supervisor_ctx_t *ctx = rt->ctx;

    handle_request(ctx, rt->conn_fd);
    close(rt->conn_fd);

    pthread_mutex_lock(&ctx->metadata_lock);
    ctx->active_requests--;
    pthread_cond_signal(&ctx->requests_cv);
    pthread_mutex_unlock(&ctx->metadata_lock);

    free(rt);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Supervisor main loop                                                */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    (void)rootfs;   /* base rootfs available for future use */

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = pthread_cond_init(&ctx.metadata_cv, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_cond_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    rc = pthread_cond_init(&ctx.requests_cv, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_cond_init requests_cv");
        pthread_cond_destroy(&ctx.metadata_cv);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_cond_destroy(&ctx.requests_cv);
        pthread_cond_destroy(&ctx.metadata_cv);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Open kernel monitor (non-fatal if module not loaded) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] warning: /dev/container_monitor not available: %s\n",
                strerror(errno));

    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Create UNIX domain socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen");
        goto cleanup;
    }

    /* Install signal handlers */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigchld_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, NULL);

        sa.sa_handler = sigterm_handler;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT,  &sa, NULL);
    }

    /* Start logger consumer thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create logger");
        goto cleanup;
    }

    fprintf(stderr, "[supervisor] started, control socket: %s\n", CONTROL_PATH);

    /* Event loop */
    while (!g_shutdown) {
        /* Reap any pending children */
        if (g_sigchld_pending) {
            g_sigchld_pending = 0;
            reap_children(&ctx);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        struct timeval tv = {0, 200000};  /* 200ms timeout to check g_shutdown */

        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (sel == 0) continue;

        int conn_fd = accept(ctx.server_fd, NULL, NULL);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        request_thread_args_t *rt = malloc(sizeof(*rt));
        if (!rt) {
            handle_request(&ctx, conn_fd);
            close(conn_fd);
            continue;
        }

        rt->ctx = &ctx;
        rt->conn_fd = conn_fd;

        pthread_mutex_lock(&ctx.metadata_lock);
        ctx.active_requests++;
        pthread_mutex_unlock(&ctx.metadata_lock);

        pthread_t req_tid;
        rc = pthread_create(&req_tid, NULL, request_thread_main, rt);
        if (rc != 0) {
            pthread_mutex_lock(&ctx.metadata_lock);
            ctx.active_requests--;
            pthread_mutex_unlock(&ctx.metadata_lock);
            free(rt);
            handle_request(&ctx, conn_fd);
            close(conn_fd);
            continue;
        }
        pthread_detach(req_tid);
    }

    fprintf(stderr, "[supervisor] shutting down...\n");

    /* Kill all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *c = ctx.containers;
        while (c) {
            if (c->state == CONTAINER_RUNNING) {
                c->stop_requested = 1;
                kill(c->host_pid, SIGTERM);
            }
            c = c->next;
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Brief wait for containers to exit */
    {
        int wstatus;
        while (waitpid(-1, &wstatus, WNOHANG) > 0)
            ;
        usleep(500000);
        while (waitpid(-1, &wstatus, WNOHANG) > 0)
            ;
    }

cleanup:
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    if (ctx.logger_thread)
        pthread_join(ctx.logger_thread, NULL);

    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    while (ctx.active_requests > 0)
        pthread_cond_wait(&ctx.requests_cv, &ctx.metadata_lock);
    {
        container_record_t *c = ctx.containers;
        while (c) {
            container_record_t *next = c->next;
            free(c);
            c = next;
        }
        ctx.containers = NULL;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_cond_destroy(&ctx.requests_cv);
    pthread_cond_destroy(&ctx.metadata_cv);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.server_fd >= 0) close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] exited cleanly.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Client side: send a control request to the supervisor              */
/* ------------------------------------------------------------------ */

static int send_control_request(const control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;
    control_response_t resp;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n"
                "Is the supervisor running?\n",
                CONTROL_PATH, strerror(errno));
        close(sock);
        return 1;
    }

    if (send(sock, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(sock);
        return 1;
    }

    if (req->kind == CMD_RUN) {
        struct sigaction sa, old_int, old_term;
        int stop_forwarded = 0;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = run_client_signal_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, &old_int);
        sigaction(SIGTERM, &sa, &old_term);
        g_run_stop_forward_requested = 0;

        for (;;) {
            fd_set rfds;
            struct timeval tv;
            int sel;

            if (g_run_stop_forward_requested && !stop_forwarded) {
                if (send_stop_request_for_run_client(req->container_id) == 0)
                    stop_forwarded = 1;
                g_run_stop_forward_requested = 0;
            }

            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            tv.tv_sec = 0;
            tv.tv_usec = 200000;

            sel = select(sock + 1, &rfds, NULL, NULL, &tv);
            if (sel < 0) {
                if (errno == EINTR)
                    continue;
                perror("select");
                sigaction(SIGINT, &old_int, NULL);
                sigaction(SIGTERM, &old_term, NULL);
                close(sock);
                return 1;
            }
            if (sel == 0)
                continue;

            if (recv(sock, &resp, sizeof(resp), 0) <= 0) {
                perror("recv");
                sigaction(SIGINT, &old_int, NULL);
                sigaction(SIGTERM, &old_term, NULL);
                close(sock);
                return 1;
            }
            break;
        }

        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
    } else {
        ssize_t n = recv(sock, &resp, sizeof(resp), 0);
        if (n <= 0) {
            perror("recv");
            close(sock);
            return 1;
        }
    }

    /* For LOG commands, stream additional data */
    if (req->kind == CMD_LOGS && resp.status == 0) {
        printf("%s\n", resp.message);
        char chunk[4096];
        ssize_t r;
        while ((r = recv(sock, chunk, sizeof(chunk), 0)) > 0) {
            fwrite(chunk, 1, (size_t)r, stdout);
        }
    } else {
        printf("%s\n", resp.message);
    }

    close(sock);
    if (req->kind == CMD_RUN)
        return (resp.status >= 0) ? resp.status : 1;
    return (resp.status == 0) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* CLI command handlers (client side)                                  */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_command_and_flags(&req, argc, argv, 4) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_command_and_flags(&req, argc, argv, 4) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
