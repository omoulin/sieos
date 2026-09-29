/*
 * sia/sia.h - The SIEOS assistant for applications (libsia).
 *
 *   #include <sia/sia.h>          cc app.c -lsia
 *
 * The library uses the model registered with sia for the user running the
 * program (~/.sia/config, written by sia's first-use setup or at build
 * time): it finds the endpoint, model and key, speaks HTTPS to the model and
 * keeps conversations.  Applications only ask.
 *
 *     char err[256];
 *     char *answer = sia_complete(NULL, "Name three prime numbers.", err, sizeof(err));
 *     if (answer) { puts(answer); free(answer); } else fprintf(stderr, "%s\n", err);
 *
 * Conversations remember what was said:
 *
 *     sia_chat *c = sia_chat_new("You help people write haiku.");
 *     char *a = sia_chat_send(c, "One about the sea.", err, sizeof(err));
 *     char *b = sia_chat_send(c, "Now make it about the night.", err, sizeof(err));
 *
 * A request takes seconds, and the model writes its answer as it goes.
 * sia_chat_send_stream() hands each piece to a callback as it arrives.  An
 * interactive program (a Facet application, say) starts the request with
 * sia_chat_send_async(), watches the descriptor it returns in its poll
 * loop, and each time it is readable takes the new text with
 * sia_chat_poll() until that says the answer is done -- or simply collects
 * the whole answer with sia_chat_result().
 * Answers are UTF-8; sia_plain() turns one into text for the 8-bit
 * console and Facet fonts.  All functions return NULL or -1 on failure,
 * with a message in err.
 */
#ifndef SIA_SIA_H
#define SIA_SIA_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Is a model registered for this user?  Its name (static), or NULL. */
bool sia_available(void);
const char *sia_model(void);

/* One question, one answer (malloc'd).  instructions: the system prompt, or
 * NULL for sia's default ("a concise, helpful assistant on SIEOS"). */
char *sia_complete(const char *instructions, const char *prompt, char *err, size_t errlen);

/* ---------------- conversations ---------------- */

typedef struct sia_chat sia_chat;

sia_chat *sia_chat_new(const char *instructions);     /* NULL if no model is registered */
void sia_chat_free(sia_chat *c);
void sia_chat_reset(sia_chat *c);                     /* forget what was said */

/* Say something and wait for the answer (malloc'd). */
char *sia_chat_send(sia_chat *c, const char *text, char *err, size_t errlen);

/* The same, passing each piece of the answer to piece() as it arrives
 * (pieces end on UTF-8 character boundaries).  Returns the whole answer. */
char *sia_chat_send_stream(sia_chat *c, const char *text, void (*piece)(void *ctx, const char *utf8), void *ctx,
                           char *err, size_t errlen);

/* The same without waiting: returns a descriptor that becomes readable when
 * the answer is ready (poll it), or -1.  One request at a time per chat. */
int  sia_chat_send_async(sia_chat *c, const char *text, char *err, size_t errlen);
bool sia_chat_busy(const sia_chat *c);
/* When the descriptor is readable: the text that arrived since the last
 * call (malloc'd, possibly ""), without waiting.  *done is set once the
 * answer is complete -- the descriptor is then closed and the exchange
 * becomes part of the conversation -- or the request failed (NULL, err). */
char *sia_chat_poll(sia_chat *c, bool *done, char *err, size_t errlen);
/* The whole answer to the request started by sia_chat_send_async (waits
 * until it is complete, including any part already taken with
 * sia_chat_poll); the descriptor is closed. */
char *sia_chat_result(sia_chat *c, char *err, size_t errlen);
void sia_chat_cancel(sia_chat *c);                    /* drop the request in progress */

/* Text for the 8-bit fonts: ASCII punctuation, no Markdown emphasis (malloc'd). */
char *sia_plain(const char *utf8);

#ifdef __cplusplus
}
#endif

#endif
