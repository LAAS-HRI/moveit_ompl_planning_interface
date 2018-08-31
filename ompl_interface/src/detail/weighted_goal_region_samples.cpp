/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2010, Rice University
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Rice University nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Ioan Sucan */
/* Modified by: Juan David Hernandez Vega */

#include <moveit/ompl_interface/detail/weighted_goal_region_samples.h>
#include <utility>

#include <ompl/base/ScopedState.h>
#include <ompl/util/Time.h>

ompl::base::WeightedGoalRegionSamples::WeightedGoalRegionSamples(const SpaceInformationPtr& si,
                                                                 GoalRegionSamplingFn samplerFunc,
                                                                 const unsigned int max_sampled_goals, bool autoStart,
                                                                 double minDist)
  : GoalStates(si)
  , samplerFunc_(std::move(samplerFunc))
  , terminateSamplingThread_(false)
  , samplingThread_(nullptr)
  , samplingAttempts_(0)
  , minDist_(minDist)
  , max_sampled_goals_(max_sampled_goals)
  , sample_goals_(true)
  , num_sampled_goals_(0)
{
  type_ = GOAL_LAZY_SAMPLES;
  if (autoStart)
    startSampling();
}

ompl::base::WeightedGoalRegionSamples::~WeightedGoalRegionSamples()
{
  stopSampling();
}

void ompl::base::WeightedGoalRegionSamples::startSampling()
{
  std::lock_guard<std::mutex> slock(lock_);
  if (samplingThread_ == nullptr)
  {
    OMPL_DEBUG("Starting goal sampling thread");
    terminateSamplingThread_ = false;
    samplingThread_ = new std::thread(&WeightedGoalRegionSamples::goalSamplingThread, this);
  }
}

void ompl::base::WeightedGoalRegionSamples::stopSampling()
{
  /* Set termination flag */
  {
    std::lock_guard<std::mutex> slock(lock_);
    if (!terminateSamplingThread_)
    {
      OMPL_DEBUG("Attempting to stop goal sampling thread...");
      terminateSamplingThread_ = true;
    }
  }

  /* Join thread */
  if (samplingThread_ != nullptr)
  {
    samplingThread_->join();
    delete samplingThread_;
    samplingThread_ = nullptr;
  }
}

void ompl::base::WeightedGoalRegionSamples::goalSamplingThread()
{
  {
    /* Wait for startSampling() to finish assignment
     * samplingThread_ */
    std::lock_guard<std::mutex> slock(lock_);
  }

  if (!si_->isSetup())  // this looks racy
  {
    OMPL_DEBUG("Waiting for space information to be set up before the sampling thread can begin computation...");
    // wait for everything to be set up before performing computation
    while (!terminateSamplingThread_ && !si_->isSetup())
      std::this_thread::sleep_for(time::seconds(0.01));
  }
  unsigned int prevsa = samplingAttempts_;
  if (isSampling() && samplerFunc_)
  {
    OMPL_DEBUG("Beginning sampling thread computation");
    while (isSampling())
    {
      std::vector<State*> sampled_states;
      if (num_sampled_goals_ < max_sampled_goals_)
      {
        samplerFunc_(this, sampled_states);
        //        std::cout << "max_sampled_goals_: " << max_sampled_goals_ << std::endl;
        //        std::cout << "num_sampled_goals_ (before): " << num_sampled_goals_ << std::endl;
        //        std::cout << "sampled_states.size(): " << sampled_states.size() << std::endl;

        bool increase_num_sampled_goals = false;
        for (auto& sampled_state : sampled_states)
        {
          // si_->printState(sampled_state->as<State>());
          if (si_->satisfiesBounds(sampled_state) && si_->isValid(sampled_state))
          {
            increase_num_sampled_goals = true;
            ++num_sampled_goals_;
            OMPL_DEBUG("Adding goal state");
            // addStateIfDifferent(sampled_state, minDist_);
            std::lock_guard<std::mutex> slock(lock_);
            GoalStates::addState(sampled_state);
          }
          else
          {
            OMPL_DEBUG("Invalid goal candidate");
          }
        }
        if (num_sampled_goals_ >= max_sampled_goals_)
        {
          sample_goals_ = false;
        }
        // std::cout << "num_sampled_goals_ (after): " << num_sampled_goals_ << std::endl;
        if (increase_num_sampled_goals)
          ++samplingAttempts_;
      }
    }
  }
  else
    OMPL_WARN("Goal sampling thread never did any work.%s",
              samplerFunc_ ? (si_->isSetup() ? "" : " Space information not set up.") : " No sampling function "
                                                                                        "set.");
  {
    std::lock_guard<std::mutex> slock(lock_);
    terminateSamplingThread_ = true;
  }

  OMPL_DEBUG("Stopped goal sampling thread after %u sampling attempts", samplingAttempts_ - prevsa);
}

bool ompl::base::WeightedGoalRegionSamples::isSampling() const
{
  std::lock_guard<std::mutex> slock(lock_);
  return !terminateSamplingThread_ && samplingThread_ != nullptr;
}

bool ompl::base::WeightedGoalRegionSamples::couldSample() const
{
  return canSample() || isSampling();
}

void ompl::base::WeightedGoalRegionSamples::clear()
{
  std::lock_guard<std::mutex> slock(lock_);
  GoalStates::clear();
  goals_priority_queue_.clear();
}

