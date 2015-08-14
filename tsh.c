/* 
 * tsh - A tiny shell program with job control
 * 
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF         0   /* undefined */
#define FG            1   /* running in foreground */
#define BG            2   /* running in background */
#define ST            3   /* stopped */

/* JID or PID states */
#define JID 4
#define PID 5

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Parsing states */
#define ST_NORMAL   0x0   /* next token is an argument */
#define ST_INFILE   0x1   /* next token is the input file */
#define ST_OUTFILE  0x2   /* next token is the output file */
#define DEF_MODE S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH

/* Global variables*/
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */



struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t job_list[MAXJOBS]; /* The job list */

struct cmdline_tokens {
    int argc;               /* Number of arguments */
    char *argv[MAXARGS];    /* The arguments list */
    char *infile;           /* The input file */
    char *outfile;          /* The output file */
    enum builtins_t {       /* Indicates if argv[0] is a builtin command */
        BUILTIN_NONE,
        BUILTIN_QUIT,
        BUILTIN_JOBS,
        BUILTIN_BG,
		BUILTIN_FG} builtins;
};

/* End global variables */


/* Function prototypes */
void eval(char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, struct cmdline_tokens *tok); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *job_list);
int maxjid(struct job_t *job_list); 
int addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *job_list, pid_t pid); 
pid_t fgpid(struct job_t *job_list);
struct job_t *getjobpid(struct job_t *job_list, pid_t pid);
struct job_t *getjobjid(struct job_t *job_list, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *job_list, int output_fd);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/* Here are helper functions I built */
int Sigemptyset(sigset_t *set);
int Sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int Sigaddset(sigset_t *set, int signum);
void waitchld();
void printbg(pid_t pid, char *cmd);
void safe_printf(const char *format, ...);
void job_bg_fg(int fgorbg, int pidorjid, int argv1);

/*
 * main - The shell's main routine 
 */
int 
main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];    /* cmdline for fgets */
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
            break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(job_list);


    /* Execute the shell's read/eval loop */
    while (1) {

        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { 
            /* End of file (ctrl-d) */
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            exit(0);
        }
        
        /* Remove the trailing newline */
        cmdline[strlen(cmdline)-1] = '\0';
        
        /* Evaluate the command line */
        eval(cmdline);

        fflush(stdout);
        fflush(stdout);
    } 
    
    exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
void 
eval(char *cmdline) 
{
    int bg;              /* should the job run in bg or fg? */
    int pid;
	struct cmdline_tokens tok;
	sigset_t mask;
    char *str; 
	int fd_in, fd_out;
	int dup_in, dup_out;
	dup_in = dup(0);
	dup_out = dup(1);
	/* Parse command line */
    bg = parseline(cmdline, &tok); 

    if (bg == -1) /* parsing error */
        return;
    if (tok.argv[0] == NULL) /* ignore empty lines */
        return;
	/* switch the command line*/
    switch(tok.builtins) {
        case BUILTIN_JOBS: /*built in jobs*/
			if(tok.outfile != NULL) {
				if((fd_out = open(tok.outfile, O_CREAT|O_WRONLY, 0)) != 1) {
					listjobs(job_list, fd_out);
				} else {
					safe_printf("Open output file failed.\n");
				}
			} else {
				listjobs(job_list, STDOUT_FILENO);
			}
            break;

        case BUILTIN_QUIT: /*built in quit*/
            exit(0);
            break;

        case BUILTIN_FG: /* built in fg */
			str = tok.argv[1];
            if(tok.argc != 2) {
                safe_printf("Please specify pid or jid in the command!\n");
            } else if(str[0] == '%') {
				job_bg_fg(FG, JID, atoi(&str[1]));
            } else {
                job_bg_fg(FG, PID, atoi(str));
            }
            break;

        case BUILTIN_BG: /* built in bg*/
			str = tok.argv[1];
			if(tok.argc != 2) {
				safe_printf("Please specify pid or jid in the command!\n");
			} else if(str[0] == '%') {
                job_bg_fg(BG, JID, atoi(&str[1]));
			} else {
                job_bg_fg(BG, PID, atoi(str));
            }
			break;

        case BUILTIN_NONE: /* none built ins */
			/* block signals before fork */
			Sigemptyset(&mask);        
			Sigaddset(&mask, SIGCHLD);
			Sigaddset(&mask, SIGINT);
			Sigaddset(&mask, SIGTSTP);	
			Sigprocmask(SIG_BLOCK, &mask, NULL);
			
			/* io redirection, handle both infile and outfile */
			if(tok.outfile != NULL) {
				if((fd_out = open(tok.outfile, O_CREAT | O_TRUNC | O_WRONLY,					DEF_MODE)) != -1) {
					dup2(fd_out, 1); /* stdout redirect */
				} else {
					safe_printf("Open output file failed.\n");
				}
			}
			if(tok.infile != NULL) {
				if((fd_in = open(tok.infile, O_RDONLY)) != -1) {
					dup2(fd_in, 0); /* stadin redirect */
				} else {
					safe_printf("Open input file failed.\n");
				}
			}
		
			if((pid = fork()) < 0) { /* fork a new process*/
				unix_error("Fork Error!");
			}
			if(pid == 0) { /* child process to execute non built-ins */	
				Sigprocmask(SIG_UNBLOCK, &mask, NULL);
				setpgid(0, 0);
				if(execve(tok.argv[0], tok.argv, environ) < 0) {
					unix_error("Execve Error!");
				}
				fflush(stdout);
			} else { 
				if(!bg) { /* foreground process */
					addjob(job_list, pid, FG, cmdline);
					Sigprocmask(SIG_UNBLOCK, &mask, NULL);		
					waitchld();
				} else { /* background process*/
					addjob(job_list, pid, BG, cmdline);				
					safe_printf("[%d] (%d) %s\n",pid2jid(pid),pid,cmdline);	
					Sigprocmask(SIG_UNBLOCK, &mask, NULL);	
				}
			}
		
			break;
    }

	if(fd_in != 0) { /* close the descripter and redirect stdin */
		close(fd_in);
		dup2(dup_in, 0);
	}
	if(fd_out != 0) { /* close the descripter and redirect stdout */
		close(fd_out);
		dup2(dup_out, 1);
	}
    return;
}

