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
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16

/* --- Data Structures --- */

typedef enum { CMD_SUPERVISOR = 0, CMD_START, CMD_RUN, CMD_PS, CMD_LOGS, CMD_STOP } command_kind_t;

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
} control_request_t;

typedef struct { int status; char message[CONTROL_MESSAGE_LEN]; } control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int log_pipe_fd; // Pipe to send logs to supervisor
} child_config_t;

typedef struct {
    int server_fd;
    bounded_buffer_t log_buffer;
    pthread_t logger_thread;
} supervisor_ctx_t;

/* --- TASK 3: Bounded Buffer Logic (Producer-Consumer) --- */

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item) {
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    
    if (buffer->shutting_down) { pthread_mutex_unlock(&buffer->mutex); return -1; }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item) {
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    
    if (buffer->count == 0 && buffer->shutting_down) { pthread_mutex_unlock(&buffer->mutex); return -1; }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* --- TASK 3: Logging Consumer Thread --- */

void *logging_thread(void *arg) {
    bounded_buffer_t *buffer = (bounded_buffer_t *)arg;
    log_item_t item;
    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

/* --- TASK 1: Child Entrypoint --- */

int child_fn(void *arg) {
    child_config_t *config = (child_config_t *)arg;
    sethostname(config->id, strlen(config->id));
    
    // Redirect stdout/stderr to the pipe
    dup2(config->log_pipe_fd, STDOUT_FILENO);
    dup2(config->log_pipe_fd, STDERR_FILENO);
    close(config->log_pipe_fd);

    if (chroot(config->rootfs) != 0 || chdir("/") != 0) return 1;
    mount("proc", "/proc", "proc", 0, NULL);
    
    char *exec_args[] = { config->command, NULL };
    execv(config->command, exec_args);
    return 1;
}

/* --- TASK 2: Client IPC --- */

static int send_control_request(const control_request_t *req) {
    int fd; struct sockaddr_un addr; control_response_t res;
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) return 1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) { close(fd); return 1; }
    write(fd, req, sizeof(*req));
    read(fd, &res, sizeof(res));
    printf("[%s] %s\n", res.status == 0 ? "OK" : "ERROR", res.message);
    close(fd); return res.status;
}

/* --- TASK 2 & 3: Supervisor Implementation --- */

static int run_supervisor(const char *rootfs) {
    supervisor_ctx_t ctx; struct sockaddr_un addr;
    memset(&ctx, 0, sizeof(ctx));
    
    pthread_mutex_init(&ctx.log_buffer.mutex, NULL);
    pthread_cond_init(&ctx.log_buffer.not_empty, NULL);
    pthread_cond_init(&ctx.log_buffer.not_full, NULL);
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx.log_buffer);

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctx.server_fd, 5);

    printf("Supervisor ready with Logging Thread (Task 3)\n");

    while (1) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        control_request_t req;
        control_response_t res = {0, "OK"};
        if (read(client_fd, &req, sizeof(req)) > 0 && req.kind == CMD_START) {
            int pipefds[2];
            pipe(pipefds);

            child_config_t *config = malloc(sizeof(child_config_t));
            strncpy(config->id, req.container_id, CONTAINER_ID_LEN);
            strncpy(config->rootfs, req.rootfs, PATH_MAX);
            strncpy(config->command, req.command, CHILD_COMMAND_LEN);
            config->log_pipe_fd = pipefds[1];

            char *stack = malloc(STACK_SIZE);
            pid_t pid = clone(child_fn, stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, config);
            
            close(pipefds[1]); // Close write end in parent

            // Task 3: Background thread to read logs for THIS container
            pthread_t t;
            struct { int fd; char id[32]; bounded_buffer_t *buf; } *info = malloc(sizeof(*info));
            info->fd = pipefds[0]; strcpy(info->id, req.container_id); info->buf = &ctx.log_buffer;
            
            pthread_create(&t, NULL, (void* (*)(void*))({ 
                void* _f(void* a) {
                    char buf[LOG_CHUNK_SIZE]; int n;
                    while((n = read(info->fd, buf, LOG_CHUNK_SIZE)) > 0) {
                        log_item_t item; memcpy(item.data, buf, n);
                        item.length = n; strcpy(item.container_id, info->id);
                        bounded_buffer_push(info->buf, &item);
                    }
                    close(info->fd); free(info); return NULL;
                } _f; 
            }), info);

            snprintf(res.message, CONTROL_MESSAGE_LEN, "Started %s (PID %d)", req.container_id, pid);
        }
        write(client_fd, &res, sizeof(res));
        close(client_fd);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "supervisor") == 0) return run_supervisor(argv[2]);
    if (strcmp(argv[1], "start") == 0) {
        control_request_t req = { .kind = CMD_START };
        strncpy(req.container_id, argv[2], 31); strncpy(req.rootfs, argv[3], PATH_MAX-1); strncpy(req.command, argv[4], 255);
        return send_control_request(&req);
    }
    return 0;
}
