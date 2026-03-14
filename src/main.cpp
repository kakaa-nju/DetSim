#include <cstdio>
#include "core/state_store.h"
#include "core/sysstate_store.h"

void init_monitor(int argc, char *argv[]);
void ui_mainloop();
void init_state();
void init_dwarf();
void cleanup_all();

int main(int argc, char *argv[])
{
  /* parse arguments, read configuration */
  init_monitor(argc, argv);

  /* start tracee, get initial state */
  init_state();

  /* analyze variable addresses */
  init_dwarf();

  printf("[DEBUG] About to call ui_mainloop\n");
  ui_mainloop();
  printf("[DEBUG] ui_mainloop returned\n");
  
  /* cleanup all resources before exit */
  cleanup_all();
  printf("[DEBUG] cleanup completed\n");
}
