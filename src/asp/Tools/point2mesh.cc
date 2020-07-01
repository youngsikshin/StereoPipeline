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


/// \file point2mesh2.cc
///

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <math.h>
#include <boost/filesystem.hpp>

#include <vw/Image/Transform.h>
#include <vw/Cartography/PointImageManipulation.h>
#include <vw/Image/MaskViews.h>
#include <asp/Core/PointUtils.h>
#include <vw/Image/Manipulation.h>
#include <asp/Core/Macros.h>
#include <asp/Core/Common.h>

using namespace vw;
using namespace vw::cartography;
namespace po = boost::program_options;

struct Options : vw::cartography::GdalWriteOptions {
  Options() {};
  // Input
  std::string pointcloud_filename, texture_file_name;

  // Settings
  int point_cloud_step_size, texture_step_size, precision;
  bool center;

  // Output
  std::string output_prefix, output_file_type;
};

// Create a blank float image of given size
class BlankImage: public ReturnFixedType<float> {
public:
  float operator()(Vector3 const& v) const {
    return 1.0;
  }
};

// Form the mtl file having the given png texture
void save_mtl(std::string const& output_prefix, std::string const& output_prefix_no_dir) {
  
  std::string mtl_file = output_prefix + ".mtl";
  std::cout << "Writing: " << mtl_file << std::endl;

  std::ofstream ofs(mtl_file.c_str());
  ofs << "newmtl material0000\n";
  ofs << "Ka 1.000000 1.000000 1.000000\n";
  ofs << "Kd 1.000000 1.000000 1.000000\n";
  ofs << "Ks 0.000000 0.000000 0.000000\n";
  ofs << "Tr 0.000000\n";
  ofs << "illum 1\n";
  ofs << "Ns 1.000000\n";
  ofs << "map_Kd " << output_prefix_no_dir << ".png\n";
  ofs.close();
}

void save_texture(std::string const& output_prefix, ImageViewRef<float> texture_image) {
  std::string texture_file = output_prefix + ".png";
  std::cout << "Writing: " << texture_file << std::endl;
  //DiskImageView<PixelGray<uint8> > new_texture(tex_file+".tif");

  double image_min = 0.0, image_max = 1.0;
  ImageViewRef<float> normalized_image =
    normalize(texture_image, image_min, image_max, 0.0, 1.0);

  std::cout << "--image is normalzied!" << std::endl;
  write_image(texture_file, normalized_image);
  
}

// A point at the center of the planet or which has NaN elements cannot be valid
inline bool is_valid_pt(Vector3 const& P) {
  if (P != P || P == Vector3()) 
    return false;
  return true;
}

