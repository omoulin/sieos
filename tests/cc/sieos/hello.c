/* hello.c - compiled on SIEOS by its own compiler (tools/cctest.py). */
#include "mk.h"

int main(int argc, char **argv)
{
    printf("hello from sicc on SIEOS: %d argument(s), first %s\n", argc, argc > 1 ? argv[1] : "-");
    return 0;
}
