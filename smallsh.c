/* smallsh.c - modified with signals and job control */
#include "smallsh.h"

/* program buffers and work pointers */
static char inpbuf[MAXBUF], tokbuf[2 * MAXBUF],
    *ptr = inpbuf, *tok = tokbuf;

static char special[] = {' ', '\t', '&', ';', '\n', '\0'};
char *prompt = "Command> "; /* prompt */

/* struct to keep track of jobs */
struct job {
    int jid;                /* job number like 1, 2, etc. */
    pid_t pid;              /* process id */
    pid_t pgid;             /* process group id */
    int state;              /* RUNNING or STOPPED */
    char command[MAXBUF];   /* the command typed by the user */
    int active;             /* 1 if being used, 0 if empty */
};

static struct job jobs_list[MAXJOBS];
static int next_jid = 1;
static pid_t fg_pid = 0;       /* pid of current foreground process */
static pid_t shell_pgid = 0;   /* group id of smallsh */

/* catch Ctrl+C so it doesn't close our shell */
void sigint_handler(int sig)
{
    (void)sig;
    if (fg_pid > 0) {
        /* send Ctrl+C to the foreground job instead */
        kill(-fg_pid, SIGINT);
    }
}

/* catch Ctrl+Z to pause the foreground job */
void sigtstp_handler(int sig)
{
    (void)sig;
    if (fg_pid > 0) {
        /* send Ctrl+Z to the foreground job */
        kill(-fg_pid, SIGTSTP);
    }
}

/* reset job counter back to 1 if there are no active jobs left */
void reset_next_jid(void)
{
    int i;
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs_list[i].active)
            return;
    }
    next_jid = 1;
}

/* add a job to the array or update it if it's already there */
int add_or_update_job(pid_t pid, pid_t pgid, int state, const char *cmd)
{
    int i;

    /* check if the job is already in our list */
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs_list[i].active && jobs_list[i].pid == pid) {
            jobs_list[i].state = state;
            return jobs_list[i].jid;
        }
    }

    /* find an open spot in the array for the new job */
    for (i = 0; i < MAXJOBS; i++) {
        if (!jobs_list[i].active) {
            jobs_list[i].active = 1;
            jobs_list[i].jid = next_jid++;
            jobs_list[i].pid = pid;
            jobs_list[i].pgid = pgid;
            jobs_list[i].state = state;
            strncpy(jobs_list[i].command, cmd, MAXBUF - 1);
            jobs_list[i].command[MAXBUF - 1] = '\0';
            return jobs_list[i].jid;
        }
    }
    printf("smallsh: job list is full\n");
    return -1;
}

/* remove a finished job using its pid */
void remove_job_by_pid(pid_t pid)
{
    int i;
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs_list[i].active && jobs_list[i].pid == pid) {
            jobs_list[i].active = 0;
            reset_next_jid();
            return;
        }
    }
}

/* look up a job by %jid, jid, or pid (or get the newest job if nothing typed) */
struct job *find_job(char *arg)
{
    int i;
    struct job *latest = NULL;

    /* if no argument given, grab the most recent job */
    if (arg == NULL) {
        for (i = 0; i < MAXJOBS; i++) {
            if (jobs_list[i].active) {
                if (latest == NULL || jobs_list[i].jid > latest->jid)
                    latest = &jobs_list[i];
            }
        }
        return latest;
    }

    /* if it starts with %, look for the job id */
    if (arg[0] == '%') {
        int jid = atoi(arg + 1);
        for (i = 0; i < MAXJOBS; i++) {
            if (jobs_list[i].active && jobs_list[i].jid == jid)
                return &jobs_list[i];
        }
        return NULL;
    }

    /* otherwise check if the number matches a pid or job id */
    int val = atoi(arg);
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs_list[i].active && jobs_list[i].pid == val)
            return &jobs_list[i];
    }
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs_list[i].active && jobs_list[i].jid == val)
            return &jobs_list[i];
    }
    return NULL;
}

/* check if any background jobs finished or changed state */
void update_jobs(void)
{
    int i, status;
    pid_t res;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs_list[i].active) {
            /* WNOHANG checks without pausing the shell */
            res = waitpid(jobs_list[i].pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (res > 0) {
                if (WIFEXITED(status)) {
                    printf("[%d] Done                    %s (PID: %d)\n",
                           jobs_list[i].jid, jobs_list[i].command, jobs_list[i].pid);
                    jobs_list[i].active = 0;
                    reset_next_jid();
                } else if (WIFSIGNALED(status)) {
                    printf("[%d] Terminated              %s (PID: %d)\n",
                           jobs_list[i].jid, jobs_list[i].command, jobs_list[i].pid);
                    jobs_list[i].active = 0;
                    reset_next_jid();
                } else if (WIFSTOPPED(status)) {
                    jobs_list[i].state = STOPPED;
                } else if (WIFCONTINUED(status)) {
                    jobs_list[i].state = RUNNING;
                }
            } else if (res == -1 && errno == ECHILD) {
                jobs_list[i].active = 0;
                reset_next_jid();
            }
        }
    }
}