/* Helper functions starts here */
void safe_printf(const char *format, ...) {
	char buf[MAXLINE];
	va_list args;
	sigset_t mask, prev_mask;

	sigfillset(&mask);
	sigprocmask(SIG_BLOCK, &mask, &prev_mask);

	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	write(1, buf, strlen(buf));

	sigprocmask(SIG_SETMASK, &prev_mask, NULL);
}

/* sigemptyset wrapper function */
int Sigemptyset(sigset_t *set) {
	int ret;
	if((ret = sigemptyset(set)) < 0) {
		unix_error("Sigemptyset Error!");
	}
	return ret;
}
	
/* sigprocmask wrapper function */
int Sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
	int ret;
	if((ret = sigprocmask(how, set, oldset)) < 0) {
		unix_error("Sigprocmask Error!");
	}
	return ret;
}

/* sigaddset wrapper function */
int Sigaddset(sigset_t *set, int signum) {
	int ret;
	if((ret = sigaddset(set, signum)) < 0) {
		unix_error("Sigaddset Error!");
	}
	return ret;
}

/* parent process waits for the child process */	
void waitchld() {
	sigset_t mask;
	sigemptyset(&mask);
	while(fgpid(job_list)) {
		sigsuspend(&mask);  /* suspend until child ends */
	}
	return;
}

//void printbg(pid_t pid, char *cmd) {
//	safe_printf("[%d] (%d) %s\n", pid2jid(pid), pid, cmd);	
//}

/* change the state of a job */
void job_bg_fg(int fgorbg, int pidorjid, int argv1) {
    
	struct job_t *job;
    if(pidorjid == JID) {
        if((job = getjobjid(job_list, argv1))) {
            if(kill(job->pid, SIGCONT) != 0) {  /* send the SIGCONT signal*/
                unix_error("SIGCONT sends failed!\n");
            } else {
                if(fgorbg == BG) {   
                    job -> state = BG;  /* chage the state to BG*/         
                    safe_printf("[%d] (%d) %s\n", job->jid, job->pid, 
					job->cmdline);
                } else if(fgorbg == FG){
                    job -> state = FG; /* change the state to FG*/
                }
            }   
        }
    } else if(pidorjid == PID) {
        if(!(job = getjobpid(job_list, argv1))) {
            if(kill(job->pid, SIGCONT) != 0) { /* send the SIGCONT signal */
                unix_error("SIGCONT sends failed!\n");
            } else {
                if(fgorbg == BG) {  
                    job -> state = BG;   /* change the state to BG */
					safe_printf("[%d] (%d) %s\n", job->jid, job->pid, 
					job->cmdline);
                } else if(fgorbg == FG){
                    job -> state = FG;   /* change the state to FG */
                }
            }   
        }
    }
	return;
}

