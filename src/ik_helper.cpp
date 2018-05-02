#include <cmath>
#include <eigen_conversions/eigen_msg.h>
#include <moveit/robot_state/conversions.h>
#include <robot_reach_study/ik_helper.h>
#include <robot_reach_study/reach_database.h>
#include <robot_reach_study/utils.h>
#include <ros/ros.h>

const static std::string ROBOT_DESCRIPTION_TOPIC = "robot_description";
const static std::vector<std::string> MESH_FILE_EXTENSIONS = {".stl", ".ply", ".obj"};

namespace
{
  double getManipulability(const moveit::core::RobotState& state,
                           const moveit::core::JointModelGroup* jmg)
  {
    // Calculate manipulability of kinematic chain of input robot state
    // Create new robot state to avoid dirty link transforms
    moveit::core::RobotState temp_state(state);

    // Get the Jacobian matrix
    Eigen::MatrixXd jacobian = temp_state.getJacobian(jmg);

    // Calculate manipulability by multiplying Jacobian matrix singular values together
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(jacobian);
    Eigen::MatrixXd singular_values = svd.singularValues();
    double m = 1.0;
    for(unsigned int i = 0; i < singular_values.rows(); ++i)
    {
      m *= singular_values(i, 0);
    }
    return m;
  }

  double getJointPenalty(const moveit::core::RobotState& state,
                         const moveit::core::JointModelGroup* jmg,
                         std::vector<std::vector<double>>& joint_limits)
  {
    std::vector<double> max, min, current;
    min = joint_limits[0];
    max = joint_limits[1];

    // Get current joint values in jmg chain
    moveit::core::RobotState temp_state(state);
    temp_state.copyJointGroupPositions(jmg, current);

    double penalty = 1.0;
    for(std::size_t i = 0; i < max.size(); ++i)
    {
      double range = max[i] - min[i];
      penalty *= ((current[i] - min[i])*(max[i] - current[i])) / pow(range, 2);
    }
    return std::max(0.0, 1.0 - exp(-1.0 * penalty));
  }
}

robot_reach_study::IkHelper::IkHelper(const std::string kin_group_name,
                                      const std::string manip_group_name)
{
  model_loader_ = robot_model_loader::RobotModelLoaderPtr(new robot_model_loader::RobotModelLoader(ROBOT_DESCRIPTION_TOPIC));
  model_ = model_loader_->getModel();
  scene_ptr_ = planning_scene::PlanningScenePtr(new planning_scene::PlanningScene(model_));

  kin_jmgroup_ = model_->getJointModelGroup(kin_group_name);
  manip_jmgroup_ = model_->getJointModelGroup(manip_group_name);
  kin_joint_limits_ = this->getJointLimits(kin_jmgroup_);
  manip_joint_limits_ = this->getJointLimits(manip_jmgroup_);

  constraint_ = boost::bind(&IkHelper::isIKSolutionValid, this, _1, _2, _3);
}

boost::optional<double> robot_reach_study::IkHelper::solveIKFromSeed(const geometry_msgs::Pose& tgt_pose,
                                                                     moveit::core::RobotState& seed_state,
                                                                     moveit::core::RobotState& goal_state)
{
  goal_state = seed_state;
  if(goal_state.setFromIK(kin_jmgroup_, tgt_pose, sol_attempts_, sol_timeout_, constraint_))
  {
    const double manip = getManipulability(goal_state, manip_jmgroup_);
    const double joint_penalty = getJointPenalty(goal_state, manip_jmgroup_, manip_joint_limits_);
    const double dist = scene_ptr_->distanceToCollision(goal_state, scene_ptr_->getAllowedCollisionMatrix());
    const double dist_penalty = std::pow((dist / dist_threshold_), 2);

    // Set score based on desired cost function
    boost::optional<double> score;
    switch(cost_function_)
    {
    case this->CostFunction::M:
      score = manip;
      break;
    case this->CostFunction::M_JP:
      score = manip * joint_penalty;
      break;
    case this->CostFunction::M_JP_DP:
      score = manip * joint_penalty * dist_penalty;
      break;
    case this->CostFunction::M_DP:
      score = manip * dist_penalty;
      break;
    case this->CostFunction::M_DP_DT:
      if(dist > dist_threshold_)
      {
        score = manip * dist_penalty;
      }
      break;
    case this->CostFunction::M_JP_DP_DT:
      if(dist > dist_threshold_)
      {
        score = manip * joint_penalty * dist_penalty;
      }
      break;
    }

    return {score};
  }

  return {};
}

