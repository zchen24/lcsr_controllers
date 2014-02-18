

#include <iostream>
#include <algorithm>
#include <map>

#include <Eigen/Dense>

#include <rtt/internal/GlobalService.hpp>

#include <kdl/tree.hpp>

#include <kdl_parser/kdl_parser.hpp>

#include <rtt_rosclock/rtt_rosclock.h>
#include <rtt_rosparam/rosparam.h>
#include <rtt_rostopic/rostopic.h>

#include <kdl_urdf_tools/tools.h>
#include "joint_traj_generator_rml.h"

using namespace lcsr_controllers;

JointTrajGeneratorRML::JointTrajGeneratorRML(std::string const& name) :
  TaskContext(name)
  // Properties
  ,robot_description_("")
  ,root_link_("")
  ,tip_link_("")
  // Working variables
  ,n_dof_(0)
  ,kdl_tree_()
  ,kdl_chain_()
  // Trajectory state
  ,segments_()
  // Debugging
  ,ros_publish_throttle_(0.02)
{
  // Declare properties
  this->addProperty("robot_description",robot_description_).doc("The WAM URDF xml string.");
  this->addProperty("root_link",root_link_).doc("The root link for the controller.");
  this->addProperty("tip_link",tip_link_).doc("The tip link for the controller.");
  this->addProperty("max_velocities",max_velocities_).doc("Maximum velocities for traj generation.");
  this->addProperty("max_accelerations",max_accelerations_).doc("Maximum accelerations for traj generation.");
  this->addProperty("max_jerks",max_jerks_).doc("Maximum jerks for traj generation.");
  this->addProperty("position_tolerance",position_tolerance_).doc("Maximum position error.");
  this->addProperty("sampling_resolution",sampling_resolution_).doc("Sampling resolution in seconds.");
  
  // Configure data ports
  this->ports()->addPort("joint_position_in", joint_position_in_)
    .doc("Current joint position. (required)");
  this->ports()->addPort("joint_velocity_in", joint_velocity_in_)
    .doc("Current joint velocity. (required)");
  this->ports()->addPort("joint_position_cmd_in", joint_position_cmd_in_)
    .doc("Desired joint position, to be acquired as fast as possible.");
  this->ports()->addPort("joint_position_out", joint_position_out_)
    .doc("Interpolated joint position subject to velocity and acceleration limits.");
  this->ports()->addPort("joint_velocity_out", joint_velocity_out_)
    .doc("Interpolated joint velocity subject to velocity and acceleration limits.");

  // ROS ports
  this->ports()->addPort("joint_position_cmd_ros_in", joint_position_cmd_ros_in_);
  this->ports()->addPort("joint_traj_cmd_in", joint_traj_cmd_in_);
  this->ports()->addPort("joint_state_desired_out", joint_state_desired_out_);

  // Load Conman interface
  conman_hook_ = conman::Hook::GetHook(this);
  conman_hook_->setInputExclusivity("joint_position_in", conman::Exclusivity::EXCLUSIVE);
  conman_hook_->setInputExclusivity("joint_velocity_in", conman::Exclusivity::EXCLUSIVE);
  conman_hook_->setInputExclusivity("joint_position_cmd_in", conman::Exclusivity::EXCLUSIVE);
  conman_hook_->setInputExclusivity("joint_position_cmd_ros_in", conman::Exclusivity::EXCLUSIVE);
}

