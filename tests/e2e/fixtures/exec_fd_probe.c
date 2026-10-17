#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    int last = atoi(argv[2]);
    int remaining = atoi(argv[3]);
    if (strcmp(argv[1], "launch") == 0) {
        for (int fd = 3; fd <= 300; ++fd) close(fd);
        for (int fd = 3; fd <= last; ++fd) {
            if (dup2(STDOUT_FILENO, fd) != fd) return 3;
        }
        int fd = open("/caller-data", O_RDONLY);
        if (fd < 0 || dup2(fd, 300) != 300) return 4;
        if (fd != 300) close(fd);
    } else {
        errno = 0;
        if (fcntl(last + 1, F_GETFD) != -1 || errno != EBADF) {
            fprintf(stderr, "descriptor %d unexpectedly survived exec\n", last + 1);
            return 5;
        }
        /* The wrapper must not close unrelated caller-owned descriptors. */
        char data[6] = {0};
        if (pread(300, data, 5, 0) != 5 || strcmp(data, "hello") != 0) return 6;
        for (int fd = 3; fd <= last; ++fd) {
            if (fcntl(fd, F_GETFD) < 0) return 7;
        }
        if (--remaining == 0) {
            puts("descriptors-ok");
            return 0;
        }
    }
    /* Reserve a predictable close-on-exec descriptor like Gnulib test-fcntl. */
    if (fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, last + 1) != last + 1) return 8;
    char count[16];
    snprintf(count, sizeof(count), "%d", remaining);
    execl("/bin/probe", "probe", "child", argv[2], count, (char *) NULL);
    perror("execl");
    return 9;
}
