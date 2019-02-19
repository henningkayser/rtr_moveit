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
 * Desc: Generation of occupancy data from pcl or planning scenes
 */

#include <rtr_moveit/occupancy_handler.h>
#include <pcl_conversions/pcl_conversions.h>

// Eigen
#include <Eigen/Geometry>
#include <eigen_conversions/eigen_msg.h>

// collision checks
#include <moveit/collision_detection/world.h>
#include <moveit/collision_detection_fcl/collision_world_fcl.h>
#include <geometric_shapes/shapes.h>

// RapidPlan
#include <rtr-occupancy/Voxel.hpp>

namespace rtr_moveit
{
const std::string LOGNAME = "occupancy_handler";
OccupancyHandler::OccupancyHandler()
  : nh_("")
{
}

OccupancyHandler::OccupancyHandler(const ros::NodeHandle& nh)
  : nh_(nh)
{
}

OccupancyHandler::OccupancyHandler(const ros::NodeHandle& nh, const std::string& pcl_topic)
  : nh_(nh), pcl_topic_(pcl_topic)
{
}

void OccupancyHandler::setVolumeRegion(const RoadmapVolume& roadmap_volume)
{
  volume_region_ = roadmap_volume;
}

void OccupancyHandler::setPointCloudTopic(const std::string& pcl_topic)
{
  pcl_topic_ = pcl_topic;
}

bool OccupancyHandler::fromPCL(OccupancyData& occupancy_data)
{
  // if point cloud is older than 100ms, get a new one
  if (!shared_pcl_ptr_ ||  (ros::Time::now().toNSec() * 1000 - shared_pcl_ptr_->header.stamp > 100000))
  {
    std::unique_lock<std::mutex> lock(pcl_mtx_);
    ros::Subscriber pcl_sub = nh_.subscribe(pcl_topic_, 1, &OccupancyHandler::pclCallback, this);
    pcl_condition_.wait(lock, [&](){return pcl_ready_;});
    pcl_ready_ = false;
    pcl_sub.shutdown();
  }

  // get result
  occupancy_data.type = OccupancyData::Type::POINT_CLOUD;
  occupancy_data.point_cloud = shared_pcl_ptr_;
  return occupancy_data.point_cloud != NULL;
}

void OccupancyHandler::pclCallback(const pcl::PCLPointCloud2ConstPtr& cloud_pcl2)
{
  std::unique_lock<std::mutex> lock(pcl_mtx_);
  if (!shared_pcl_ptr_)
    shared_pcl_ptr_.reset(new pcl::PointCloud<pcl::PointXYZ>());
  pcl::fromPCLPointCloud2(*cloud_pcl2, *shared_pcl_ptr_);
  pcl_ready_ = true;
  lock.unlock();
  pcl_condition_.notify_one();
}

bool OccupancyHandler::fromPlanningScene(const planning_scene::PlanningSceneConstPtr& planning_scene,
                                         OccupancyData& occupancy_data)
{
  // occupancy box id and dimensions
  // TODO(RTR-57): Check that box id is not present in planning scene - should be unique
  std::string box_id = "rapidplan_collision_box";
  double voxel_dimension = volume_region_.voxel_dimension;
  float x_length = volume_region_.dimensions[0];
  float y_length = volume_region_.dimensions[1];
  float z_length = volume_region_.dimensions[2];

  int x_voxels = x_length / voxel_dimension;
  int y_voxels = y_length / voxel_dimension;
  int z_voxels = z_length / voxel_dimension;

  // Compute transform: world->volume
  // world_to_volume points at the corner of the volume with minimal x,y,z
  // we use auto to support Affine3d and Isometry3d (kinetic + melodic)
  auto world_to_base(planning_scene->getFrameTransform(volume_region_.base_frame));
  auto base_to_volume = world_to_base;
  tf::poseMsgToEigen(volume_region_.center_pose, base_to_volume);
  auto world_to_volume = world_to_base * base_to_volume;

  // create collision world and add voxel box shape one step outside the volume grid
  collision_detection::CollisionWorldFCL world;
  shapes::Box box(voxel_dimension, voxel_dimension, voxel_dimension);
  Eigen::Translation3d box_start_position(-(voxel_dimension + x_length)/2,
                                          -(voxel_dimension + y_length)/2,
                                          -(voxel_dimension + z_length)/2);
  world.getWorld()->addToObject(box_id, std::make_shared<const shapes::Box>(box), world_to_volume * box_start_position);

  // collision request and result
  collision_detection::CollisionRequest request;
  collision_detection::CollisionResult result;

  // clear scene boxes vector
  occupancy_data.type = OccupancyData::Type::VOXELS;
  occupancy_data.voxels.resize(0);

  // x/y/z step transforms
  auto identity = world_to_volume;
  identity.setIdentity();
  auto x_step(identity * Eigen::Translation3d(voxel_dimension, 0, 0));
  auto y_step(identity * Eigen::Translation3d(0, voxel_dimension, 0));
  auto z_step(identity * Eigen::Translation3d(0, 0, voxel_dimension));

  // x/y reset transforms
  auto y_reset(identity * Eigen::Translation3d(0, -y_voxels * voxel_dimension, 0));
  auto z_reset(identity * Eigen::Translation3d(0, 0, -z_voxels * voxel_dimension));

  // Loop over X/Y/Z voxel positions and check for box collisions in the collision scene
  // NOTE: This implementation is a prototype and will be replaced by more efficient methods as described below
  // TODO(RTR-57): More efficient implementations:
  //                          * Iterate over collision objects and only sample local bounding boxes
  //                          * Use octree search, since boxes can have variable sizes
  // TODO(RTR-57): adjust grid to odd volume dimensions
  // TODO(RTR-57): Do we need extra Box padding here?
  for (uint16_t x = 0; x < x_voxels; ++x)
  {
    world.getWorld()->moveObject(box_id, x_step);
    for (uint16_t y = 0; y < y_voxels; ++y)
    {
      world.getWorld()->moveObject(box_id, y_step);
      for (uint16_t z = 0; z < z_voxels; ++z)
      {
        world.getWorld()->moveObject(box_id, z_step);
        planning_scene->getCollisionWorld()->checkWorldCollision(request, result, world);
        if (result.collision)
        {
          occupancy_data.voxels.push_back(rtr::Voxel(x, y, z));
          result.clear();  // TODO(RTR-57): Is this really necessary?
        }
      }
      // move object back to z start
      world.getWorld()->moveObject(box_id, z_reset);
    }
    // move object back to y start
    world.getWorld()->moveObject(box_id, y_reset);
  }
  return true;
}
}  // namespace rtr_moveit