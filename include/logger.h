#ifndef LOGGER_H
#define LOGGER_H

int log_open(const char *path);
void log_event(const char *format, ...);
void log_close(void);

#endif
