// __BEGIN_LICENSE__
//  Copyright (c) 2009-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NGT platform is licensed under the Apache License, Version 2.0 (the
//  "License"); you may not use this file except in compliance with the
//  License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__

// Cost functions used in solving for jitter. These need access to the camera models,
// so they are stored in the Camera folder.

#include <asp/Camera/JitterSolveCostFuns.h>
#include <asp/Camera/BundleAdjustCamera.h>
#include <asp/Core/CameraTransforms.h>
#include <asp/Core/SatSimBase.h>
#include <asp/Core/BundleAdjustUtils.h>
#include <asp/Rig/transform_utils.h>
#include <asp/Core/EigenTransformUtils.h>

#include <vw/Cartography/GeoReferenceBaseUtils.h>
#include <vw/Cartography/GeoReferenceUtils.h>
#include <vw/Core/Exception.h>

namespace asp {

const double g_big_pixel_value = 1000.0;  // don't make this too big

// An error function minimizing the error of projecting an xyz point
// into a given CSM linescan camera pixel. The variables of optimization are a
// portion of the position and quaternion variables affected by this, and the 
// triangulation point.
struct LsPixelReprojErr {
  LsPixelReprojErr(vw::Vector2 const& observation, double weight,
                   UsgsAstroLsSensorModel* ls_model,
                   int begQuatIndex, int endQuatIndex, 
                   int begPosIndex, int endPosIndex):
    m_observation(observation), m_weight(weight),
    m_begQuatIndex(begQuatIndex), m_endQuatIndex(endQuatIndex),
    m_begPosIndex(begPosIndex),   m_endPosIndex(endPosIndex),
    m_ls_model(ls_model) {}

  // The implementation is further down
  bool operator()(double const * const * parameters, double * residuals) const; 

  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(vw::Vector2 const& observation, double weight,
                                     UsgsAstroLsSensorModel* ls_model,
                                     int begQuatIndex, int endQuatIndex,
                                     int begPosIndex, int endPosIndex) {

    // TODO(oalexan1): Try using here the analytical cost function
    ceres::DynamicNumericDiffCostFunction<LsPixelReprojErr>* cost_function =
      new ceres::DynamicNumericDiffCostFunction<LsPixelReprojErr>
      (new LsPixelReprojErr(observation, weight, ls_model,
                                  begQuatIndex, endQuatIndex,
                                  begPosIndex, endPosIndex));

    // The residual size is always the same.
    cost_function->SetNumResiduals(PIXEL_SIZE);

    // Add a parameter block for each quaternion and each position
    for (int it = begQuatIndex; it < endQuatIndex; it++)
      cost_function->AddParameterBlock(NUM_QUAT_PARAMS);
    for (int it = begPosIndex; it < endPosIndex; it++)
      cost_function->AddParameterBlock(NUM_XYZ_PARAMS);

    // Add a parameter block for the xyz point
    cost_function->AddParameterBlock(NUM_XYZ_PARAMS);
    
    return cost_function;
  }

private:
  vw::Vector2 m_observation; // The pixel observation for this camera/point pair
  double m_weight;
  UsgsAstroLsSensorModel* m_ls_model;
  int m_begQuatIndex, m_endQuatIndex;
  int m_begPosIndex, m_endPosIndex;
}; // End class LsPixelReprojErr

// An error function minimizing the error of projecting an xyz point
// into a given CSM Frame camera pixel. The variables of optimization are 
// the camera position, quaternion, and triangulation point.
struct FramePixelReprojErr {
  FramePixelReprojErr(vw::Vector2 const& observation, double weight,
                   UsgsAstroFrameSensorModel* frame_model):
    m_observation(observation), m_weight(weight),
    m_frame_model(frame_model) {}

