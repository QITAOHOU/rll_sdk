/*
 * This file is part of the Robot Learning Lab SDK
 *
 * Copyright (C) 2020 Wolfgang Wiedmeyer <wolfgang.wiedmeyer@kit.edu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <rll_kinematics/forward_kinematics.h>

const double RLLForwardKinematics::SINGULARITY_CHECK_DISTANCE_TOL = 15.0 / 180.0 * M_PI;

bool RLLForwardKinematics::initialize(const RLLKinLimbs& limb_lengths, const RLLKinJoints& lower_joint_limits,
                                      const RLLKinJoints& upper_joint_limits)
{
  limb_lengths_ = limb_lengths;

  lower_joint_limits_ = lower_joint_limits;
  upper_joint_limits_ = upper_joint_limits;

  return true;
}

RLLKinMsg RLLForwardKinematics::fk(const RLLKinJoints& joint_angles, RLLKinPoseConfig* eef_pose) const
{
  eef_pose->config.set(joint_angles);

  RLLKinFrame mbw;  // wrist pose
  RLLKinShoulderWristVec sw;
  RLLKinMsg result = armAngle(joint_angles, eef_pose->config, &eef_pose->arm_angle, &mbw, &sw);
  if (result.error())
  {
    return result;
  }

  // flange pose
  RLLKinFrame mbf = mbw * RLLKinFrame(limb_lengths_[3], joint_angles(6), 0.0, 0.0);
  eef_pose->pose = mbf;

  return checkSingularities(joint_angles, sw);
}

RLLKinMsg RLLForwardKinematics::armAngle(const RLLKinJoints& joint_angles, const RLLKinGlobalConfig& config,
                                         double* arm_angle) const
{
  RLLKinFrame wrist_pose;
  RLLKinShoulderWristVec sw;

  return armAngle(joint_angles, config, arm_angle, &wrist_pose, &sw);
}

RLLKinMsg RLLForwardKinematics::armAngle(const RLLKinJoints& joint_angles, const RLLKinGlobalConfig& config,
                                         double* arm_angle, RLLKinFrame* wrist_pose, RLLKinShoulderWristVec* sw) const
{
  if (jointLimitsViolated(joint_angles))
  {
    return RLLKinMsg::JOINT_LIMIT_VIOLATED;
  }

  // shoulder pose
  RLLKinFrame mbs = RLLKinFrame(limb_lengths_[0], joint_angles(0), 0.0, -M_PI / 2.0) *
                    RLLKinFrame(0.0, joint_angles(1), 0.0, M_PI / 2.0);
  // elbow pose
  RLLKinFrame mbe = mbs * RLLKinFrame(limb_lengths_[1], joint_angles(2), 0.0, M_PI / 2.0) *
                    RLLKinFrame(0.0, joint_angles(3), 0.0, -M_PI / 2.0);
  // wrist pose
  RLLKinFrame mbw = mbe * RLLKinFrame(limb_lengths_[2], joint_angles(4), 0.0, -M_PI / 2.0) *
                    RLLKinFrame(0.0, joint_angles(5), 0.0, M_PI / 2.0);
  *wrist_pose = mbw;

  // reference plane and arm angle

  Eigen::Vector3d xsw = mbw.pos() - mbs.pos();  // vector shoulder to wrist
  Eigen::Vector3d xsw_n = xsw.normalized();
  double lsw = xsw.norm();
  sw->xsw = xsw;
  sw->lsw = lsw;

  double joint_angle_1_v = jointAngle1Virtual(xsw);
  double joint_angle_2_v = jointAngle2Virtual(xsw, lsw, config);

  // virtual shoulder pose
  RLLKinFrame mbs_v = RLLKinFrame(limb_lengths_[0], joint_angle_1_v, 0.0, -M_PI / 2.0) *
                      RLLKinFrame(0.0, joint_angle_2_v, 0.0, M_PI / 2.0);
  // virtual shoulder to elbow vector
  Eigen::Vector3d xse_v(0.0, 0.0, limb_lengths_[1]);
  // virtual shoulder to elbow vector in base coordinates
  Eigen::Vector3d xseb_n_v = (mbs_v.ori() * xse_v).normalized();
  // virtual reference plane (shoulder, elbow, wrist) normal vector
  // xsw_n_v and xsw_n are identical, vector from shoulder to wrist is the same for virtual manipulator
  Eigen::Vector3d v_sew_v = (xseb_n_v.cross(xsw_n)).normalized();

  // real normalized shoulder to elbow vector in base coordinates
  Eigen::Vector3d xseb_n = (mbe.pos() - mbs.pos()).normalized();
  // real reference plane (shoulder, elbow, wrist) normal vector
  Eigen::Vector3d v_sew = (xseb_n.cross(xsw_n)).normalized();

  double psi_sign = kSign((v_sew_v.cross(v_sew)).transpose() * xsw);
  *arm_angle = psi_sign * kAcos(v_sew_v.transpose() * v_sew);

  return RLLKinMsg::SUCCESS;
}

RLLKinMsg RLLForwardKinematics::shoulderWristVec(const RLLKinFrame& eef_pose, Eigen::Vector3d* xsw, double* lsw) const
{
  RLLKinFrame mfw;  // wrist pose in flange coordinates
  mfw.setPosition(2, -limb_lengths_[3]);

  Eigen::Vector3d xw = (eef_pose * mfw).pos();  // wrist position in base coordinates
  Eigen::Vector3d xs(0, 0, limb_lengths_[0]);   // shoulder pos

  *xsw = xw - xs;
  *lsw = xsw->norm();

  // check if target is too close/far
  if (*lsw > limb_lengths_[1] + limb_lengths_[2])
  {
    return RLLKinMsg::TARGET_TOO_FAR;
  }

  if (*lsw < fabs(limb_lengths_[1] - limb_lengths_[2]))
  {
    return RLLKinMsg::TARGET_TOO_CLOSE;
  }

  return RLLKinMsg::SUCCESS;
}

double RLLForwardKinematics::jointAngle1Virtual(const Eigen::Vector3d& xsw) const
{
  double dist_z_1 = kSqrt(pow(xsw(0), 2) + pow(xsw(1), 2));
  if (kZero(dist_z_1))
  {
    // shoulder to wrist vector aligned with z-axis of first joint
    // singularity with 4=0
    return 0.0;
  }

  return atan2(xsw(1), xsw(0));
}

double RLLForwardKinematics::jointAngle2Virtual(const Eigen::Vector3d& xsw, const double lsw,
                                                const RLLKinGlobalConfig& config) const
{
  // determine virtual shoulder joint angle, depending on configuration
  // phi is the angle between xsw and the shoulder-elbow limb
  double joint_angle_2_v_phi =
      kAcos((pow(limb_lengths_[1], 2) + pow(lsw, 2) - pow(limb_lengths_[2], 2)) / (2 * limb_lengths_[1] * lsw));

  return atan2(kSqrt(pow(xsw(0), 2) + pow(xsw(1), 2)), xsw(2)) + config.gc4() * joint_angle_2_v_phi;
}

double RLLForwardKinematics::jointAngle4(const double lsw, const RLLKinGlobalConfig& config) const
{
  return config.gc4() * kAcos((pow(lsw, 2) - pow(limb_lengths_[1], 2) - pow(limb_lengths_[2], 2)) /
                              (2 * limb_lengths_[1] * limb_lengths_[2]));
}

RLLKinMsg RLLForwardKinematics::checkSingularities(const RLLKinJoints& joint_angles,
                                                   const RLLKinShoulderWristVec& sw) const
{
  // check hinge joints
  for (int j = 1; j <= 5; j += 2)
  {
    if (fabs(joint_angles(j)) < SINGULARITY_CHECK_DISTANCE_TOL)
    {
      return RLLKinMsg::CLOSE_TO_SINGULARITY;
    }
  }

  Eigen::Vector3d z;
  z << 0, 0, 1;

  // check if first and seventh axis are on one line
  double overhead = kAcos(sw.xsw.dot(z) / sw.lsw);
  if (overhead < SINGULARITY_CHECK_DISTANCE_TOL || overhead > (M_PI - SINGULARITY_CHECK_DISTANCE_TOL))
  {
    return RLLKinMsg::CLOSE_TO_SINGULARITY;
  }

  return RLLKinMsg::SUCCESS;
}
