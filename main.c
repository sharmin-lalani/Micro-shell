/******************************************************************************
 *
 *  File Name........: main.c
 *
 *  Description......: ush is a command interpreter with a syntax similar to UNIX C shell, csh(1). 
 *                     However, it is for instructional purposes only, therefore it is much simpler.
 *                     It performs the following tasks:
 *                     Command line parsing
 *                     I/O redirection
 *                     Command execution
 *                     Handlin environment variables
 *                     Signal handling
 *                     Built-in commands
 *
 *  Author...........: Sharmin Lalani
 *
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <ctype.h>
#include<signal.h>
#include "parse.h"

void process_pipe(Pipe p);
int process_cmd(Cmd c);

void perform_io_redirect(Cmd c);
void perform_pipe_redirect(Cmd c);

int is_builtin(char *cmd_name);
void exec_cd(Cmd c);
void exec_echo(Cmd c);
void exec_logout(Cmd c);
void exec_nice(Cmd c);
void exec_pwd(Cmd c);
void exec_setenv(Cmd c);
void exec_unsetenv(Cmd c);
void exec_where(Cmd c);

int is_valid_cmd(char *path);
int is_dir(char *path);
int is_number(char* str);

struct builtin_cmd_handle_t {
		char *cmd_name;
		void (*exec_cmd) (Cmd c);
};

struct builtin_cmd_handle_t builtin_cmd_handle[] = {
		{"cd", exec_cd},
		{"echo", exec_echo},
		{"logout", exec_logout},
		{"nice", exec_nice},
		{"pwd", exec_pwd},
		{"setenv", exec_setenv},
		{"unsetenv", exec_unsetenv},
		{"where", exec_where}
};

extern char **environ;
int pipenum;
int mypipes[2][2];
int processing_rc = 0;

int main() {
		Pipe p; 
		char hostname[64], *rcfile_name;
		int saved_stdin;

		gethostname(hostname, sizeof(hostname));

		signal(SIGINT, SIG_IGN); // Interrupt signal CTRL+C
		signal(SIGQUIT, SIG_IGN); // Quit signal CTRL+'\'
		//signal(SIGTSTP, SIG_IGN); // Stop signal /CTRL+Z

		/* When first stared, ush normally performs commands from the file ˜/.ushrc, 
		   provided that it is readable. Commands in this file are processed just the same 
		   as if they were taken from standard input.
		 */

		rcfile_name = (char*)malloc(PATH_MAX);
		strcat(rcfile_name, getenv("HOME"));
		strcat(rcfile_name, "/.ushrc");
		int rcfile = open(rcfile_name, O_RDONLY);
		if (rcfile != -1) {
				processing_rc = 1;
				saved_stdin = dup(0);
				dup2(rcfile, 0);
				close(rcfile);

				while(1) {
						p = parse();
						if(strcmp(p->head->args[0], "end") == 0) {
								freePipe(p);
								break;
						}
						process_pipe(p);
						freePipe(p);
				}
				processing_rc = 0;
				dup2(saved_stdin, 0);
				close(saved_stdin);
		}
		setbuf(stdout, NULL);
		setbuf(stdin, NULL);
		setbuf(stderr, NULL);

		/* After startup processing, an interactive ush shell begins reading commands 
		   from the terminal, prompting with hostname%. 
		   The shell then repeatedly performs the following actions: 
		   a line of command input is read and broken into words; 
		   this sequence of words is parsed; 
		   and the shell executes each command in the current line.
		 */
		while (1) {
				//if (isatty(STDIN_FILENO)) { // print the prompt if stdin is associated with a terminal
				printf("%s%% ", hostname);
				//fflush(NULL);
				//}

				p = parse();
				process_pipe(p);
				freePipe(p);
		}
}