  // The implementation is further down
  bool operator()(double const * const * parameters, double * residuals) const; 

  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(vw::Vector2 const& observation, double weight,
                                     UsgsAstroFrameSensorModel* frame_model) {

    // TODO(oalexan1): Try using here the analytical cost function
    ceres::DynamicNumericDiffCostFunction<FramePixelReprojErr>* cost_function =
      new ceres::DynamicNumericDiffCostFunction<FramePixelReprojErr>
      (new FramePixelReprojErr(observation, weight, frame_model));

    // The residual size is always the same.
    cost_function->SetNumResiduals(PIXEL_SIZE);

    // Add a parameter block for each position and quaternion, in this order
    cost_function->AddParameterBlock(NUM_XYZ_PARAMS);
    cost_function->AddParameterBlock(NUM_QUAT_PARAMS);

    // Add a parameter block for the xyz point
    cost_function->AddParameterBlock(NUM_XYZ_PARAMS);
    
    return cost_function;
  }

private:
  vw::Vector2 m_observation; // The pixel observation for this camera/point pair
  double m_weight;
  UsgsAstroFrameSensorModel* m_frame_model;
}; // End class FramePixelReprojErr

// See the documentation higher up in the file.
bool LsPixelReprojErr::operator()(double const * const * parameters, 
                                  double * residuals) const {

  try {
    // Make a copy of the model, as we will update quaternion and position values
    // that are being modified now. This may be expensive.
    UsgsAstroLsSensorModel cam = *m_ls_model;

    // Update the relevant quaternions in the local copy
    int shift = 0;
    for (int qi = m_begQuatIndex; qi < m_endQuatIndex; qi++) {
      for (int coord = 0; coord < NUM_QUAT_PARAMS; coord++) {
        cam.m_quaternions[NUM_QUAT_PARAMS * qi + coord]
          = parameters[qi + shift - m_begQuatIndex][coord];
      }
    }

    // Same for the positions. Note how we move forward in the parameters array,
    // as this is after the quaternions
    shift += (m_endQuatIndex - m_begQuatIndex);
    for (int pi = m_begPosIndex; pi < m_endPosIndex; pi++) {
      for (int coord = 0; coord < NUM_XYZ_PARAMS; coord++) {
        cam.m_positions[NUM_XYZ_PARAMS * pi + coord]
          = parameters[pi + shift - m_begPosIndex][coord];
      }
    }

    // Move forward in the array of parameters, then recover the triangulated point
    shift += (m_endPosIndex - m_begPosIndex);
    csm::EcefCoord P;
    P.x = parameters[shift][0];
    P.y = parameters[shift][1];
    P.z = parameters[shift][2];

    // Project in the camera with high precision. Do not use here
    // anything lower than 1e-8, as the linescan model will then
    // return junk.
    double desired_precision = asp::DEFAULT_CSM_DESIRED_PRECISION;
    csm::ImageCoord imagePt = cam.groundToImage(P, desired_precision);

    // Convert to what ASP expects
    vw::Vector2 pix;
    asp::fromCsmPixel(pix, imagePt);

    residuals[0] = m_weight*(pix[0] - m_observation[0]);
    residuals[1] = m_weight*(pix[1] - m_observation[1]);
    
  } catch (std::exception const& e) {
    residuals[0] = g_big_pixel_value;
    residuals[1] = g_big_pixel_value;
    return true; // accept the solution anyway
  }

  return true;
}

// See the .h file for the documentation.
bool FramePixelReprojErr::operator()(double const * const * parameters, 
                                     double * residuals) const {

  try {
    // Make a copy of the model, as we will update position and quaternion
    // values that are being modified now. Use the same order as in
    // UsgsAstroFrameSensorModel::m_currentParameterValue.
    UsgsAstroFrameSensorModel cam = *m_frame_model;

    // The latest position is in parameters[0].
    for (int coord = 0; coord < NUM_XYZ_PARAMS; coord++)
      cam.setParameterValue(coord, parameters[0][coord]);

    // The latest quaternion is in parameters[1]. Note how we below
    // move forward when invoking cam.setParameterValue().
    for (int coord = 0; coord < NUM_QUAT_PARAMS; coord++) 
      cam.setParameterValue(coord + NUM_XYZ_PARAMS, parameters[1][coord]);

    // The triangulation parameter is after the position and orientation
    csm::EcefCoord P;
    P.x = parameters[2][0];
    P.y = parameters[2][1];
    P.z = parameters[2][2];

    // Project in the camera with high precision. Do not use here
    // anything lower than 1e-8, as the linescan model will then
    // return junk.
    double desired_precision = asp::DEFAULT_CSM_DESIRED_PRECISION;
    csm::ImageCoord imagePt = cam.groundToImage(P, desired_precision);

    // Convert to what ASP expects
    vw::Vector2 pix;
    asp::fromCsmPixel(pix, imagePt);

    residuals[0] = m_weight*(pix[0] - m_observation[0]);
    residuals[1] = m_weight*(pix[1] - m_observation[1]);
    
  } catch (std::exception const& e) {
    residuals[0] = g_big_pixel_value;
    residuals[1] = g_big_pixel_value;
    return true; // accept the solution anyway
  }

  return true;
}

// An error function minimizing the error of projecting an xyz point into a
// given CSM frame camera pixel that is on a rig with a linescan camera. The
// variables of optimization are a portion of the position and quaternion
// variables affected by this, the triangulation point, and the transform from
// the ref linescan sensor to the current frame sensor.
struct RigLsFramePixelReprojErr {
  RigLsFramePixelReprojErr(vw::Vector2 const& frame_pix, double weight,
                           asp::RigCamInfo const& rig_cam_info,
                           UsgsAstroLsSensorModel* ref_ls_model,
                           UsgsAstroFrameSensorModel * curr_frame_model,
                           int begQuatIndex, int endQuatIndex, 
                           int begPosIndex, int endPosIndex):
    m_frame_pix(frame_pix), m_weight(weight),
    m_rig_cam_info(rig_cam_info),
    m_begQuatIndex(begQuatIndex), m_endQuatIndex(endQuatIndex),
    m_begPosIndex(begPosIndex),   m_endPosIndex(endPosIndex),
    m_ref_ls_model(ref_ls_model), m_curr_frame_model(curr_frame_model) {}

