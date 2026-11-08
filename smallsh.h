#ifndef SMALLSH_H
#define SMALLSH_H

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#define EOL        1   /* end of line */
#define ARG        2   /* normal arguments */
#define AMPERSAND  3
#define SEMICOLON  4

#define MAXARG     512 /* max. no. command args */
#define MAXBUF     512 /* max. length input line */
#define MAXJOBS    64  /* max. no. of tracked jobs */

#define FOREGROUND 0
#define BACKGROUND 1

/* Job states for job tracking */
#define RUNNING    1
#define STOPPED    2

int userin(char *p);
int gettok(char **outptr);
int inarg(char c);
int procline(void);
int runcommand(char **cline, int where);

#endif
