/*
 * engine.c — Multi-Container Runtime with Parent Supervisor
 *
 * Task 1: Multi-Container Runtime with Parent Supervisor
 *
 * Covers:
 *  - Long-running supervisor process
 *  - Multiple concurrent containers with PID, UTS, and mount namespace isolation
 *  - Per-container writable rootfs via chroot
 *  - /proc mounted inside each container
 *  - Thread-safe container metadata table
 *  - Correct SIGCHLD reaping (no zombies)
 *
 * Build:
 *   gcc -o engine engine.c -lpthread
 *
 * Usage:
 *   sudo ./engine supervisor ./rootfs-base
 *   sudo ./engine start alpha ./rootfs-alpha /bin/sh
 *   sudo ./engine ps
 *   sudo ./engine stop alpha
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/resource.h>
#include <sched.h>

/* Suppress warn_unused_result on write() calls to sockets/pipes */
static inline void write_fd(int fd, const void *buf, size_t n) {
    ssize_t r = write(fd, buf, n); (void)r;
}

/* ─────────────────────────────────────────────
 * Constants
 * ───────────────────────────────────────────── */
#define MAX_CONTAINERS   32
#define STACK_SIZE       (1024 * 1024)   /* 1 MiB clone stack */
#define SOCK_PATH        "/tmp/engine.sock"
#define LOG_DIR          "/tmp/engine-logs"

#define DEFAULT_SOFT_MIB 40
#define DEFAULT_HARD_MIB 64

/* ─────────────────────────────────────────────
 * Container states
 * ───────────────────────────────────────────── */
typedef enum {
    STATE_STARTING = 0,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_EXITED,
} ContainerState;

static const char *state_str(ContainerState s) {
    switch (s) {
        case STATE_STARTING: return "starting";
        case STATE_RUNNING:  return "running";
        case STATE_STOPPED:  return "stopped";
        case STATE_KILLED:   return "killed";
        case STATE_EXITED:   return "exited";
        default:             return "unknown";
    }
}

/* ─────────────────────────────────────────────
 * Per-container metadata
 * ───────────────────────────────────────────── */
typedef struct {
    int           used;                /* slot occupied? */
    char          id[64];              /* container name/id */
    pid_t         host_pid;            /* PID on host */
    time_t        start_time;          /* unix timestamp */
    ContainerState state;
    long          soft_mib;            /* soft memory limit */
    long          hard_mib;            /* hard memory limit */
    char          log_path[256];       /* per-container log file */
    int           exit_code;           /* exit code if exited */
    int           exit_signal;         /* terminating signal, or 0 */
    int           stop_requested;      /* set before sending stop signal */
} ContainerMeta;

/* Global container table, protected by a mutex */
static ContainerMeta  g_containers[MAX_CONTAINERS];
static pthread_mutex_t g_meta_lock = PTHREAD_MUTEX_INITIALIZER;

/* ─────────────────────────────────────────────
 * Arguments passed into the container child
 * ───────────────────────────────────────────── */
typedef struct {
    char  rootfs[256];
    char  cmd[256];
    char  hostname[64];
} ChildArgs;

/* ─────────────────────────────────────────────
 * Supervisor shutdown flag
 * ───────────────────────────────────────────── */
static volatile int g_supervisor_running = 1;

/* ─────────────────────────────────────────────
 * Utility: find a container slot by id
 *   Caller must hold g_meta_lock or be single-threaded
 * ───────────────────────────────────────────── */
