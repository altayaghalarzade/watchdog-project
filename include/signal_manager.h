#ifndef SIGNAL_MANAGER_H
#define SIGNAL_MANAGER_H
#include <signal.h>

extern volatile sig_atomic_t child_event, status_event, stop_event, timer_event;
int install_signals(sigset_t *wait_mask);
int shutdown_requested(void);

#endif
