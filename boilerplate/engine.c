#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CHILD_COMMAND_LEN 256

/* --- Data Structures --- */

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[4096];
    char command[CHILD_COMMAND_LEN];
} child_config_t;

/* --- Task 1: The Container Bubble --- */

int child_fn(void *arg) {
    child_config_t *config = (child_config_t *)arg;

    // 1. Isolation: Hostname
    sethostname(config->id, strlen(config->id));

    // 2. Isolation: Filesystem (The Fence)
    // This locks the process into its own rootfs folder
    if (chroot(config->rootfs) != 0 || chdir("/") != 0) {
        perror("chroot failed - check if folder exists");
        return 1;
    }

    // 3. Isolation: Process List
    // We must mount /proc so the container sees itself as PID 1
    mount("proc", "/proc", "proc", 0, NULL);

    // 4. Execution
    char *exec_args[] = { config->command, NULL };
    printf("[Container] Launching %s inside %s\n", config->command, config->id);
    
    execv(config->command, exec_args);

    // If we reach here, execv failed
    perror("execv failed");
    return 1;
}

/* --- Task 1: The Supervisor --- */

static int run_supervisor(const char *base_rootfs) {
    printf("[Supervisor] System initialized. Base: %s\n", base_rootfs);

    // Manually setting up "alpha" for Task 1 Demo
    child_config_t config;
    strncpy(config.id, "alpha", CONTAINER_ID_LEN);
    strncpy(config.rootfs, "./rootfs-alpha", 4096); 
    strncpy(config.command, "/bin/sh", CHILD_COMMAND_LEN);

    // Allocate stack for the child process
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc");
        return 1;
    }

    // clone() creates the namespaces
    // NEWPID = Private PIDs, NEWNS = Private Mounts, NEWUTS = Private Hostname
    pid_t child_pid = clone(child_fn, stack + STACK_SIZE, 
                            CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD, 
                            &config);

    if (child_pid == -1) {
        perror("clone failed");
        free(stack);
        return 1;
    }

    printf("[Supervisor] Container 'alpha' is live (Host PID: %d)\n", child_pid);

    // Wait for the container to exit to avoid zombies
    int status;
    waitpid(child_pid, &status, 0);

    printf("[Supervisor] Container 'alpha' has exited. Cleaning up.\n");
    free(stack);
    return 0;
}

/* --- CLI Helpers (Keep these from your boilerplate) --- */

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", prog);
    fprintf(stderr, "       %s start <id> <rootfs> <cmd>\n", prog);
}

/* --- Main Entry Point --- */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Missing base-rootfs path\n");
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    // Placeholder for Task 2 (CLI commands)
    if (strcmp(argv[1], "start") == 0) {
        printf("CLI mode will be implemented in Task 2 via Sockets.\n");
        printf("For Task 1, run: sudo ./engine supervisor ./rootfs-base\n");
        return 0;
    }

    usage(argv[0]);
    return 1;
}
