/*
 * Copyright 2019 Southwest Research Institute
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "moveit_reach_plugins/utils.h"

#include <geometric_shapes/mesh_operations.h>
#include <geometric_shapes/shape_operations.h>
#include <geometric_shapes/shapes.h>
#include <ros/console.h>
#include <eigen_conversions/eigen_msg.h>

const static double ARROW_SCALE_RATIO = 6.0;
const static double NEIGHBOR_MARKER_SCALE_RATIO = ARROW_SCALE_RATIO / 2.0;

namespace moveit_reach_plugins
{
namespace utils
{
moveit_msgs::CollisionObject createCollisionObject(const std::string& mesh_filename, const std::string& parent_link,
                                                   const std::string& object_name)
{
  // Create a CollisionObject message for the reach object
  moveit_msgs::CollisionObject obj;
  obj.header.frame_id = parent_link;
  obj.id = object_name;
  shapes::ShapeMsg shape_msg;
  shapes::Mesh* mesh = shapes::createMeshFromResource(mesh_filename);
  shapes::constructMsgFromShape(mesh, shape_msg);
  obj.meshes.push_back(boost::get<shape_msgs::Mesh>(shape_msg));
  obj.operation = obj.ADD;

  // Assign a default pose to the mesh
  geometry_msgs::Pose pose;
  pose.position.x = pose.position.y = pose.position.z = 0.0;
  pose.orientation.x = pose.orientation.y = pose.orientation.z = 0.0;
  pose.orientation.w = 1.0;
  obj.mesh_poses.push_back(pose);

  return obj;
}

visualization_msgs::Marker makeVisual(const reach_msgs::ReachRecord& r, const std::string& frame, const double scale,
                                      const std::string& ns, const boost::optional<std::vector<float>>& color)
{
  static int idx = 0;

  visualization_msgs::Marker marker;
  marker.header.frame_id = frame;
  marker.header.stamp = ros::Time::now();
  marker.ns = ns;
  marker.id = idx++;
  marker.type = visualization_msgs::Marker::ARROW;
  marker.action = visualization_msgs::Marker::ADD;

  Eigen::Isometry3d goal_eigen;
  tf::poseMsgToEigen(r.goal, goal_eigen);

  // Transform arrow such that arrow x-axis points along goal pose z-axis (Rviz convention)
  // convert msg parameter goal to Eigen matrix
  Eigen::AngleAxisd rot_flip_normal(M_PI, Eigen::Vector3d::UnitX());
  Eigen::AngleAxisd rot_x_to_z(-M_PI / 2, Eigen::Vector3d::UnitY());

  // Transform
  goal_eigen = goal_eigen * rot_flip_normal * rot_x_to_z;

  // Convert back to geometry_msgs pose
  geometry_msgs::Pose msg;
  tf::poseEigenToMsg(goal_eigen, msg);
  marker.pose = msg;

  marker.scale.x = scale;
  marker.scale.y = scale / ARROW_SCALE_RATIO;
  marker.scale.z = scale / ARROW_SCALE_RATIO;

  if (color)
  {
    std::vector<float> color_vec = *color;
    marker.color.r = color_vec[0];
    marker.color.g = color_vec[1];
    marker.color.b = color_vec[2];
    marker.color.a = color_vec[3];
  }
  else
  {
    marker.color.a = 1.0;  // Don't forget to set the alpha!

    if (r.reached)
    {
      marker.color.r = 0.0;
      marker.color.g = 0.0;
      marker.color.b = 1.0;
    }
    else
    {
      marker.color.r = 1.0;
      marker.color.g = 0.0;
      marker.color.b = 0.0;
    }
  }

  return marker;
}

visualization_msgs::InteractiveMarker makeInteractiveMarker(const reach_msgs::ReachRecord& r, const std::string& frame,
                                                            const double scale)
{
  visualization_msgs::InteractiveMarker m;
  m.header.frame_id = frame;
  m.name = r.id;

  // Control
  visualization_msgs::InteractiveMarkerControl control;
  control.interaction_mode = visualization_msgs::InteractiveMarkerControl::BUTTON;
  control.always_visible = true;

  // Visuals
  auto visual = makeVisual(r, frame, scale);
  control.markers.push_back(visual);
  m.controls.push_back(control);

  // Set the pose of the interactive marker to be the same as the visual marker
  m.pose = visual.pose;

  return m;
}

visualization_msgs::Marker makeMarker(const std::vector<geometry_msgs::Point>& pts, const std::string& frame,
                                      const double scale, const std::string& ns)
{
  visualization_msgs::Marker marker;
  marker.header.frame_id = frame;
  marker.header.stamp = ros::Time::now();
  marker.ns = ns;
  marker.type = visualization_msgs::Marker::POINTS;
  marker.action = visualization_msgs::Marker::ADD;

  marker.scale.x = marker.scale.y = marker.scale.z = scale / NEIGHBOR_MARKER_SCALE_RATIO;

  marker.color.a = 1.0;  // Don't forget to set the alpha!
  marker.color.r = 0;
  marker.color.g = 1.0;
  marker.color.b = 0;

  for (std::size_t i = 0; i < pts.size(); ++i)
  {
    marker.points.push_back(pts[i]);
  }

  return marker;
}

bool transcribeInputMap(const std::map<std::string, double>& input, const std::vector<std::string>& joint_names,
                        std::vector<double>& input_subset)
{
  if (joint_names.size() > input.size())
  {
    ROS_ERROR("Seed pose size was not at least as large as the number of joints in the planning group");
    return false;
  }

  // Pull the joints of the planning group out of the input map
  std::vector<double> tmp;
  tmp.reserve(joint_names.size());
  for (const std::string& name : joint_names)
  {
    const auto it = input.find(name);
    if (it == input.end())
    {
      ROS_ERROR_STREAM("Joint '" << name << "' in the planning group was not in the input map");
      return false;
    }
    else
    {
      tmp.push_back(it->second);
    }
  }

  input_subset = std::move(tmp);

  return true;
}

}  // namespace utils
}  // namespace moveit_reach_plugins
