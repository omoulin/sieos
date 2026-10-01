/*
 * @TITLE@ - made with MiR (Make it Real) on SIEOS.
 * A terminal program: it reads standard input and writes standard output.
 */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0;
        printf("%d: %s\n", ++n, line);
    }
    printf("%d lines\n", n);
    return 0;
}
