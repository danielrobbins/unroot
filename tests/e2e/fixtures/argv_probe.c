#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[1], "--exec") == 0) {
        execv(argv[2], argv + 3);
        perror("execv");
        return 1;
    }
    for (int i = 0; i < argc; ++i)
        printf("arg%d=<%s>\n", i, argv[i]);
    return 0;
}
