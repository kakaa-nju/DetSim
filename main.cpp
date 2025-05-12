void init_monitor(int argc, char *argv[]);
void ui_mainloop();
void init_state();
void init_dwarf();

int main(int argc, char *argv[])
{
  /* parse arguments, read configuration */
  init_monitor(argc, argv);

  /* start tracee, get initial state */
  init_state();

  /* analyze variable addresses */
  init_dwarf();

  ui_mainloop();
}
