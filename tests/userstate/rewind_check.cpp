/*
 * rewind_check.cpp - UserCheck/UserState plugin for the rewind regression test.
 *
 * Each node owns a monotone epoch in the guest (epoch_a/b/c). The check fails
 * as soon as a node reports an epoch lower than the one this plugin saw last.
 * The plugin keeps that last value in the shadow state it hands to DetSim
 * through the optional UserState hooks, so rewinding the exploration to an
 * earlier state must also rewind the shadow copy. Without that, the plugin
 * compares a restored low epoch against a stale high watermark and reports a
 * false "epoch decreased" failure.
 */

#include "debug.h"
#include "utils/expr_eval.hpp"
#include <cstring>

struct rewind_shadow
{
  long epoch;
};

static rewind_shadow g_shadow[NP] = {};

static const char *const kEpochNames[3] = {"epoch_a", "epoch_b", "epoch_c"};

extern "C"
{

  int check()
  {
    for (int i = 0; i < NP; i++)
    {
      if (i >= 3)
        continue;
      bool ok = false;
      long epoch = eval<long>(kEpochNames[i], i, &ok);
      if (!ok)
        continue;

      if (epoch < g_shadow[i].epoch)
      {
        detsim::ui::ui_printf(
            "[CHECK FAILED] Node %d epoch decreased: %ld -> %ld\n", i,
            g_shadow[i].epoch, epoch);
        return 1;
      }

      g_shadow[i].epoch = epoch;
    }
    return 0;
  }

  int user_state_size(int index)
  {
    (void)index;
    return static_cast<int>(sizeof(rewind_shadow));
  }

  void user_state_save(int index, void *buf)
  {
    memcpy(buf, &g_shadow[index], sizeof(rewind_shadow));
  }

  void user_state_load(int index, const void *buf, int size)
  {
    if (size == static_cast<int>(sizeof(rewind_shadow)))
      memcpy(&g_shadow[index], buf, sizeof(rewind_shadow));
  }
}
