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
#include <fcntl.h>

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
  {cmd_cd, "cd", "change current working directory to specified path"}
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

int create_env_path(char ***env_arr_pointer)
{
  char *env_str = getenv("PATH");
  int start=0,end=0,count=0;
  while(env_str[end] != '\0'){
    if(env_str[end] == ':'){
      char *path = (char *) malloc(end-start+1);
      memcpy(path,env_str+start,end-start);
      path[end] = '\0';
      *env_arr_pointer = (char **) realloc(*env_arr_pointer, sizeof(char *) * (count+1));
      if(*env_arr_pointer == NULL)
        printf("null env arr pointer\n");
      (*env_arr_pointer)[count++] = path;
      start=end+1;
    }
    ++end;
  }
  return count;
}

char *calc_prog_path(unused struct tokens *tokens){
  char *prog_full_name = (char *) malloc(sizeof(char)*200);
  if(tokens->tokens[0][0] == '/')
    strcpy(prog_full_name,tokens->tokens[0]);
  else{
    char **env_path = NULL;
    int size = create_env_path(&env_path);
    for(int i=0;i<size;i++)
    {
      strcat(env_path[i],"/");
      strcpy(prog_full_name,(strcat(env_path[i],tokens->tokens[0])));
      if(access(prog_full_name,F_OK|X_OK) != -1)
        break;
    }
  }
  return prog_full_name;
}

int exec_program(unused struct tokens *tokens){
  printf("pid inside exec program is %d\n",getpid());
  int status;
  char *prog_full_name = calc_prog_path(tokens);

  int redirect_fd = -1, token_index = -1;
  char *redirect_file = NULL;
  for(int i=0;i<tokens->tokens_length;++i){
    if(strcmp(tokens->tokens[i],"<")==0){
      redirect_fd = 0;
      token_index = i;
      redirect_file = tokens->tokens[i+1];
      break;
    }
    else if(strcmp(tokens->tokens[i],">")==0){
      redirect_fd = 1;
      token_index=i;
      redirect_file = tokens->tokens[i+1];
      break;
    }
  }
  pid_t cpid = fork();
  if(cpid > 0){
    printf("parent pid is %d\n",getpid());
    printf("parent pg id is %d\n",getpgid(0));
    setpgid(getpid(),0);
    wait(&status);
    tcsetpgrp(shell_terminal,shell_pgid);
  }
  else if(cpid == 0){
    printf("child pid is %d\n",getpid());
    printf("child pg id before set is %d\n",getpgid(0));
    setpgid(getpid(),0);
    printf("child pg id after set is %d\n",getpgid(0));
    tcsetpgrp(shell_terminal,getpgid(0));
    if(redirect_fd != -1 && redirect_file != NULL){
      int fd = open(redirect_file,O_RDWR);
      if (fd == -1){
        perror("Redirect error");
        exit(0);
      }
      dup2(fd,redirect_fd);
      close(fd);
    }

    tokens->tokens = (char**) realloc(tokens->tokens, sizeof(char *) * (tokens->tokens_length + 1));
    tokens->tokens[token_index] = NULL;
    int exec_status = execv(prog_full_name,tokens->tokens);
    if(exec_status == -1)
      perror("exec error");
    printf("After execv in child\n");
  }
  else{
    perror("Exec program error");
    return 0;
  }
  return 1;
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
    printf("our shell pgid is %d\n",getpgid(0));
    printf("our shell pid is %d\n",getpid());
    if(setpgid(shell_pgid,shell_pgid) < 0){
      perror("set shell pgrp error");
    }
    printf("our shell pgid after set is %d\n",getpgid(0));

    signal(SIGTSTP, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);




    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

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
      /* REPLACE this to run commands as programs.
      fprintf(stdout, "This shell doesn't know how to run programs.\n");*/
      exec_program(tokens);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