std::vector<std::string> robot_reach_study::IkHelper::reachNeighborsDirect(std::shared_ptr<robot_reach_study::Database>& db,
                                                                           const robot_reach_study::ReachRecord& rec)
{
  // Initialize return array of string IDs of msgs that have been updated
  std::vector<std::string> msg_ids;

  const auto current_pose = rec.goal;
  const float x = current_pose.position.x;
  const float y = current_pose.position.y;
  const float z = current_pose.position.z;

  // Create vectors for storing poses and reach record messages that lie within radius of current point
  std::vector<geometry_msgs::Pose> pose_arr;
  std::vector<robot_reach_study::ReachRecord> reach_records;

  // Iterate through all points in database to find those that lie within radius of current point
  for(auto it = db->begin(); it != db->end(); ++it)
  {
    auto& entry = *it;
    const robot_reach_study::ReachRecord msg_object = entry.second;
    const geometry_msgs::Pose it_pose (msg_object.goal);

    float xp = it_pose.position.x;
    float yp = it_pose.position.y;
    float zp = it_pose.position.z;
    float d2 = pow((xp-x), 2) + pow((yp-y), 2) + pow((zp-z), 2);

    if(d2 < pow(neighbor_radius_, 2) && d2 != 0.0)
    {
      reach_records.push_back(entry.second);
      pose_arr.push_back(msg_object.goal);
    }
  }

  // Solve IK for points that lie within sphere
  if(pose_arr.size() > 0)
  {
    const auto& goal_state_msg = rec.goal_state;
    moveit::core::RobotState init_goal_state(model_);
    moveit::core::robotStateMsgToRobotState(goal_state_msg, init_goal_state);

    for(std::size_t i = 0; i < pose_arr.size(); ++i)
    {
      // Initialize new target pose and new empty robot goal state
      geometry_msgs::Pose& tgt_pose = pose_arr[i];
      moveit::core::RobotState new_goal_state(scene_ptr_->getCurrentState());

      // Use current point's IK solution as seed
      boost::optional<double> score = solveIKFromSeed(tgt_pose, init_goal_state, new_goal_state);
      if(score)
      {
        // Change database if currently solved point didn't have solution before
        // or if its current manipulability is better than that saved in the databas
        robot_reach_study::ReachRecord msg = reach_records[i];

        if(!msg.reached || (*score > msg.score))
        {
          // Overwrite Reach Record msg parameters with new results
          msg.reached = true;
          moveit::core::robotStateToRobotStateMsg(init_goal_state, msg.seed_state);
          moveit::core::robotStateToRobotStateMsg(new_goal_state, msg.goal_state);
          msg.score = *score;
          db->put(msg);
        }

        // Populate return array of changed points
        msg_ids.push_back(msg.id);
      }
    }
  }
  return msg_ids;
}

