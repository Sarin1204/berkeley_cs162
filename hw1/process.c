#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>

#include "process.h"

void add_process(process *p){
  while(first_process->next != NULL){
    first_process=first_process->next;
  }
  first_process->next=p;
}

void launch_process(process *p){

  setpgid(getpid(),0);
  if(!p->background)
    tcsetpgrp(0,getpgid(0));
  printf("tcgetprp in launch process is %d\n",tcgetpgrp(0));
  signal(SIGTSTP, SIG_DFL);
  signal(SIGINT, SIG_DFL);
  signal(SIGQUIT, SIG_DFL);
  signal(SIGTTIN, SIG_DFL);
  signal(SIGTTOU, SIG_DFL);
  signal(SIGCHLD, SIG_DFL);
  p->argv = (char**) realloc(p->argv, sizeof(char *) * (p->argc+1));
  p->argv[p->argc] = NULL;
  if(p->stdin != STDIN_FILENO){
    dup2(p->stdin,STDIN_FILENO);
  }
  if(p->stdout != STDOUT_FILENO){
    dup2(p->stdout,STDOUT_FILENO);
  }
  printf("child pid is %d\n",getpid());
  int exec_status = execv(p->prog_name,p->argv);
  if(exec_status == -1){
    printf("exec name is %s\n",p->prog_name);
    for(int i=0;i<p->argc;++i){
      printf("%d: %s\n",i,p->argv[i]);
    }
    perror("exec error");
  }
}

void put_process_in_foreground(process *p){
  int status;
  tcsetpgrp(STDIN_FILENO,p->pid);
  waitpid(WAIT_ANY, &status, WUNTRACED);
  tcsetpgrp(STDIN_FILENO,first_process->pid);
}

void process_check_fg_bg(process *p){
  if(p->argv[p->argc-1][0]=='&'){
    p->background = 1;
    p->argv[p->argc-1]= NULL;
    p->argc--;
  }
  else
    p->background = 0;
}

void process_redirect_io(process *p){
  int last_arg_index = -1;
  for(int i=0; i < p->argc;++i){
    if(strcmp(p->argv[i], "<")==0){
      last_arg_index = i;
      p->stdin = open(p->argv[i+1],O_RDWR);
      if(p->stdin == -1){
        perror("Redirect stdin  error");
        exit(0);
      }
    }
    else if(strcmp(p->argv[i],">")==0){
      p->stdout = open(p->argv[i+1],O_RDWR);
      if(p->stdout == -1){
        perror("Redirect stdout error");
        exit(0);
      }
    }
  }
  if(last_arg_index != -1)
    p->argv[last_arg_index] = NULL;
}

int create_env_path(char ***env_arr_pointer){
  char *env_str = getenv("PATH");
  int start=0,end=0,count=0;
  while(env_str[end] != '\0'){
    if(env_str[end] == ':'){
      char *path = (char *) malloc(end-start+1);
      memcpy(path,env_str+start,end-start);
      path[end]='\0';
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

char *calc_prog_path(char **argv, int argc){
  char *prog_full_name = (char *) malloc(sizeof(char)*200);
  if(argv[0][0] == '/')
    strcpy(prog_full_name, argv[0]);
  else{
    char **env_path = NULL;
    int size = create_env_path(&env_path);
    for(int i=0;i<size;++i)
    {
      strcat(env_path[i],"/");
      strcpy(prog_full_name,(strcat(env_path[i],argv[0])));
      if(access(prog_full_name,F_OK|X_OK) != -1)
        break;
    }
  }
  return prog_full_name;
}