static int find_container(const char *id) {
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].used &&
            strcmp(g_containers[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* ─────────────────────────────────────────────
 * Container child entry point
 *   Runs inside the new namespaces (PID, UTS, mount).
 * ───────────────────────────────────────────── */
static int container_child(void *arg) {
    ChildArgs *a = (ChildArgs *)arg;

    /* --- UTS: set a unique hostname --- */
    if (sethostname(a->hostname, strlen(a->hostname)) < 0) {
        perror("sethostname");
        /* non-fatal */
    }

    /* --- Mount: chroot into container rootfs --- */
    if (chdir(a->rootfs) < 0) {
        perror("chdir rootfs");
        return 1;
    }
    if (chroot(".") < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    /* --- Mount /proc so tools like ps work --- */
    /* Make sure /proc exists in the rootfs */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
        /* non-fatal: continue without /proc */
    }

    /* --- Exec the requested command --- */
    char *argv[] = { a->cmd, NULL };
    char *envp[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "TERM=xterm",
        NULL
    };

    execve(a->cmd, argv, envp);
    perror("execve");
    return 1;
}

/* ─────────────────────────────────────────────
 * Launch a new container
 *   Returns the host PID on success, -1 on error.
 * ───────────────────────────────────────────── */
static pid_t launch_container(const char *id,
                               const char *rootfs,
                               const char *cmd,
                               long soft_mib,
                               long hard_mib)
{
    /* Allocate a metadata slot */
    pthread_mutex_lock(&g_meta_lock);
    int slot = -1;
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!g_containers[i].used) { slot = i; break; }
    }
    if (slot == -1) {
        pthread_mutex_unlock(&g_meta_lock);
        fprintf(stderr, "engine: container table full\n");
        return -1;
    }
    /* Mark slot in use immediately to avoid a TOCTOU race */
    g_containers[slot].used = 1;
    strncpy(g_containers[slot].id, id, sizeof(g_containers[slot].id) - 1);
    g_containers[slot].state       = STATE_STARTING;
    g_containers[slot].soft_mib    = soft_mib;
    g_containers[slot].hard_mib    = hard_mib;
    g_containers[slot].start_time  = time(NULL);
    g_containers[slot].exit_code   = 0;
    g_containers[slot].exit_signal = 0;
    g_containers[slot].stop_requested = 0;
    snprintf(g_containers[slot].log_path, sizeof(g_containers[slot].log_path),
             "%s/%s.log", LOG_DIR, id);
    pthread_mutex_unlock(&g_meta_lock);

    /* Build child args (on heap so clone child can access it) */
    ChildArgs *args = calloc(1, sizeof(ChildArgs));
    if (!args) { perror("calloc"); return -1; }
    strncpy(args->rootfs,   rootfs, sizeof(args->rootfs)   - 1);
    strncpy(args->cmd,      cmd,    sizeof(args->cmd)      - 1);
    strncpy(args->hostname, id,     sizeof(args->hostname) - 1);

    /* Allocate a stack for the child */
    char *stack = malloc(STACK_SIZE);
    if (!stack) { perror("malloc stack"); free(args); return -1; }
    char *stack_top = stack + STACK_SIZE;   /* stack grows down */

    /* Clone with PID, UTS, and mount namespace isolation */
    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

    pid_t pid = clone(container_child, stack_top, clone_flags, args);
    if (pid < 0) {
        perror("clone");
        free(stack);
        free(args);
        /* Release the metadata slot */
        pthread_mutex_lock(&g_meta_lock);
        g_containers[slot].used = 0;
        pthread_mutex_unlock(&g_meta_lock);
        return -1;
    }

    /* Update metadata with the real host PID */
    pthread_mutex_lock(&g_meta_lock);
    g_containers[slot].host_pid = pid;
    g_containers[slot].state    = STATE_RUNNING;
    pthread_mutex_unlock(&g_meta_lock);

    /* The child's stack and args will be cleaned up after waitpid().
     * For simplicity in Task 1 we leak them; Task 6 tracks them properly.
     * In a production runtime you would store these pointers in the metadata
     * and free them in the SIGCHLD handler. */
    (void)stack; /* suppress unused-variable warning */

    return pid;
}

/* ─────────────────────────────────────────────
 * SIGCHLD handler
 *   Reaps all exited children without blocking,
 *   then updates their metadata.
 * ───────────────────────────────────────────── */
