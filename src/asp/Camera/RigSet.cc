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

// Functions used for the sat_sim.cc tool that are not general enough to put
// somewhere else.

#include <asp/Camera/RigSet.h>

#include <vw/Core/Log.h>

#include <glog/logging.h>
#include <set>
#include <fstream>
#include <iostream>

namespace asp {

namespace camera {
 
CameraParameters::
CameraParameters(Eigen::Vector2i const& image_size_in,
                  double                 focal_length_in,
                  Eigen::Vector2d const& optical_center_in,
                  Eigen::VectorXd const& distortion_in,
                  DistortionType distortion_type_in):
  image_size(image_size_in), focal_length(focal_length_in),
  optical_center(optical_center_in), distortion(distortion_in),
  m_distortion_type(distortion_type_in) {}

// Domain of validity of distortion model (normally all image)
// Centered around image center.
void CameraParameters::SetDistortedCropSize(Eigen::Vector2i const& crop_size) {
  m_distorted_crop_size = crop_size;
}

void CameraParameters::SetUndistortedSize(Eigen::Vector2i const& image_size) {
  m_undistorted_size = image_size;
}

} // end namespace camera

// A ref sensor is the first sensor on each rig
bool RigSet::isRefSensor(std::string const& cam_name) const {
  for (size_t rig_it = 0; rig_it < cam_set.size(); rig_it++) 
    if (cam_set[rig_it][0] == cam_name) 
      return true;
  return false;
}

// Form an affine transform from 12 values
Eigen::Affine3d vecToAffine(Eigen::VectorXd const& vals) {
  if (vals.size() != 12)
    LOG(FATAL) << "An affine transform must have 12 parameters.\n";

  Eigen::Affine3d M = Eigen::Affine3d::Identity();
  Eigen::MatrixXd T = M.matrix();

  int count = 0;

  // linear part
  T(0, 0) = vals[count++];
  T(0, 1) = vals[count++];
  T(0, 2) = vals[count++];
  T(1, 0) = vals[count++];
  T(1, 1) = vals[count++];
  T(1, 2) = vals[count++];
  T(2, 0) = vals[count++];
  T(2, 1) = vals[count++];
  T(2, 2) = vals[count++];

  // translation part
  T(0, 3) = vals[count++];
  T(1, 3) = vals[count++];
  T(2, 3) = vals[count++];

  M.matrix() = T;

  return M;
}

// Return the id of the rig given the index of the sensor
// in cam_names.
int RigSet::rigId(int cam_id) const {
  if (cam_id < 0 || cam_id >= cam_names.size()) 
    LOG(FATAL) << "Out of bounds sensor id.\n";
  
  std::string cam_name = cam_names[cam_id];
  
  for (size_t rig_it = 0; rig_it < cam_set.size(); rig_it++) {
    for (size_t cam_it = 0; cam_it < cam_set[rig_it].size(); cam_it++) {
      if (cam_set[rig_it][cam_it] == cam_name) {
        return rig_it;
      }
    }
  }

  // Should not arrive here
  LOG(FATAL) << "Could not look up in the rig the sensor: " << cam_name << "\n";
  return -1;
}

// The name of the ref sensor for the rig having the given sensor id
std::string RigSet::refSensor(int cam_id) const {
  return cam_set[rigId(cam_id)][0];
}
  
// Index in the list of sensors of the sensor with given name
int RigSet::sensorIndex(std::string const& sensor_name) const {
  auto it = std::find(cam_names.begin(), cam_names.end(), sensor_name);
  if (it == cam_names.end()) 
    LOG(FATAL) << "Could not find sensor in rig. That is unexpected. Offending sensor: "
               << sensor_name << ".\n";
  return it - cam_names.begin();
}

void RigSet::validate() const {
  
  if (cam_set.empty()) 
    LOG(FATAL) << "Found an empty set of rigs.\n";
  
  size_t num_cams = 0;
  std::set<std::string> all_cams; // checks for duplicates
  for (size_t rig_it = 0; rig_it < cam_set.size(); rig_it++) {
    if (cam_set[rig_it].empty()) 
      LOG(FATAL) << "Found a rig with no cams.\n";
    
    num_cams += cam_set[rig_it].size();
    for (size_t cam_it = 0; cam_it < cam_set[rig_it].size(); cam_it++)
      all_cams.insert(cam_set[rig_it][cam_it]);
  }
  
  if (num_cams != all_cams.size() || num_cams != cam_names.size())
    LOG(FATAL) << "Found a duplicate sensor name in the rig set.\n";
  
  if (num_cams != ref_to_cam_trans.size()) 
    LOG(FATAL) << "Number of sensors is not equal to number of ref-to-cam transforms.\n";
  
  if (num_cams != depth_to_image.size()) 
    LOG(FATAL) << "Number of sensors is not equal to number of depth-to-image transforms.\n";
  
  if (num_cams != ref_to_cam_timestamp_offsets.size()) 
    LOG(FATAL) << "Number of sensors is not equal to number of ref-to-cam timestamp offsets.\n";
  
  if (num_cams != cam_params.size()) 
    LOG(FATAL) << "Number of sensors is not equal to number of camera models.\n";
  
  for (size_t cam_it = 0; cam_it < cam_names.size(); cam_it++) {
    if (isRefSensor(cam_names[cam_it]) && ref_to_cam_timestamp_offsets[cam_it] != 0) 
      LOG(FATAL) << "The timestamp offsets for the reference sensors must be always 0.\n";
  }
}
  
// Create a rig set having a single rig  
RigSet RigSet::subRig(int rig_id) const {

  if (rig_id < 0 || rig_id >= cam_set.size()) 
    LOG(FATAL) << "Out of range in rig set.\n";

  RigSet sub_rig;
  sub_rig.cam_set.push_back(cam_set[rig_id]);

  // Add the relevant portion of each rig member
  for (size_t subrig_it = 0; subrig_it < cam_set[rig_id].size(); subrig_it++) {
    
    std::string sensor_name = cam_set[rig_id][subrig_it];
    int rig_index = sensorIndex(sensor_name);

    sub_rig.cam_names.push_back(cam_names[rig_index]);
    sub_rig.ref_to_cam_trans.push_back(ref_to_cam_trans[rig_index]);
    sub_rig.depth_to_image.push_back(depth_to_image[rig_index]);
    sub_rig.ref_to_cam_timestamp_offsets.push_back(ref_to_cam_timestamp_offsets[rig_index]);
    sub_rig.cam_params.push_back(cam_params[rig_index]);
  }
  sub_rig.validate();

  return sub_rig;
}

// Read real values after given tag. Ignore comments, so any line starting
// with #, and empty lines. If desired_num_vals >=0, validate that we
// read the desired number.
void readConfigVals(std::ifstream & f, std::string const& tag, int desired_num_vals,
                    Eigen::VectorXd & vals) {

  // Clear the output
  vals.resize(0);

  std::vector<double> local_vals;  // std::vector has push_back()
  std::string line;
  while (getline(f, line)) {

    // Remove everything after any point sign
    bool have_comment = (line.find('#') != line.npos);
    if (have_comment) {
      std::string new_line;
      for (size_t c = 0; c < line.size(); c++) {
        if (line[c] == '#') 
          break; // got to the pound sign
        
        new_line += line[c];
      }

      line = new_line;
    }
    
    // Here must remove anything after the pound sign
    
    if (line.empty() || line[0] == '#') continue;
    
    // Remove commas that occasionally appear in the file
    for (size_t c = 0; c < line.size(); c++) {
      if (line[c] == ',') 
        line[c] = ' ';
    }

    std::istringstream iss(line);
    std::string token;
    iss >> token;
    std::string val; 
    while (iss >> val)
      local_vals.push_back(atof(val.c_str()));

    if (token == "") 
      continue; // likely just whitespace is present on the line
    
    if (token != tag) throw std::runtime_error("Could not read value for: " + tag);

    // Copy to Eigen::VectorXd
    vals.resize(local_vals.size());
    for (int it = 0; it < vals.size(); it++) vals[it] = local_vals[it];

    if (desired_num_vals >= 0 && vals.size() != desired_num_vals)
      throw std::runtime_error("Read an incorrect number of values for: " + tag);

    return;
  }

  throw std::runtime_error("Could not read value for: " + tag);
}

// Read strings separated by spaces after given tag. Ignore comments, so any line starting
// with #, and empty lines. If desired_num_vals >=0, validate that we
// read the desired number.
void readConfigVals(std::ifstream & f, std::string const& tag, int desired_num_vals,
                    std::vector<std::string> & vals) {

  // Clear the output
  vals.resize(0);

  std::string line;
  while (getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;

    std::istringstream iss(line);
    std::string token;
    iss >> token;
    std::string val;
    while (iss >> val) {
      vals.push_back(val);
    }

    if (token != tag) 
      throw std::runtime_error("Could not read value for: " + tag);

    if (desired_num_vals >= 0 && vals.size() != desired_num_vals)
      throw std::runtime_error("Read an incorrect number of values for: " + tag);

    return;
  }

  throw std::runtime_error("Could not read value for: " + tag);
}

// Read a rig configuration. Check if the transforms among the sensors
// on the rig is not 0, in that case will use it.
void readRigConfig(std::string const& rig_config, bool have_rig_transforms, RigSet & R) {

  try {
    // Initialize the outputs
    R = RigSet();

    // Open the file
    vw::vw_out() << "Reading: " << rig_config << "\n";
    std::ifstream f;
    f.open(rig_config.c_str(), std::ios::in);
    if (!f.is_open()) 
      LOG(FATAL) << "Cannot open rig file for reading: " << rig_config << "\n";

    int ref_sensor_count = 0;
    Eigen::VectorXd vals;
    std::vector<std::string> str_vals;

    // Read each sensor
    while (1) {
      std::string ref_sensor_name;
      int curr_pos = f.tellg(); // where we are in the file
      // Read the reference sensor
      try {
        readConfigVals(f, "ref_sensor_name:", 1, str_vals);
        ref_sensor_name = str_vals[0];
        ref_sensor_count++; // found a ref sensor
        R.cam_set.resize(ref_sensor_count);
      } catch (...) {
        // No luck, go back to the line we tried to read, and continue reading other fields
        f.seekg(curr_pos, std::ios::beg);
      }
      
      // TODO(oalexan1): Must test that ref_sensor_name is unique.
      // Also that the first sensor in each rig is the reference sensor.
      // This must be tested with one and several rigs.
      
      try {
        readConfigVals(f, "sensor_name:", 1, str_vals);
      } catch(...) {
        // Likely no more sensors
        return;
      }
      std::string sensor_name = str_vals[0];

      // It is convenient to store each sensor in cam_set, which has the rig set structure,
      // and in R.cam_names, which is enough for almost all processing.
      R.cam_set.back().push_back(sensor_name);
      R.cam_names.push_back(sensor_name);
      
      readConfigVals(f, "focal_length:", 1, vals);
      double focal_length = vals[0];

      readConfigVals(f, "optical_center:", 2, vals);
      Eigen::Vector2d optical_center(vals[0], vals[1]);

      // Read distortion
      camera::DistortionType dist_type = camera::NO_DISTORTION;
      readConfigVals(f, "distortion_coeffs:", -1, vals);
      if (vals.size() != 0 && vals.size() != 1 && vals.size() != 4 && vals.size() < 5)
        LOG(FATAL) << "Expecting 0, 1, 4, 5, or more distortion coefficients.\n";
      Eigen::VectorXd distortion = vals;
      readConfigVals(f, "distortion_type:", 1, str_vals);
      if (distortion.size() == 0 && str_vals[0] != camera::NO_DISTORTION_STR)
        LOG(FATAL) << "When there are no distortion coefficients, distortion type must be: "
                   << camera::NO_DISTORTION_STR << "\n";
      
      // For backward compatibility, accept camera::FISHEYE_DISTORTION_STR with
      // 1 distortion coefficient, but use the FOV model
      if (distortion.size() == 1 && str_vals[0] == camera::FISHEYE_DISTORTION_STR)
        str_vals[0] = camera::FOV_DISTORTION_STR;
      
      // Validation 
      if (distortion.size() == 1 && str_vals[0] != camera::FOV_DISTORTION_STR)
          LOG(FATAL) << "When there is 1 distortion coefficient, distortion type must be: "
                     << camera::FOV_DISTORTION_STR << "\n";
      if (distortion.size() == 4 &&
          str_vals[0] != camera::FISHEYE_DISTORTION_STR &&
          str_vals[0] != camera::RADTAN_DISTORTION_STR)
        LOG(FATAL) << "When there are 4 distortion coefficients, distortion type "
                    << "must be: " << camera::FISHEYE_DISTORTION_STR << " or "
                    << camera::RADTAN_DISTORTION_STR << "\n";
      if (distortion.size() == 5 &&
          str_vals[0] != camera::RADTAN_DISTORTION_STR)
        LOG(FATAL) << "When there are 5 distortion coefficient, distortion type must be: "
                   << camera::RADTAN_DISTORTION_STR << "\n";
      if ((distortion.size() > 5) &&
          str_vals[0] != camera::RPC_DISTORTION_STR)
        LOG(FATAL) << "When there are more than 5 distortion coefficients, distortion "
                   << "type must be: " << camera::RPC_DISTORTION_STR << "\n";

      // Set distortion type based on str_vals[0]
      if (str_vals[0] == camera::NO_DISTORTION_STR) 
        dist_type = camera::NO_DISTORTION;
      else if (str_vals[0] == camera::FOV_DISTORTION_STR) 
        dist_type = camera::FOV_DISTORTION;
      else if (str_vals[0] == camera::FISHEYE_DISTORTION_STR) 
        dist_type = camera::FISHEYE_DISTORTION;
      else if (str_vals[0] == camera::RADTAN_DISTORTION_STR)
        dist_type = camera::RADTAN_DISTORTION;
      else if (str_vals[0] == camera::RPC_DISTORTION_STR)
        dist_type = camera::RPC_DISTORTION;
      else
        LOG(FATAL) << "Unknown distortion type: " << str_vals[0] << "\n";
      
      readConfigVals(f, "image_size:", 2, vals);
      Eigen::Vector2i image_size(vals[0], vals[1]);

      readConfigVals(f, "distorted_crop_size:", 2, vals);
      Eigen::Vector2i distorted_crop_size(vals[0], vals[1]);

      readConfigVals(f, "undistorted_image_size:", 2, vals);
      Eigen::Vector2i undistorted_image_size(vals[0], vals[1]);
      camera::CameraParameters params(image_size, focal_length, optical_center, 
                                      distortion, dist_type);
      
      params.SetDistortedCropSize(distorted_crop_size);
      params.SetUndistortedSize(undistorted_image_size);
      R.cam_params.push_back(params);

      readConfigVals(f, "ref_to_sensor_transform:", 12, vals);
      R.ref_to_cam_trans.push_back(vecToAffine(vals));

      // Sanity check
      if (have_rig_transforms &&
          R.ref_to_cam_trans.back().matrix() == 0 * R.ref_to_cam_trans.back().matrix()) {
        LOG(FATAL) << "Failed to read valid transforms between the sensors on the rig\n";
      }

      readConfigVals(f, "depth_to_image_transform:", 12, vals);
      R.depth_to_image.push_back(vecToAffine(vals));

      readConfigVals(f, "ref_to_sensor_timestamp_offset:", 1, vals);
      double timestamp_offset = vals[0];
      R.ref_to_cam_timestamp_offsets.push_back(timestamp_offset);
    }
    
    // Sanity check
    if (have_rig_transforms) {
      for (size_t cam_it = 0; cam_it < R.cam_names.size(); cam_it++) {
        if (R.isRefSensor(R.cam_names[cam_it]) &&
            R.ref_to_cam_trans[cam_it].matrix() != Eigen::Affine3d::Identity().matrix())
          LOG(FATAL) << "The transform from the reference sensor to itself must be the identity.\n";
      }
    }
    
    R.validate();
    
  } catch(std::exception const& e) {
    LOG(FATAL) << e.what() << "\n";
  }

  return;
}

} // end namespace asp