  // The implementation is further down
  bool operator()(double const * const * parameters, double * residuals) const; 

  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(vw::Vector2 const& frame_pix, double weight,
                                     asp::RigCamInfo const& rig_cam_info,
                                     UsgsAstroLsSensorModel* ref_ls_model,
                                     UsgsAstroFrameSensorModel* curr_frame_model,
                                     int begQuatIndex, int endQuatIndex,
                                     int begPosIndex, int endPosIndex) {

    // TODO(oalexan1): Try using here the analytical cost function
    ceres::DynamicNumericDiffCostFunction<RigLsFramePixelReprojErr>* cost_function =
      new ceres::DynamicNumericDiffCostFunction<RigLsFramePixelReprojErr>
      (new RigLsFramePixelReprojErr(frame_pix, weight, rig_cam_info, ref_ls_model,
                                    curr_frame_model, 
                                    begQuatIndex, endQuatIndex,
                                    begPosIndex, endPosIndex));

    // The residual size is always the same.
    cost_function->SetNumResiduals(PIXEL_SIZE);

    // Add a parameter block for each quaternion and each position
    for (int it = begQuatIndex; it < endQuatIndex; it++)
      cost_function->AddParameterBlock(NUM_QUAT_PARAMS);
    for (int it = begPosIndex; it < endPosIndex; it++)
      cost_function->AddParameterBlock(NUM_XYZ_PARAMS);

    // Add a parameter block for the xyz point
    cost_function->AddParameterBlock(NUM_XYZ_PARAMS);
    
    // Add a parameter block for the ref to curr sensor rig transform
    cost_function->AddParameterBlock(rig::NUM_RIGID_PARAMS);
    
    return cost_function;
  }

private:
  vw::Vector2 m_frame_pix; // The frame camera pixel 
  double m_weight;
  asp::RigCamInfo m_rig_cam_info;
  UsgsAstroLsSensorModel* m_ref_ls_model;
  UsgsAstroFrameSensorModel* m_curr_frame_model;
  int m_begQuatIndex, m_endQuatIndex;
  int m_begPosIndex, m_endPosIndex;
}; // End class RigLsFramePixelReprojErr

// The implementation of operator() for RigLsFramePixelReprojErr
bool RigLsFramePixelReprojErr::operator()(double const * const * parameters, 
                                  double * residuals) const {

  try {
    
    // Make a copy of the linescan model, and update with latest position and
    // orientation This may be expensive.
    UsgsAstroLsSensorModel ls_cam = *m_ref_ls_model;
    // Update the relevant quaternions in the local copy
    int shift = 0;
    for (int qi = m_begQuatIndex; qi < m_endQuatIndex; qi++) {
      for (int coord = 0; coord < NUM_QUAT_PARAMS; coord++) {
        ls_cam.m_quaternions[NUM_QUAT_PARAMS * qi + coord]
          = parameters[qi + shift - m_begQuatIndex][coord];
      }
    }
    // Same for the positions. Note how we move forward in the parameters array,
    // as this is after the quaternions
    shift += (m_endQuatIndex - m_begQuatIndex);
    for (int pi = m_begPosIndex; pi < m_endPosIndex; pi++) {
      for (int coord = 0; coord < NUM_XYZ_PARAMS; coord++) {
        ls_cam.m_positions[NUM_XYZ_PARAMS * pi + coord]
          = parameters[pi + shift - m_begPosIndex][coord];
      }
    }

    // Move forward in the array of parameters, then recover the triangulated point
    shift += (m_endPosIndex - m_begPosIndex);
    csm::EcefCoord P;
    P.x = parameters[shift][0];
    P.y = parameters[shift][1];
    P.z = parameters[shift][2];

    // Move forward in the array of parameters, then recover the ref to curr transform
    shift += 1;
    double const* ref_to_curr_trans = parameters[shift];
    
    // Current camera to world transform based on the ref cam and the rig
    std::vector<double> cam2world_vec(rig::NUM_RIGID_PARAMS);
    asp::linescanToCurrSensorTrans(ls_cam, m_rig_cam_info, ref_to_curr_trans,
                                   &cam2world_vec[0]); // output
    
    // Make a copy of the frame camera and set the latest position and orientation
    UsgsAstroFrameSensorModel frame_cam = *m_curr_frame_model;
    for (int coord = 0; coord < NUM_XYZ_PARAMS + NUM_QUAT_PARAMS; coord++)
      frame_cam.setParameterValue(coord, cam2world_vec[coord]);
    
    // Project in the camera with high precision. Do not use here anything lower
    // than 1e-8, as the CSM model can return junk. Convert to ASP pixel.
    double desired_precision = asp::DEFAULT_CSM_DESIRED_PRECISION;
    csm::ImageCoord imagePt = frame_cam.groundToImage(P, desired_precision);
    vw::Vector2 pix;
    asp::fromCsmPixel(pix, imagePt);
    
    // Compute the residuals  
    residuals[0] = m_weight*(pix[0] - m_frame_pix[0]);
    residuals[1] = m_weight*(pix[1] - m_frame_pix[1]);
    
  } catch (std::exception const& e) {
    residuals[0] = g_big_pixel_value;
    residuals[1] = g_big_pixel_value;
    return true; // accept the solution anyway
  }

  return true;
}

// Constructor for weightedRollYawError. See the .h file for the documentation.
weightedRollYawError::weightedRollYawError
                  (std::vector<double>           const& positions, 
                   std::vector<double>           const& quaternions,
                   vw::cartography::GeoReference const& georef,
                   int cur_pos, double rollWeight, double yawWeight,
                   bool initial_camera_constraint): 
                   m_rollWeight(rollWeight), m_yawWeight(yawWeight), 
                   m_initial_camera_constraint(initial_camera_constraint) {

    int num_pos = positions.size()/NUM_XYZ_PARAMS;
    int num_quat = quaternions.size()/NUM_QUAT_PARAMS;
    if (num_pos != num_quat)
      vw::vw_throw(vw::ArgumentErr() 
        << "weightedRollYawError: Expecting the same number of positions and quaternions.\n");
    if (cur_pos < 0 || cur_pos >= num_pos)
      vw::vw_throw(vw::ArgumentErr() 
        << "weightedRollYawError: Expecting position index in range.\n");

    // Find the nearest neighbors of the current position
    int beg_pos = std::max(0, cur_pos - 1);
    int end_pos = std::min(num_pos - 1, cur_pos + 1);
    if (beg_pos >= end_pos)
      vw::vw_throw(vw::ArgumentErr() 
        << "weightedRollYawError: Expecting at least 2 camera positions.\n");

    // Find the segment along which the cameras are located, in projected coordinates
    // Here we mirror the logic from SatSim.cc
    int b = beg_pos * NUM_XYZ_PARAMS;
    int c = cur_pos * NUM_XYZ_PARAMS;
    int e = end_pos * NUM_XYZ_PARAMS;
    vw::Vector3 beg_pt(positions[b], positions[b+1], positions[b+2]);
    vw::Vector3 cur_pt(positions[c], positions[c+1], positions[c+2]);
    vw::Vector3 end_pt(positions[e], positions[e+1], positions[e+2]);

    // Orbital points before the current one, the current one, and after the
    // current one, in projected coordinates
    vw::Vector3 beg_proj = vw::cartography::ecefToProj(georef, beg_pt);
    vw::Vector3 cur_proj = vw::cartography::ecefToProj(georef, cur_pt);
    vw::Vector3 end_proj = vw::cartography::ecefToProj(georef, end_pt);
    
    // Find satellite along and across track directions in projected coordinates
    vw::Vector3 proj_along, proj_across;
    asp::calcProjAlongAcross(beg_proj, end_proj, proj_along, proj_across);

    // Find along and across in ECEF
    vw::Vector3 along, across;
    asp::calcEcefAlongAcross(georef, asp::satSimDelta(), 
                              proj_along, proj_across, cur_proj,
                              along, across); // outputs

    // Find the z vector as perpendicular to both along and across
    vw::Vector3 down = vw::math::cross_prod(along, across);
    down = down / norm_2(down);

    // Find the rotation matrix from satellite to world coordinates, and 90
    // degree in-camera rotation. It is assumed, as in sat_sim, that:
    // cam2world = sat2World * rollPitchYaw * rotXY.
    asp::assembleCam2WorldMatrix(along, across, down, m_sat2World);
    m_rotXY = asp::rotationXY();

    // Initial camera rotation matrix, before we optimize it
    m_initCam2World = asp::quaternionToMatrix(&quaternions[cur_pos*NUM_QUAT_PARAMS]);
}

// See the .h file for the documentation.
bool weightedRollYawError::operator()(double const * const * parameters, 
                                      double * residuals) const {

  // Convert to rotation matrix. Order of quaternion is x, y, z, w.  
  vw::Matrix3x3 cam2world = asp::quaternionToMatrix(parameters[0]);

  if (m_initial_camera_constraint) {
    // Find the new camera orientation relative to the initial camera, not
    // relative to the satellite along-track direction. Then find the roll and
    // yaw from it. This is experimental.
    vw::Matrix3x3 cam2cam =  vw::math::inverse(cam2world) * m_initCam2World;

    double roll, pitch, yaw;
    rollPitchYawFromRotationMatrix(cam2cam, roll, pitch, yaw);

    // Fix for roll / yaw being determined with +/- 180 degree ambiguity.
    roll  = roll  - 180.0 * round(roll  / 180.0);
    pitch = pitch - 180.0 * round(pitch / 180.0);
    yaw   = yaw   - 180.0 * round(yaw   / 180.0);

    // Roll, pitch, yaw in camera coordinates are pitch, roll, yaw in satellite
    // coordinates. So adjust below accordingly.
    // CERES is very tolerant if one of the weights used below is 0. So there is
    // no need to use a special cost function for such cases.
    residuals[0] = pitch * m_rollWeight; // per above, swap roll and pitch
    residuals[1] = yaw  * m_yawWeight;

    return true;
  }

  vw::Matrix3x3 rollPitchYaw  
    = vw::math::inverse(m_sat2World) * cam2world * vw::math::inverse(m_rotXY);

  double roll, pitch, yaw;
  rollPitchYawFromRotationMatrix(rollPitchYaw, roll, pitch, yaw);

  // Fix for roll / yaw being determined with +/- 180 degree ambiguity.
  roll = roll - 180.0 * round(roll / 180.0);
  pitch = pitch - 180.0 * round(pitch / 180.0);
  yaw  = yaw  - 180.0 * round(yaw  / 180.0);

  // CERES is very tolerant if one of the weights used below is 0. So there is
  // no need to use a special cost function for such cases.
  residuals[0] = roll * m_rollWeight;
  residuals[1] = yaw  * m_yawWeight;

  return true;
}

// Calc the range of indices in the samples needed to interpolate between time1 and time2.
// Based on lagrangeInterp() in usgscsm.
void calcIndexBounds(double time1, double time2, double t0, double dt, int numVals,
                     // Outputs
                     int & begIndex, int & endIndex) {

  // Order of Lagrange interpolation
  int numInterpSamples = 8;

  // Starting and ending  index (ending is exclusive).
  int index1 = static_cast<int>((time1 - t0) / dt);
  int index2 = static_cast<int>((time2 - t0) / dt);
  
  // TODO(oalexan1): Maybe the indices should be more generous, so not adding 1
  // to begIndex, even though what is here seems correct according to 
  // lagrangeInterp().
  begIndex = std::min(index1, index2) - numInterpSamples / 2 + 1;
  endIndex = std::max(index1, index2) + numInterpSamples / 2 + 1;
  
  // Keep in bounds
  begIndex = std::max(0, begIndex);
  endIndex = std::min(endIndex, numVals);
  if (begIndex >= endIndex)
    vw::vw_throw(vw::ArgumentErr() << "Book-keeping error in interpolation. " 
      << "Likely image order is different than camera order.\n"); 
    
  return;
}

// Add the linescan model reprojection error to the cost function
void addLsReprojectionErr(asp::BaBaseOptions  const& opt,
                          UsgsAstroLsSensorModel   * ls_model,
                          vw::Vector2         const& observation,
                          double                   * tri_point,
                          double                     weight,
                          ceres::Problem           & problem) {

  // Find all positions and quaternions that can affect the current pixel. Must
  // grow the number of quaternions and positions a bit because during
  // optimization the 3D point and corresponding pixel may move somewhat.
  double line_extra = opt.max_init_reproj_error + 5.0; // add some more just in case
  csm::ImageCoord imagePt1, imagePt2;
  asp::toCsmPixel(observation - vw::Vector2(0.0, line_extra), imagePt1);
  asp::toCsmPixel(observation + vw::Vector2(0.0, line_extra), imagePt2);
  double time1 = ls_model->getImageTime(imagePt1);
  double time2 = ls_model->getImageTime(imagePt2);

  // Find the range of indices that can affect the current pixel
  int numQuat       = ls_model->m_quaternions.size() / NUM_QUAT_PARAMS;
  double quatT0     = ls_model->m_t0Quat;
  double quatDt     = ls_model->m_dtQuat;
  int begQuatIndex = -1, endQuatIndex = -1;
  calcIndexBounds(time1, time2, quatT0, quatDt, numQuat, 
                  begQuatIndex, endQuatIndex); // outputs

  // Same for positions
  int numPos       = ls_model->m_positions.size() / NUM_XYZ_PARAMS;
  double posT0     = ls_model->m_t0Ephem;
  double posDt     = ls_model->m_dtEphem;
  int begPosIndex = -1, endPosIndex = -1;
  calcIndexBounds(time1, time2, posT0, posDt, numPos, 
                  begPosIndex, endPosIndex); // outputs

  ceres::CostFunction* pixel_cost_function =
    LsPixelReprojErr::Create(observation, weight, ls_model,
                             begQuatIndex, endQuatIndex,
                             begPosIndex, endPosIndex);
  ceres::LossFunction* pixel_loss_function = new ceres::CauchyLoss(opt.robust_threshold);

  // The variable of optimization are camera quaternions and positions stored in the
  // camera models, and the triangulated point.
  std::vector<double*> vars;
  for (int it = begQuatIndex; it < endQuatIndex; it++)
    vars.push_back(&ls_model->m_quaternions[it * NUM_QUAT_PARAMS]);
  for (int it = begPosIndex; it < endPosIndex; it++)
    vars.push_back(&ls_model->m_positions[it * NUM_XYZ_PARAMS]);
  vars.push_back(tri_point);
  problem.AddResidualBlock(pixel_cost_function, pixel_loss_function, vars);

  return;   
}

// Add the frame camera model reprojection error to the cost function
void addFrameReprojectionErr(asp::BaBaseOptions  const & opt,
                             UsgsAstroFrameSensorModel * frame_model,
                             vw::Vector2         const & observation,
                             double                    * frame_params,
                             double                    * tri_point,
                             double                      weight,
                             ceres::Problem            & problem) {

  ceres::CostFunction* pixel_cost_function =
    FramePixelReprojErr::Create(observation, weight, frame_model);
  ceres::LossFunction* pixel_loss_function = new ceres::CauchyLoss(opt.robust_threshold);

  // The variable of optimization are camera positions and quaternion stored 
  // in frame_cam_params, in this order, and the triangulated point.
  // This is different from the linescan model, where we can directly access
  // these quantities inside the model, so they need not be stored separately.
  std::vector<double*> vars;
  vars.push_back(&frame_params[0]);              // positions start here
  vars.push_back(&frame_params[NUM_XYZ_PARAMS]); // quaternions start here
  vars.push_back(tri_point);
  problem.AddResidualBlock(pixel_cost_function, pixel_loss_function, vars);

  return;   
}

// Reprojection error with ls ref sensor and frame curr sensor
void addRigLsFrameReprojectionErr(asp::BaBaseOptions  const & opt,
                                  asp::RigCamInfo     const & rig_cam_info,
                                  vw::Vector2         const & frame_pix,
                                  double                      weight,
                                  UsgsAstroLsSensorModel    * ref_ls_model,
                                  UsgsAstroFrameSensorModel * curr_frame_model,
                                  double                    * ref_to_curr_trans,
                                  double                    * tri_point,
                                  ceres::Problem            & problem) {

  // The time when the frame camera pixel was observed
  double frame_time = rig_cam_info.beg_pose_time;
  if (frame_time != rig_cam_info.end_pose_time)
   vw::vw_throw(vw::ArgumentErr() 
                << "For a frame sensor beg and end pose time must be same.\n");
  
  // The solver needs to see beyond the current time as later the poses will
  // change relative to the observations. Here using linescan pixels,
  // not the frame pixel in the input.
  double line_extra = opt.max_init_reproj_error + 5.0; // add some more just in case
  csm::ImageCoord imagePt1, imagePt2;
  asp::toCsmPixel(vw::Vector2(0.0, 0.0), imagePt1);
  asp::toCsmPixel(vw::Vector2(0.0, line_extra), imagePt2);
  double ans1 = ref_ls_model->getImageTime(imagePt1);
  double ans2 = ref_ls_model->getImageTime(imagePt2);
  double delta = std::abs(ans2 - ans1);
  double time1 = frame_time - delta;
  double time2 = frame_time + delta;

  // Find the range of indices that can affect the current pixel
  int numQuat       = ref_ls_model->m_quaternions.size() / NUM_QUAT_PARAMS;
  double quatT0     = ref_ls_model->m_t0Quat;
  double quatDt     = ref_ls_model->m_dtQuat;
  int begQuatIndex = -1, endQuatIndex = -1;
  calcIndexBounds(time1, time2, quatT0, quatDt, numQuat, 
                  begQuatIndex, endQuatIndex); // outputs

  // Same for positions
  int numPos       = ref_ls_model->m_positions.size() / NUM_XYZ_PARAMS;
  double posT0     = ref_ls_model->m_t0Ephem;
  double posDt     = ref_ls_model->m_dtEphem;
  int begPosIndex = -1, endPosIndex = -1;
  calcIndexBounds(time1, time2, posT0, posDt, numPos, 
                  begPosIndex, endPosIndex); // outputs
  
  ceres::CostFunction* pixel_cost_function =
    RigLsFramePixelReprojErr::Create(frame_pix, weight, rig_cam_info, ref_ls_model,
                                     curr_frame_model, 
                                     begQuatIndex, endQuatIndex,
                                     begPosIndex, endPosIndex);
  ceres::LossFunction* pixel_loss_function = new ceres::CauchyLoss(opt.robust_threshold);

  // The variable of optimization are camera quaternions and positions stored in the
  // camera models, the triangulated point, and the rig transform.
  std::vector<double*> vars;
  for (int it = begQuatIndex; it < endQuatIndex; it++)
    vars.push_back(&ref_ls_model->m_quaternions[it * NUM_QUAT_PARAMS]);
  for (int it = begPosIndex; it < endPosIndex; it++)
    vars.push_back(&ref_ls_model->m_positions[it * NUM_XYZ_PARAMS]);
  vars.push_back(tri_point);
  vars.push_back(ref_to_curr_trans); // transform from ref to curr sensor on the rig
  problem.AddResidualBlock(pixel_cost_function, pixel_loss_function, vars);
}

} // end namespace asp
