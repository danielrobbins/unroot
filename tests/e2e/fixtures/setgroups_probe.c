#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;

    gid_t group = (gid_t)strtoul(argv[1], NULL, 10);
    if (setgroups(1, &group) != 0) {
        printf("error:%d\n", errno);
        return 1;
    }

    gid_t actual = (gid_t)-1;
    if (getgroups(1, &actual) != 1) return 3;
    printf("group:%u\n", (unsigned)actual);
    return actual == group ? 0 : 4;
}
