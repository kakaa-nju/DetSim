/*
 * commands.h - CLI command handlers
 *
 * This module provides the command handlers for the DetSim CLI.
 * Each command is implemented as a function that takes arguments and returns
 * an integer status code.
 */

#ifndef __CLI_COMMANDS_H
#define __CLI_COMMANDS_H

#include <sys/types.h>

/* ======================================================================
 * Command Table
 * ====================================================================== */

struct CommandEntry
{
  const char *name;
  const char *description;
  int (*handler)(char *args);
};

/* Get the command table and its size */
const CommandEntry *get_command_table();
size_t get_command_count();

/* Find a command by name */
const CommandEntry *find_command(const char *name);

/* Execute a command line (parses and dispatches) */
int execute_command_line(char *cmd_line);

/* ======================================================================
 * Individual Command Handlers
 * ====================================================================== */

int cmd_help(char *args);
int cmd_bfs(char *args);
int cmd_dfs(char *args);
int cmd_rand(char *args);
int cmd_q(char *args);
int cmd_si(char *args);
int cmd_sw(char *args);
int cmd_thread(char *args);
int cmd_load(char *args);
int cmd_info(char *args);
int cmd_batch(char *args);
int cmd_p(char *args);
int cmd_x(char *args);
int cmd_bt(char *args);
int cmd_frame(char *args);
int cmd_locals(char *args);
int cmd_ls(char *args);
int cmd_stat(char *args);
int cmd_hexdump(char *args);
int cmd_diff(char *args);

/* ======================================================================
 * Command Utilities
 * ====================================================================== */

/* Display welcome message */
void show_welcome();

/* Cleanup readline resources */
void cleanup_readline();

/* Read a line from the user (using readline) */
char *read_line(const char *prompt);

#endif /* __CLI_COMMANDS_H */