/* Helper functions ends */

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Parameters:
 *   cmdline:  The command line, in the form:
 *
 *                command [arguments...] [< infile] [> oufile] [&]
 *
 *   tok:      Pointer to a cmdline_tokens structure. The elements of this
 *          structure will be populated with the parsed tokens. Characters 
 *             enclosed in single or double quotes are treated as a single
 *             argument. 
 * Returns:
 *   1:        if the user has requested a BG job
 *   0:        if the user has requested a FG job  
 *  -1:        if cmdline is incorrectly formatted
 * 
 * Note:       The string elements of tok (e.g., argv[], infile, outfile) 
 *             are statically allocated inside parseline() and will be 
 *             overwritten the next time this function is invoked.
 */
int 
parseline(const char *cmdline, struct cmdline_tokens *tok) 
{

    static char array[MAXLINE];   /* holds local copy of command line */
    const char delims[10] = " \t\r\n";/*argument delimiters (white-space) */
    char *buf = array;            /* ptr that traverses command line */
    char *next;                  /* ptr to the end of the current arg */
    char *endbuf;                        /* ptr to end of cmdline string */
    int is_bg;                           /* background job? */

    int parsing_state;             /* indicates if the next token is the
                                            input or output file */

    if (cmdline == NULL) {
        (void) fprintf(stderr, "Error: command line is NULL\n");
        return -1;
    }

    (void) strncpy(buf, cmdline, MAXLINE);
    endbuf = buf + strlen(buf);

    tok->infile = NULL;
    tok->outfile = NULL;

    /* Build the argv list */
    parsing_state = ST_NORMAL;
    tok->argc = 0;

    while (buf < endbuf) {
        /* Skip the white-spaces */
        buf += strspn (buf, delims);
        if (buf >= endbuf) break;

        /* Check for I/O redirection specifiers */
        if (*buf == '<') {
            if (tok->infile) {
                (void) fprintf(stderr,"Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_INFILE;
            buf++;
            continue;
        }
        if (*buf == '>') {
            if (tok->outfile) {
                (void) fprintf(stderr,"Error: Ambiguous I/O redirection\n");
                return -1;
            }
            parsing_state |= ST_OUTFILE;
            buf ++;
            continue;
        }

        if (*buf == '\'' || *buf == '\"') {
            /* Detect quoted tokens */
            buf++;
            next = strchr (buf, *(buf-1));
        } else {
            /* Find next delimiter */
            next = buf + strcspn (buf, delims);
        }
        
        if (next == NULL) {
            /* Returned by strchr(); this means that the closing
               quote was not found. */
            (void) fprintf (stderr, "Error: unmatched %c.\n", *(buf-1));
            return -1;
        }

        /* Terminate the token */
        *next = '\0';

        /* Record the token as either the next argument or the i/o file */
        switch (parsing_state) {
        case ST_NORMAL:
            tok->argv[tok->argc++] = buf;
            break;
        case ST_INFILE:
            tok->infile = buf;
            break;
        case ST_OUTFILE:
            tok->outfile = buf;
            break;
        default:
            (void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
            return -1;
        }
        parsing_state = ST_NORMAL;

        /* Check if argv is full */
        if (tok->argc >= MAXARGS-1) break;

        buf = next + 1;
    }

    if (parsing_state != ST_NORMAL) {
        (void) fprintf(stderr,
                       "Error: must provide file name for redirection\n");
        return -1;
    }

    /* The argument list must end with a NULL pointer */
    tok->argv[tok->argc] = NULL;

    if (tok->argc == 0)  /* ignore blank line */
        return 1;

    if (!strcmp(tok->argv[0], "quit")) {                 /* quit command */
        tok->builtins = BUILTIN_QUIT;
    } else if (!strcmp(tok->argv[0], "jobs")) {          /* jobs command */
        tok->builtins = BUILTIN_JOBS;
    } else if (!strcmp(tok->argv[0], "bg")) {            /* bg command */
        tok->builtins = BUILTIN_BG;
    } else if (!strcmp(tok->argv[0], "fg")) {            /* fg command */
        tok->builtins = BUILTIN_FG;
     } else {
        tok->builtins = BUILTIN_NONE;
    }

    /* Should the job run in the background? */
    if ((is_bg = (*tok->argv[tok->argc-1] == '&')) != 0)
        tok->argv[--tok->argc] = NULL;

    return is_bg;
}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU signal. The 
 *     handler reaps all available zombie children, but doesn't wait 
 *     for any other currently running children to terminate.  
 */
void 
sigchld_handler(int sig) 
{
	if(sig == SIGCHLD) {
		pid_t pid;
		int status;
		while((pid = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0) {
			if(WIFEXITED(status)) { /* terminate normally */
				if(deletejob(job_list, pid) == 0) {
					unix_error("Deleting job error.\n");
				}
			}else if(WIFSIGNALED(status)) { /* terminated by signal sigint*/
				safe_printf("Job [%d] (%d) terminated by signal %d\n", 
				pid2jid(pid), pid, WTERMSIG(status));
				if(deletejob(job_list, pid) == 0) {
					unix_error("Deleting job error.\n");
				}
			} else if (WIFSTOPPED(status)) { /* terminated by sigstp */
				safe_printf("Job [%d] (%d) stopped by signal %d\n", pid2jid(
				pid), pid, WSTOPSIG(status));
				getjobpid(job_list, pid)->state = ST;
			} else {
				safe_printf("Sigchld unhandled situation.\n");
			}
		}
	} else {
		safe_printf("Signal received error.\n");
	}
	return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void 
sigint_handler(int sig) 
{
    pid_t fg_pid;
	if(sig == SIGINT) {
		fg_pid = fgpid(job_list);
   		if(fg_pid != 0) { /* send sigint signal */
			if(kill(-fg_pid, sig) == -1) {	
				unix_error("Kill foreground job failed.\n");
			}
		}
	} else {
		safe_printf("Signal received error.\n");
	}
	return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void 
sigtstp_handler(int sig) 
{ 
    pid_t fg_pid;
	if(sig == SIGTSTP) {
		fg_pid = fgpid(job_list);
		if(fg_pid != 0) { /* send sigtstp signal */
			if(kill(-fg_pid, sig) == -1) {	
				unix_error("Kill foreground job failed.\n");
			}
		}
	} else {
		safe_printf("Signal received error.\n");
	}
	return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void 
clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void 
initjobs(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&job_list[i]);
}

/* maxjid - Returns largest allocated job ID */
int 
maxjid(struct job_t *job_list) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid > max)
            max = job_list[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int 
addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline) 
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == 0) {
            job_list[i].pid = pid;
            job_list[i].state = state;
            job_list[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(job_list[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n",
                       job_list[i].jid,
                       job_list[i].pid,
                       job_list[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int 
deletejob(struct job_t *job_list, pid_t pid) 
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (job_list[i].pid == pid) {
            clearjob(&job_list[i]);
            nextjid = maxjid(job_list)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t 
fgpid(struct job_t *job_list) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].state == FG)
		    return job_list[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t 
*getjobpid(struct job_t *job_list, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid)
            return &job_list[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *job_list, int jid) 
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].jid == jid)
            return &job_list[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int 
pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (job_list[i].pid == pid) {
            return job_list[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void 
listjobs(struct job_t *job_list, int output_fd) 
{
    int i;
    char buf[MAXLINE];

    for (i = 0; i < MAXJOBS; i++) {
        memset(buf, '\0', MAXLINE);
        if (job_list[i].pid != 0) {
            sprintf(buf, "[%d] (%d) ", job_list[i].jid, job_list[i].pid);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            switch (job_list[i].state) {
            case BG:
                sprintf(buf, "Running    ");
                break;
            case FG:
                sprintf(buf, "Foreground ");
                break;
            case ST:
                sprintf(buf, "Stopped    ");
                break;
            default:
                sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
                        i, job_list[i].state);
            }
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
            memset(buf, '\0', MAXLINE);
            sprintf(buf, "%s\n", job_list[i].cmdline);
            if(write(output_fd, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Error writing to output file\n");
                exit(1);
            }
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void 
usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void 
unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void 
app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t 
*Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void 
sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}

