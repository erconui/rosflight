/*
 * Copyright (c) 2017 Daniel Koch, James Jackson and Gary Ellingson, BYU MAGICC Lab.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#pragma GCC diagnostic ignored "-Wwrite-strings"

#include <sstream>
#include <stdint.h>
#include <stdio.h>

#include <eigen3/Eigen/Core>

#include <rosflight_sim/rosflight_sil.h>
#include <rosflight_sim/sil_board.h>


namespace rosflight_sim
{

ROSflightSIL::ROSflightSIL() :
  gazebo::ModelPlugin(),
  nh_(nullptr),
  firmware_(board_)
{}

ROSflightSIL::~ROSflightSIL()
{
  updateConnection_.reset();
  // gazebo::event::Events::DisconnectWorldUpdateBegin(updateConnection_);
  if (nh_) {
    nh_->shutdown();
    delete nh_;
  }
}

void ROSflightSIL::Load(gazebo::physics::ModelPtr _model, sdf::ElementPtr _sdf)
{
  if (!ros::isInitialized())
  {
    ROS_FATAL("A ROS node for Gazebo has not been initialized, unable to load plugin");
    return;
  }
  ROS_INFO("Loaded the ROSflight SIL plugin");

  model_ = _model;
  world_ = model_->GetWorld();

  namespace_.clear();

  /*
   * Connect the Plugin to the Robot and Save pointers to the various elements in the simulation
   */
  if (_sdf->HasElement("namespace"))
    namespace_ = _sdf->GetElement("namespace")->Get<std::string>();
  else
    gzerr << "[ROSflight_SIL] Please specify a namespace.\n";
  nh_ = new ros::NodeHandle(namespace_);

  gzmsg << "loading parameters from " << namespace_ << " ns\n";

  if (_sdf->HasElement("linkName"))
    link_name_ = _sdf->GetElement("linkName")->Get<std::string>();
  else
    gzerr << "[ROSflight_SIL] Please specify a linkName of the forces and moments plugin.\n";
  link_ = model_->GetLink(link_name_);
  if (link_ == NULL)
    gzthrow("[ROSflight_SIL] Couldn't find specified link \"" << link_name_ << "\".");

  /* Load Params from Gazebo Server */
  if (_sdf->HasElement("mavType")) {
    mav_type_ = _sdf->GetElement("mavType")->Get<std::string>();
  }
  else {
    mav_type_ = "multirotor";
    gzerr << "[rosflight_sim] Please specify a value for parameter \"mavType\".\n";
  }

  if(mav_type_ == "multirotor")
    mav_dynamics_ = new Multirotor(nh_);
  else if(mav_type_ == "fixedwing")
    mav_dynamics_ = new Fixedwing(nh_);
  else
    gzthrow("unknown or unsupported mav type\n");

  // Initialize the Firmware
  board_.gazebo_setup(link_, world_, model_, nh_, mav_type_);
  firmware_.init();

  // Connect the update function to the simulation
  updateConnection_ = gazebo::event::Events::ConnectWorldUpdateBegin(boost::bind(&ROSflightSIL::OnUpdate, this, _1));

  initial_pose_ = link_->WorldCoGPose();

  truth_NED_pub_ = nh_->advertise<nav_msgs::Odometry>("truth/NED", 1);
  truth_NWU_pub_ = nh_->advertise<nav_msgs::Odometry>("truth/NWU", 1);
}


