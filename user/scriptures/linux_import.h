/*
 * linux_import.h - Linux only (linux_import.c): the old Python library and Firefox.
 */
#ifndef LINUX_IMPORT_H
#define LINUX_IMPORT_H

/* Chapters, contents and notes from DATA/library.db; returns the chapters seen,
 * 0 when there is no such file, -1 when libsqlite3 is missing. */
int import_python_library(void (*progress)(const char *msg, void *ctx), void *ctx);

/* The churchofjesuschrist.org session in Firefox (a Cookie header), or NULL. */
char *firefox_cookie(char **profile);

/* The default web browser's name ("Brave"), and whether it is Firefox. */
const char *default_browser(int *is_firefox);

#endif
