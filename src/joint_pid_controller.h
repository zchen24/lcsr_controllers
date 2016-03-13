#ifndef __LCSR_CONTROLLERS_JOINT_PID_CONTROLLER_H
#define __LCSR_CONTROLLERS_JOINT_PID_CONTROLLER_H

#include <iostream>

#include <boost/scoped_ptr.hpp>

#include <rtt/RTT.hpp>
#include <rtt/Port.hpp>

#include <kdl/jntarrayvel.hpp>
#include <kdl/tree.hpp>
#include <kdl/chain.hpp>
#include <kdl/chaindynparam.hpp>

#include <conman/hook.h>

#include <sensor_msgs/JointState.h>

#include <rtt_ros_tools/tools.h>

#include "friction/joint_friction_compensator_hss.h"

namespace lcsr_controllers {
  class JointPIDController : public RTT::TaskContext
  {
    // RTT Properties
    std::string robot_description_;
    std::string robot_description_param_;
    std::string root_link_;
    std::string tip_link_;
    size_t tolerance_violations_;
    Eigen::VectorXd
      position_tolerance_,
      velocity_tolerance_,
      p_gains_,
      i_gains_,
      d_gains_,
      i_clamps_;
    bool compensate_friction_;
    double static_eps_;
    bool verbose_;
    bool is_first_start_;
    bool is_ros_mode_;
    bool is_antiwindup_;
    int debug_ver_;

    Eigen::VectorXd t_gains_;
    Eigen::VectorXd torque_limits_;

    // RTT Ports
    RTT::InputPort<Eigen::VectorXd> joint_position_in_;
    RTT::InputPort<Eigen::VectorXd> joint_velocity_in_;
    RTT::InputPort<Eigen::VectorXd> joint_position_cmd_in_;
    RTT::InputPort<Eigen::VectorXd> joint_velocity_cmd_in_;
    RTT::InputPort<Eigen::VectorXd> joint_acceleration_cmd_in_;
    RTT::OutputPort<Eigen::VectorXd> joint_effort_out_;

    // RTT Ports ROS
    RTT::InputPort<sensor_msgs::JointState> joint_position_cmd_ros_in_;
    RTT::OutputPort<sensor_msgs::JointState> joint_state_desired_out_;

  public:
    JointPIDController(std::string const& name);
    virtual bool configureHook();
    virtual bool startHook();
    virtual void updateHook();
    virtual void stopHook();
    virtual void cleanupHook();

  private:

    // Robot model
    unsigned int n_dof_;
    KDL::Tree kdl_tree_;
    KDL::Chain kdl_chain_;

    // State
    Eigen::VectorXd 
      joint_p_error_,
      joint_p_error_last_,
      joint_d_error_,
      joint_i_error_,
      joint_position_,
      joint_position_last_,
      joint_position_cmd_,
      joint_velocity_,
      joint_velocity_raw_,
      joint_velocity_cmd_,
      joint_acceleration_cmd_,
      joint_effort_,
      static_effort_,
      static_deadband_;

    Eigen::VectorXd joint_effort_cmd_;
    Eigen::VectorXd joint_effort_cmd_clamped_;
    Eigen::VectorXd joint_i_error_antiwindup_;

    bool has_last_position_data_;

    // Conman interface
    boost::shared_ptr<conman::Hook> conman_hook_;

    sensor_msgs::JointState joint_state_cmd_;
    sensor_msgs::JointState joint_state_desired_;
    rtt_ros_tools::PeriodicThrottle ros_publish_throttle_;

    boost::scoped_ptr<KDL::ChainDynParam> chain_dynamics_;
    KDL::JntSpaceInertiaMatrix joint_inertia_;
  };
}


#endif // ifndef __LCSR_CONTROLLERS_PID_CONTROLLER_H