bool JointTrajGeneratorRML::configureHook()
{
  // ROS topics
  rtt_rostopic::ROSTopic rostopic;
  if(rostopic.ready()) {
    RTT::log(RTT::Info) << "ROS Topics ready..." <<RTT::endlog();
  } else {
    RTT::log(RTT::Error) << "ROS Topics not ready..." <<RTT::endlog();
    return false;
  }

  if(!joint_position_cmd_ros_in_.createStream(rostopic.connection("~" + this->getName() + "/joint_position_cmd"))
     || !joint_state_desired_out_.createStream(rostopic.connection("~" + this->getName() + "/joint_state_desired")))
  {
    RTT::log(RTT::Error) << "ROS Topics could not be streamed..." <<RTT::endlog();
    return false;
  }

  // ROS parameters
  boost::shared_ptr<rtt_rosparam::ROSParam> rosparam =
    this->getProvider<rtt_rosparam::ROSParam>("rosparam");
  rosparam->getAbsolute("robot_description");
  rosparam->getComponentPrivate("root_link");
  rosparam->getComponentPrivate("tip_link");

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
  joint_velocity_.resize(n_dof_);

  joint_position_cmd_.resize(n_dof_);
  joint_position_sample_.resize(n_dof_);
  joint_velocity_sample_.resize(n_dof_);

  position_tolerance_ = Eigen::VectorXd::Constant(n_dof_,0.0);
  max_velocities_ = Eigen::VectorXd::Constant(n_dof_,0.0);
  max_accelerations_ = Eigen::VectorXd::Constant(n_dof_,0.0);
  max_jerks_ = Eigen::VectorXd::Constant(n_dof_,0.0);

  rosparam->getComponentPrivate("position_tolerance");
  rosparam->getComponentPrivate("max_velocities");
  rosparam->getComponentPrivate("max_accelerations");
  rosparam->getComponentPrivate("max_jerks");
  rosparam->getComponentPrivate("sampling_resolution");

  // Create trajectory generator
  rml_.reset(new ReflexxesAPI(n_dof_, sampling_resolution_));
  rml_in_.reset(new RMLPositionInputParameters(n_dof_));
  rml_out_.reset(new RMLPositionOutputParameters(n_dof_));

  // Get individual joint properties from urdf and parameter server
  joint_names_.resize(n_dof_);

  // Generate joint map here

  for(size_t j = 0; j < n_dof_; j++)
  { 
    // Get RML parameters from RTT Properties
    rml_in_->MaxVelocityVector->VecData[j] =  max_velocities_[j];
    rml_in_->MaxAccelerationVector->VecData[j] = max_accelerations_[j];
    rml_in_->MaxJerkVector->VecData[j] = max_jerks_[j];

    // Enable this joint
    rml_in_->SelectionVector->VecData[j] = true;
  }

  // Check if the reflexxes config is valud
  if(rml_in_->CheckForValidity()) {
    RTT::log(RTT::Info) << ("RML INPUT Configuration Valid.") << RTT::endlog();

    this->rml_debug(RTT::Debug);
  } else {
    RTT::log(RTT::Error) << ("RML INPUT Configuration Invalid!") << RTT::endlog();
    RTT::log(RTT::Error) << ("NOTE: MaxVelocityVector, MaxAccelerationVector, and MaxJerkVector must all be non-zero for a solution to exist.") << RTT::endlog();
    this->rml_debug(RTT::Error);
    return false;
  }

  return true;
}

bool JointTrajGeneratorRML::startHook()
{
  return true;
}

bool JointTrajGeneratorRML::UpdateTrajectory(
    JointTrajGeneratorRML::TrajSegments &current_segments,
    const JointTrajGeneratorRML::TrajSegments &new_segments)
{
  // Make sure there are new segments
  if(new_segments.begin() == new_segments.end()) {
    return false;
  }

  // Determine where the segments should begin to be inserted in the current trajectory via binary search
  std::pair<TrajSegments::iterator, TrajSegments::iterator> insertion_range = 
    std::equal_range(
        current_segments.begin(),
        current_segments.end(), 
        new_segments.front(),
        TrajSegment::StartTimeCompare);

  // Remove all segments with start times after the start time of this trajectory
  current_segments.erase(insertion_range.first, current_segments.end());

  // Add the new segments to the end of the trajectory
  current_segments.insert(
      current_segments.end(), 
      new_segments.begin(), 
      new_segments.end());

  return true;
}

