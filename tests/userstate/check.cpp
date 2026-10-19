/*
 * check.cpp - UserState plugin for the UserState test.
 *
 * Demonstrates that a plugin can keep custom "shadow" state that DetSim
 * checkpoints and restores together with the program state, and that the
 * restored state is visible again from check().
 */

#include "debug.h"
#include "user_state.h"
#include "utils/expr_eval.hpp"
#include <cstring>

struct user_shadow
{
  long counter;
};

static user_shadow g_shadow[NP] = {};

extern "C"
{

  int check()
  {
    for (int i = 0; i < NP; i++)
    {
      bool ok = false;
      long counter = eval<long>("user_counter", i, &ok);
      if (!ok)
        continue;

      if (counter < g_shadow[i].counter)
      {
        detsim::ui::ui_printf(
            "[CHECK FAILED] user_counter decreased for node %d: %ld -> %ld\n",
            i, g_shadow[i].counter, counter);
        return 1;
      }

      g_shadow[i].counter = counter;
    }
    return 0;
  }

  int user_state_size(int index)
  {
    (void)index;
    return static_cast<int>(sizeof(user_shadow));
  }

  void user_state_save(int index, void *buf)
  {
    memcpy(buf, &g_shadow[index], sizeof(user_shadow));
  }

  void user_state_load(int index, const void *buf, int size)
  {
    if (size == static_cast<int>(sizeof(user_shadow)))
      memcpy(&g_shadow[index], buf, sizeof(user_shadow));
  }
}