static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_meta_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!g_containers[i].used || g_containers[i].host_pid != pid)
                continue;

            if (WIFEXITED(status)) {
                g_containers[i].exit_code = WEXITSTATUS(status);
                g_containers[i].state     = STATE_EXITED;
            } else if (WIFSIGNALED(status)) {
                g_containers[i].exit_signal = WTERMSIG(status);
                /* Distinguish voluntary stop from hard-limit kill */
                if (g_containers[i].stop_requested) {
                    g_containers[i].state = STATE_STOPPED;
                } else {
                    g_containers[i].state = STATE_KILLED;
                }
            }
            break;
        }
        pthread_mutex_unlock(&g_meta_lock);
    }
    errno = saved_errno;
}

/* ─────────────────────────────────────────────
 * SIGINT / SIGTERM handler — orderly supervisor shutdown
 * ───────────────────────────────────────────── */
static void sigterm_handler(int sig) {
    (void)sig;
    g_supervisor_running = 0;
}

/* ─────────────────────────────────────────────
 * Print container metadata table (ps command)
 * ───────────────────────────────────────────── */
static void cmd_ps(int fd) {
    char buf[4096];
    int  n = 0;

    n += snprintf(buf + n, sizeof(buf) - n,
        "%-16s %-8s %-10s %-10s %-10s %-10s %s\n",
        "ID", "PID", "STATE", "SOFT_MIB", "HARD_MIB", "EXIT", "LOG");

    pthread_mutex_lock(&g_meta_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!g_containers[i].used) continue;
        ContainerMeta *m = &g_containers[i];
        char exit_info[16] = "-";
        if (m->state == STATE_EXITED)
            snprintf(exit_info, sizeof(exit_info), "exit(%d)", m->exit_code);
        else if (m->state == STATE_STOPPED || m->state == STATE_KILLED)
            snprintf(exit_info, sizeof(exit_info), "sig(%d)", m->exit_signal);

        n += snprintf(buf + n, sizeof(buf) - n,
            "%-16s %-8d %-10s %-10ld %-10ld %-10s %s\n",
            m->id, m->host_pid, state_str(m->state),
            m->soft_mib, m->hard_mib, exit_info, m->log_path);
    }
    pthread_mutex_unlock(&g_meta_lock);

    write_fd(fd, buf, n);
}

/* ─────────────────────────────────────────────
 * Process a single CLI command string, write response to fd
 * ───────────────────────────────────────────── */