void robot_reach_study::IkHelper::reachNeighborsRecursive(std::shared_ptr<robot_reach_study::Database>& db,
                                                          const robot_reach_study::ReachRecord& rec,
                                                          std::vector<std::string>& reached_pts,
                                                          double& joint_distance)
{
  // Add the current point to the output list of msg IDs
  reached_pts.push_back(rec.id);

  // Get data out of the current message
  const float x = rec.goal.position.x;
  const float y = rec.goal.position.y;
  const float z = rec.goal.position.z;

  // Create vectors for storing reach record messages that lie within radius of current point
  std::vector<robot_reach_study::ReachRecord> neighbors;

  // Iterate through all points in database to find those that lie within radius of current point
  for(auto it = db->begin(); it != db->end(); ++it)
  {
    auto& entry = *it;
    const robot_reach_study::ReachRecord tmp_rec = entry.second;

    float xp = tmp_rec.goal.position.x;
    float yp = tmp_rec.goal.position.y;
    float zp = tmp_rec.goal.position.z;
    float d2 = pow((xp-x), 2) + pow((yp-y), 2) + pow((zp-z), 2);

    if(d2 < pow(neighbor_radius_, 2) && d2 != 0.0)
    {
      // Save reach record for all points within the sphere
      neighbors.push_back(tmp_rec);
    }
  }

  // Solve IK for points that lie within sphere
  if(neighbors.size() > 0)
  {
    // Create new empty seed based off where the robot is currently
    moveit::core::RobotState current_state (scene_ptr_->getCurrentState());
    moveit::core::robotStateMsgToRobotState(rec.goal_state, current_state);

    for(std::size_t i = 0; i < neighbors.size(); ++i)
    {
      // Check if the current potential point has been solved previously in the recursion chain
      if(std::find(reached_pts.begin(), reached_pts.end(), neighbors[i].id) == std::end(reached_pts))
      {
        // Initialize new target pose and new empty robot goal state
        moveit::core::RobotState new_goal_state(scene_ptr_->getCurrentState());

        // Use current point's IK solution as seed
        boost::optional<double> score = solveIKFromSeed(neighbors[i].goal, current_state, new_goal_state);
        if(score)
        {
          // Calculate the joint distance between the seed and new goal states
          joint_distance += new_goal_state.distance(current_state);

          // Store information in new reach record object
          robot_reach_study::ReachRecord new_rec;
          new_rec.id = neighbors[i].id;
          new_rec.goal = neighbors[i].goal;
          moveit::core::robotStateToRobotStateMsg(new_goal_state, new_rec.goal_state);
          moveit::core::robotStateToRobotStateMsg(current_state, new_rec.seed_state);
          new_rec.reached = true;
          new_rec.score = *score;

          // Recursively enter this function at the new neighboring location
          this->reachNeighborsRecursive(db, new_rec, reached_pts, joint_distance);
        }
      }
    }
  }
}

bool robot_reach_study::IkHelper::addCollisionObjectToScene(const std::string& mesh_filename,
                                                            const std::string& parent_link,
                                                            const std::vector<std::string>& touch_links)
{
  // Get name of mesh for Collision object ID
  std::string object_name = mesh_filename;

  size_t ind = 0;
  size_t ext_pos = std::string::npos;
  for(size_t i = 0; i < MESH_FILE_EXTENSIONS.size(); ++i)
  {
    ext_pos = object_name.find(MESH_FILE_EXTENSIONS[i]);
    if(ext_pos != std::string::npos)
    {
      ind = i;
      break;
    }
  }
  if(ext_pos == std::string::npos)
  {
    ROS_ERROR("Mesh file name does not contain an acceptable extension");
    return false;
  }

  object_name.erase(ext_pos, MESH_FILE_EXTENSIONS[ind].length());
  const size_t prefix_pos = object_name.find_last_of("/", 0);
  object_name.erase(0, prefix_pos + 1);

  // Create a CollisionObject message for the reach object
  moveit_msgs::CollisionObject obj = robot_reach_study::utils::createCollisionObject(mesh_filename, parent_link, touch_links, object_name);
  if(scene_ptr_->processCollisionObjectMsg(obj))
  {
    // Update the allowed collision matrix
    scene_ptr_->getAllowedCollisionMatrixNonConst().setEntry(object_name, touch_links, true);
    return true;
  }

  return false;
}

bool robot_reach_study::IkHelper::isIKSolutionValid(moveit::core::RobotState* state,
                                                     const moveit::core::JointModelGroup* jmg,
                                                     const double* ik_solution) const
{
  state->setJointGroupPositions(jmg, ik_solution);
  state->update();
  return !scene_ptr_->isStateColliding(*state, jmg->getName(), false);
}

std::vector<std::vector<double>> robot_reach_study::IkHelper::getJointLimits(const moveit::core::JointModelGroup* jmg)
{
  std::vector<double> max, min;
  // Get joint limits
  const auto limits_vec = jmg->getActiveJointModelsBounds();
  for(std::size_t i = 0; i < limits_vec.size(); ++i)
  {
    const auto& bounds_vec = *limits_vec[i];
    if(bounds_vec.size() > 1)
    {
      ROS_FATAL("Joint has more than one DOF; can't pull joint limits correctly");
    }
    max.push_back(bounds_vec[0].max_position_);
    min.push_back(bounds_vec[0].min_position_);
  }
  std::vector<std::vector<double>> joint_limits;
  joint_limits.push_back(min);
  joint_limits.push_back(max);
  return joint_limits;
}