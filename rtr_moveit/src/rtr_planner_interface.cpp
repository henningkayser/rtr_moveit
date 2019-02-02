/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2018, PickNik LLC
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
 *   * Neither the name of PickNik LLC nor the names of its
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

/* Author: Henning Kayser
 * Desc: henningkayser@picknik.ai
 */

#include <deque>
#include <string>
#include <vector>
#include <iostream>

#include <mutex>

#include <rtr_moveit/rtr_planner_interface.h>
#include <rtr_moveit/rtr_conversions.h>

namespace rtr_moveit
{
// joint state distance
float getConfigDistance(const rtr::Config& first, const rtr::Config& second)
{
  if (first.size() != second.size())
    return DBL_MAX;
  float distance = 0.0;
  for (unsigned int i=0; i < first.size(); i++)
    distance += std::abs(first[i] - second[i]);
  return distance;
}

// find index of closest config in config list
unsigned int findClosestConfigId(const rtr::Config& config, const std::vector<rtr::Config>& configs)
{
  unsigned int result_id = -1;
  float min_distance = DBL_MAX;
  for (std::size_t i=0; i < configs.size(); i++)
  {
    float distance = getConfigDistance(config, configs[i]);
    if (distance < min_distance)
    {
      min_distance = distance;
      result_id = i;
    }
  }
  return result_id;
}

RTRPlannerInterface::RTRPlannerInterface(const ros::NodeHandle& nh)
  : nh_(nh)
{
}

RTRPlannerInterface::~RTRPlannerInterface()
{
  // TODO(henningkayser) implement destructor
}

bool RTRPlannerInterface::initialize()
{
  #if RAPID_PLAN_INTERFACE_ENABLED
  // check if hardware is connected
  if (!rapidplan_interface_.Connected())
  {
    ROS_ERROR_NAMED(LOGNAME, "Unable to initialize RapidPlan interface. Hardware is not connected.");
    return false;
  }

  // try to initialize hardware
  if (!rapidplan_interface_.Init())
  {
    ROS_ERROR_NAMED(LOGNAME, "Unable to initialize RapidPlan interface. Failed to initialize Hardware.");
    return false;
  }

  // perform handshake
  if (!rapidplan_interface_.Handshake())
  {
    ROS_ERROR_NAMED(LOGNAME, "Unable to initialize RapidPlan interface. Handshake failed.");
    return false;
  }
  #endif

  ROS_INFO_NAMED(LOGNAME, "RapidPlan interface initialized.");
  return true;
}

bool RTRPlannerInterface::isReady() const
{
  #if RAPID_PLAN_INTERFACE_ENABLED
  // try handshake
  if (!rapidplan_interface_.Handshake())
  {
    ROS_WARN_NAMED(LOGNAME, "RapidPlan interface is not ready. Handshake failed.");
    return false;
  }
  #endif

  ROS_DEBUG_NAMED(LOGNAME, "RapidPlan interface is ready.");
  return true;
}

bool RTRPlannerInterface::solve(const RoadmapSpecification& roadmap_spec, const rtr::Config& start_config,
                                const RapidPlanGoal& goal, const std::vector<rtr::Voxel>& occupancy_voxels,
                                std::vector<rtr::Config>& solution_path)
{
  std::deque<unsigned int> waypoints, edges;
  std::vector<rtr::Config> roadmap_states;
  bool success = solve(roadmap_spec, start_config, goal, occupancy_voxels, roadmap_states, waypoints, edges);
  //TODO(henningkayser): verify waypoints and states? This should already be done in the PathPlanner.
  if (success)
  {
    // fill solution path
    for (unsigned int waypoint : waypoints)
      solution_path.push_back(roadmap_states[waypoint]);

    // TODO(henningkayser): only print in debug mode
    std::cout << "Solution path:" << std::endl;
    for (unsigned int waypoint : waypoints)
    {
      std::cout << "waypoint " << (int) waypoint << ": ";
      for (float joint_value : roadmap_states[waypoint])
        std::cout << joint_value << " ";
      std::cout << std::endl;
    }
  }
  return success;
}

bool RTRPlannerInterface::solve(const RoadmapSpecification& roadmap_spec, const rtr::Config& start_config,
                                RapidPlanGoal goal, const std::vector<rtr::Voxel>& occupancy_voxels,
                                std::vector<rtr::Config>& roadmap_states, std::deque<unsigned int>& waypoints,
                                std::deque<unsigned int>& edges)
{
  {  // SCOPED MUTEX LOCK
    std::lock_guard<std::mutex> scoped_lock(mutex_);

    // Load roadmap to PathPlanner and MPA and get roadmap storage index
    uint16_t roadmap_index;
    if (!prepareRoadmap(roadmap_spec, roadmap_index))
      return false;

    // Check collisions using the RapidPlanInterface
    std::vector<uint8_t> collisions;
    #if RAPID_PLAN_INTERFACE_ENABLED
    if (!rapidplan_interface_.CheckScene(occupancy_voxels, roadmap_index, collisions))
    {
      ROS_ERROR_NAMED(LOGNAME, "HardwareInterface failed to check collision scene.");
      return false;
    }
    #else
    collisions.resize(planner_.GetNumEdges());  // dummy
    #endif

    // Call PathPlanner
    int result = -1;
    planner_.SetEdgeCost(&getConfigDistance);  // simple joint distance - TODO(henningkayser): use weighted distance?
    roadmap_states = planner_.GetConfigs();
    // Find closest existing configuration in roadmap that can be connected to the start state
    // TODO(henningkayser): add start state tolerance parameter
    // TODO(henningkayser): discuss API - we should search for this more efficiently and outside of the mutex scope
    unsigned int start_id = findClosestConfigId(start_config, roadmap_states);
    switch (goal.type)
    {
      case RapidPlanGoal::Type::TRANSFORM:
        {
          rtr::ToolPose tool_pose;
          rtrTransformToRtrToolPose(goal.transform, tool_pose);
          result = planner_.FindPath(start_id, tool_pose, collisions, goal.tolerance, goal.weights, waypoints, edges);
          break;
        }
      case RapidPlanGoal::Type::JOINT_STATE:
        {
          // TODO(henningkayser): add goal state tolerance
          // TODO(henningkayser): discuss API - we should search for this more efficiently and outside of the mutex scope
          // find goal state id and treat as STATE_IDS type
          goal.state_ids = { findClosestConfigId(goal.joint_state, roadmap_states) };
        }
      case RapidPlanGoal::Type::STATE_IDS:
        {
          result = planner_.FindPath(start_id, goal.state_ids, collisions, waypoints, edges);
        }
    }
    // process result
    if (result == 0)
    {
      ROS_INFO_STREAM_NAMED(LOGNAME, "RapidPlan found solution path with " << waypoints.size() << " waypoints.");
      //TODO(henningkayser): Only print if debugging is enabled
      std::cout << "Waypoint ids: ";
      for (unsigned int waypoint : waypoints)
        std::cout << (int) waypoint << " ";
      std::cout << std::endl << "Edges: ";
      std::vector<std::array<unsigned int, 2>> roadmap_edges = planner_.GetEdges();
      for (unsigned int edge_id : edges)
        std::cout << (int) roadmap_edges[(int) edge_id][0] << "-" << (int) roadmap_edges[(int) edge_id][1] << " ";
      std::cout << std::endl;
    }
    else
    {
      ROS_ERROR_STREAM_NAMED(LOGNAME, "RapidPlan failed at finding a valid path - " << planner_.GetError(result));
    }
    return result == 0;  // 0 == SUCESS
  }  // SCOPED MUTEX UNLOCK
}

bool RTRPlannerInterface::prepareRoadmap(const RoadmapSpecification& roadmap_spec, uint16_t& roadmap_index)
{
  // save new roadmap if it is new
  std::string roadmap_id = roadmap_spec.roadmap_id;
  if (roadmaps_.find(roadmap_spec.roadmap_id) == roadmaps_.end())
    roadmaps_[roadmap_spec.roadmap_id] = roadmap_spec;

  // TODO(henningkayser): Only store *.og file paths, others will be deprecated with the next API
  RoadmapFiles files = roadmaps_[roadmap_id].files;

  // verify that the roadmap is loaded in the PathPlanner
  if (roadmap_id != loaded_roadmap_)
  {
    ROS_INFO_STREAM_NAMED(LOGNAME, "Loading roadmap: " << files.occupancy);
    if (!planner_.LoadRoadmap(files.occupancy))
    {
      ROS_ERROR_STREAM_NAMED(LOGNAME, "Failed to load roadmap '" << roadmap_id << "' to RapidPlan PathPlanner.");
      return false;
    }
  }
  // check if roadmap is already written to hardware
  if (!findRoadmapIndex(roadmap_id, roadmap_index))
  {
    #if RAPID_PLAN_INTERFACE_ENABLED
    // write roadmap and retrieve new roadmap index
    if (!rapidplan_interface_.WriteRoadmap(files.occupancy, roadmap_index))
    {
      ROS_ERROR_STREAM_NAMED(LOGNAME, "Failed to write roadmap '" << roadmap_id << "' to RapidPlan MPU.");
      return false;
    }
    #else
    // if we don't use hardware, we increase the numbers
    roadmap_index = roadmap_indices_.size();
    #endif
    roadmap_indices_[roadmap_index] = roadmap_id;
  }
  ROS_INFO_STREAM_NAMED(LOGNAME, "RapidPlan initialized with with roadmap '" << roadmap_id << "'");
  return true;
}
}  // namespace rtr_moveit