bool JointTrajGeneratorRML::TrajectoryMsgToSegments(
    const trajectory_msgs::JointTrajectory &msg,
    const size_t n_dof,
    const ros::Time new_traj_start_time,
    TrajSegments &segments)
{
  // Clear the output segment list
  segments.clear();

  // TODO: Permute the joint names properly
  
  // Make sure the traj isn't empty
  if(msg.points.size() == 0) {
    return false;
  }

  // It's assumed that the header timestamp is when this new
  // trajectory should start being executed. This also means that if there
  // is a queued trajectory which contains this start time that that
  // trajectory should not be disturbed until that start time. We achieve
  // this by checking the next segment point's start time. If the start time is
  // 0.0 then it will not be activated until the end-time of the active segment
  // goal. If the start time is non-zero, then it will become active once
  // the start time has passed.

  // By default, start the trajectory now (header stamp is zero)
  //ros::Time new_traj_start_time = rtt_now;

  // Convert the ROS joint trajectory to a set of Eigen TrajSegment structures
  for(std::vector<trajectory_msgs::JointTrajectoryPoint>::const_iterator it = msg.points.begin();
      it != msg.points.end();
      ++it) 
  {
    ros::Time new_segment_start_time;

    // Compute the start time for the new segment. If this is the first
    // point, then it's the trajectory start time. Otherwise, it's the end
    // time of the preceeding point.
    if(it == msg.points.begin()) {
      new_segment_start_time = new_traj_start_time;
    } else {
      new_segment_start_time = segments.back().goal_time;
    }

    // Create and add the new segment 
    segments.push_back(
        TrajSegment(
            n_dof, 
            new_segment_start_time, 
            new_traj_start_time + it->time_from_start));

    // Copy in the data
    TrajSegment &new_segment = segments.back();
    if(it->positions.size() == n_dof) std::copy(it->positions.begin(), it->positions.end(), new_segment.goal_positions.data());
    if(it->velocities.size() == n_dof) std::copy(it->velocities.begin(), it->velocities.end(), new_segment.goal_velocities.data());
    if(it->accelerations.size() == n_dof) std::copy(it->accelerations.begin(), it->accelerations.end(), new_segment.goal_accelerations.data());
  }

  return true;
}