/* A simple command is a sequence of words, the first of which specifies the command to be executed. 

   A pipeline is a sequence of one or more simple commands separated by | or |&. 
   With |, the standard output of the preceding command is redirected to 
   the standard input of the command that follows. 
   With |&, both the standard error and the standard output are redirected through the pipeline.

   A list is a sequence of one or more pipelines separated by ; or &. 
   Pipelines joined into sequences with ; will be executed sequentially. 
   Pipelines ending with & are executed asynchronously. 
   In which case, the shell does not wait for the pipeline to finish; 
   instead, it displays the job number and associated process ID, 
   and begins processing the subsequent pipelines (prompting if necessary).

   Here we are looping over all pipeline sequences.

   Each pipeline sequence is executed sequentially, whereas commands in a pipeline are executed concurrently.

   Consider the following pipeline: ls | grep main | wc
   The shell forks a new process for each command, and waits for them to complete only after starting all processes. 
   We do not fork - wait for process to exit - repeat.
   Instead we fork - repeat step 1 for all commands - wait for all processes to exit.

   All commands except the first and last are connected to 2 pipes.
   Thus each process has an array of 2 pipes.

   We use int mypipes[2][2] to store the FDs for the 2 pipes and pipenum as an index.
   mypipes[0] <- 1st pipe
   mypipes[1] <- 2nd pipe

   Sequence of steps for ls | grep main | wc:

   shell executing:
	mypipes: -1 | -1 | -1 | -1
	pipenum: 0

	We replace mypipes[pipenum][0] with stdin.

	pipenum = !pipenum
	Create pipe and store FDs at index pipenum in mypipes.

	mypipes: 0 | -1 | R0 | W0 
	pipenum: 1

	Fork a new process for running ls.

	ls executing:

	Dup 0 to stdin and W0 to stdout.
	Close R0, ls doesn't need it.

	shell executing: (either shell starts executing first after fork, or ls is blocked/complete)

	pipenum = !pipenum
	Create pipe and store FDs at index pipenum in mypipes.
	mypipes: R1 | W1 | R0 | W0
	pipenum: 0
	Fork a new process for running grep.

	grep executing:

	Dup R0 to stdin and W1 to stdout.
	Close R1 and W0, grep doesn't need it.

	shell executing: (either shell starts executing first after fork, or grep is blocked/complete)

	pipenum = !pipenum
	Since next command is the last in the pipeline, we don't create a new pipe.
	We replace mypipes[pipenum][1] with stdout.
	Close R0 and W0, as the shell no longer needs to keep these FDs open.
	We needed R0 and W0 open for passing them on to ls and grep.

	mypipes: R1 | W1 |  -1 | 1
	pipenum: 1
	Fork a new process for running wc.

	wc executing:

	Dup R1 to stdin and 1 to stdout.
	Close W1, wc doesn't need it

	shell executing: (either shell starts executing first after fork, or wc is blocked/complete)

	No more commands in the pipeline.
	Close R1 and W1, as the shell no longer needs to keep these FDs open.
	We needed R1 and W1 open for passing them on to ls and grep.
	mypipes: -1 | -1 | -1 | 1
	pipenum: 0
	Wait for all processes to exit. 

	Summary: For any process, this is how the values in mypipes should be interpreted:
	mypipes[!pipenum][0] -> input stream
	mypipes[pipenum][1] -> output stream
	mypipes[!pipenum][1], mypipes[pipenum][0] are not needed for current process.
	*/
		void process_pipe(Pipe p) {

				Cmd c;
				int ret = 0, wpid, child_status, no_of_child=0;
				pipenum = 0;
				mypipes[0][0] = mypipes[0][1] = mypipes[1][0] = mypipes[1][1] = -1;

				if(p == NULL) 
						return;

				//printf("Begin pipe%s\n", p->type == Pout ? "" : " Error");

				mypipes[pipenum][0] = 0;

				for(c = p->head; c != NULL; c = c->next) {
						no_of_child ++;
						//printf("next cmd:%s\n", c->args[0]);

						pipenum = !pipenum;

						// close FDS that we no longer need
						if(mypipes[pipenum][0] != 0)
								close(mypipes[pipenum][0]);
						if(mypipes[pipenum][1] != 1)
								close(mypipes[pipenum][1]);

						// replace the closed FDS with new FDs needed for next command

						if(c->next != NULL) // create new pipes for all commands except last
								pipe(mypipes[pipenum]);
						else { // for the last command, we need to redirect output to stdout
								mypipes[pipenum][1] = 1;
								mypipes[pipenum][0] = -1;
						}
						//printf("in shell, mypipes %d %d %d %d\n", mypipes[0][0], mypipes[0][1], mypipes[1][0], mypipes[1][1]);

						ret = process_cmd(c);
						if(ret < 0)
								break;

				}
				//printf("all commands started\n");

				// close all newly created FDs after every pipeline, so that we don't have unused open files
				if(mypipes[0][0] != 0)
						close(mypipes[0][0]);
				if(mypipes[0][1] != 1)
						close(mypipes[0][1]);
				if(mypipes[1][0] != 0)
						close(mypipes[1][0]);
				if(mypipes[1][1] != 1)
						close(mypipes[1][1]);

				while(no_of_child--) {
						wpid = wait(child_status);
						//printf("waiting for all children to terminate\n");

						/* If an error occurs with any component of a pipeline the entire pipeline is aborted, 
						   even though some of the pipeline may already be executing.
						 */
						if (WEXITSTATUS(child_status) == -1) {
								printf("command failed, aborting entire pipeline\n");
								kill(-0, SIGQUIT);
						}
				}

				//printf("End pipe\n"); 
				process_pipe(p->next);
		}


