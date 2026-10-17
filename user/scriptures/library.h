/*
 * library.h - Downloading from the content API, and syncing notes with a
 * Church Account. Both run on their own thread; the window reads their
 * progress (job_t) and is woken when something changes.
 */
#ifndef LIBRARY_H
#define LIBRARY_H

#include <pthread.h>

typedef struct { const char *code, *title; int standard; } volinfo_t;
extern const volinfo_t VOLUMES[];
extern const int NVOLUMES;

typedef struct job {
    pthread_mutex_t mu;
    volatile int running, cancel;
    int total, done, failed;
    char message[256];
    char log[4096];                  /* sync: what it did, line by line */
    int auth_error;                  /* sync: the session was refused */
    void (*wake)(void);              /* called (from the job's thread) when something changed */
} job_t;

/* Downloads volumes (codes) for a language: contents, then every page not yet here. */
int  download_start(job_t *j, const char *lang, const char **codes, int ncodes);

/* One page, now (a link to something not downloaded). 0: saved. */
int  fetch_chapter(const char *lang, const char *uri, char **err);

/* Sync: pull every annotation; with push, also send local changes. */
int  sync_start(job_t *j, int push);

/* Notes synced in the v3 shape before it was understood: their verses, back from the
 * server copy kept with each. Returns how many were repaired. */
int  notes_repair(void);

/* Is this Cookie header a working session? 1 yes, 0 refused, -1 no answer. */
int  session_check(const char *cookie, char **err);

#endif