static void process_command(const char *line, int fd) {
    char cmd[32], id[64], rootfs[256], exec_cmd[256];
    long soft_mib = DEFAULT_SOFT_MIB, hard_mib = DEFAULT_HARD_MIB;
    int  nice_val = 0;
    char resp[512];

    /* Tokenise the command */
    if (sscanf(line, "%31s", cmd) != 1) {
        write_fd(fd, "ERR bad command\n", 16);
        return;
    }

    /* ── start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N] ── */
    if (strcmp(cmd, "start") == 0 || strcmp(cmd, "run") == 0) {
        if (sscanf(line, "%*s %63s %255s %255s", id, rootfs, exec_cmd) != 3) {
            write_fd(fd, "ERR usage: start <id> <rootfs> <cmd>\n", 37);
            return;
        }
        /* Parse optional flags */
        const char *p = line;
        char *flag;
        if ((flag = strstr(p, "--soft-mib"))) sscanf(flag, "--soft-mib %ld", &soft_mib);
        if ((flag = strstr(p, "--hard-mib"))) sscanf(flag, "--hard-mib %ld", &hard_mib);
        if ((flag = strstr(p, "--nice")))     sscanf(flag, "--nice %d",       &nice_val);

        /* Check for duplicate id */
        pthread_mutex_lock(&g_meta_lock);
        int dup = find_container(id);
        pthread_mutex_unlock(&g_meta_lock);
        if (dup >= 0) {
            snprintf(resp, sizeof(resp), "ERR container '%s' already exists\n", id);
            write_fd(fd, resp, strlen(resp));
            return;
        }

        pid_t pid = launch_container(id, rootfs, exec_cmd, soft_mib, hard_mib);
        if (pid < 0) {
            snprintf(resp, sizeof(resp), "ERR failed to start container '%s'\n", id);
            write_fd(fd, resp, strlen(resp));
            return;
        }

        /* Apply nice value to child */
        if (nice_val != 0) setpriority(PRIO_PROCESS, pid, nice_val);

        snprintf(resp, sizeof(resp), "OK started '%s' pid=%d\n", id, pid);
        write_fd(fd, resp, strlen(resp));

        /* For 'run': block until container exits */
        if (strcmp(cmd, "run") == 0) {
            int status;
            waitpid(pid, &status, 0);
            int ec = WIFEXITED(status) ? WEXITSTATUS(status)
                                       : 128 + WTERMSIG(status);
            snprintf(resp, sizeof(resp), "OK '%s' exited status=%d\n", id, ec);
            write_fd(fd, resp, strlen(resp));
        }
        return;
    }

    /* ── ps ── */
    if (strcmp(cmd, "ps") == 0) {
        cmd_ps(fd);
        return;
    }

    /* ── logs <id> ── */
    if (strcmp(cmd, "logs") == 0) {
        if (sscanf(line, "%*s %63s", id) != 1) {
            write_fd(fd, "ERR usage: logs <id>\n", 21);
            return;
        }
        pthread_mutex_lock(&g_meta_lock);
        int slot = find_container(id);
        char log_path[256] = "";
        if (slot >= 0) strncpy(log_path, g_containers[slot].log_path,
                               sizeof(log_path) - 1);
        pthread_mutex_unlock(&g_meta_lock);

        if (slot < 0) {
            snprintf(resp, sizeof(resp), "ERR unknown container '%s'\n", id);
            write_fd(fd, resp, strlen(resp));
            return;
        }
        /* Stream the log file */
        int lfd = open(log_path, O_RDONLY | O_CREAT, 0644);
        if (lfd < 0) {
            snprintf(resp, sizeof(resp), "ERR cannot open log '%s'\n", log_path);
            write_fd(fd, resp, strlen(resp));
            return;
        }
        char chunk[4096];
        ssize_t nr;
        while ((nr = read(lfd, chunk, sizeof(chunk))) > 0)
            write_fd(fd, chunk, nr);
        close(lfd);
        return;
    }

    /* ── stop <id> ── */
    if (strcmp(cmd, "stop") == 0) {
        if (sscanf(line, "%*s %63s", id) != 1) {
            write_fd(fd, "ERR usage: stop <id>\n", 21);
            return;
        }
        pthread_mutex_lock(&g_meta_lock);
        int slot = find_container(id);
        if (slot < 0) {
            pthread_mutex_unlock(&g_meta_lock);
            snprintf(resp, sizeof(resp), "ERR unknown container '%s'\n", id);
            write_fd(fd, resp, strlen(resp));
            return;
        }
        pid_t pid = g_containers[slot].host_pid;
        g_containers[slot].stop_requested = 1;
        ContainerState cur = g_containers[slot].state;
        pthread_mutex_unlock(&g_meta_lock);

        if (cur != STATE_RUNNING && cur != STATE_STARTING) {
            snprintf(resp, sizeof(resp), "OK container '%s' is not running\n", id);
            write_fd(fd, resp, strlen(resp));
            return;
        }
        /* Send SIGTERM first, then SIGKILL after a grace period */
        kill(pid, SIGTERM);
        /* Give the process up to 5 s to terminate gracefully */
        for (int i = 0; i < 50; i++) {
            usleep(100000);  /* 100 ms */
            pthread_mutex_lock(&g_meta_lock);
            int still_running =
                (g_containers[slot].state == STATE_RUNNING ||
                 g_containers[slot].state == STATE_STARTING);
            pthread_mutex_unlock(&g_meta_lock);
            if (!still_running) break;
        }
        /* If still alive, force kill */
        pthread_mutex_lock(&g_meta_lock);
        int still = (g_containers[slot].state == STATE_RUNNING ||
                     g_containers[slot].state == STATE_STARTING);
        pthread_mutex_unlock(&g_meta_lock);
        if (still) kill(pid, SIGKILL);

        snprintf(resp, sizeof(resp), "OK stop signalled '%s'\n", id);
        write_fd(fd, resp, strlen(resp));
        return;
    }

    /* ── shutdown (internal) ── */
    if (strcmp(cmd, "shutdown") == 0) {
        g_supervisor_running = 0;
        write_fd(fd, "OK shutting down\n", 17);
        return;
    }

    snprintf(resp, sizeof(resp), "ERR unknown command '%s'\n", cmd);
    write_fd(fd, resp, strlen(resp));
}

