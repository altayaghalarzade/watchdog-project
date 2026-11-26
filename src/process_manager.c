#define _POSIX_C_SOURCE 200809L

#include "process_manager.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>


/* Report a child setup or exec failure to the parent. */
static void child_start_failed(int fd)
{
    int error = errno;

    while (write(fd, &error, sizeof error) == -1
           && errno == EINTR) {
    }

    _exit(127);
}

int start_worker(Worker *w, const char *worker_path,
                 const sigset_t *child_mask)
{
    if (w == NULL || worker_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (w->pid > 0) {
        errno = EBUSY;
        return -1;
    }

    int error_pipe[2];

    if (pipe(error_pipe) == -1)
        return -1;

    /* Successful exec automatically closes the write end. */
    if (fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
        int saved = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        errno = saved;
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        int saved = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        errno = saved;
        return -1;
    }

    if (pid == 0) {
        close(error_pipe[0]);

        struct sigaction sa = {0};
        sa.sa_handler = SIG_DFL;

        if (sigemptyset(&sa.sa_mask) == -1)
            child_start_failed(error_pipe[1]);

        const int signals[] = {
            SIGINT, SIGTERM, SIGCHLD,
            SIGUSR1, SIGUSR2, SIGALRM,
            SIGHUP, SIGQUIT, SIGPIPE
        };

        for (size_t i = 0;
             i < sizeof signals / sizeof signals[0]; ++i) {
            if (sigaction(signals[i], &sa, NULL) == -1)
                child_start_failed(error_pipe[1]);
        }

        sigset_t empty;

        if (child_mask == NULL) {
            if (sigemptyset(&empty) == -1)
                child_start_failed(error_pipe[1]);

            child_mask = &empty;
        }

        if (sigprocmask(SIG_SETMASK, child_mask, NULL) == -1)
            child_start_failed(error_pipe[1]);

        if (w->mode && w->mode[0] != '\0') {
            execl(worker_path, worker_path,
                  w->mode, (char *)NULL);
        } else {
            execl(worker_path, worker_path, (char *)NULL);
        }

        child_start_failed(error_pipe[1]);
    }

    close(error_pipe[1]);

    int child_error = 0;
    int read_error = 0;
    size_t received = 0;

    while (received < sizeof child_error) {
        ssize_t n = read(
            error_pipe[0],
            (char *)&child_error + received,
            sizeof child_error - received
        );

        if (n > 0) {
            received += (size_t)n;
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            read_error = errno;
            break;
        }
    }

    close(error_pipe[0]);

    if (read_error || received != 0) {
        int saved = read_error ? read_error :
            (received == sizeof child_error && child_error
             ? child_error : EIO);

        if (read_error || received != sizeof child_error)
            kill(pid, SIGKILL);

        while (waitpid(pid, NULL, 0) == -1
               && errno == EINTR) {
        }

        log_event("ERROR worker=%s could not start errno=%d",
                  w->name ? w->name : "unknown", saved);

        errno = saved;
        return -1;
    }

    w->pid = pid;

    log_event("START pid=%d worker=%s mode=%s",
              (int)w->pid,
              w->name ? w->name : "unknown",
              w->mode ? w->mode : "default");

    return 0;
}

int reap_workers(Worker *workers, size_t count, const char *worker_path, const sigset_t *child_mask, int shutting_down) {
    if (workers == NULL && count > 0) {
        return -1;
    }

    int restart_error = 0;

    while (1) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);

        if (pid == 0) {
            /* No more terminated children available right now */
            break;
        }

        if (pid < 0) {
            if (errno == ECHILD) {
                /* All children reaped or none exist: normal condition */
                break;
            }
            if (errno == EINTR) {
                /* Interrupted by signal, retry waitpid */
                continue;
            }
            /* Real error from waitpid */
            return -1;
        }

        /* Find matching worker structure */
        Worker *w = NULL;
        for (size_t i = 0; i < count; i++) {
            if (workers[i].pid == pid) {
                w = &workers[i];
                break;
            }
        }

        const char *wname = (w && w->name) ? w->name : "unknown";

        /* Determine termination status */
        int is_normal = 0;
        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            if (exit_code == 0) {
                is_normal = 1;
                log_event("EXIT pid=%d worker=%s result=normal exit_code=0", (int)pid, wname);
            } else {
                log_event("EXIT pid=%d worker=%s exit_code=%d", (int)pid, wname, exit_code);
            }
        } else if (WIFSIGNALED(status)) {
            int term_sig = WTERMSIG(status);
            log_event("EXIT pid=%d worker=%s signal=%d", (int)pid, wname, term_sig);
        } else {
            log_event("EXIT pid=%d worker=%s status=%d", (int)pid, wname, status);
        }

        if (w == NULL) {
            /* Child was not in our tracked list */
            continue;
        }

        /* Mark worker process as dead */
        w->pid = 0;

        /* Check shutdown condition and pending SIGTERM / SIGINT */
        int is_shutting_down = shutting_down;
        sigset_t pending;
        sigemptyset(&pending);
        if (sigpending(&pending) == 0) {
            if (sigismember(&pending, SIGTERM) || sigismember(&pending, SIGINT)) {
                is_shutting_down = 1;
            }
        }

        if (is_normal || is_shutting_down) {
            /* No restart on normal exit (code 0) or during shutdown / pending TERM/INT */
            continue;
        }

        /* Unexpected termination: bounded automatic restart up to MAX_RESTARTS (3) */
        if (w->restarts < MAX_RESTARTS) {
            w->restarts++;
            log_event("RESTART pid=%d worker=%s attempt=%u", (int)pid, wname, w->restarts);

            if (start_worker(w, worker_path, child_mask) < 0) {
                restart_error = errno;
                log_event("ERROR restart worker=%s errno=%d",
                          wname, restart_error);
            }
        } else {
            log_event("LIMIT pid=%d worker=%s restarts=%u", (int)pid, wname, w->restarts);
        }
    }

    if (restart_error != 0) {
        errno = restart_error;
        return -1;
    }

    return 0;
}


/* Worker monitoring interval in seconds */
enum { CHECK_SECONDS = 2 };

size_t active_workers(const Worker *workers, size_t count)
{
    size_t active = 0;

    for (size_t i = 0; i < count; ++i) {
        if (workers[i].pid > 0)
            ++active;
    }

    return active;
}

void start_periodic_check(void)
{
    alarm(CHECK_SECONDS);
}

void periodic_check(const Worker *workers, size_t count)
{
    log_event("CHECK active=%zu",
              active_workers(workers, count));

    start_periodic_check();
}
