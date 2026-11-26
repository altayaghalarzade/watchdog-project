#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    if (strcmp(argv[1], "normal") == 0)
        return 0;
    if (strcmp(argv[1], "error") == 0)
        return 1;
    if (strcmp(argv[1], "stubborn") == 0) {
        struct sigaction action = {0};
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGTERM, &action, NULL) == -1) {
            perror("sigaction");
            return 1;
        }
    } else if (strcmp(argv[1], "idle") != 0) {
        fprintf(stderr, "Unknown worker mode: %s\n", argv[1]);
        return 2;
    }

    for (;;)
        pause();
}