/* wait for a foreground process to finish or get paused */
int wait_for_fg(pid_t pid, const char *cmd_str)
{
    int status = 0;
    fg_pid = pid;

    /* give terminal control to the foreground process group */
    if (isatty(STDIN_FILENO)) {
        tcsetpgrp(STDIN_FILENO, pid);
    }

    /* wait until the process exits or is stopped with Ctrl+Z */
    while (waitpid(pid, &status, WUNTRACED) == -1) {
        if (errno != EINTR) {
            fg_pid = 0;
            if (isatty(STDIN_FILENO))
                tcsetpgrp(STDIN_FILENO, shell_pgid);
            return -1;
        }
    }

    /* give terminal control back to smallsh */
    if (isatty(STDIN_FILENO)) {
        tcsetpgrp(STDIN_FILENO, shell_pgid);
    }

    /* if stopped with Ctrl+Z, save it in the jobs list */
    if (WIFSTOPPED(status)) {
        int jid = add_or_update_job(pid, pid, STOPPED, cmd_str);
        printf("\n[%d] Stopped                 %s (PID: %d)\n", jid, cmd_str, pid);
    } else {
        if (WIFSIGNALED(status)) {
            printf("\n");
        }
        remove_job_by_pid(pid);
    }

    fg_pid = 0;
    return status;
}

/* built-in jobs command: print all active jobs */
void builtin_jobs(void)
{
    int i, found = 0;
    update_jobs();

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs_list[i].active) {
            printf("[%d] %-10s %s (PID: %d, PGID: %d)\n",
                   jobs_list[i].jid,
                   (jobs_list[i].state == RUNNING) ? "Running" : "Stopped",
                   jobs_list[i].command,
                   jobs_list[i].pid,
                   jobs_list[i].pgid);
            found = 1;
        }
    }
    if (!found) {
        printf("No active jobs.\n");
    }
}

/* built-in fg command: bring a background or stopped job to the foreground */
void builtin_fg(char **cline)
{
    update_jobs();
    struct job *j = find_job(cline[1]);
    if (j == NULL) {
        printf("fg: job not found\n");
        return;
    }

    printf("%s\n", j->command);
    j->state = RUNNING;

    /* tell the process group to keep running in case it was stopped */
    if (kill(-j->pgid, SIGCONT) == -1) {
        perror("smallsh: fg");
        return;
    }

    wait_for_fg(j->pid, j->command);
}

/* built-in bg command: resume a stopped job in the background */
void builtin_bg(char **cline)
{
    update_jobs();
    struct job *j = find_job(cline[1]);
    if (j == NULL) {
        printf("bg: job not found\n");
        return;
    }

    if (j->state == RUNNING) {
        printf("bg: job [%d] is already running in background\n", j->jid);
        return;
    }

    /* send continue signal to the job's group */
    if (kill(-j->pgid, SIGCONT) == -1) {
        perror("smallsh: bg");
        return;
    }

    j->state = RUNNING;
    printf("[%d] %s & (PID: %d)\n", j->jid, j->command, j->pid);
}

/* built-in kill command: stop or terminate a job or pid */
void builtin_kill(char **cline)
{
    int sig = SIGTERM;
    char *target = cline[1];

    update_jobs();

    if (target == NULL) {
        printf("Usage: kill [-signal] <job_id | %%job_id | pid>\n");
        return;
    }

    /* check if the user passed a signal number like -9 */
    if (target[0] == '-' && cline[2] != NULL) {
        sig = atoi(target + 1);
        if (sig <= 0)
            sig = SIGTERM;
        target = cline[2];
    }

    struct job *j = find_job(target);
    if (j != NULL) {
        if (kill(-j->pgid, sig) == -1) {
            perror("smallsh: kill");
            return;
        }
        if (sig == SIGSTOP || sig == SIGTSTP) {
            j->state = STOPPED;
            printf("[%d] Stopped                 %s (PID: %d)\n", j->jid, j->command, j->pid);
        } else if (sig == SIGCONT) {
            j->state = RUNNING;
            printf("[%d] Running                 %s (PID: %d)\n", j->jid, j->command, j->pid);
        } else {
            /* make sure stopped processes wake up so they can exit, then clean up */
            kill(-j->pgid, SIGCONT);
            waitpid(j->pid, NULL, 0);
            printf("[%d] Terminated              %s (PID: %d)\n", j->jid, j->command, j->pid);
            j->active = 0;
            reset_next_jid();
        }
    } else if (target[0] != '%') {
        /* if not in our jobs list, try killing by regular pid */
        pid_t raw_pid = atoi(target);
        if (raw_pid > 0 && kill(raw_pid, sig) == 0) {
            printf("Process %d killed\n", raw_pid);
        } else {
            perror("smallsh: kill");
        }
    } else {
        printf("kill: job %s not found\n", target);
    }
}

