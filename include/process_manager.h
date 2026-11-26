#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <sys/types.h>
#include <signal.h>
#include <stddef.h>

#define MAX_RESTARTS 3

typedef struct {
    const char *name;
    const char *mode;
    pid_t pid;
    unsigned restarts;
} Worker;

/**
 * @brief Starts a worker child process using fork() and exec().
 *
 * @param w Pointer to Worker struct to start and record PID into.
 * @param worker_path Path to the worker executable.
 * @param child_mask Signal mask to apply in child (unblocks all if NULL).
 * @return int 0 on success, -1 on error.
 */
int start_worker(Worker *w, const char *worker_path, const sigset_t *child_mask);

/**
 * @brief Reaps terminated worker processes using waitpid(-1, WNOHANG) and
 *        restarts workers that terminated unexpectedly (up to MAX_RESTARTS).
 *
 * @param workers Array of Worker structures.
 * @param count Number of workers in array.
 * @param worker_path Path to the worker executable for restarts.
 * @param child_mask Signal mask to apply to restarted children.
 * @param shutting_down Non-zero if watchdog is currently shutting down.
 * @return int 0 on success, 1 on child startup failure, -1 on system call error.
 */
int reap_workers(Worker *workers, size_t count, const char *worker_path, const sigset_t *child_mask, int shutting_down);

/* Periodic worker monitoring */
size_t active_workers(const Worker *workers, size_t count);
void start_periodic_check(void);
void periodic_check(const Worker *workers, size_t count);

#endif /* PROCESS_MANAGER_H */