// This gets called by the world update event.
void ROSflightSIL::OnUpdate(const gazebo::common::UpdateInfo& _info)
{
  // We run twice so that that functions that take place when we don't have new IMU data get run
  firmware_.run();
  firmware_.run();
  Eigen::Matrix3d NWU_to_NED;
  NWU_to_NED << 1, 0, 0, 0, -1, 0, 0, 0, -1;

  MAVForcesAndMoments::Current_State state;
  ignition::math::Pose3d pose = link_->WorldCoGPose();
  ignition::math::Vector3d vel = link_->RelativeLinearVel();
  ignition::math::Vector3d omega = link_->RelativeAngularVel();

  // Convert gazebo types to Eigen and switch to NED frame
  state.pos = NWU_to_NED * vec3_to_eigen_from_gazebo(pose.Pos()) ;
  state.rot = NWU_to_NED * rotation_to_eigen_from_gazebo(pose.Rot());
  state.vel = NWU_to_NED * vec3_to_eigen_from_gazebo(vel);
  state.omega = NWU_to_NED * vec3_to_eigen_from_gazebo(omega);
  state.t = _info.simTime.Double();

  forces_ = mav_dynamics_->updateForcesAndTorques(state, board_.get_outputs());

  // apply the forces and torques to the joint (apply in NWU)
  ignition::math::Vector3d force = vec3_to_gazebo_from_eigen(NWU_to_NED * forces_.block<3,1>(0,0));
  ignition::math::Vector3d torque = vec3_to_gazebo_from_eigen(NWU_to_NED *  forces_.block<3,1>(3,0));
  link_->AddRelativeForce(force);
  link_->AddRelativeTorque(torque);

  publishTruth();

}

void ROSflightSIL::Reset()
{
    link_->SetWorldPose(initial_pose_);
    link_->ResetPhysicsStates();
//  start_time_us_ = (uint64_t)(world_->SimTime().Double() * 1e3);
//  rosflight_init();
}

void ROSflightSIL::windCallback(const geometry_msgs::Vector3 &msg)
{
  Eigen::Vector3d wind;
  wind << msg.x, msg.y, msg.z;
  mav_dynamics_->set_wind(wind);
}

void ROSflightSIL::publishTruth()
{
  ignition::math::Pose3d pose = link_->WorldCoGPose();
  ignition::math::Vector3d vel = link_->RelativeLinearVel();
  ignition::math::Vector3d omega = link_->RelativeAngularVel();

  // Publish truth
  nav_msgs::Odometry truth;
  truth.header.stamp.sec = world_->SimTime().sec;
  truth.header.stamp.nsec = world_->SimTime().nsec;
  truth.header.frame_id = link_name_ + "_NWU";
  truth.pose.pose.orientation.w = pose.Rot().W();
  truth.pose.pose.orientation.x = pose.Rot().X();
  truth.pose.pose.orientation.y = pose.Rot().Y();
  truth.pose.pose.orientation.z = pose.Rot().Z();
  truth.pose.pose.position.x = pose.Pos().X();
  truth.pose.pose.position.y = pose.Pos().Y();
  truth.pose.pose.position.z = pose.Pos().Z();
  truth.twist.twist.linear.x = vel.X();
  truth.twist.twist.linear.y = vel.Y();
  truth.twist.twist.linear.z = vel.Z();
  truth.twist.twist.angular.x = omega.X();
  truth.twist.twist.angular.y = omega.Y();
  truth.twist.twist.angular.z = omega.Z();
  truth_NWU_pub_.publish(truth);

  // Convert to NED
  truth.header.frame_id = link_name_ + "_NED";
  truth.pose.pose.orientation.y *= -1.0;
  truth.pose.pose.orientation.z *= -1.0;
  truth.pose.pose.position.y *= -1.0;
  truth.pose.pose.position.z *= -1.0;
  truth.twist.twist.linear.y *= -1.0;
  truth.twist.twist.linear.z *= -1.0;
  truth.twist.twist.angular.y *= -1.0;
  truth.twist.twist.angular.z *= -1.0;
  truth_NED_pub_.publish(truth);
}

Eigen::Vector3d ROSflightSIL::vec3_to_eigen_from_gazebo(ignition::math::Vector3d vec)
{
  Eigen::Vector3d out;
  out << vec.X(), vec.Y(), vec.Z();
  return out;
}

ignition::math::Vector3d ROSflightSIL::vec3_to_gazebo_from_eigen(Eigen::Vector3d vec)
{
  ignition::math::Vector3d out(vec(0), vec(1), vec(2));
  return out;
}

Eigen::Matrix3d ROSflightSIL::rotation_to_eigen_from_gazebo(ignition::math::Quaterniond quat)
{
  Eigen::Quaterniond eig_quat(quat.W(), quat.X(), quat.Y(), quat.Z());
  return eig_quat.toRotationMatrix();
}

GZ_REGISTER_MODEL_PLUGIN(ROSflightSIL)
} // namespace rosflight_sim
