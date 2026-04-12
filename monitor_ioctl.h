#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

/*
 * monitor_ioctl.h — Shared ioctl definitions between engine.c and monitor.c
 *
 * Fully implemented in Task 4. This stub lets engine.c compile for Tasks 1-3
 * without the kernel module being present.
 */

#include <linux/ioctl.h>

#define MONITOR_MAGIC 'M'

/* Register a container PID with soft/hard memory limits */
struct monitor_reg {
    pid_t pid;
    long  soft_mib;
    long  hard_mib;
};

/* Unregister a container PID */
struct monitor_unreg {
    pid_t pid;
};

#define MONITOR_IOC_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_reg)
#define MONITOR_IOC_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_unreg)

#define MONITOR_DEVICE "/dev/container_monitor"

#endif /* MONITOR_IOCTL_H */