void JointTrajGeneratorRML::updateHook()
{
  // Get the current and the time since the last update
  const RTT::Seconds 
    time = conman_hook_->getTime(), 
    period = conman_hook_->getPeriod();

  ros::Time rtt_now = rtt_rosclock::rtt_now();

  // Read in the current joint positions & velocities
  RTT::FlowStatus position_status = joint_position_in_.readNewest(joint_position_);
  RTT::FlowStatus velocity_status = joint_velocity_in_.readNewest(joint_velocity_);

  // If we don't get a position or velocity update, we don't write any new data to the ports
  if(position_status == RTT::OldData || velocity_status == RTT::OldData) 
  {
    return;
  }

  // Read in any newly commanded joint positions 
  RTT::FlowStatus point_status = joint_position_cmd_in_.readNewest( joint_position_cmd_ );
  RTT::FlowStatus traj_status = joint_traj_cmd_in_.readNewest( joint_traj_cmd_ );

  // Do nothing and generate no output if there's no command
  if(point_status == RTT::NoData && traj_status == RTT::NoData)
  {
    return;
  }

  // Check if there's a new desired point
  if(point_status == RTT::NewData) 
  {
    // Check the size of the jointspace command
    if(joint_position_cmd_.size() == n_dof_) {
      // Handle a position given as an Eigen vector
      TrajSegment segment(n_dof_);
      segment.start_time = rtt_now;
      segment.goal_time = rtt_now;

      segment.goal_positions = joint_position_;
      segments_.clear();
      segments_.push_back(segment);

      recompute_trajectory_ = true;
    } else {
      //TODO: Report warning
    }
  } 
  // Check if there's a new desired trajectory
  else if(traj_status == RTT::NewData) 
  {
    // Create a new list of segments to be spliced in
    TrajSegments new_segments;
    // By default, set the start time to now
    ros::Time new_traj_start_time = rtt_now;

    // If the header stamp is non-zero, then determine which points we should pursue
    if(!joint_traj_cmd_.header.stamp.isZero()) {
      // Offset the NTP-corrected time to get the RTT-time
      // Correct the timestamp so that its relative to the realtime clock
      // TODO: make it so this can be disabled or make two different ports
      new_traj_start_time = joint_traj_cmd_.header.stamp - ros::Duration(rtt_rosclock::host_rt_offset_from_rtt());
    }

    // Convert the trajectory message to a list of segments for splicing
    TrajectoryMsgToSegments(
        joint_traj_cmd_, 
        n_dof_, 
        new_traj_start_time, 
        new_segments);
    
    // Update the trajectory
    UpdateTrajectory(segments_, new_segments);
  }

  /** After this point, only work in TrajSegment structures **/

  // Find segments which should be removed
  // - segments whose goal times are in the past
  // - segments before segments whose start times are now or in the past
  TrajSegments::iterator erase_it = segments_.begin();
  for(TrajSegments::iterator it = segments_.begin(); it != segments_.end(); ++it) 
  {
    // Peek at the next segment
    TrajSegments::iterator next = it;  ++next;
    
    // Mark this segment for removal if it's expired or if we need to start processing the next one
    if(it->goal_time <= rtt_now || (next != segments_.end() && next->start_time <= rtt_now)) {
      erase_it = it;
      recompute_trajectory_ = true;
    }
  }

  // Check if there's a new active segment
  if(recompute_trajectory_) {
    // Remove the segments whose end times are in the past
    segments_.erase(segments_.begin(), erase_it);
  }

  // Check if there are any more segments to process and if we should start processing them
  if(segments_.size() > 0 && segments_.front().start_time <= rtt_now) 
  {
    // Get a reference to the active segment
    TrajSegment &active_segment = segments_.front();

    // Determine if any of the joint tolerances have been violated (this means we need to recompute the traj)
    for(int i=0; i<n_dof_; i++) 
    {
      double tracking_error = std::abs(rml_out_->GetNewPositionVectorElement(i) - joint_position_[i]);
      if(tracking_error > position_tolerance_[i]) {
        recompute_trajectory_ = true;
      }
    }

    // Initialize RML result
    int rml_result = 0;

    // Compute RML traj after the start time and if there are still points in the queue
    if(recompute_trajectory_) 
    {
      // Compute the trajectory
      RTT::log(RTT::Debug) << ("RML Recomputing trajectory...") << RTT::endlog(); 

      // Update RML input parameters
      for(size_t i=0; i<n_dof_; i++) {
        rml_in_->SetCurrentPositionVectorElement(joint_position_(i), i);
        rml_in_->SetCurrentVelocityVectorElement(joint_velocity_(i), i);
        rml_in_->SetCurrentAccelerationVectorElement(0.0, i);

        rml_in_->SetTargetPositionVectorElement(active_segment.goal_positions(i), i);
        rml_in_->SetTargetVelocityVectorElement(active_segment.goal_velocities(i), i);

        rml_in_->SetSelectionVectorElement(true,i);
      }

      // Set desired execution time for this trajectory (definitely > 0)
      rml_in_->SetMinimumSynchronizationTime(
          std::max(0.0,(active_segment.goal_time - active_segment.start_time).toSec()));

      RTT::log(RTT::Debug) << "RML IN: time: "<<rml_in_->GetMinimumSynchronizationTime() << RTT::endlog();

      // Hold fixed at final point once trajectory is complete
      rml_flags_.BehaviorAfterFinalStateOfMotionIsReached = RMLPositionFlags::RECOMPUTE_TRAJECTORY;
      rml_flags_.SynchronizationBehavior = RMLPositionFlags::ONLY_TIME_SYNCHRONIZATION;

      // Compute trajectory
      rml_result = rml_->RMLPosition(
          *rml_in_.get(), 
          rml_out_.get(), 
          rml_flags_);

      // Disable recompute flag
      recompute_trajectory_ = false;
    } 
    else 
    {
      // Sample the already computed trajectory
      rml_result = rml_->RMLPositionAtAGivenSampleTime(
          (rtt_now - active_segment.start_time).toSec(),
          rml_out_.get());
    }

    // Only set a non-zero effort command if the 
    switch(rml_result) 
    {
      case ReflexxesAPI::RML_WORKING:
        // S'all good.
        break;
      case ReflexxesAPI::RML_FINAL_STATE_REACHED:
        // Remove the active segment from the trajectory
        segments_.pop_front();
        break;
      default:
        RTT::log(RTT::Error) << "Reflexxes error code: "<<rml_result<<". Not writing a desired position." << RTT::endlog();
        this->error();
        return;
        break;
    };

    // Get the new sampled reference
    for(size_t i=0; i<n_dof_; i++) {
      joint_position_sample_(i) = rml_out_->GetNewPositionVectorElement(i);
      joint_velocity_sample_(i) = rml_out_->GetNewVelocityVectorElement(i);
    }
  }

  // Send instantaneous joint position and velocity commands
  joint_position_out_.write(joint_position_sample_);
  joint_velocity_out_.write(joint_velocity_sample_);
  
  // Publish debug traj to ros
  if(ros_publish_throttle_.ready()) 
  {
    joint_state_desired_.header.stamp = rtt_rosclock::host_rt_now();
    joint_state_desired_.position.resize(n_dof_);
    joint_state_desired_.velocity.resize(n_dof_);
    std::copy(joint_position_sample_.data(), joint_position_sample_.data() + n_dof_, joint_state_desired_.position.begin());
    std::copy(joint_velocity_sample_.data(), joint_velocity_sample_.data() + n_dof_, joint_state_desired_.velocity.begin());
    joint_state_desired_out_.write(joint_state_desired_);
  }
}