/* print prompt and read a line */
int userin(char *p)
{
    int c, count;

    /* check if any background jobs finished before showing the prompt */
    update_jobs();

    /* initialization for later routines */
    ptr = inpbuf;
    tok = tokbuf;

    /* display prompt */
    printf("%s", p);
    fflush(stdout);
    count = 0;

    while (1) {
        if ((c = getchar()) == EOF) {
            /* if Ctrl+C was pressed at the prompt, just print a new line and prompt */
            if (errno == EINTR) {
                clearerr(stdin);
                count = 0;
                printf("\n%s", p);
                fflush(stdout);
                continue;
            }
            return (EOF);
        }
        if (count < MAXBUF)
            inpbuf[count++] = c;
        if (c == '\n' && count < MAXBUF) {
            inpbuf[count] = '\0';
            return count;
        }
        /* if line too long restart */
        if (c == '\n') {
            printf("smallsh: input line too long\n");
            count = 0;
            printf("%s", p);
            fflush(stdout);
        }
    }
}

/* get token, place into tokbuf */
int gettok(char **outptr)
{
    int type;

    /* set the outptr string to tok */
    *outptr = tok;

    /* strip white space from the buffer containing the tokens */
    while (*ptr == ' ' || *ptr == '\t')
        ptr++;

    /* set the token pointer to the first token in the buffer */
    *tok++ = *ptr;

    /* set the type variable depending on the token in the buffer */
    switch (*ptr++) {
    case '\n':
        type = EOL;
        break;
    case '&':
        type = AMPERSAND;
        break;
    case ';':
        type = SEMICOLON;
        break;
    default:
        type = ARG;
        /* keep reading valid ordinary characters */
        while (inarg(*ptr))
            *tok++ = *ptr++;
    }

    *tok++ = '\0';
    return type;
}

/* check if a character is normal or special */
int inarg(char c)
{
    char *wrk;

    for (wrk = special; *wrk; wrk++) {
        if (c == *wrk)
            return (0);
    }
    return (1);
}

/* process input line */
int procline(void)
{
    char *arg[MAXARG + 1]; /* pointer array for runcommand */
    int toktype;           /* type of token in command */
    int narg;              /* number of arguments so far */
    int type;              /* FOREGROUND or BACKGROUND */

    narg = 0;

    for (;;) { /* loop forever */
        /* take action according to token type */
        switch (toktype = gettok(&arg[narg])) {
        case ARG:
            if (narg < MAXARG)
                narg++;
            break;
        case EOL:
        case SEMICOLON:
        case AMPERSAND:
            if (toktype == AMPERSAND)
                type = BACKGROUND;
            else
                type = FOREGROUND;

            if (narg != 0) {
                arg[narg] = NULL;
                runcommand(arg, type);
            }
            if (toktype == EOL)
                return 0;
            narg = 0;
            break;
        }
    }
}

/* execute a command with optional wait */
int runcommand(char **cline, int where)
{
    pid_t pid;
    char cmd_str[MAXBUF] = "";
    int i;

    /* check if the user typed a built-in command first */
    if (strcmp(cline[0], "exit") == 0 || strcmp(cline[0], "quit") == 0) {
        exit(0);
    }
    if (strcmp(cline[0], "jobs") == 0) {
        builtin_jobs();
        return 0;
    }
    if (strcmp(cline[0], "fg") == 0) {
        builtin_fg(cline);
        return 0;
    }
    if (strcmp(cline[0], "bg") == 0) {
        builtin_bg(cline);
        return 0;
    }
    if (strcmp(cline[0], "kill") == 0) {
        builtin_kill(cline);
        return 0;
    }

    /* put the command words back together so we can show the name in jobs */
    for (i = 0; cline[i] != NULL; i++) {
        if (i > 0 && strlen(cmd_str) + 1 < MAXBUF)
            strcat(cmd_str, " ");
        if (strlen(cmd_str) + strlen(cline[i]) < MAXBUF - 1)
            strcat(cmd_str, cline[i]);
    }

    switch (pid = fork()) {
    case -1:
        perror("smallsh");
        return (-1);
    case 0:
        /* put child process in its own group */
        setpgid(0, 0);

        /* reset signals back to normal for the child process */
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        execvp(*cline, cline);
        perror(*cline);
        exit(1);
    }

    /* code for parent */
    /* make sure child is in its own process group */
    setpgid(pid, pid);

    /* if background process, add to jobs list, print pid, and return */
    if (where == BACKGROUND) {
        int jid = add_or_update_job(pid, pid, RUNNING, cmd_str);
        printf("[%d] [Process id %d]\n", jid, pid);
        return (0);
    }

    /* wait until process pid exits */
    return wait_for_fg(pid, cmd_str);
}

int main(void)
{
    struct sigaction sa_int, sa_tstp;

    /* set up the shell's own process group and terminal control */
    shell_pgid = getpid();
    setpgid(shell_pgid, shell_pgid);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    if (isatty(STDIN_FILENO)) {
        tcsetpgrp(STDIN_FILENO, shell_pgid);
    }

    /* set up Ctrl+C handler */
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, NULL);

    /* set up Ctrl+Z handler */
    sa_tstp.sa_handler = sigtstp_handler;
    sigemptyset(&sa_tstp.sa_mask);
    sa_tstp.sa_flags = 0;
    sigaction(SIGTSTP, &sa_tstp, NULL);

    while (userin(prompt) != EOF)
        procline();

    printf("\n");
    return 0;
}
