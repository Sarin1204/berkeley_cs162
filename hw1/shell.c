#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "process.h"
#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);
int cmd_wait(struct tokens *tokens);
int cmd_fg(struct tokens *tokens);
int cmd_bg(struct tokens *tokens);

void update_status();
int background_processes_complete();

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_exit, "exit", "exit the command shell"},
  {cmd_pwd, "pwd", "print the current working directory path"},
  {cmd_cd, "cd", "change current working directory to specified path"},
  {cmd_wait, "wait", "wait for all background processes to complete"},
  {cmd_fg, "fg", "bring specified program with pid to terminal foreground"},
  {cmd_bg,"bg", "continue execution of specified program with pid in background"}
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) {
  exit(0);
}

int cmd_pwd(unused struct tokens *tokens) {
  if(tokens->tokens_length > 1){
    printf("pwd does not take command line arguments\n");
    return 1;
  }
  else{
    char buf[1024];
    if(getcwd(buf,sizeof(buf))!=NULL)
      printf("%s\n",buf);
    else
      perror("pwd error");
  }
  return 1;
}

int cmd_cd(unused struct tokens *tokens) {
  if(tokens->tokens_length != 2){
    printf("cd requires a single directory path\n");
    return 1;
  }
  else{
    if(chdir(tokens->tokens[1])!=0)
      perror("cd error");
    return 1;
  }
}

int cmd_wait(unused struct tokens *tokens){
  if(tokens->tokens_length != 1){
    printf("wait does not take any cmd line arguments\n");
    return 1;
  }
  else{
    int count = 0;
    while(!background_processes_complete()){
      if(count>100){
        printf("backbround count break\n");
        return 1;
      }
      update_status();
    }
  return 1;
  }
}

int cmd_fg(unused struct tokens *tokens){
  pid_t pid;
  if(tokens->tokens_length > 1){
    pid = strtol(tokens->tokens[1],NULL,10);
  }
  else
    pid = -1;
  process *p;
  for(p=first_process;p->next!=NULL;p=p->next){
    if(p->pid == pid)
      break;
  }
  put_process_in_foreground(p);
  return 1;
}

int cmd_bg(unused struct tokens *tokens){
  pid_t pid;
  if(tokens->tokens_length > 1){
    pid = strtol(tokens->tokens[1],NULL,10);
  }
  else
    pid = -1;
  process *p;
  for(p=first_process;p->next!=NULL;p=p->next){
    if(p->pid == pid)
      break;
  }
  put_process_in_background(p);
  return 1;
}

int background_processes_complete(){
  process *curr_proc;
  for(curr_proc=first_process;curr_proc!=NULL;curr_proc=curr_proc->next){
    if(curr_proc->completed == 0)
      return 0;
  }
  return 1;
}

void update_status(){
  int status;
  pid_t pid;
  do{
    pid = waitpid(WAIT_ANY,&status,WNOHANG||WUNTRACED);
    if(pid < 0)
      perror("pid update status error");
  }while(!mark_process_complete(pid,status));
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    if(setpgid(shell_pgid, shell_pgid) < 0){
      perror("Couldn't put shell in its own process group\n");
      exit(1);
    }

    signal(SIGTSTP, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    printf("shell pid is %d\n",shell_pgid);

    first_process = (process *) malloc( sizeof(process) );
    first_process->pid = getpid();
    first_process->prog_name = NULL;
    first_process->stdin = STDIN_FILENO;
    first_process->stdout = STDOUT_FILENO;
    first_process->background=0;
    first_process->completed=1;
    first_process->next = NULL;

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

int main(unused int argc, unused char *argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      printf("shell tcgetpgrp is %d\n",tcgetpgrp(0));
      process *p = (process *) malloc(sizeof(process));
      p->argv = tokens->tokens;
      p->argc = tokens->tokens_length;
      p->completed = 0;
      p->stdin = STDIN_FILENO;
      p->stdout = STDOUT_FILENO;
      p->next = NULL;

      process_check_fg_bg(p);
      p->prog_name = calc_prog_path(p->argv,p->argc);
      process_redirect_io(p);
      add_process(p);

      pid_t cpid = fork();
      if(cpid > 0){
        setpgid(cpid,cpid);
        p->pid = cpid;
        if(!p->background)
          put_process_in_foreground(p);
      }
      else if(cpid == 0){
        p->pid = getpid();
        launch_process(p);
      }
      update_status();
      printf("tcgetpgrp at fork end is %d,shell is %d\n",tcgetpgrp(0),shell_pgid);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