void JointTrajGeneratorRML::stopHook()
{
  // Clear data buffers (this will make them return OldData if nothing new is written to them)
  joint_position_in_.clear();
  joint_velocity_in_.clear();
}

void JointTrajGeneratorRML::cleanupHook()
{
}

template<class T>
std::ostream& operator<< (std::ostream& stream, const RMLVector<T>& rml_vec) {
  stream<<"[ ";
  for(int i=0; i<rml_vec.VectorDimension; i++) { stream<<(rml_vec.VecData[i])<<", "; }
  stream<<"]";
  return stream;
}


void JointTrajGeneratorRML::rml_debug(const RTT::LoggerLevel level) {
  RTT::log(level) << "RML INPUT: "<< RTT::endlog();
  RTT::log(level) << " - NumberOfDOFs:               "<<rml_in_->NumberOfDOFs << RTT::endlog();
  RTT::log(level) << " - MinimumSynchronizationTime: "<<rml_in_->MinimumSynchronizationTime << RTT::endlog();

  RTT::log(level) << " - SelectionVector: "<<*(rml_in_->SelectionVector) << RTT::endlog();

  RTT::log(level) << " - CurrentPositionVector:     "<<*(rml_in_->CurrentPositionVector) << RTT::endlog();
  RTT::log(level) << " - CurrentVelocityVector:     "<<*(rml_in_->CurrentVelocityVector) << RTT::endlog();
  RTT::log(level) << " - CurrentAccelerationVector: "<<*(rml_in_->CurrentAccelerationVector) << RTT::endlog();

  RTT::log(level) << " - MaxVelocityVector:     "<<*(rml_in_->MaxVelocityVector) << RTT::endlog();
  RTT::log(level) << " - MaxAccelerationVector: "<<*(rml_in_->MaxAccelerationVector) << RTT::endlog();
  RTT::log(level) << " - MaxJerkVector:         "<<*(rml_in_->MaxJerkVector) << RTT::endlog();

  RTT::log(level) << " - TargetPositionVector:            "<<*(rml_in_->TargetPositionVector) << RTT::endlog();
  RTT::log(level) << " - TargetVelocityVector:            "<<*(rml_in_->TargetVelocityVector) << RTT::endlog();

  RTT::log(level) << " - AlternativeTargetVelocityVector: "<<*(rml_in_->AlternativeTargetVelocityVector) << RTT::endlog();
}
