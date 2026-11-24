#include "logger.h"
#include "process_manager.h"
#include "signal_manager.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

enum { CHECK_SECONDS = 2, SHUTDOWN_SECONDS = 3 };

static size_t active_workers(const Worker *workers, size_t count)
{
    size_t active = 0;
    for (size_t i = 0; i < count; ++i)
        active += workers[i].pid > 0;
    return active;
}

static void signal_workers(const Worker *workers, size_t count, int sig)
{
    for (size_t i = 0; i < count; ++i) {
        if (workers[i].pid <= 0)
            continue;
        if (kill(workers[i].pid, sig) == -1 && errno != ESRCH)
            log_event("ERROR kill worker=%s pid=%ld signal=%d errno=%d",
                      workers[i].name, (long)workers[i].pid, sig, errno);
    }
}

int main(int argc, char **argv)
{
    if (argc > 4) {
        fprintf(stderr, "Usage: %s [LOG [MODE1 [MODE2]]]\n", argv[0]);
        return 2;
    }
    const char *path = argc > 1 ? argv[1] : "watchdog.log";
    const char *worker_path = "./watchdog-worker";
    Worker workers[] = {
        {.name = "alpha", .mode = argc > 2 ? argv[2] : "idle"},
        {.name = "beta", .mode = argc > 3 ? argv[3] : "idle"}
    };
    const size_t count = sizeof workers / sizeof workers[0];
    sigset_t wait_mask;
    if (install_signals(&wait_mask) == -1) {
        perror("install_signals");
        return 1;
    }
    if (log_open(path) == -1) {
        perror(path);
        return 1;
    }

    int failed = 0;
    int stopping = 0;
    int forced = 0;
    log_event("WATCHDOG pid=%ld", (long)getpid());
    for (size_t i = 0; i < count && !shutdown_requested(); ++i) {
        if (start_worker(&workers[i], worker_path, &wait_mask) == -1) {
            log_event("ERROR start worker=%s errno=%d", workers[i].name, errno);
            failed = 1;
            break;
        }
    }
    alarm(CHECK_SECONDS);
    for (;;) {
        if (!stopping && (failed || shutdown_requested())) {
            stopping = 1;
            alarm(0);
            sigset_t alarm_set;
            const struct timespec no_wait = {0, 0};
            sigemptyset(&alarm_set);
            sigaddset(&alarm_set, SIGALRM);
            /* Discard an expired periodic tick before starting the grace timer. */
            while (sigtimedwait(&alarm_set, NULL, &no_wait) == -1 && errno == EINTR)
                ;
            timer_event = 0;
            log_event("SHUTDOWN begin timeout=%d", SHUTDOWN_SECONDS);
            signal_workers(workers, count, SIGTERM);
            alarm(SHUTDOWN_SECONDS);
        }
        if (child_event || timer_event || stopping || status_event) {
            child_event = 0;
            int reap_result = reap_workers(workers, count, worker_path, &wait_mask, stopping);
            if (reap_result != 0) {
                if (reap_result == -1)
                    log_event("ERROR reap errno=%d", errno);
                failed = 1;
                if (!stopping)
                    continue;
            }
        }
        if (status_event) {
            status_event = 0;
            for (size_t i = 0; i < count; ++i)
                log_event("STATUS worker=%s pid=%ld state=%s restarts=%u",
                          workers[i].name, (long)workers[i].pid,
                          workers[i].pid > 0 ? "running" : "inactive", workers[i].restarts);
        }
        if (active_workers(workers, count) == 0) {
            if (!stopping && shutdown_requested())
                continue;
            break;
        }
        if (timer_event) {
            timer_event = 0;
            if (!stopping) {
                log_event("CHECK active=%zu", active_workers(workers, count));
                alarm(CHECK_SECONDS);
            } else if (!forced) {
                forced = 1;
                log_event("SHUTDOWN escalate signal=SIGKILL");
                signal_workers(workers, count, SIGKILL);
            }
        }
        if (!stopping && shutdown_requested())
            continue;
        sigsuspend(&wait_mask);
    }
    alarm(0);
    log_event("SHUTDOWN complete result=%s", failed ? "error" : "normal");
    log_close();
    return failed ? 1 : 0;
}