/* If the command is an ush shell built-in, the shell executes it directly. 
   Otherwise, the shell searches for a file by that name with execute access. 

   If the command path does not begin with a /, the path is relative to the current directory. 
   If the command path contains no /, 
   the shell searches each directory in the PATH variable for the command.

   When a file, with its pathname resolved, is found to have proper execute permission, 
   the shell forks a new process to run the command. 
   The shell also passes along any arguments to the command. 
   If successful, the shell is silent.
 */
int process_cmd(Cmd c) {
		pid_t child_pid;
		int i, saved_stdin, saved_stdout;

		if(strcmp(c->args[0], "end") == 0 && processing_rc == 0)
				exit(0);

		i = is_builtin(c->args[0]);

		if(i != -1) { // shell built-in command 

				//printf("built in cmd\n");

				if (c->next == NULL) { //last command in a pipe, execute built-in in current shell

						/* Save standard input and output before redirecting, 
						   as we are not executing inside a new process, but inside the shell process.
						 */
						saved_stdin = dup(0);
						saved_stdout = dup(1);

						perform_pipe_redirect(c);
						perform_io_redirect(c);
						builtin_cmd_handle[i].exec_cmd(c);

						// Restore standard input and output
						dup2(saved_stdin, 0);
						dup2(saved_stdout, 1);
						close(saved_stdin);
						close(saved_stdout);

				} else { //command in pipeline, execute built-in in a subshell

						child_pid = fork();

						if(child_pid == 0) { //child process executing 
								//printf("%s executing after fork\n", c->args[0]);
								signal(SIGINT, SIG_DFL);
								signal(SIGQUIT, SIG_DFL);
								signal(SIGTSTP, SIG_DFL);

								perform_pipe_redirect(c);
								perform_io_redirect(c); /* do we need IO redirection in the middle of a pipeline?
														   BASH supports commands like: echo 'hello' > out.txt | wc
														   though it doesn't make sense, as the output is 0 0 0.
														 */
								builtin_cmd_handle[i].exec_cmd(c);
								exit(0);
						} else {
								//printf("shell executing after fork for %s\n", c->args[0]);
						}
				}			

		} else { // user-defined executable

				child_pid = fork();

				if(child_pid == 0) { //child process executing
						//printf("%s executing after fork\n", c->args[0]);
						signal(SIGINT, SIG_DFL);
						signal(SIGQUIT, SIG_DFL);
						signal(SIGTSTP, SIG_DFL);

						perform_pipe_redirect(c);
						perform_io_redirect(c);

						execvp(c->args[0], c->args);

						//If execvp returns, it must have failed.

						switch(errno) {			
								case EACCES: 
										printf("permission denied\n"); 
										break;
								case ENOENT: 
										printf("command not found\n"); 
						}

						exit(-1);
				} else {
						//printf("shell executing after fork for %s\n", c->args[0]);
				}
		}

		return 0;
}