void save_mesh(std::string const& output_prefix,
               std::string const& output_prefix_no_dir,
               ImageViewRef<Vector3> point_cloud,
               Vector3 C, int precision) {

  std::string mesh_file = output_prefix + ".obj";
  std::cout << "Writing: " << mesh_file << std::endl;
  std::ofstream ofs(mesh_file.c_str());
  ofs.precision(precision);
  ofs << "mtllib " << output_prefix_no_dir << ".mtl\n";

  // Some constants for the calculation in here
  int cloud_cols = point_cloud.cols();
  int cloud_rows = point_cloud.rows();
  
  std::cout << "cloud cols and rows are " << cloud_cols << ' ' << cloud_rows << std::endl;

  TerminalProgressCallback vertex_progress("asp", "\tVertices:   ");
  double vertex_progress_mult = 1.0/double(std::max(cloud_cols - 1, 1));

  // Make sure we we save a vertex only once even if we encounter it
  // multiple times.
  std::map<std::pair<int, int>, int> pix_to_vertex;
    
  std::vector<Vector3i> faces;
  int vertex_count = 0;
  for (int col = 0; col < cloud_cols - 1; col++) {
    vertex_progress.report_progress(col*vertex_progress_mult);
    
    for (int row = 0; row < cloud_rows - 1; row++) {
      // We have a square that needs to be split into two triangles
      // Here the image is viewed as having the origin on the upper-left,
      // the column axis going right, and the row axis going down.
      Vector3 UL = point_cloud(col,     row);
      Vector3 UR = point_cloud(col + 1, row);
      Vector3 LL = point_cloud(col,     row + 1);
      Vector3 LR = point_cloud(col + 1, row + 1);

      double u, v;
      if (is_valid_pt(UL) && is_valid_pt(LL) && is_valid_pt(UR)) {

        std::pair<int, int> pix0 = std::make_pair(col, row);
        if (pix_to_vertex.find(pix0) == pix_to_vertex.end()) {
          ofs << "v " << UL[0] - C[0] << " " << UL[1] - C[1] << " " << UL[2] - C[2]<< '\n';
          u = double(pix0.first)/cloud_cols; v = double(pix0.second)/cloud_rows;
          ofs << "vt " << u  << ' ' << 1.0 - v << std::endl;
          pix_to_vertex[pix0] = vertex_count;
          vertex_count++;
        }
        
        std::pair<int, int> pix1 = std::make_pair(col, row + 1);
        if (pix_to_vertex.find(pix1) == pix_to_vertex.end()) {
          ofs << "v " << LL[0] - C[0] << " " << LL[1] - C[1] << " " << LL[2] - C[2]<< '\n';
          u = double(pix1.first)/cloud_cols; v = double(pix1.second)/cloud_rows;
          ofs << "vt " << u  << ' ' << 1.0 - v << std::endl;
          pix_to_vertex[pix1] = vertex_count;
          vertex_count++;
        }
        
        std::pair<int, int> pix2 = std::make_pair(col + 1, row);
        if (pix_to_vertex.find(pix2) == pix_to_vertex.end()) {
          ofs << "v " << UR[0] - C[0] << " " << UR[1] - C[1] << " " << UR[2] - C[2]<< '\n';
          u = double(pix2.first)/cloud_cols; v = double(pix2.second)/cloud_rows;
          ofs << "vt " << u  << ' ' << 1.0 - v << std::endl;
        }
        
        Vector3 face(vertex_count + 1, vertex_count + 2, vertex_count + 3);
        faces.push_back(face);
      }
      
      if (is_valid_pt(UR) && is_valid_pt(LL) && is_valid_pt(LR)) {
        ofs << "v " << UR[0] - C[0] << " " << UR[1] - C[1] << " " << UR[2] - C[2]<< '\n';
        u = double(col + 1)/cloud_cols; v = double(row)/cloud_rows;
        ofs << "vt " << u  << ' ' << 1.0 - v << std::endl;
        
        ofs << "v " << LL[0] - C[0] << " " << LL[1] - C[1] << " " << LL[2] - C[2]<< '\n';
        u = double(col)/cloud_cols; v = double(row + 1)/cloud_rows;
        ofs << "vt " << u  << ' ' << 1.0 - v << std::endl;

        ofs << "v " << LR[0] - C[0] << " " << LR[1] - C[1] << " " << LR[2] - C[2]<< '\n';
        u = double(col + 1)/cloud_cols; v = double(row + 1)/cloud_rows;
        ofs << "vt " << u  << ' ' << 1.0 - v << std::endl;

        Vector3 face(vertex_count + 1, vertex_count + 2, vertex_count + 3);
        faces.push_back(face);
        vertex_count += 3;
      }
      
    }
  }
  vertex_progress.report_finished();

  TerminalProgressCallback face_progress("asp", "\tFaces:   ");
  double face_progress_mult = 1.0/double(std::max(faces.size(), size_t(1)));
  for (size_t face_iter = 0; face_iter < faces.size(); face_iter++) {
    face_progress.report_progress(face_iter*face_progress_mult);

    Vector3 const& F = faces[face_iter]; // alias
    ofs << "f "
        << F[0] << "/" << F[0] << " "
        << F[1] << "/" << F[1] << " "
        << F[2] << "/" << F[2]
        << std::endl;
  }
  face_progress.report_finished();
}


// MAIN
// ---------------------------------------------------------

