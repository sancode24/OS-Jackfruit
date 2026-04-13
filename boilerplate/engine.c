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

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

/* --- Boilerplate Enums & Structs --- */

typedef enum {
    CMD_SUPERVISOR = 0, CMD_START, CMD_RUN, CMD_PS, CMD_LOGS, CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0, CONTAINER_RUNNING, CONTAINER_STOPPED, CONTAINER_KILLED, CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head; size_t tail; size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty; pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* --- Helper Functions (Boilerplate) --- */

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n  %s supervisor <base-rootfs>\n  %s start <id> <rootfs> <cmd>\n  %s ps\n", prog, prog, prog);
}

static int bounded_buffer_init(bounded_buffer_t *buffer) {
    pthread_mutex_init(&buffer->mutex, NULL);
    pthread_cond_init(&buffer->not_empty, NULL);
    pthread_cond_init(&buffer->not_full, NULL);
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer) {
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

/* --- TASK 1: Child Entrypoint --- */

int child_fn(void *arg) {
    child_config_t *config = (child_config_t *)arg;
    
    // Isolation: Hostname
    sethostname(config->id, strlen(config->id));

    // Isolation: Filesystem
    if (chroot(config->rootfs) != 0 || chdir("/") != 0) {
        perror("chroot failed");
        return 1;
    }

    // Isolation: Mount /proc
    mount("proc", "/proc", "proc", 0, NULL);

    char *exec_args[] = { config->command, NULL };
    execv(config->command, exec_args);
    return 1;
}

/* --- TASK 2: Client Side (CLI) --- */

static int send_control_request(const control_request_t *req) {
    int fd;
    struct sockaddr_un addr;
    control_response_t res;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) return 1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        fprintf(stderr, "Supervisor not found at %s. Did you start it with sudo?\n", CONTROL_PATH);
        close(fd); return 1;
    }

    write(fd, req, sizeof(*req));
    if (read(fd, &res, sizeof(res)) > 0) {
        printf("[%s] %s\n", res.status == 0 ? "OK" : "ERROR", res.message);
    }

    close(fd);
    return res.status;
}

/* --- TASK 2: Supervisor Side --- */

static int run_supervisor(const char *rootfs) {
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctx.server_fd, 5);

    printf("Supervisor ready (Socket: %s)\n", CONTROL_PATH);

    while (!ctx.should_stop) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        control_request_t req;
        control_response_t res = {0, "Success"};

        if (read(client_fd, &req, sizeof(req)) > 0) {
            if (req.kind == CMD_START || req.kind == CMD_RUN) {
                child_config_t *config = malloc(sizeof(child_config_t));
                strncpy(config->id, req.container_id, CONTAINER_ID_LEN);
                strncpy(config->rootfs, req.rootfs, PATH_MAX);
                strncpy(config->command, req.command, CHILD_COMMAND_LEN);

                char *stack = malloc(STACK_SIZE);
                pid_t pid = clone(child_fn, stack + STACK_SIZE, 
                                  CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, 
                                  config);
                
                if (pid > 0) snprintf(res.message, CONTROL_MESSAGE_LEN, "Launched %s (PID: %d)", req.container_id, pid);
                else { res.status = 1; strcpy(res.message, "Launch failed"); }
            } else if (req.kind == CMD_PS) {
                strcpy(res.message, "Status: Monitoring containers...");
            }
        }
        write(client_fd, &res, sizeof(res));
        close(client_fd);
    }
    return 0;
}

/* --- CLI Wrappers --- */

static int cmd_start(int argc, char *argv[]) {
    control_request_t req = { .kind = CMD_START };
    if (argc < 5) return 1;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs, argv[3], PATH_MAX - 1);
    strncpy(req.command, argv[4], CHILD_COMMAND_LEN - 1);
    return send_control_request(&req);
}

static int cmd_ps(void) {
    control_request_t req = { .kind = CMD_PS };
    return send_control_request(&req);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "supervisor") == 0) return run_supervisor(argv[2]);
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "ps") == 0) return cmd_ps();
    usage(argv[0]);
    return 1;
}