void perform_io_redirect(Cmd c) { 

		//printf("redirecting for %s\n", c->args[0]);

		int input, output;
		if(c->in == Tin) {
				//printf("<(%s) ", c->infile);
				input = open(c->infile, O_RDONLY);
				if(input != -1) {
						dup2(input, 0);
						close(input);
				} else
						exit(-1);
		}
		if(c->out != Tnil)
				switch (c->out) {
						case Tout:
								//printf(">(%s) ", c->outfile);
								output = open(c->outfile, O_WRONLY | O_CREAT | O_TRUNC,
												S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
								if(output != -1) {
										dup2(output, 1);
										close(output);
								}
								break;
						case Tapp:
								//printf(">>(%s) ", c->outfile);
								output = open(c->outfile, O_RDWR | O_CREAT | O_APPEND,
												S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
								if(output != -1) {
										dup2(output, 1);
										close(output);
								}
								break;
						case ToutErr:
								//printf(">&(%s) ", c->outfile);
								output = open(c->outfile, O_WRONLY | O_CREAT | O_TRUNC,
												S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
								if(output != -1) {
										dup2(output, 1);
										dup2(output, 2);
										close(output);
								}
								break;
						case TappErr:
								//printf(">>&(%s) ", c->outfile);
								output = open(c->outfile, O_RDWR | O_CREAT | O_APPEND,
												S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
								if(output != -1) {
										dup2(output, 1);
										dup2(output, 2);
										close(output);
								}
								break;
				}
}


void perform_pipe_redirect(Cmd c) {
		//printf("redirecting pipe for %s\n", c->args[0]);
		//printf("%d->0 %d->1, close %d and %d\n", mypipes[!pipenum][0], mypipes[pipenum][1], mypipes[!pipenum][1], mypipes[pipenum][0]);

		dup2(mypipes[!pipenum][0], 0);
		close(mypipes[!pipenum][1]);

		dup2(mypipes[pipenum][1], 1);
		close(mypipes[pipenum][0]);

		if(c->out == TpipeErr) {
				dup2(mypipes[pipenum][1], 2);
		}
}


int is_builtin(char *cmd_name) {
		int i = 0, len = sizeof(builtin_cmd_handle)/sizeof(builtin_cmd_handle[0]);

		if(!cmd_name)
				return;

		while (i < len) {
				if(strcmp(builtin_cmd_handle[i].cmd_name, cmd_name) == 0)
						return i;
				i++;
		}
		return -1;
}


/* Change the working directory of the shell to dir, 
   provided it is a directory and the shell has the appropriate permissions. 
   Without an argument, it changes the working directory to the home directory.
Note: We are not handling tilde expansion.
 */
void exec_cd(Cmd c) {
		int ret;
		char* home = getenv("HOME");
		if (c->args[1] == NULL) {
				chdir(home);
				return;
		}

		ret = chdir(c->args[1]);
		if(ret == -1) {
				switch(errno) {
						case EACCES: 
								printf("%s: Permission denied.\n", c->args[1]); 
								break;
						case ENOENT: 
								printf("%s: No such file or directory.\n", c->args[1]); 
								break;
						case ENOTDIR: 
								printf("%s: Not a directory.\n", c->args[1]);
				}
		}
}


/* Format: echo <word>
   Write each word to the shell’s standard output, separated by spaces and terminated with a newline.
   Note: Not handling variable expansion, e.g. echo abc*, echo $PATH
 */
void exec_echo(Cmd c) {
		int i=0;

		while(c->args[++i] != NULL)
				printf("%s ", c->args[i]);

		if(i != 1)
				printf("\n");
}


/* Exit the shell
 */
void exec_logout(Cmd c) {
		exit(0);
}


/* Format: nice [[+/-]<number>] [<command>]
   Sets the scheduling priority for the shell to number, or, without number, to 4. 
   With command, runs command at the appropriate priority. 
   The greater the number, the less cpu the process gets.
 */
void exec_nice(Cmd c) {
		int which, who, priority;
		char **cmd = NULL;

		which = PRIO_PROCESS; // The value of which can be one of PRIO_PROCESS, PRIO_PGRP, or PRIO_USER
		who = 0; //  A zero value for who denotes the calling process
		priority = 4; // The default priority is 0; lower priorities cause more favorable scheduling.

		if(c->nargs > 1) {
				if(is_number(c->args[1]) == 1) {  // nice <priority> or nice <priority> <command>
						priority = atoi(c->args[1]);

						if(priority < -19)
								priority = -19;
						else if(priority > 20)
								priority = 20;

						if (c->args[2])			
								cmd = &c->args[2];

				} else { // nice <command>
						cmd = &c->args[1];
				}			
		}


		/* Set the priority of the shell. 
           Note: Only the superuser may lower priorities.
		 */

		setpriority(which, who, priority);
		//printf("priority: %d\n", priority); 

		if(cmd != NULL)	{
				//printf("command:%s\n", *cmd);
				Cmd temp;
				temp = malloc(sizeof(struct cmd_t));
				temp->args = cmd; //malloc(sizeof(char*));
				//temp->args[0] = malloc(strlen(cmd));

				temp->in = temp->out = Tnil;
				//strcpy(temp->args[0], cmd);

				/* A child created by fork inherits its parent's nice value. 
				   The nice value is preserved across execve.
				 */
				process_cmd(temp);

				//free(temp->args[0]);
				//free(temp->args);
				free(temp);
		}
}


/* Print the current working directory.
 */
void exec_pwd(Cmd c) {
		char path[PATH_MAX];
		getcwd(path, sizeof(path));
		printf("%s\n", path);
}


/* format: setenv [VAR [word]]
   Without arguments, prints the names and values of all environment variables. 
   Given VAR, sets the environment variable VAR to word or, without word, to the null string.
 */
void exec_setenv(Cmd c) {
		int i;
		if (c->args[1] == NULL) {
				for (i = 0; environ[i] != NULL; i++) 
						printf("%s\n", environ[i]);			
		} else
				setenv(c->args[1], c->args[2], 1);
}


/* format: unsetenv VAR
   Remove environment variable whose name matches VAR.
 */
void exec_unsetenv(Cmd c) {
		if(c->args[1] == NULL)
				printf("unsetenv: too few arguments\n");
		else
				unsetenv(c->args[1]);
}


/* format: where command
   Reports all known instances of command, including builtins and executables in path.
 */
void exec_where(Cmd c) {
		char *path, path_copy[PATH_MAX], *curr_path, abs_path[PATH_MAX];

		if(is_builtin(c->args[1]) != -1)
				printf("%s\n", c->args[1]);

		path = getenv("PATH");
		//We don't want to modify the original PATH env variable
		strcpy(path_copy, path);
		curr_path = strtok(path_copy,":");

		while(curr_path != NULL) {
				memset(abs_path, 0, PATH_MAX);
				strcpy(abs_path, curr_path);
				strcat(abs_path, "/");
				strcat(abs_path, c->args[1]);
				//printf("checking %s\n", abs_path);

				if(is_valid_cmd(abs_path) == 1)
						printf("%s\n",abs_path);
				curr_path = strtok(NULL,":");
		}
}


int is_valid_cmd(char *path) {
		if(access(path, X_OK) != 0 || is_dir(path))	
				return 0;
		return 1;
}


int is_dir(char *path) {
		struct stat buf;
		stat(path, &buf);
		return buf.st_mode & S_IFDIR;
} 


int is_number(char* str) {
		if (*str == '-') {
				str++;
		}

		while (*str) {
				if (!isdigit(*str))
						return 0;
				str++;
		}
		return 1;
}
/*........................ end of main.c ....................................*/
