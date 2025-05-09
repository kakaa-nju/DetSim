const char *syscalls[450];
void init_monitor(int argc, char *argv[]);
void ui_mainloop();
void init_state();
void init_dwarf();

void init_syscalls() {
  #include "nr2call.h"
}

int main(int argc, char *argv[]) {
  /* syscall list */
  init_syscalls();

  /* parse arguments, read configuration */
  init_monitor(argc, argv);

  /* start tracee, get initial state */
  init_state();

  /* analyze variable addresses */
  init_dwarf();

  ui_mainloop();
}