double ompl::base::WeightedGoalRegionSamples::distanceGoal(const State* st) const
{
  std::lock_guard<std::mutex> slock(lock_);
  return GoalStates::distanceGoal(st);
}

void ompl::base::WeightedGoalRegionSamples::sampleGoal(base::State* st) const
{
  std::lock_guard<std::mutex> slock(lock_);
  GoalStates::sampleGoal(st);
}

void ompl::base::WeightedGoalRegionSamples::setNewStateCallback(const NewStateCallbackFn& callback)
{
  callback_ = callback;
}

void ompl::base::WeightedGoalRegionSamples::addState(const State* st)
{
  std::lock_guard<std::mutex> slock(lock_);
  GoalStates::addState(st);
}

const ompl::base::State* ompl::base::WeightedGoalRegionSamples::getState(unsigned int index) const
{
  std::lock_guard<std::mutex> slock(lock_);
  return GoalStates::getState(index);
}

bool ompl::base::WeightedGoalRegionSamples::hasStates() const
{
  std::lock_guard<std::mutex> slock(lock_);
  return GoalStates::hasStates();
}

std::size_t ompl::base::WeightedGoalRegionSamples::getStateCount() const
{
  std::lock_guard<std::mutex> slock(lock_);
  return GoalStates::getStateCount();
}

unsigned int ompl::base::WeightedGoalRegionSamples::maxSampleCount() const
{
  std::lock_guard<std::mutex> slock(lock_);
  return GoalStates::maxSampleCount();
}

bool ompl::base::WeightedGoalRegionSamples::addStateIfDifferent(const State* st, double minDistance)
{
  const base::State* newState = nullptr;
  bool added = false;
  {
    std::lock_guard<std::mutex> slock(lock_);
    if (GoalStates::distanceGoal(st) > minDistance)
    {
      GoalStates::addState(st);
      added = true;
      if (callback_)
        newState = states_.back();
    }
  }

  // the lock is released at this; if needed, issue a call to the callback
  if (newState != nullptr)
    callback_(newState);
  return added;
}

void ompl::base::WeightedGoalRegionSamples::penalizeWeightedGoal(WeightedGoal& weighted_goal)
{
  double w = weighted_goal.heap_element_->data->weight_;
  // std::cout << "penalize -> prev w: " << w << std::endl;
  weighted_goal.heap_element_->data->weight_ = w / (w + 1.);
  goals_priority_queue_.update(weighted_goal.heap_element_);

  // std::cout << "penalize -> after w: " << weighted_goal.heap_element_->data->weight_ << std::endl;

  if (w < 0.2 && !sample_goals_)
  {
    std::vector<WeightedGoal*> existing_weighted_goals;
    goals_priority_queue_.getContent(existing_weighted_goals);
    for (auto weighted_goal : existing_weighted_goals)
    {
      weighted_goal->heap_element_->data->weight_ = 0.5;
      goals_priority_queue_.update(weighted_goal->heap_element_);
    }

    sample_goals_ = true;
    max_sampled_goals_ += 10;  // addSampledGoalStates();
    // std::cout << "sample more goals !!!!!!!!!!!!!!!!!!!!!!!!!!!!" << std::endl;
  }
}

void ompl::base::WeightedGoalRegionSamples::rewardWeightedGoal(WeightedGoal& weighted_goal)
{
  double w = weighted_goal.heap_element_->data->weight_;
  // std::cout << "reward -> prev w: " << w << std::endl;

  if (w < 1.0)
  {
    double new_w = w / (1. - w);

    if (new_w > 1.0)
      new_w = 1.0;

    weighted_goal.heap_element_->data->weight_ = new_w;
    goals_priority_queue_.update(weighted_goal.heap_element_);

    // std::cout << "reward -> after w: " << new_w << std::endl;
  }
}

void ompl::base::WeightedGoalRegionSamples::sampleWeightedGoal(WeightedGoal& weighted_goal)
{
  if (states_.empty())
    throw Exception("There are no goals to sample");

  ompl::BinaryHeap<WeightedGoal*, WeightedGoalCompare>::Element* heap_element = goals_priority_queue_.top();

  si_->copyState(weighted_goal.state_, heap_element->data->state_);
  weighted_goal.weight_ = heap_element->data->weight_;
  weighted_goal.heap_element_ = heap_element->data->heap_element_;
}

void ompl::base::WeightedGoalRegionSamples::sampleConsecutiveGoal(WeightedGoal& weighted_goal)
{
  if (states_.empty())
    throw Exception("There are no goals to sample");

  //  std::lock_guard<std::mutex> slock(lock_);
  GoalStates::sampleGoal(weighted_goal.state_);

  //  si_->printState(weighted_goal.state_);
  //  std::cout << "this->num_sampled_goals_: " << this->num_sampled_goals_ << std::endl;
  //
  //  ompl::BinaryHeap<WeightedGoal*, WeightedGoalCompare>::Element* heap_element = goals_priority_queue_.top();
  //
  //  si_->copyState(weighted_goal.state_, heap_element->data->state_);
  //  weighted_goal.weight_ = heap_element->data->weight_;
  //  weighted_goal.heap_element_ = heap_element->data->heap_element_;
}
