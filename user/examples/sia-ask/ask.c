/*
 * ask.c - Ask the assistant from the command line with libsia: no
 * endpoints, keys or HTTP here, the library uses the model registered with
 * sia.  Answers are printed as the model writes them (sia_chat_send_stream).
 *
 *   cc ask.c -lsia -o ask
 *   ./ask "What is the capital of Australia?"
 *   ./ask -c                  a conversation: one line per turn, empty line ends
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sia/sia.h>

static void piece(void *ctx, const char *utf8)
{
    (void)ctx;
    char *p = sia_plain(utf8);
    fputs(p, stdout);
    fflush(stdout);
    free(p);
}

/* One turn: the answer streamed to stdout; false on failure. */
static int turn(sia_chat *c, const char *text)
{
    char err[256];
    char *a = sia_chat_send_stream(c, text, piece, NULL, err, sizeof(err));
    if (!a) {
        fprintf(stderr, "ask: %s\n", err);
        return 0;
    }
    if (!*a || a[strlen(a) - 1] != '\n')
        putchar('\n');
    free(a);
    return 1;
}

int main(int argc, char **argv)
{
    if (!sia_available()) {
        fprintf(stderr, "ask: no model is registered (run sia once)\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "-c")) {
        sia_chat *c = sia_chat_new(NULL);
        int ok = c && turn(c, argv[1]);
        sia_chat_free(c);
        return !ok;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: ask question | ask -c\n");
        return 2;
    }
    sia_chat *c = sia_chat_new(NULL);
    printf("Talking to %s. An empty line ends.\n", sia_model());
    char line[1024];
    while (printf("> "), fflush(stdout), fgets(line, sizeof(line), stdin) && line[0] != '\n') {
        line[strcspn(line, "\n")] = 0;
        turn(c, line);
    }
    sia_chat_free(c);
    return 0;
}
