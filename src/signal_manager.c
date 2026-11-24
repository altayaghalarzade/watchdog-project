#include "signal_manager.h"

volatile sig_atomic_t child_event, status_event, stop_event, timer_event;

static void record_signal(int signal_number)
{
    switch (signal_number) {
    case SIGCHLD: child_event = 1; break;
    case SIGUSR1: status_event = 1; break;
    case SIGALRM: timer_event = 1; break;
    default: stop_event = 1; break;
    }
}

int install_signals(sigset_t *wait_mask)
{
    const int signals[] = {SIGCHLD, SIGUSR1, SIGALRM, SIGTERM, SIGINT};
    struct sigaction action = {0};
    action.sa_handler = record_signal;
    sigemptyset(&action.sa_mask);
    for (unsigned i = 0; i < sizeof signals / sizeof signals[0]; ++i)
        sigaddset(&action.sa_mask, signals[i]);
    if (sigprocmask(SIG_BLOCK, &action.sa_mask, wait_mask) == -1)
        return -1;
    for (unsigned i = 0; i < sizeof signals / sizeof signals[0]; ++i) {
        action.sa_flags = signals[i] == SIGCHLD ? SA_NOCLDSTOP : 0;
        if (sigaction(signals[i], &action, 0) == -1)
            return -1;
        sigdelset(wait_mask, signals[i]);
    }
    return 0;
}

int shutdown_requested(void)
{
    sigset_t pending;
    return stop_event || (sigpending(&pending) == 0 &&
        (sigismember(&pending, SIGTERM) == 1 || sigismember(&pending, SIGINT) == 1));
}
