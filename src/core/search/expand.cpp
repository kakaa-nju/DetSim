#include "search/expand.h"

#include "config.h"
#include "monitor.h"
#include "state/state_transition.h"
#include "state/transition_profile.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace detsim::search
{

TransitionResult expand_actor_choice(const sys_state& source,
                                     const ActorAction& actor, int preset,
                                     int& cardinality,
                                     std::vector<int>& remaining)
{
  const detsim::profile::DetailScope call_scope(detsim::profile::kTransitionCall);
  ptmc_state.n_choose = 0;
  ptmc_state.choose = preset;
  TransitionResult result;
  try
  {
    result = state_transition(source, actor.process, actor.thread, actor.action);
  }
  catch (const ActionRejected& error)
  {
    throw std::runtime_error(std::string("Enumerated actor action was rejected: ") + error.what());
  }
  if (result.code == CKPT_DISCARD)
    throw std::runtime_error("Enumerated actor action did not execute");
  const int discovered = ptmc_state.n_choose;
  const int selected = ptmc_state.choose;
  if (discovered < 0 || (discovered && (selected < 0 || selected >= discovered)))
    throw std::runtime_error("Invalid syscall choice discovery");
  if (cardinality < 0)
  {
    cardinality = discovered;
    for (int choice = 0; choice < discovered; ++choice)
      if (choice != selected) remaining.push_back(choice);
  }
  else if (discovered != cardinality || !discovered || selected != preset)
  {
    throw std::runtime_error("Syscall choice cardinality/preset changed for the same saved actor action");
  }
  return result;
}

Expansion::Expansion(const sys_state& source, bool may_expand,
                     ExpandOptions options)
    : source_(&source), actors_(enabled_actor_actions(source)),
      kind_(classify_enumeration(source, actors_, may_expand)), options_(options)
{
}

bool Expansion::next(TransitionResult& result, int& choice, int& cardinality)
{
  choice = -1;
  cardinality = 0;
  if (!expanded())
    return false;

  if (options_.random_actor)
  {
    if (random_done_ || actors_.empty())
      return false;
    random_done_ = true;
    current_actor_ = static_cast<size_t>(std::rand()) % actors_.size();
    const ActorAction& actor = actors_[current_actor_];
    std::vector<int> unused;
    int discovered = -1;
    /* The random walk has never discovered a preset: it lets the mode's auto
     * choice pick the branch, exactly as it did before this loop was shared. */
    result = expand_actor_choice(*source_, actor, options_.first_choice_preset,
                                discovered, unused);
    choice = actor.action == TransitionAction::Timeout ? DETSIM_CHOICE_TIMEOUT :
        (ptmc_state.n_choose ? ptmc_state.choose : -1);
    cardinality = ptmc_state.n_choose;
    return true;
  }

  for (;;)
  {
    if (actor_pos_ >= actors_.size())
      return false;
    int preset;
    if (!actor_started_)
      preset = options_.first_choice_preset;
    else if (!remaining_.empty())
    {
      preset = remaining_.back();
      remaining_.pop_back();
    }
    else
    {
      ++actor_pos_;
      cardinality_ = -1;
      actor_started_ = false;
      remaining_.clear();
      continue;
    }
    current_actor_ = actor_pos_;
    result = expand_actor_choice(*source_, actors_[current_actor_], preset,
                                 cardinality_, remaining_);
    actor_started_ = true;
    choice = actors_[current_actor_].action == TransitionAction::Timeout ? DETSIM_CHOICE_TIMEOUT :
        (ptmc_state.n_choose ? ptmc_state.choose : -1);
    cardinality = cardinality_;
    return true;
  }
}

} // namespace detsim::search
