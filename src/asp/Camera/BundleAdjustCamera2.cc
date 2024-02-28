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

/// \file BundleAdjustCamera2.cc
///

// Move some code here because BundleAdjustCamera.cc is getting too big.
// These are all very related functions that are not easy to split into
// separate files by functionality.
// TODO(oalexan1): Move most of BundleAdjustCamera.h code to here, and put it
// all in the asp namespace. 

#include <asp/Camera/BundleAdjustCamera.h>
#include <asp/Camera/CsmModel.h>
#include <asp/Core/IpMatchingAlgs.h>         // Lightweight header
#include <asp/Camera/LinescanUtils.h>
#include <asp/Camera/RPC_XML.h>
#include <asp/Camera/CameraErrorPropagation.h>
#include <asp/IsisIO/IsisInterface.h>
#include <asp/Core/StereoSettings.h>

#include <vw/Camera/PinholeModel.h>
#include <vw/Camera/OpticalBarModel.h>
#include <vw/Camera/CameraImage.h>

#include <boost/algorithm/string.hpp>

#include <string>

using namespace vw;
using namespace vw::camera;
using namespace vw::ba;

namespace asp {

// Read previous adjustments and store them in params. The params must be well-formed
// by now, but any prior adjustment in them will be overwritten.
void put_adjustments_in_params(std::string const& input_prefix,
                               std::vector<std::string> const& image_files,
                               std::vector<std::string> const& camera_files,
                               // Output
                               asp::BAParams & param_storage) {

  const size_t num_cameras = param_storage.num_cameras();
  
  for (size_t icam = 0; icam < num_cameras; icam++) {
    std::string adjust_file
      = asp::bundle_adjust_file_name(input_prefix, image_files[icam], camera_files[icam]);
  
    vw_out() << "Reading input adjustment: " << adjust_file << std::endl;
    double * cam_ptr = param_storage.get_camera_ptr(icam);
    CameraAdjustment adjustment;
    adjustment.read_from_adjust_file(adjust_file);
    adjustment.pack_to_array(cam_ptr);
  }
}

// Take input cameras and corrections in param_storage, and create new cameras
// incorporating the corrections. 
void create_corrected_cameras(std::vector<vw::CamPtr> const& input_cameras,
                              asp::BAParams const& param_storage,
                              std::vector<vw::CamPtr> & out_cameras) {
  const size_t num_cameras = param_storage.num_cameras();
  out_cameras.resize(num_cameras);

  for (size_t icam = 0; icam < num_cameras; icam++) {
    CameraAdjustment correction(param_storage.get_camera_ptr(icam));
    out_cameras[icam] = vw::CamPtr(new camera::AdjustedCameraModel(input_cameras[icam],
                                   correction.position(), correction.pose()));
  }                              
}

/// Create the param storage. Collect in it any input adjustments and initial transform.
/// Return a copy of the cameras having these adjustments applied to them.
bool init_cams(asp::BaBaseOptions const& opt, asp::BAParams & param_storage,
    std::string const& initial_transform_file, vw::Matrix<double> const& initial_transform,
    std::vector<boost::shared_ptr<camera::CameraModel> > &new_cam_models) {

  bool cameras_changed = false;
  
  // Initialize all of the camera adjustments to zero.
  param_storage.init_cams_as_zero();
  const size_t num_cameras = param_storage.num_cameras();

  // Sanity check, must have same number of cameras
  if (num_cameras != opt.camera_models.size())
      vw_throw(ArgumentErr() << "Expecting " << num_cameras << " cameras, got "
                           << opt.camera_models.size() << ".\n");

  // Read the adjustments from a previous run, if present. Put them in params.
  if (opt.input_prefix != "") {
    put_adjustments_in_params(opt.input_prefix, opt.image_files, opt.camera_files, 
                              param_storage); // output
    cameras_changed = true;
  }

  // Apply any initial transform to params
  if (initial_transform_file != "") {
    if (opt.stereo_session == "csm") {
      double scale = pow(vw::math::det(initial_transform), 1.0/3.0);
      if (std::abs(scale - 1.0) > 1e-6) {
        // TODO(oalexan1): This gives wrong results for now so needs to be sorted out.
        // Likely the only way to apply a scale to a linescan camera is to multiply
        // all camera centers by the scale. Using a rotation and translation center
        // like for AdjustedCameraModel is not enough.
        vw_throw(ArgumentErr()
                 << "CSM camera models do not support applying a transform with a scale.\n");
      }
    }

    // Update param_storage with the alignment. This may be on top of any initial adjustment.
    // from the previous code, already contained in param_storage. Cameras
    // do not change.
    apply_transform_to_params(initial_transform, param_storage, opt.camera_models);
    cameras_changed = true;
  }
  
  // Make a copy of the cameras with given corrections in param_storage, incorporating
  // any adjustments and initial transform.
  create_corrected_cameras(opt.camera_models, param_storage, new_cam_models);
  
  return cameras_changed;
}

/// Specialization for pinhole cameras
bool init_cams_pinhole(asp::BaBaseOptions const& opt, asp::BAParams & param_storage,
     std::string const& initial_transform_file, vw::Matrix<double> const& initial_transform,
     std::vector<vw::CamPtr> & new_cam_models) {

  bool cameras_changed = false;
  
  // Copy the camera parameters from the models to param_storage
  const size_t num_cameras = param_storage.num_cameras();

  for (int icam=0; icam < num_cameras; icam++) {
    PinholeModel* pin_ptr = dynamic_cast<PinholeModel*>(opt.camera_models[icam].get());
    vw::vw_out() << "Loading input model: " << *pin_ptr << std::endl;

    // Make a deep copy of the camera, including of the lens distortion
    PinholeModel pin_cam = *pin_ptr;
    
    // Read the adjustments from a previous run, if present
    if (opt.input_prefix != "") {
      std::string adjust_file
        = asp::bundle_adjust_file_name(opt.input_prefix, opt.image_files[icam],
                                       opt.camera_files[icam]);
      vw_out() << "Reading input adjustment: " << adjust_file << std::endl;
      CameraAdjustment adjustment;
      adjustment.read_from_adjust_file(adjust_file);

      // Strictly speaking, it is not necessary to call unadjusted_model(), as
      // in bundle_adjust the input cameras are loaded unadjusted, unlike in stereo.
      AdjustedCameraModel adj_cam(vw::camera::unadjusted_model(opt.camera_models[icam]),
                                  adjustment.position(), adjustment.pose());
      vw::Matrix4x4 ecef_transform = adj_cam.ecef_transform();
      pin_cam.apply_transform(ecef_transform);
    
      cameras_changed = true;
    }
    
    // Apply any initial transform to the pinhole cameras. This may be on top
    // of any initial adjustment. 
    if (initial_transform_file != "") {
      pin_cam.apply_transform(initial_transform);
      cameras_changed = true;
    }
    
    pack_pinhole_to_arrays(pin_cam, icam, param_storage);
  } // End loop through cameras

  // Fill out the new camera model vector
  new_cam_models.resize(num_cameras);
  for (size_t icam = 0; icam < num_cameras; icam++){

    PinholeModel* in_cam  = dynamic_cast<PinholeModel*>(opt.camera_models[icam].get());

    // Start with a copy of the input camera. Then overwrite its parameters.
    // Note that out_cam originally shares the distortion with in_cam, as
    // that one is a pointer. But the second line below takes care of that.
    // This is awkward. An object having members as pointers is not safe.
    PinholeModel* out_cam = new PinholeModel(*in_cam);
    *out_cam = transformedPinholeCamera(icam, param_storage, *in_cam);

    new_cam_models[icam] = vw::CamPtr(out_cam);
  }

  return cameras_changed;
}

// TODO: Share more code with the similar pinhole case.
/// Specialization for optical bar cameras.
bool init_cams_optical_bar(asp::BaBaseOptions const& opt, asp::BAParams & param_storage,
                    std::string const& initial_transform_file, 
                    vw::Matrix<double> const& initial_transform,
                    std::vector<vw::CamPtr> &new_cam_models) {

  if (opt.input_prefix != "")
    vw::vw_throw(vw::ArgumentErr()
                 << "Applying initial adjustments to optical bar cameras "
                 << "and --inline-adjustments is not implemented. "
                 << "Remove this option.\n");

  bool cameras_changed = false;

  // Copy the camera parameters from the models to param_storage
  const size_t num_cameras = param_storage.num_cameras();
  for (int icam=0; icam < num_cameras; icam++) {
    vw::camera::OpticalBarModel* bar_ptr
      = dynamic_cast<vw::camera::OpticalBarModel*>(opt.camera_models[icam].get());
    vw::vw_out() << "Loading input model: " << *bar_ptr << std::endl;
    pack_optical_bar_to_arrays(*bar_ptr, icam, param_storage);
  } // End loop through cameras

  // Apply any initial transform to the pinhole cameras
  if (initial_transform_file != "") {
    apply_transform_to_cameras_optical_bar(initial_transform, param_storage, opt.camera_models);
    cameras_changed = true;
  }

  // Fill out the new camera model vector
  new_cam_models.resize(num_cameras);
  for (size_t icam = 0; icam < num_cameras; icam++) {

    vw::camera::OpticalBarModel* in_cam
      = dynamic_cast<vw::camera::OpticalBarModel*>(opt.camera_models[icam].get());

    // Start with a copy of the input camera, then overwrite its content
    vw::camera::OpticalBarModel* out_cam = new vw::camera::OpticalBarModel(*in_cam); 
    *out_cam = transformedOpticalBarCamera(icam, param_storage, *in_cam);

    new_cam_models[icam] = boost::shared_ptr<camera::CameraModel>(out_cam);
  }

  return cameras_changed;
}

// TODO: Share more code with the similar pinhole case.
/// Specialization for CSM
bool init_cams_csm(asp::BaBaseOptions const& opt, asp::BAParams & param_storage,
                   std::string const& initial_transform_file, 
                   vw::Matrix<double> const& initial_transform,
                   std::vector<vw::CamPtr> &new_cam_models) {

  bool cameras_changed = false;

  // Apply any adjustments inline. Copy the camera parameters from the models to
  // param_storage. Do not copy the adjustments, as they are already applied
  // to the camera proper.
  const size_t num_cameras = param_storage.num_cameras();
  for (int icam = 0; icam < num_cameras; icam++) {
    asp::CsmModel* csm_ptr
        = dynamic_cast<asp::CsmModel*>(opt.camera_models[icam].get());

    // Read the adjustments from a previous run, if present. Apply them
    // inline to the camera model.
    if (opt.input_prefix != "") {
      std::string adjust_file
        = asp::bundle_adjust_file_name(opt.input_prefix, opt.image_files[icam],
                                       opt.camera_files[icam]);
      vw_out() << "Reading input adjustment: " << adjust_file << std::endl;
      CameraAdjustment adjustment;
      adjustment.read_from_adjust_file(adjust_file);

      // Strictly speaking, it is not necessary to call unadjusted_model(), as
      // in bundle_adjust the input cameras are loaded unadjusted, unlike in stereo.
      AdjustedCameraModel adj_cam(vw::camera::unadjusted_model(opt.camera_models[icam]),
                                  adjustment.position(), adjustment.pose());
      vw::Matrix4x4 ecef_transform = adj_cam.ecef_transform();
      csm_ptr->applyTransform(ecef_transform);
    
      cameras_changed = true;
    }

    // This does not copy the camera position and orientation, only the intrinsics    
    pack_csm_to_arrays(*csm_ptr, icam, param_storage); 
  } // End loop through cameras

  // Apply any initial transform to the CSM cameras
  if (initial_transform_file != "") {
    // Apply the transform to the cameras inline. This modifies opt.camera_models.
    // The transform does not get copied to param_storage. Only intrinsics get copied.
    apply_transform_to_cameras_csm(initial_transform, param_storage, opt.camera_models);
    cameras_changed = true;
  }

  // Fill out the new camera model vector
  new_cam_models.resize(num_cameras);
  for (size_t icam = 0; icam < num_cameras; icam++){

    asp::CsmModel* in_cam
      = dynamic_cast<asp::CsmModel*>(opt.camera_models[icam].get());
    if (in_cam == NULL)
      vw_throw(ArgumentErr() << "Expecting a CSM camera.\n");

    auto out_cam = transformedCsmCamera(icam, param_storage, *in_cam);
    new_cam_models[icam] = boost::shared_ptr<camera::CameraModel>(out_cam);
  }

  return cameras_changed;
}

/// Write a pinhole camera file to disk.
void write_pinhole_output_file(asp::BaBaseOptions const& opt, int icam,
                               vw::cartography::Datum const& datum,
                               asp::BAParams const& param_storage) {

  // Get the output file path
  std::string cam_file = asp::bundle_adjust_file_name(opt.out_prefix,
                                                      opt.image_files [icam],
                                                      opt.camera_files[icam]);
  cam_file = boost::filesystem::path(cam_file).replace_extension("tsai").string();

  // Get the camera model from the original one with parameters in
  // param_storage applied to it (which could be original ones or optimized). 
  // Note that we do not modify the original camera.
  vw::camera::PinholeModel const* in_cam
    = dynamic_cast<vw::camera::PinholeModel const*>(opt.camera_models[icam].get());
  if (in_cam == NULL)
    vw_throw(ArgumentErr() << "Expecting a pinhole camera.\n");
  vw::camera::PinholeModel out_cam = transformedPinholeCamera(icam, param_storage, *in_cam);

  vw::vw_out() << "Writing: " << cam_file << std::endl;
  out_cam.write(cam_file);
  vw::vw_out() << "Writing output model: " << out_cam << std::endl;

  bool has_datum = (datum.name() != asp::UNSPECIFIED_DATUM);
  if (has_datum) {
    vw::vw_out() << "Camera center for " << cam_file << ": "
                  << datum.cartesian_to_geodetic(out_cam.camera_center())
                  << " (longitude, latitude, height above datum(m))\n\n";
  }
}


/// Write an optical bar camera file to disk.
void write_optical_bar_output_file(asp::BaBaseOptions const& opt, int icam,
                                   vw::cartography::Datum const& datum,
                                   asp::BAParams const& param_storage) {

  // Get the output file path
  std::string cam_file = asp::bundle_adjust_file_name(opt.out_prefix,
                                                      opt.image_files [icam],
                                                      opt.camera_files[icam]);
  cam_file = boost::filesystem::path(cam_file).replace_extension("tsai").string();

  // Get the final camera model from the original one with optimized
  // parameters applied to it. Note that we do not modify the original
  // camera.
  vw::camera::OpticalBarModel* in_cam
    = dynamic_cast<vw::camera::OpticalBarModel*>(opt.camera_models[icam].get());
  if (in_cam == NULL)
    vw_throw(ArgumentErr() << "Expecting an optical bar camera.\n");
  vw::camera::OpticalBarModel out_cam 
    = transformedOpticalBarCamera(icam, param_storage, *in_cam);
  
  vw::vw_out() << "Writing: " << cam_file << std::endl;
  out_cam.write(cam_file);
  vw::vw_out() << "Writing output model: " << out_cam << std::endl;

  bool has_datum = (datum.name() != asp::UNSPECIFIED_DATUM);
  if (has_datum) {
    vw::vw_out() << "Camera center for " << cam_file << ": "
                  << datum.cartesian_to_geodetic(out_cam.camera_center())
                  << " (longitude, latitude, height above datum(m))\n\n";
  }
}

/// Write a CSM camera file to disk. Assumes that the intriniscs are optimized.
void write_csm_output_file(asp::BaBaseOptions const& opt, int icam,
                           vw::cartography::Datum const& datum,
                           asp::BAParams const& param_storage) {

  // Get the output file path
  std::string cam_file = asp::bundle_adjust_file_name(opt.out_prefix,
                                                      opt.image_files [icam],
                                                      opt.camera_files[icam]);
  cam_file = asp::csmStateFile(cam_file);

  // Get the final camera model from the original one with optimized
  // parameters applied to it. Note that we do not modify the original
  // camera.
  asp::CsmModel const* in_cam
    = dynamic_cast<asp::CsmModel const*>(opt.camera_models[icam].get()); 
  if (in_cam == NULL)
    vw_throw(ArgumentErr() << "Expecting a CSM camera.\n");
  boost::shared_ptr<asp::CsmModel> out_cam
    = transformedCsmCamera(icam, param_storage, *in_cam);
     
  bool has_datum = (datum.name() != asp::UNSPECIFIED_DATUM);
  if (has_datum)
    vw::vw_out() << "Camera center for " << cam_file << ": "
                 << datum.cartesian_to_geodetic(out_cam->camera_center(vw::Vector2()))
                 << " (longitude, latitude, height above datum(m))\n";

  // Save the updated state     
  out_cam->saveState(cam_file);

  if (opt.update_isis_cubes_with_csm_state) {
    // Save the CSM state to the image file. Wipe any spice info.
    std::string image_name = opt.image_files[icam]; 
    std::string plugin_name = out_cam->plugin_name();
    std::string model_name  = out_cam->model_name();
    std::string model_state = out_cam->model_state();
    vw::vw_out() << "Adding updated CSM state to image file: " << image_name << std::endl;
    asp:isis::saveCsmStateToIsisCube(image_name, plugin_name, model_name, model_state);
  }
}

/// Write a csm camera state file to disk. Assumes no intrinsics are optimized.
void write_csm_output_file_no_intr(asp::BaBaseOptions const& opt, int icam,
                                   std::string const& adjustFile, 
                                   asp::BAParams const& param_storage) {
  
  CameraAdjustment cam_adjust(param_storage.get_camera_ptr(icam));
  AdjustedCameraModel adj_cam(vw::camera::unadjusted_model(opt.camera_models[icam]),
                              cam_adjust.position(), cam_adjust.pose());
  
  vw::Matrix4x4 ecef_transform = adj_cam.ecef_transform();
  std::string csmFile          = asp::csmStateFile(adjustFile);
  asp::CsmModel * csm_model    = asp::csm_model(opt.camera_models[icam], 
                                                opt.stereo_session);

  // Save a transformed copy of the camera model
  boost::shared_ptr<asp::CsmModel> out_cam;
  csm_model->deep_copy(out_cam);
  out_cam ->applyTransform(ecef_transform);
  out_cam->saveState(csmFile);

  if (opt.update_isis_cubes_with_csm_state) {
    // Save the CSM state to the image file. Wipe any spice info.
    std::string image_name = opt.image_files[icam]; 
    std::string plugin_name = out_cam->plugin_name();
    std::string model_name  = out_cam->model_name();
    std::string model_state = out_cam->model_state();
    vw::vw_out() << "Adding updated CSM state to image file: " << image_name << std::endl;
    asp:isis::saveCsmStateToIsisCube(image_name, plugin_name, model_name, model_state);
  }
}

// Read image and camera lists. Can have several comma-separated lists
// in image_list and camera_list, when sharing intrinsics per sensor.
void read_image_cam_lists(std::string const& image_list, 
                std::string const& camera_list,
                std::vector<std::string> & images_or_cams,
                asp::IntrinsicOptions & intrinsics_opts) {

  // Wipe the output
  images_or_cams.clear();
  intrinsics_opts.share_intrinsics_per_sensor = false;
  intrinsics_opts.cam2sensor.clear();
  intrinsics_opts.num_sensors = 0; // must be initialized to zero

  // See if there comma-separated lists passed in the image list
  if (image_list.find(",") == std::string::npos && 
      camera_list.find(",") == std::string::npos) {
    // Single list, so just read the lists as usual, and return
    asp::read_list(image_list, images_or_cams);
    if (camera_list.empty()) {
      // This is usual for ISIS cameras.
      vw_out() << "An image list was provided but not a camera list.\n";
    } else {
      std::vector<std::string> cams;
      asp::read_list(camera_list, cams);
      if (images_or_cams.size() != cams.size())
        vw_throw(ArgumentErr() << "Expecting the same number of images and cameras.\n");
      for (size_t it = 0; it < cams.size(); it++) 
        images_or_cams.push_back(cams[it]);
    }
    return;
  }
  
  vw_out() << "Multiple image lists and camera lists were passed in. " 
           << "Solving for intrinsics per sensor.\n";

  // This is a very important bit
  intrinsics_opts.share_intrinsics_per_sensor = true;

  std::vector<std::string> image_lists, camera_lists;
  boost::split(image_lists, image_list, boost::is_any_of(","));
  boost::split(camera_lists, camera_list, boost::is_any_of(","));

  if (image_lists.size() != camera_lists.size())
    vw_throw(ArgumentErr() << "Expecting the same number of image and camera lists. "
      << "They must be separated by commas on input.\n");

  // Read separately the images and cameras
  std::vector<std::string> images, cameras;
  for (size_t sensor_it = 0; sensor_it < image_lists.size(); sensor_it++) {
    std::vector<std::string> local_images, local_cameras;
    asp::read_list(image_lists[sensor_it], local_images);
    asp::read_list(camera_lists[sensor_it], local_cameras);
    if (local_images.size() != local_cameras.size() || local_images.empty())
      vw_throw(ArgumentErr() << "Expecting the same positive number of images and cameras "
      << "in lists: '" << image_lists[sensor_it] << "' and '" 
      << camera_lists[sensor_it] << "'.\n");

    // Append to the global lists
    images.insert(images.end(), local_images.begin(), local_images.end());
    cameras.insert(cameras.end(), local_cameras.begin(), local_cameras.end());

    // Create the map from camera index to sensor index
    for (size_t cam_it = 0; cam_it < local_cameras.size(); cam_it++)
      intrinsics_opts.cam2sensor.push_back(sensor_it);
  }

  intrinsics_opts.num_sensors = image_lists.size();
  vw_out() << "Number of sensors: " << intrinsics_opts.num_sensors << std::endl;

  // Append both images and cameras to images_or_cams
  images_or_cams.insert(images_or_cams.end(), images.begin(), images.end());
  images_or_cams.insert(images_or_cams.end(), cameras.begin(), cameras.end());
  return;
} 

// When distortion params are shared, their number must agree
void distortion_sanity_check(std::vector<int> const& num_dist_params,
                             IntrinsicOptions const& intrinsics_opts,
                             std::vector<double> const& intrinsics_limits) {

  // If nothing is shared, there is nothing to do

  // If all distortion params are shared, all sizes must agree
  if (!intrinsics_opts.share_intrinsics_per_sensor && 
      intrinsics_opts.distortion_shared) {
    for (size_t it = 1; it < num_dist_params.size(); it++) {
      if (num_dist_params[it] != num_dist_params[0])
           vw_throw(ArgumentErr() << "When sharing distortion parameters, "
             << "they must have the same size.\n");
    }
  }

  // If distortion is shared per sensor
  if (intrinsics_opts.share_intrinsics_per_sensor) {
    std::vector<std::set<int>> dist_sizes(intrinsics_opts.num_sensors);
    for (size_t cam_it = 0; cam_it < num_dist_params.size(); cam_it++) {
      int sensor_it = intrinsics_opts.cam2sensor[cam_it];
      dist_sizes[sensor_it].insert(num_dist_params[cam_it]); // all found sizes
    }   
    // Now check that each dist_sizes[sensor_it] has size 1
    for (size_t sensor_it = 0; sensor_it < intrinsics_opts.num_sensors; sensor_it++) {
      if (dist_sizes[sensor_it].size() != 1)
        vw_throw(ArgumentErr() << "When sharing distortion parameters per sensor, "
         << "they must have the same size for all cameras of the same sensor.\n");
    }
  }
  
  // Intrinsics limits only can be used for now when all distortion vectors have
  // the same size. This could be fixed but it is a rarely used option
  // and would require a lot of bookkeeping.
  if (!intrinsics_limits.empty()) {
    bool have_same_size = true;
    for (size_t it = 1; it < num_dist_params.size(); it++) {
      if (num_dist_params[it] != num_dist_params[0]) {
        have_same_size = false;
        break;
      }
    }
    if (!have_same_size)
      vw_throw(ArgumentErr() << "When using --intrinsics-limits, all cameras "
               << "must have the same number of distortion coefficients.\n");
  }
  
  return;
}

// A little function to replace separators with space. Note that the backslash
// is a separator, in case, it used as a continuation line.
void replace_separators_with_space(std::string & str) {
  std::string sep = "\\:;, \t\r\n";
  for (size_t it = 0; it < sep.size(); it++) 
    std::replace(str.begin(), str.end(), sep[it], ' ');
}

// Split a string into a vector of strings with space as separator
std::vector<std::string> split_str_with_space(std::string const& str) {
  std::istringstream is(str);
  std::vector<std::string> ret;
  std::string val;
  while (is >> val) 
    ret.push_back(val);
  return ret;
}

// Function that tells if a string is a non-negative integer
bool is_str_non_neg_integer(std::string const& str) {
  if (str.empty()) return false;
  for (size_t it = 0; it < str.size(); it++) {
    if (!isdigit(str[it])) return false;
  }
  return true;
}

// Parse format:
// "1:focal_length,optical_center 2:focal_length,other_intrinsics 3:none"  
// Applies when optimizing intrinsics per sensor.
// The numbers are sensor indices, starting with 1.
void fine_grained_parse(bool share_intrinsics_per_sensor,
                        int num_sensors,
                        std::vector<std::string> const& options, 
                        // Outputs
                        std::vector<bool> & float_center, 
                        std::vector<bool> & float_focus,
                        std::vector<bool> & float_distortion) {

  // Sanity checks
  if (!share_intrinsics_per_sensor) 
    vw_throw(ArgumentErr() << "fine_grained_parse() is only for when intrinsics are "
             << "optimized per sensor.\n");
  if (num_sensors <= 0)
    vw_throw(ArgumentErr() << "Expecting a positive number of sensors.\n");
  if (options.empty())
    vw_throw(ArgumentErr() << "Expecting at least one option.\n");
 
  // Wipe the outputs
  float_center.clear();
  float_focus.clear();
  float_distortion.clear();
  
  // It is convenient to initialize all to false. 0th element must always exist.
  for (int i = 0; i < std::max(num_sensors, 1); i++) {
    float_center.push_back(false);
    float_focus.push_back(false);
    float_distortion.push_back(false);
  }
  
  // First entity must be an integer. That is sensor id, starting from 1.
  if (!is_str_non_neg_integer(options[0]))
    vw_throw(ArgumentErr() << "Expecting an integer as the first option.\n");
  
  int sensor_id = atoi(options[0].c_str()) - 1; // subtract 1 to make it zero-based
 
  // check for duplicate ids
  std::set<int> seen_ids;
  // Iterate over options
  for (size_t it = 0; it < options.size(); it++) {
    
    // If it is an integer, update the sensor id, and continue.
    // Subtract 1 to make it zero-based.
    if (is_str_non_neg_integer(options[it])) {
      sensor_id = atoi(options[it].c_str()) - 1; 
      
      // Sensor id must be in bounds
      if (sensor_id < 0 || sensor_id >= num_sensors)
        vw_throw(ArgumentErr() << "Sensor id " << options[it] << " is out of bounds.\n");
      
      // If seen, that's a problem
      if (seen_ids.find(sensor_id) != seen_ids.end())
        vw_throw(ArgumentErr() << "Sensor id " << options[it] << " is repeated.\n");
      // Record as seen
      seen_ids.insert(sensor_id);
            
      continue;
    }
   
    // Handle the optical center
    if (options[it] == "optical_center") {
      float_center[sensor_id] = true;
      continue;
    }

    // Handle the focal length
    if (options[it] == "focal_length") {
      float_focus[sensor_id] = true;
      continue;
    }

    // Handle the distortion (other_intrinsics)
    if (options[it] == "other_intrinsics" || options[it] == "distortion") {
      float_distortion[sensor_id] = true;
      continue;
    }
    
    // For all, populate all fields
    if (options[it] == "all") {
      float_center[sensor_id]     = true;
      float_focus[sensor_id]      = true;
      float_distortion[sensor_id] = true;
      continue;
    }
    
    // For none, just skip
    if (options[it] == "none") {
      continue;
    }
    
    vw_throw(ArgumentErr() << "Found unknown option when parsing which "
              << "sensor intrinsics to float: " << options[it] << ".\n");
  } 
}                        

// Parse format:
// "focal_length optical_center other_intrinsics"
// Applies to all sensors and when not optimizing intrinsics per sensor.
void coarse_grained_parse(int num_sensors,
                          std::vector<std::string> const& options, 
                          // Outputs
                          std::vector<bool> & float_center, 
                          std::vector<bool> & float_focus,
                          std::vector<bool> & float_distortion) {

  // Wipe the outputs
  float_center.clear();
  float_focus.clear();
  float_distortion.clear();

  if (num_sensors < 0)
    vw_throw(ArgumentErr() << "Cameras were not parsed correctly.\n");
 
   // It is convenient to initialize all to false. 0th element must always exist.
  for (int i = 0; i < std::max(num_sensors, 1); i++) {
    float_center.push_back(false);
    float_focus.push_back(false);
    float_distortion.push_back(false);
  }

  // For now, populate only for sensor with id 0
  int sensor_id = 0;
  
  // Iterate over options
  for (size_t it = 0; it < options.size(); it++) {
    
     // Must not have an integer here
     if (is_str_non_neg_integer(options[0]))
        vw_throw(ArgumentErr() << "When parsing intrinsics to float, expecting a "
                 << "string, not an integer. Check your inputs.\n");

    // Handle the optical center
    if (options[it] == "optical_center") {
      float_center[sensor_id] = true;
      continue;
    }
    
    // Handle the focal length
    if (options[it] == "focal_length") {
      float_focus[sensor_id] = true;
      continue;
    }

    // Handle the distortion (other_intrinsics)
    if (options[it] == "other_intrinsics" || options[it] == "distortion") {
      float_distortion[sensor_id] = true;
      continue;
    }
    
    // For all, populate all fields
    if (options[it] == "all") {
      float_center[sensor_id]     = true;
      float_focus[sensor_id]      = true;
      float_distortion[sensor_id] = true;
      continue;
    }
    
    // For none, just skip
    if (options[it] == "none") {
      continue;
    }
    
    // We should not arrive here
    vw_throw(ArgumentErr() << "Found unknown option when parsing which "
              << "sensor intrinsics to float: " << options[it] << ".\n");
  } 
  
  // Distribute for all sensors.
  // This will happen only if we share intrinsics per sensor.
  for (int sensor_id = 0; sensor_id < num_sensors; sensor_id++) {
     float_center[sensor_id]     = float_center[0];
     float_focus[sensor_id]      = float_focus[0];
     float_distortion[sensor_id] = float_distortion[0];  
  }
  
  return;
}

void print_intr_vec(std::vector<bool> const& intrinsics, std::string const& name) {
  vw_out() << name << ": ";
  for (size_t it = 0; it < intrinsics.size(); it++)
    vw_out() << intrinsics[it] << " ";
  vw_out() << std::endl;
}
                         
/// For each option, the string must include a subset of the entries:
///  "focal_length, optical_center, distortion_params"
/// - Need the extra boolean to handle the case where --intrinsics-to-share
///   is provided as "" in order to share none of them.
void load_intrinsics_options(bool        solve_intrinsics,
                             bool        shared_is_specified,
                             std::string intrinsics_to_float_str, // make a copy
                             std::string intrinsics_to_share_str, // make a copy
                             asp::IntrinsicOptions & intrinsics_options) {

  // Share everything unless told otherwise
  intrinsics_options.focus_shared        = true;
  intrinsics_options.center_shared       = true;
  intrinsics_options.distortion_shared   = true;

  // We need these to be initialized even when not solving for intrinsics, as
  // the intrinsics are always added to the cost function when
  // --inline-adjustments is used. In that case they are just expected to be
  // kept fixed.
  intrinsics_options.float_center.resize(1, false);
  intrinsics_options.float_focus.resize(1, false);
  intrinsics_options.float_distortion.resize(1, false);
  
  if (((intrinsics_to_float_str != "") || (intrinsics_to_share_str != "")) 
      && !solve_intrinsics) {
    vw_throw( ArgumentErr() << "To be able to specify only certain intrinsics, "
                            << "the option --solve-intrinsics must be on.\n" );
  }

  if (!solve_intrinsics)
    return;
  
  // If the user did not specify which intrinsics to float, float all of them.
  boost::to_lower(intrinsics_to_float_str);
  if (intrinsics_to_float_str == "" || intrinsics_to_float_str == "all")
    intrinsics_to_float_str = "focal_length optical_center other_intrinsics";
  // This is the right place in which to turn 'none' to empty string,
  // which now will mean float nothing.
  if (intrinsics_to_float_str == "none") 
    intrinsics_to_float_str = "";

  // If the user did not specify which intrinsics to share, share all of them.
  boost::to_lower(intrinsics_to_share_str);
  if (!shared_is_specified) {
    intrinsics_to_share_str = "focal_length optical_center other_intrinsics";
  } else {
    // Otherwise, 'all' also means share all of them, 'none' means share none
    if (intrinsics_to_share_str == "all") 
      intrinsics_to_share_str = "focal_length optical_center other_intrinsics";
    if (intrinsics_to_share_str == "none")
      intrinsics_to_share_str = "";
  }

  if (intrinsics_options.share_intrinsics_per_sensor && shared_is_specified) 
    vw_out() << "When sharing intrinsics per sensor, option "
              << "--intrinsics-to-share is ignored. The intrinsics will "
              << "always be shared for a sensor and never across sensors.\n";

  // If sharing intrinsics per sensor, the only supported mode is that 
  // the intrinsics are always shared per sensor and never across sensors.
  if (shared_is_specified && !intrinsics_options.share_intrinsics_per_sensor) {
    intrinsics_options.focus_shared      = false;
    intrinsics_options.center_shared     = false;
    intrinsics_options.distortion_shared = false;
  }

  // Replace any separators (:;, \t\r\n) with spaces. It can be convenient to
  // use commas and colons as separators when passing in the options from the command line.
  asp::replace_separators_with_space(intrinsics_to_float_str);
  asp::replace_separators_with_space(intrinsics_to_share_str);

  // Parse float options. Supported formats:
  // "1:focal_length,optical_center 2:focal_length,other_intrinsics 3:none"  
  // "focal_length optical_center other_intrinsics"
  // In the first case, the numbers are sensor indices, starting with 1.
  std::vector<std::string> float_options = asp::split_str_with_space(intrinsics_to_float_str);
  if (!float_options.empty() && asp::is_str_non_neg_integer(float_options[0])) {
    asp::fine_grained_parse(intrinsics_options.share_intrinsics_per_sensor,
                            intrinsics_options.num_sensors,
                            float_options, 
                            intrinsics_options.float_center, 
                            intrinsics_options.float_focus,
                            intrinsics_options.float_distortion);
  } else {
    asp::coarse_grained_parse(intrinsics_options.num_sensors,
                              float_options, 
                              intrinsics_options.float_center, 
                              intrinsics_options.float_focus,
                              intrinsics_options.float_distortion);
  }
  // Useful reporting
  std::string center_name = "Optical center";
  std::string focus_name  = "Focal length";
  std::string dist_name   = "Other intrinsics (distortion)";  
  if (intrinsics_options.share_intrinsics_per_sensor) {
    vw_out() << "Intrinsics are shared for all cameras with given sensor.\n";
    vw_out() << "Number of sensors: " << intrinsics_options.num_sensors << "\n";
    vw_out() << "For each sensor (1 = floated, 0 = not floated):\n";
    print_intr_vec(intrinsics_options.float_center, center_name);
    print_intr_vec(intrinsics_options.float_focus, focus_name);
    print_intr_vec(intrinsics_options.float_distortion, dist_name);
  } else {
    vw_out() << "Intrinsics are shared for all or no cameras.\n";
    vw_out() << "(1 = floated, 0 = not floated)\n";
    vw_out() << center_name << ": " << intrinsics_options.float_center[0] <<"\n";
    vw_out() << focus_name  << ": " << intrinsics_options.float_focus[0] << "\n";
    vw_out() << dist_name   << ": " << intrinsics_options.float_distortion[0] << "\n";
  }

  // No parsing is done when sharing intrinsics per sensor, per above 
  std::string val; 
  if (shared_is_specified && !intrinsics_options.share_intrinsics_per_sensor) {
    std::istringstream is(intrinsics_to_share_str);
    while (is >> val) {
      if (val == "focal_length")
        intrinsics_options.focus_shared = true;
      else if (val == "optical_center")
        intrinsics_options.center_shared = true;
      else if (val == "other_intrinsics" || val == "distortion")
        intrinsics_options.distortion_shared = true;
      else
        vw_throw(ArgumentErr() << "Error: Found unknown intrinsic to share: " 
          << val << ".\n");
    }
  }

  std::string sensor_mode = " (across sensors): ";
  if (intrinsics_options.share_intrinsics_per_sensor)
    sensor_mode = " (per sensor): "; // useful clarification

  // Useful info
  vw_out() << "Sharing (1 = shared, 0 = not shared):\n";
  vw_out() << center_name << sensor_mode << intrinsics_options.center_shared << "\n";
  vw_out() << focus_name << sensor_mode  << intrinsics_options.focus_shared << "\n";
  vw_out() << dist_name << sensor_mode   << intrinsics_options.distortion_shared << "\n";
} // End function load_intrinsics_options

/// Parse the string of limits and make sure they are all valid pairs.
void parse_intrinsics_limits(std::string const& intrinsics_limits_str,
                             std::vector<double> & intrinsics_limits) {

  intrinsics_limits.clear();
  std::istringstream is(intrinsics_limits_str);
  double val;
  int    count = 0;
  while (is >> val) {
    intrinsics_limits.push_back(val);
    if (count % 2 == 1) {
      if (intrinsics_limits[count] < intrinsics_limits[count-1])
        vw_throw(vw::ArgumentErr()
                  << "Error: Intrinsic limit pairs must be min before max.\n");
    }
    count++;
  }

  if (count % 2 != 0)
    vw::vw_throw(vw::ArgumentErr()
              << "Error: Intrinsic limits must always be provided in min max pairs.\n");
}

/// Attempt to automatically create the overlap list file estimated
///  footprints for each of the input images.
/// - Currently this only supports cameras with Worldview style XML files.
void auto_build_overlap_list(asp::BaBaseOptions &opt, double lonlat_buffer) {

  typedef std::pair<std::string, std::string> StringPair;

  const size_t num_images = opt.camera_files.size();
  opt.overlap_list.clear();

  vw_out() << "Attempting to automatically estimate image overlaps...\n";
  int  num_overlaps = 0;
  bool read_success = false;

  // Loop through all image pairs
  for (size_t i = 0; i < num_images - 1; i++) {

    // Try to get the lonlat bounds for this image
    std::vector<vw::Vector2> pixel_corners_i, lonlat_corners_i;
    try {
      read_success = asp::read_WV_XML_corners(opt.camera_files[i], pixel_corners_i,
                                              lonlat_corners_i);
    } catch(...) {
      read_success = false;
    }
    if (!read_success) {
      vw_throw( ArgumentErr() << "Unable to get corner estimate from file: "
                              << opt.camera_files[i] << ".\n" );
    }

    vw::BBox2 bbox_i; // Convert to BBox
    for (size_t p = 0; p < lonlat_corners_i.size(); p++)
      bbox_i.grow(lonlat_corners_i[p]);
    bbox_i.expand(lonlat_buffer); // Only expand this bounding box by the buffer.

    for (size_t j = i+1; j < num_images; j++) {

      std::vector<vw::Vector2> pixel_corners_j, lonlat_corners_j;
      try {
        read_success = asp::read_WV_XML_corners(opt.camera_files[j], pixel_corners_j,
                                                lonlat_corners_j);
      } catch(...) {
        read_success = false;
      }
      if (!read_success) {
        vw_throw( ArgumentErr() << "Unable to get corner estimate from file: "
                                << opt.camera_files[j] << ".\n" );
      }

      vw::BBox2 bbox_j; // Convert to BBox
      for (size_t p = 0;  p < lonlat_corners_j.size(); p++)
        bbox_j.grow(lonlat_corners_j[p]);

      // Record the files if the bboxes overlap
      // - TODO: Use polygon intersection instead of bounding boxes!
      if (bbox_i.intersects(bbox_j)) {
        vw_out() << "Predicted overlap between images " << opt.image_files[i]
                 << " and " << opt.image_files[j] << std::endl;
        opt.overlap_list.insert(StringPair(opt.image_files[i], opt.image_files[j]));
        opt.overlap_list.insert(StringPair(opt.image_files[j], opt.image_files[i]));
        ++num_overlaps;
      }
    } // End inner loop through cameras
  } // End outer loop through cameras

  if (num_overlaps == 0)
    vw_throw( ArgumentErr() << "Failed to automatically detect any overlapping images!" );

  vw_out() << "Will try to match at " << num_overlaps << " detected overlaps\n.";
} // End function auto_build_overlap_list

// Parse data needed for error propagation. Note that horizontal_stddevs
// comes from the user, or is otherwise populated from cameras.
void setup_error_propagation(std::string const& session_name,
                             double horizontal_stddev,
                             std::vector<vw::CamPtr> const& cameras,
                             vw::Vector<double> & horizontal_stddev_vec) {

  // Initialize the output
  horizontal_stddev_vec.set_size(cameras.size());
  horizontal_stddev_vec.set_all(horizontal_stddev);

  bool message_printed = false;
  if (horizontal_stddev == 0.0) {
    // Read from cameras
    for (size_t icam = 0; icam < cameras.size(); icam++)
      horizontal_stddev_vec[icam] 
        = asp::horizontalStDevFromCamera(cameras[icam], message_printed);
  }
  
  asp::horizontalStdDevCheck(horizontal_stddev_vec, session_name);
}

// Find the cameras with the latest adjustments. Note that we do not modify
// opt.camera_models, but make copies as needed.
void calcOptimizedCameras(asp::BaBaseOptions const& opt,
                          asp::BAParams const& param_storage,
                          std::vector<vw::CamPtr> & optimized_cams) {

  optimized_cams.clear();
  
  int num_cameras = opt.image_files.size();
  for (int icam = 0; icam < num_cameras; icam++) {
    
    // TODO(oalexan1): The logic below may need to be a function and should be called
    // in a couple other places.
    switch (opt.camera_type) {
    case BaCameraType_Pinhole:
      {
        vw::camera::PinholeModel const* in_cam
          = dynamic_cast<vw::camera::PinholeModel const*>(opt.camera_models[icam].get());
        if (in_cam == NULL)
          vw_throw(ArgumentErr() << "Expecting a pinhole camera.\n");
        vw::camera::PinholeModel * out_cam = new PinholeModel();
        *out_cam = transformedPinholeCamera(icam, param_storage, *in_cam);
        optimized_cams.push_back(vw::CamPtr(out_cam));
      }
      break;
    case BaCameraType_OpticalBar:
      {
        vw::camera::OpticalBarModel const* in_cam
          = dynamic_cast<vw::camera::OpticalBarModel const*>(opt.camera_models[icam].get());
        if (in_cam == NULL)
          vw_throw(ArgumentErr() << "Expecting an optical bar camera.\n");
        vw::camera::OpticalBarModel * out_cam = new OpticalBarModel();
        *out_cam = transformedOpticalBarCamera(icam, param_storage, *in_cam);
        optimized_cams.push_back(vw::CamPtr(out_cam)); // will manage the memory
      }
      break;
    case  BaCameraType_CSM:
      {
        asp::CsmModel const* in_cam
          = dynamic_cast<asp::CsmModel const*>(opt.camera_models[icam].get());
        if (in_cam == NULL)
          vw_throw(ArgumentErr() << "Expecting a CSM camera.\n");
        auto out_cam = transformedCsmCamera(icam, param_storage, *in_cam);
        optimized_cams.push_back(out_cam);
      }
      break;
    case BaCameraType_Other:
      {
        CameraAdjustment cam_adjust(param_storage.get_camera_ptr(icam));
        vw::CamPtr out_cam
          (new AdjustedCameraModel(vw::camera::unadjusted_model(opt.camera_models[icam]),
                                          cam_adjust.position(), cam_adjust.pose()));
        optimized_cams.push_back(out_cam);
      }
      break;
    default:
      vw_throw(ArgumentErr() << "Unknown camera type.\n");
    }
  }
}

// Write updated camera models to disk
void saveUpdatedCameras(asp::BaBaseOptions const& opt, asp::BAParams const& param_storage) {
  
  int num_cameras = opt.image_files.size();
  for (int icam = 0; icam < num_cameras; icam++) {

    switch(opt.camera_type) {
    case BaCameraType_Pinhole:
      write_pinhole_output_file(opt, icam, opt.datum, param_storage);
      break;
    case BaCameraType_OpticalBar:
      write_optical_bar_output_file(opt, icam, opt.datum, param_storage);
      break;
    case BaCameraType_CSM:
      // When solving for intrinsics and using CSM
      write_csm_output_file(opt, icam, opt.datum, param_storage);
      break;
    case BaCameraType_Other: {
        // TODO(oalexan1): Make this into a function and move it out.
        std::string adjust_file = asp::bundle_adjust_file_name(opt.out_prefix,
                                                              opt.image_files[icam],
                                                              opt.camera_files[icam]);
        vw::vw_out() << "Writing: " << adjust_file << std::endl;
        
        CameraAdjustment cam_adjust(param_storage.get_camera_ptr(icam));
        asp::write_adjustments(adjust_file, cam_adjust.position(), cam_adjust.pose());

        // For CSM camera models export, in addition, the JSON state
        // with the adjustment applied to it.
        // When not solving for intrinsics and using CSM
        if (opt.stereo_session == "csm" || opt.stereo_session == "pleiades" ||
            opt.stereo_session == "dg"  ||
            (opt.stereo_session == "aster" && asp::stereo_settings().aster_use_csm))
          write_csm_output_file_no_intr(opt, icam, adjust_file, param_storage);
      }
      break;
    default:
      vw::vw_throw(vw::ArgumentErr() << "Unknown camera type.\n");
    }
    
  } // End loop through cameras

  return;
}

// // Find the average for the gsd for all pixels whose rays intersect at the given
// triangulated point.
// TODO(oalexan1): Export points out of param_storage and crn, then use the 
// other function further down instead.
void estimateGsdPerTriPoint(std::vector<std::string> const& images, 
                            std::vector<vw::CamPtr>  const& cameras,
                            asp::CRNJ                const& crn,
                            asp::BAParams            const& param_storage, 
                            // Output
                            std::vector<double>     & gsds) {

  // Sanity checks
  if (crn.size() != images.size())
    vw_throw(ArgumentErr() << "Expecting the same number of images and crn points.\n");
  if (crn.size() != cameras.size())
    vw_throw(ArgumentErr() << "Expecting the same number of images and cameras.\n");
  if (crn.size() != param_storage.num_cameras())
    vw_throw(ArgumentErr() << "Expecting the same number of images and cameras.\n");

  // Image bboxes
  std::vector<vw::BBox2> bboxes;
  for (size_t i = 0; i < images.size(); i++) {
    vw::DiskImageView<float> img(images[i]);
    bboxes.push_back(bounding_box(img));
  }
        
  int num_cameras = param_storage.num_cameras();
  int num_points  = param_storage.num_points();
  
  // Initialize all gsd to 0
  gsds.resize(num_points, 0.0);
  std::vector<int> count(num_points, 0);
  
  for (int icam = 0; icam < num_cameras; icam++) { // Camera loop
    for (auto fiter = crn[icam].begin(); fiter != crn[icam].end(); fiter++) { // IP loop

      // The index of the 3D point this IP is for.
      int ipt = (**fiter).m_point_id;
      
      VW_ASSERT(int(icam) < num_cameras,
                ArgumentErr() << "Out of bounds in the number of cameras.");
      VW_ASSERT(int(ipt)  < num_points,
                ArgumentErr() << "Out of bounds in the number of points.");
      
      if (param_storage.get_point_outlier(ipt))
        continue; // skip outliers

      vw::Vector2 pix = (**fiter).m_location;
      
      double const* point = param_storage.get_point_ptr(ipt);
      Vector3 xyz(point[0], point[1], point[2]);

      // Estimate the GSD at the given pixel given an estimate of the ground point
      double gsd = 0.0;
      try {
        gsd = vw::camera::estimatedGSD(cameras[icam].get(), bboxes[icam], pix, xyz);
      } catch (...) {
        continue;
      }
      gsds[ipt] += gsd;
      count[ipt]++;
    }
  }
  
  // Find the average gsd
  for (int ipt = 0; ipt < num_points; ipt++) {
    if (count[ipt] > 0)
      gsds[ipt] /= count[ipt];
  }
  
  return;  
}

// Find the average for the gsd for all pixels whose rays intersect at the given
// triangulated point. This is used in jitter solving.
void estimateGsdPerTriPoint(std::vector<std::string> const& images, 
                            std::vector<vw::CamPtr>  const& cameras,
                            asp::CRNJ                const& crn,
                            std::set<int>            const& outliers,
                            std::vector<double>      const& tri_points_vec, 
                            // Output
                            std::vector<double>     & gsds) {

  // Sanity checks
  if (crn.size() != images.size())
    vw_throw(ArgumentErr() << "Expecting the same number of images and crn points.\n");
  if (crn.size() != cameras.size())
    vw_throw(ArgumentErr() << "Expecting the same number of images and cameras.\n");

  // Image bboxes
  std::vector<vw::BBox2> bboxes;
  for (size_t i = 0; i < images.size(); i++) {
    vw::DiskImageView<float> img(images[i]);
    bboxes.push_back(bounding_box(img));
  }
        
  int num_cameras = cameras.size();
  int num_points  = tri_points_vec.size()/3;
  
  // Initialize all gsd to 0
  gsds.resize(num_points, 0.0);
  std::vector<int> count(num_points, 0);
  
  for (int icam = 0; icam < num_cameras; icam++) { // Camera loop
    for (auto fiter = crn[icam].begin(); fiter != crn[icam].end(); fiter++) { // IP loop

      // The index of the 3D point this IP is for.
      int ipt = (**fiter).m_point_id;
      
      VW_ASSERT(int(icam) < num_cameras,
                ArgumentErr() << "Out of bounds in the number of cameras.");
      VW_ASSERT(int(ipt)  < num_points,
                ArgumentErr() << "Out of bounds in the number of points.");
      
      if (outliers.find(ipt) != outliers.end())
        continue; // Skip outliers

      vw::Vector2 pix = (**fiter).m_location;
      
      double const* point = &tri_points_vec[3*ipt];
      Vector3 xyz(point[0], point[1], point[2]);

      // Estimate the GSD at the given pixel given an estimate of the ground point
      double gsd = 0.0;
      try {
        gsd = vw::camera::estimatedGSD(cameras[icam].get(), bboxes[icam], pix, xyz);
      } catch (...) {
        continue;
      }

      gsds[ipt] += gsd;
      count[ipt]++;
    }
  }
  
  // Find the average gsd
  for (int ipt = 0; ipt < num_points; ipt++) {
    if (count[ipt] > 0)
      gsds[ipt] /= count[ipt];
  }
  
  return;  
}

} // end namespace asp
