
#include <iostream>
#include <map>

#include <Eigen/Dense>

#include <kdl/tree.hpp>

#include <kdl_parser/kdl_parser.hpp>

#include <rtt_rosparam/rosparam.h>

#include <kdl_urdf_tools/tools.h>
#include "joint_traj_generator_kdl.h"

using namespace lcsr_controllers;

JointTrajGeneratorKDL::JointTrajGeneratorKDL(std::string const& name) :
  TaskContext(name)
  // Properties
  ,robot_description_("")
  ,root_link_("")
  ,tip_link_("")
  // Working variables
  ,n_dof_(0)
  ,kdl_tree_()
  ,kdl_chain_()
{
  // Declare properties
  this->addProperty("robot_description",robot_description_).doc("The WAM URDF xml string.");
  this->addProperty("root_link",root_link_).doc("The root link for the controller.");
  this->addProperty("tip_link",tip_link_).doc("The tip link for the controller.");
  this->addProperty("trap_max_vels",trap_max_vels_).doc("Maximum velocities for trap generation.");
  this->addProperty("trap_max_accs",trap_max_accs_).doc("Maximum acceperations for trap generation.");
  this->addProperty("velocity_smoothing_factor",velocity_smoothing_factor_).doc("Exponential smoothing factor to use when estimating veolocity from finite differences.");
  
  // ROS parameters
  boost::shared_ptr<rtt_rosparam::ROSParam> rosparam =
    this->getProvider<rtt_rosparam::ROSParam>("rosparam");
  rosparam->getAbsolute("robot_description");
  rosparam->getComponentPrivate("root_link");
  rosparam->getComponentPrivate("tip_link");
  rosparam->getComponentPrivate("trap_max_vels");
  rosparam->getComponentPrivate("trap_max_accs");
  rosparam->getComponentPrivate("velocity_smoothing_factor");

  // Configure data ports
  this->ports()->addPort("joint_position_in", joint_position_in_);
  this->ports()->addPort("joint_velocity_in", joint_velocity_in_);
  this->ports()->addPort("joint_position_cmd_in", joint_position_cmd_in_);
  this->ports()->addPort("joint_position_out", joint_position_out_)
    .doc("Output port: nx1 vector of joint positions. (n joints)");
  this->ports()->addPort("joint_velocity_out", joint_velocity_out_)
    .doc("Output port: nx1 vector of joint velocities. (n joints)");

  // ROS ports

  // Load Conman interface
  conman_hook_ = conman::Hook::GetHook(this);
  conman_hook_->setInputExclusivity("joint_position_in", conman::Exclusivity::EXCLUSIVE);
  conman_hook_->setInputExclusivity("joint_velocity_in", conman::Exclusivity::EXCLUSIVE);
  conman_hook_->setInputExclusivity("joint_position_cmd_in", conman::Exclusivity::EXCLUSIVE);
}

bool JointTrajGeneratorKDL::configureHook()
{
  // Initialize kinematics (KDL tree, KDL chain, and #DOF)
  urdf::Model urdf_model;
  if(!kdl_urdf_tools::initialize_kinematics_from_urdf(
        robot_description_, root_link_, tip_link_,
        n_dof_, kdl_chain_, kdl_tree_, urdf_model))
  {
    RTT::log(RTT::Error) << "Could not initialize robot kinematics!" << RTT::endlog();
    return false;
  }

  // Resize IO vectors
  joint_position_.resize(n_dof_);
  joint_position_last_.resize(n_dof_);
  joint_position_cmd_.resize(n_dof_);
  joint_position_sample_.resize(n_dof_);
  joint_velocity_.resize(n_dof_);
  joint_velocity_raw_.resize(n_dof_);
  joint_velocity_sample_.resize(n_dof_);

  trajectory_start_times_.resize(n_dof_);
  trajectory_end_times_.resize(n_dof_);
  
  // Create trajectory generators
  trajectories_.resize(n_dof_);
  for(unsigned i=0; i<n_dof_; i++){
    trajectory_start_times_[i] = 0.0;
    trajectory_end_times_[i] = 0.0;
    trajectories_[i] = KDL::VelocityProfile_Trap(trap_max_vels_[i], trap_max_accs_[i]);
  }

  // Prepare ports for realtime processing
  joint_position_out_.setDataSample(joint_position_sample_);
  joint_velocity_out_.setDataSample(joint_velocity_sample_);

  return true;
}

bool JointTrajGeneratorKDL::startHook()
{
  // Reset the last position flag
  has_last_position_data_ = false;

  return true;
}

void JointTrajGeneratorKDL::updateHook()
{
  // Get the current and the time since the last update
  const RTT::Seconds 
    time = conman_hook_->getTime(), 
    period = conman_hook_->getPeriod();

  // Read in the current joint positions & velocities
  bool new_position_data = (joint_position_in_.readNewest(joint_position_) == RTT::NewData);
  bool new_velocity_data = (joint_velocity_in_.readNewest(joint_velocity_raw_) == RTT::NewData);

  // If we don't get any position update, we don't write any new data to the ports
  if(!new_position_data) {
    return;
  }

  // Check the minimum requirements to compute the control command
  if(new_velocity_data || has_last_position_data_) {
    // Trust a supplied velocity, or compute it from an exponentially-smothed finite difference
    if(new_velocity_data) {
      // Trust the velocity input
      joint_velocity_ = joint_velocity_raw_;
    } else {
      // Estimate the joint velocity if we don't get a joint velocity estimate
      joint_velocity_ = 
        (velocity_smoothing_factor_*(joint_position_ - joint_position_last_)/period)
        + (1-velocity_smoothing_factor_)*(joint_velocity_);
    }

    // Read in any newly commanded joint positions 
    if(joint_position_cmd_in_.readNewest( joint_position_cmd_ ) == RTT::NoData) {
      // Do nothing if we don't have any desired positions
      return;
    } else {
      for(unsigned i=0; i<n_dof_; i++) {
        // Check to make sure the last trajectory has completed in this degree-of-freedom
        if(time > trajectory_end_times_[i]) {
          // Set the 1-dof trajectory profile
          // This will compute a trajectory subject to the velocity and
          // acceleration limits, but assume it's starting with zero velocity.
          trajectories_[i].SetProfile(joint_position_[i], joint_position_cmd_[i]);
          trajectory_start_times_[i] = time;
          trajectory_end_times_[i] = trajectory_start_times_[i] + trajectories_[i].Duration();
        }
      }
    }

    // Sample the trajectory
    for(unsigned i=0; i<n_dof_; i++) {
      joint_position_sample_[i] = trajectories_[i].Pos(time - trajectory_start_times_[i]);
      joint_velocity_sample_[i] = trajectories_[i].Vel(time - trajectory_start_times_[i]);
    }

    // Send instantaneous joint position and velocity commands
    joint_position_out_.write(joint_position_sample_);
    joint_velocity_out_.write(joint_velocity_sample_);
  }

  // Save the last joint position
  joint_position_last_ = joint_position_;
  has_last_position_data_ = true;
}

void JointTrajGeneratorKDL::stopHook()
{
}

void JointTrajGeneratorKDL::cleanupHook()
{
}