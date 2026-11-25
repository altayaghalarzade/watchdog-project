#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static FILE *log_stream;

int log_open(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd == -1)
        return -1;
    log_stream = fdopen(fd, "a");
    if (log_stream == NULL) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return 0;
}

void log_event(const char *format, ...)
{
    int error = errno;
    char stamp[32] = "time-unavailable";
    time_t now = time(NULL);
    struct tm local;
    if (now != (time_t)-1 && localtime_r(&now, &local) != NULL) {
        if (strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local) == 0)
            snprintf(stamp, sizeof stamp, "%s", "time-unavailable");
    }
    va_list arguments;
    fprintf(log_stream, "[%s] ", stamp);
    va_start(arguments, format);
    vfprintf(log_stream, format, arguments);
    va_end(arguments);
    fputc('\n', log_stream);
    if (fflush(log_stream) == EOF)
        perror("watchdog log");
    errno = error;
}

void log_close(void)
{
    if (fclose(log_stream) == EOF)
        perror("watchdog log");
    log_stream = NULL;
}
