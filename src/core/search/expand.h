/*
 * search/expand.h - the single place that turns a product state into its
 * successor edges.
 *
 * Four search paths used to spell out the same actor/choice loop themselves:
 * the BFS queue, the DFS frame stack, the random walk, and the parallel
 * worker's single-state expander. Any change to which successors get expanded
 * therefore had to be made in four places, and missing one was a silent
 * completeness hole (the plan's C4 invariant).
 *
 * `Expansion` is a resumable enumerator, not a callback loop: DFS has to dive
 * into a successor before it asks for the next choice, so the caller must be
 * able to return to the enumeration later. BFS, the worker expander and the
 * random walk drive the same object synchronously.
 *
 * A future reduction policy (ample set / DPOR) belongs here: it can trim the
 * actor set or the choice list inside this object and every path inherits it.
 */
#ifndef DETSIM_SEARCH_EXPAND_H
#define DETSIM_SEARCH_EXPAND_H

#include "scheduler.h"
#include "state/state.h"

#include <vector>

namespace detsim::search
{

struct ExpandOptions
{
  /* First syscall-choice index tried for an actor. 0 enumerates in a fixed
   * order, which is what every completeness-bearing path uses (BFS, DFS and the
   * parallel worker) and why DFS now commits the same edge sequence for the same
   * root: MODE_DFS only randomizes when the preset is negative. Pass -1 to leave
   * the first choice to the mode's auto policy (differential runs against the
   * pre-unification tracer); the random walk does, because MODE_RAND always picks
   * its own choice regardless of the preset. */
  int first_choice_preset = 0;
  /* Execute one uniformly random enabled actor and then report the source
   * exhausted. The random walk uses this; it makes no completeness claim. */
  bool random_actor = false;
};

class Expansion
{
  public:
  Expansion(const sys_state& source, bool may_expand, ExpandOptions options = {});

  EnumerationKind kind() const { return kind_; }
  const std::vector<ActorAction>& actors() const { return actors_; }
  bool expanded() const { return kind_ == EnumerationKind::Expanded; }

  /* Execute the next (actor, choice) pair. Returns false when the source has no
   * further edges for this caller. `choice` is the syscall choice the action
   * actually used (-1 when it had none) and `cardinality` the discovered count
   * for that actor (0 when it had none), both valid until the next call. */
  bool next(TransitionResult& result, int& choice, int& cardinality);

  /* The actor of the most recent successful next(). */
  const ActorAction& current_actor() const { return actors_[current_actor_]; }

  /* Re-point the enumerator at an equivalent copy of the source. A caller that
   * had to reopen the state (the root, or a frame resumed after a coverage memo)
   * must rebind before it continues enumerating. The copy must be the same
   * product state. */
  void rebind(const sys_state& source) { source_ = &source; }

  private:
  const sys_state* source_;
  std::vector<ActorAction> actors_;
  EnumerationKind kind_;
  ExpandOptions options_;
  size_t actor_pos_ = 0;
  std::vector<int> remaining_;
  int cardinality_ = -1;
  bool actor_started_ = false;
  bool random_done_ = false;
  size_t current_actor_ = 0;
};

/* One transition of one enumerated actor, with the preset/cardinality contract
 * every path relies on: the first call for an actor discovers the choice count,
 * later calls must replay one of the discovered presets. */
TransitionResult expand_actor_choice(const sys_state& source,
                                     const ActorAction& actor, int preset,
                                     int& cardinality,
                                     std::vector<int>& remaining);

} // namespace detsim::search

#endif
