
typedef struct process{

  pid_t pid;
  char *prog_name;
  char **argv;
  int argc;
  int stdin;
  int stdout;
  char background;
  char completed;
  struct process *next;

} process;

process *first_process;

char *calc_prog_path(char **argv, int argc);
void process_redirect_io(process *p);
void add_process(process *p);
void launch_process(process *p);
void process_check_fg_bg(process *p);
int mark_process_complete(pid_t pid, int status);
void put_process_in_foreground(process *p);
void put_process_in_background(process *p);
