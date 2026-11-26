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

int start_worker(Worker *w, const char *worker_path, const sigset_t *child_mask) {
    if (w == NULL || worker_path == NULL) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* Child process */

        /* Reset inherited signal handlers to default before unmasking signals */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;

        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGCHLD, &sa, NULL);
        sigaction(SIGUSR1, &sa, NULL);
        sigaction(SIGUSR2, &sa, NULL);
        sigaction(SIGALRM, &sa, NULL);
        sigaction(SIGHUP, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
        sigaction(SIGPIPE, &sa, NULL);

        /* Apply child_mask to unmask signals */
        if (child_mask != NULL) {
            if (sigprocmask(SIG_SETMASK, child_mask, NULL) < 0) {
                _exit(127);
            }
        } else {
            sigset_t empty;
            sigemptyset(&empty);
            if (sigprocmask(SIG_SETMASK, &empty, NULL) < 0) {
                _exit(127);
            }
        }

        /* Execute worker executable with MODE argument */
        const char *arg0 = worker_path;
        if (w->mode && w->mode[0] != '\0') {
            execl(worker_path, arg0, w->mode, (char *)NULL);
        } else {
            execl(worker_path, arg0, (char *)NULL);
        }

        /* On exec error, exit immediately with _exit without flushing stdio */
        _exit(127);
    }

    /* Parent process */
    w->pid = pid;
    log_event("START pid=%d worker=%s mode=%s", (int)w->pid, w->name ? w->name : "unknown", w->mode ? w->mode : "default");
    return 0;
}

int reap_workers(Worker *workers, size_t count, const char *worker_path, const sigset_t *child_mask, int shutting_down) {
    if (workers == NULL && count > 0) {
        return -1;
    }

    int result = 0;

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

        /* The child reserves 127 for failures before executing the worker. */
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            log_event("ERROR worker=%s could not start", wname);
            if (result == 0)
                result = 1;
            continue;
        }

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

            if (worker_path != NULL) {
                if (start_worker(w, worker_path, child_mask) < 0) {
                    result = -1;
                }
            } else {
                result = -1;
            }
        } else {
            log_event("LIMIT pid=%d worker=%s restarts=%u", (int)pid, wname, w->restarts);
        }
    }

    return result;
}