void handle_arguments( int argc, char *argv[], Options& opt ) {
  po::options_description general_options("");
  general_options.add_options()
    ("cloud-step-size,s", po::value(&opt.point_cloud_step_size)->default_value(10),
     "Sampling step for the point cloud. Pick one out of these many samples.")
    ("texture-step-size", po::value(&opt.texture_step_size)->default_value(2),
     "Sampling step for the texture. Pick one out of these many samples.")
    
    ("output-prefix,o", po::value(&opt.output_prefix),
     "Specify the output prefix.")
    ("center", po::bool_switch(&opt.center)->default_value(false),
     "Center the model around the origin. Use this option if you are experiencing numerical precision issues.")
    ("precision", po::value(&opt.precision)->default_value(17),
     "How many digits of precision to save.");
  
  general_options.add( vw::cartography::GdalWriteOptionsDescription(opt) );

  po::options_description positional("");
  positional.add_options()
    ("input-file", po::value(&opt.pointcloud_filename),
       "Explicitly specify the input file")
    ("texture-file", po::value(&opt.texture_file_name),
       "Explicity specify the texture file");

  po::positional_options_description positional_desc;
  positional_desc.add("input-file", 1);
  positional_desc.add("texture-file", 1);

  std::string usage("[options] <pointcloud> <texture-file>");
  bool allow_unregistered = false;
  std::vector<std::string> unregistered;
  po::variables_map vm =
    asp::check_command_line(argc, argv, opt, general_options, general_options,
                             positional, positional_desc, usage,
                             allow_unregistered, unregistered);

  if (opt.pointcloud_filename.empty())
    vw_throw(ArgumentErr() << "Missing point cloud.\n"
              << usage << general_options);
  if ( opt.output_prefix.empty() )
    opt.output_prefix =
      asp::prefix_from_pointcloud_filename( opt.pointcloud_filename );

  if (opt.point_cloud_step_size <= 0)
    vw_throw(ArgumentErr() << "Step size must be positive.\n"
             << usage << general_options);
  
  if (opt.precision <= 0)
    vw_throw(ArgumentErr() << "Precision must be positive.\n"
             << usage << general_options);

  // Create the output directory
  vw::create_out_dir(opt.output_prefix);

  // Turn on logging to file
  asp::log_to_file(argc, argv, "", opt.output_prefix);
}

int main(int argc, char *argv[]){

  Options opt;
  try {
    handle_arguments(argc, argv, opt);

    std::string input_file = opt.pointcloud_filename;
    int num_channels = get_num_channels(input_file);
    GeoReference georef;
    bool has_georef = read_georeference(georef, input_file);
    double nodata_val = -std::numeric_limits<double>::max();
    vw::read_nodata_val(input_file, nodata_val);

    Vector2 image_size = vw::file_image_size(input_file);
    
    vw_out() << "\t--> Original cloud size: "
             << image_size[0] << " x " << image_size[1] << "\n";
    
    // Loading point cloud
    ImageViewRef<Vector3> point_cloud;
    if (num_channels == 1 && has_georef) {
      // The input is a DEM. Convert it to a point cloud.
      DiskImageView<double> dem(input_file);
      point_cloud = vw::subsample(geodetic_to_cartesian(dem_to_geodetic
                                                        (create_mask(dem, nodata_val), georef),
                                                        georef.datum()),
                                  opt.point_cloud_step_size);
    }else if (num_channels >= 3){
      // The input DEM is a point cloud
      point_cloud = vw::subsample(asp::read_asp_point_cloud<3>(input_file), opt.point_cloud_step_size);
    }else{
      vw_throw( ArgumentErr() << "The input must be a point cloud or a DEM.\n");
    }

    vw_out() << "\t--> Subsampled cloud size:   "
             << point_cloud.cols() << " x " << point_cloud.rows() << "\n";
    
    // Centering option (helpful if you are experiencing round-off error)
    Vector3 C(0, 0, 0);
    if (opt.center) {
      bool is_geodetic = false; // raw xyz values
      BBox3 bbox = asp::pointcloud_bbox(point_cloud, is_geodetic);
      vw_out() << "\t--> Centering model around the origin.\n";
      vw_out() << "\t    Initial point image bounding box: " << bbox << "\n";
      C = (bbox.max() + bbox.min()) / 2.0;
      vw_out() << "\t    Midpoint: " << C << "\n";
    }
    
    ImageViewRef<float> texture_image;
    std::cout << "--texturex size " << texture_image.cols() << ' ' << texture_image.rows()
              << std::endl;
    
    // Resample the image if it is too fine.
    if ( !opt.texture_file_name.empty() ) {
      texture_image = vw::subsample(DiskImageView<float>(opt.texture_file_name),
                                    opt.texture_step_size);
    } else {
      // Use a blank texture image with each pixel being white
      texture_image = per_pixel_filter(point_cloud, BlankImage());
    }
    
    std::cout << "--texture size " << texture_image.cols() << ' ' << texture_image.rows()
              << std::endl;

    boost::filesystem::path p(opt.output_prefix);
    std::string output_prefix_no_dir = p.filename().string();
  
    save_mesh(opt.output_prefix, output_prefix_no_dir,
              point_cloud, C, opt.precision);

    save_texture(opt.output_prefix, texture_image);
    
    save_mtl(opt.output_prefix, output_prefix_no_dir);
    
  } ASP_STANDARD_CATCHES;

  return 0;
}