/* ─────────────────────────────────────────────
 * Supervisor main loop
 * ───────────────────────────────────────────── */
static void run_supervisor(const char *base_rootfs) {
    (void)base_rootfs;   /* kept for future use */

    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Install signal handlers */
    struct sigaction sa_chld = { .sa_handler = sigchld_handler,
                                  .sa_flags   = SA_RESTART | SA_NOCLDSTOP };
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = { .sa_handler = sigterm_handler };
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGINT,  &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    /* Create UNIX domain socket for CLI communication */
    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(srv, 8) < 0) { perror("listen"); exit(1); }

    /* Make the socket non-blocking so accept() doesn't stall shutdown */
    fcntl(srv, F_SETFL, O_NONBLOCK);

    printf("engine supervisor ready (socket: %s)\n", SOCK_PATH);
    fflush(stdout);

    while (g_supervisor_running) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000);  /* 50 ms poll */
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        /* Read the command line */
        char line[1024] = {0};
        ssize_t nr = read(cli, line, sizeof(line) - 1);
        if (nr > 0) {
            /* Strip trailing newline */
            line[strcspn(line, "\n")] = '\0';
            process_command(line, cli);
        }
        close(cli);
    }

    /* Orderly shutdown: SIGTERM all running containers */
    printf("engine: shutting down…\n");
    pthread_mutex_lock(&g_meta_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_containers[i].used &&
            (g_containers[i].state == STATE_RUNNING ||
             g_containers[i].state == STATE_STARTING)) {
            g_containers[i].stop_requested = 1;
            kill(g_containers[i].host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&g_meta_lock);
    /* Brief wait for children */
    sleep(2);

    close(srv);
    unlink(SOCK_PATH);
    printf("engine: supervisor exited cleanly\n");
}

/* ─────────────────────────────────────────────
 * CLI client — sends a command to the supervisor
 * ───────────────────────────────────────────── */
static void send_command(int argc, char *argv[]) {
    /* Reconstruct the command line from remaining args */
    char line[1024] = {0};
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(line, " ", sizeof(line) - strlen(line) - 1);
        strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "engine: cannot connect to supervisor at %s: %s\n",
                SOCK_PATH, strerror(errno));
        close(fd);
        exit(1);
    }

    /* Send the command */
    write_fd(fd, line, strlen(line));
    shutdown(fd, SHUT_WR);   /* signal EOF to supervisor */

    /* Print the response */
    char buf[4096];
    ssize_t nr;
    while ((nr = read(fd, buf, sizeof(buf))) > 0)
        fwrite(buf, 1, nr, stdout);

    close(fd);
}

/* ─────────────────────────────────────────────
 * main
 * ───────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs  <id>\n"
            "  %s stop  <id>\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        run_supervisor(argv[2]);
        return 0;
    }

    /* All other sub-commands are forwarded to the running supervisor */
    send_command(argc, argv);
    return 0;
}
