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


/// \file sfs.cc

// Turn off warnings from boost and other packages
#if defined(__GNUC__) || defined(__GNUG__)
#define LOCAL_GCC_VERSION (__GNUC__ * 10000                    \
                           + __GNUC_MINOR__ * 100              \
                           + __GNUC_PATCHLEVEL__)
#if LOCAL_GCC_VERSION >= 40600
#pragma GCC diagnostic push
#endif
#if LOCAL_GCC_VERSION >= 40202
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#endif
#endif

#include <asp/Core/Macros.h>
#include <asp/Core/Common.h>
#include <asp/Sessions.h>
#include <asp/Core/BundleAdjustUtils.h>
#include <vw/Image/MaskViews.h>
#include <vw/Image/AntiAliasing.h>
#include <ceres/ceres.h>
#include <ceres/loss_function.h>

#if defined(__GNUC__) || defined(__GNUG__)
#if LOCAL_GCC_VERSION >= 40600
#pragma GCC diagnostic pop
#endif
#undef LOCAL_GCC_VERSION
#endif

namespace po = boost::program_options;
namespace fs = boost::filesystem;

using namespace vw;
using namespace vw::camera;
using namespace vw::cartography;

typedef InterpolationView<ImageViewRef< PixelMask<float> >, BilinearInterpolation> BilinearInterpT;

// TODO: Study more the multi-resolution approach.
// TODO: Must specify in the SfS doc that the lunar lambertian model fails at poles
// TODO: If this code becomes multi-threaded, need to keep in mind
// that camera models are shared and modified, so
// this may cause problems.
// TODO: How to relax conditions at the boundary to improve the accuracy?
// TODO: Study if blurring the input images improves the fit.
// TODO: Add --orthoimage option, and make it clear where the final DEM is.
// Same for albedo. The other info should be printed only in debug mode.
// Implement multi-grid for SfS, it should help with bad initial DEM and bad
// camera alignment.
// TODO: Save final DEM and final ortho images.
// TODO: Document the bundle adjustment, and if necessary, manual ip selection.
// Say that if the camera are bundle adjusted, sfs can further improve
// the camera positions to make the results more self consistent,
// but this works only if the cameras are reasonably accurate to start with.
// TODO: Study the effect of reading the images as double as opposed to float.
// TODO: Study the effect of using bicubic interpolation.
// TODO: Study phaseCoeffC1, etc.
// TODO: Find a good automatic value for the smoothness weight.
// TODO: How to change the smoothness weight if resolution changes?
// How to change the smoothness weight if the number of images changes?
// TODO: Investigate the sign of the normal.
// TODO: Loop over all images when doing sfs.
// TODO: Shadow threshold needs detection.
// TODO: Check that we are within image boundaries when interpolating.
// TODO: Radiometric calibration of images.
// TODO: Handle the case when the DEM has no-data values.
// TODO: Add various kind of loss function.
// TODO: Study the normal computation formula.
// TODO: Move some code to Core.
// TODO: Make it work with non-ISIS cameras.
// TODO: Clean up some of the classes, not all members are needed.

// Compute mean and standard deviation of an image
template <class ImageT>
void compute_image_stats(ImageT const& I, double & mean, double & stdev){
  mean  = 0;
  stdev = 0;
  double sum = 0.0, sum2 = 0.0, count = 0.0;
  for (int col = 0; col < I.cols(); col++){
    for (int row = 0; row < I.rows(); row++){
      if (!is_valid(I(col, row))) continue;
      count++;
      double val = I(col, row);
      sum += val;
      sum2 += val*val;
    }
  }

  if (count > 0){
    mean = sum/count;
    stdev = sqrt( sum2/count - mean*mean );
  }

}

// Find the points on a given DEM that are shadowed by other points of
// the DEM.  Start marching from the point on the DEM on a ray towards
// the sun in small increments, until hitting the maximum DEM height.
bool isInShadow(int col, int row, Vector3 & sunPos,
                ImageView<double> const& dem, double max_dem_height,
                double grid_x, double grid_y,
                cartography::GeoReference const& geo){

  // Here bicubic interpolation won't work. It is easier to interpret
  // the DEM as piecewise-linear when dealing with rays intersecting
  // it.
  InterpolationView<EdgeExtensionView< ImageView<double>,
    ConstantEdgeExtension >, BilinearInterpolation>
    interp_dem = interpolate(dem, BilinearInterpolation(),
                             ConstantEdgeExtension());

  // The xyz position at the center grid point
  Vector2 dem_llh = geo.pixel_to_lonlat(Vector2(col, row));
  Vector3 dem_lonlat_height = Vector3(dem_llh(0), dem_llh(1), dem(col, row));
  Vector3 xyz = geo.datum().geodetic_to_cartesian(dem_lonlat_height);

  // Normalized direction from the view point
  Vector3 dir = sunPos - xyz;
  if (dir == Vector3())
    return false;
  dir = dir/norm_2(dir);

  // The projection of dir onto the tangent plane at xyz,
  // that is, the "horizontal" component at the current sphere surface.
  Vector3 dir2 = dir - dot_prod(dir, xyz)*xyz/dot_prod(xyz, xyz);

  // Ensure that we advance by at most half a grid point each time
  double delta = 0.5*std::min(grid_x, grid_y)/std::max(norm_2(dir2), 1e-16);

  // go along the ray. Don't allow the loop to go forever.
  for (int i = 1; i < 10000000; i++) {
    Vector3 ray_P = xyz + i * delta * dir;
    Vector3 ray_llh = geo.datum().cartesian_to_geodetic(ray_P);
    if (ray_llh[2] > max_dem_height) {
      // We're above the terrain, no point in continuing
      return false;
    }

    // Compensate for any longitude 360 degree offset, e.g., 270 deg vs -90 deg
    ray_llh[0] += 360.0*round((dem_llh[0] - ray_llh[0])/360.0);

    Vector2 ray_pix = geo.lonlat_to_pixel(Vector2(ray_llh[0], ray_llh[1]));

    if (ray_pix[0] < 0 || ray_pix[0] > dem.cols() - 1 ||
        ray_pix[1] < 0 || ray_pix[1] > dem.rows() - 1 ) {
      return false; // got out of the DEM, no point continuing
    }

    // Dem height at the current point on the ray
    double dem_h = interp_dem(ray_pix[0], ray_pix[1]);

    if (ray_llh[2] < dem_h) {
      // The ray goes under the DEM, so we are in shadow.
      return true;
    }
  }

  return false;
}

void areInShadow(Vector3 & sunPos, ImageView<double> const& dem,
                 double grid_x, double grid_y,
                 cartography::GeoReference const& geo,
                 ImageView<float> & shadow){

  // Find the max DEM height
  double max_dem_height = -std::numeric_limits<double>::max();
  for (int col = 0; col < dem.cols(); col++) {
    for (int row = 0; row < dem.rows(); row++) {
      if (dem(col, row) > max_dem_height) {
        max_dem_height = dem(col, row);
      }
    }
  }

  shadow.set_size(dem.cols(), dem.rows());
  for (int col = 0; col < dem.cols(); col++) {
    for (int row = 0; row < dem.rows(); row++) {
      shadow(col, row) = isInShadow(col, row, sunPos, dem,
                                    max_dem_height, grid_x, grid_y, geo);
    }
  }
}

struct Options : public asp::BaseOptions {
  std::string input_dem, out_prefix, stereo_session_string, bundle_adjust_prefix;
  std::vector<std::string> input_images, input_cameras;
  std::string shadow_thresholds;
  std::vector<float> shadow_threshold_vec;

  int max_iterations, max_coarse_iterations, reflectance_type, coarse_levels;
  bool float_albedo, float_exposure, float_cameras, model_shadows,
    float_dem_at_boundary;
  double smoothness_weight, init_dem_height, max_height_change,
    height_change_weight;
  Options():max_iterations(0), max_coarse_iterations(0), reflectance_type(0),
            coarse_levels(0), float_albedo(false), float_exposure(false), float_cameras(false),
            model_shadows(false), float_dem_at_boundary(false),
            smoothness_weight(0), max_height_change(0), height_change_weight(0){};
};

struct GlobalParams{
  int reflectanceType;
  // Two parameters used in the formula for the Lunar-Lambertian
  // reflectance
  double phaseCoeffC1, phaseCoeffC2;
};

struct ModelParams {
  vw::Vector3 sunPosition; //relative to the center of the Moon
  vw::Vector3 cameraPosition;//relative to the center of the planet
  ModelParams(){}
  ~ModelParams(){}
};

enum {NO_REFL = 0, LAMBERT, LUNAR_LAMBERT};

// computes the Lambertian reflectance model (cosine of the light
// direction and the normal to the Moon) Vector3 sunpos: the 3D
// coordinates of the Sun relative to the center of the Moon Vector2
// lon_lat is a 2D vector. First element is the longitude and the
// second the latitude.
//author Ara Nefian
double
computeLambertianReflectanceFromNormal(Vector3 sunPos, Vector3 xyz,
                                       Vector3 normal) {
  double reflectance;
  Vector3 sunDirection = normalize(sunPos-xyz);

  reflectance = sunDirection[0]*normal[0] + sunDirection[1]*normal[1] + sunDirection[2]*normal[2];

  return reflectance;
}


double computeLunarLambertianReflectanceFromNormal(Vector3 const& sunPos,
                                                   Vector3 const& viewPos,
                                                   Vector3 const& xyz,
                                                   Vector3 const& normal,
                                                   double phaseCoeffC1,
                                                   double phaseCoeffC2,
                                                   double & alpha) {
  double reflectance;
  double L;

  double len = dot_prod(normal, normal);
  if (abs(len - 1.0) > 1.0e-4){
    std::cerr << "Error: Expecting unit normal in the reflectance computation, in "
              << __FILE__ << " at line " << __LINE__ << std::endl;
    exit(1);
  }

  //compute /mu_0 = cosine of the angle between the light direction and the surface normal.
  //sun coordinates relative to the xyz point on the Moon surface
  //Vector3 sunDirection = -normalize(sunPos-xyz);
  Vector3 sunDirection = normalize(sunPos-xyz);
  double mu_0 = dot_prod(sunDirection,normal);

  double tol = 0.3;
  if (mu_0 < tol){
    // Sun is too low, reflectance is too close to 0, the albedo will be inaccurate
    return 0.0;
  }

  //compute  /mu = cosine of the angle between the viewer direction and the surface normal.
  //viewer coordinates relative to the xyz point on the Moon surface
  Vector3 viewDirection = normalize(viewPos-xyz);
  double mu = dot_prod(viewDirection,normal);

  //compute the phase angle (alpha) between the viewing direction and the light source direction
  double deg_alpha;
  double cos_alpha;

  cos_alpha = dot_prod(sunDirection,viewDirection);
  if ((cos_alpha > 1)||(cos_alpha< -1)){
    printf("cos_alpha error\n");
  }

  alpha     = acos(cos_alpha);  // phase angle in radians
  deg_alpha = alpha*180.0/M_PI; // phase angle in degrees

  //printf("deg_alpha = %f\n", deg_alpha);

  //Bob Gaskell's model
  //L = exp(-deg_alpha/60.0);

  //Alfred McEwen's model
  double A = -0.019;
  double B =  0.000242;//0.242*1e-3;
  double C = -0.00000146;//-1.46*1e-6;

  L = 1.0 + A*deg_alpha + B*deg_alpha*deg_alpha + C*deg_alpha*deg_alpha*deg_alpha;

  //printf(" deg_alpha = %f, L = %f\n", deg_alpha, L);

  if (mu_0 < 0.0){
    return 0.0;
  }

  if (mu < 0.0){ //emission angle is > 90
    mu = 0.0;
  }

  if (mu_0 + mu == 0){
    //printf("negative reflectance\n");
    return 0.0;
  }
  else{
    reflectance = 2*L*mu_0/(mu_0+mu) + (1-L)*mu_0;
  }
  if (reflectance <= 0){
    //printf("negative reflectance\n");
    return 0.0;
  }

  // Attempt to compensate for points on the terrain being too bright
  // if the sun is behind the spacecraft as seen from those points.

  //reflectance *= std::max(0.4, exp(-alpha*alpha));
  reflectance *= ( exp(-phaseCoeffC1*alpha) + phaseCoeffC2 );

  return reflectance;
}

double ComputeReflectance(Vector3 const& normal, Vector3 const& xyz,
                          ModelParams const& input_img_params,
                          GlobalParams const& global_params,
                          double & phase_angle) {
  double input_img_reflectance;

  switch ( global_params.reflectanceType )
    {
    case LUNAR_LAMBERT:
      //printf("Lunar Lambert\n");
      input_img_reflectance
        = computeLunarLambertianReflectanceFromNormal(input_img_params.sunPosition,
                                                      input_img_params.cameraPosition,
                                                      xyz,  normal,
                                                      global_params.phaseCoeffC1,
                                                      global_params.phaseCoeffC2,
                                                      phase_angle // output
                                                      );
      break;
    case LAMBERT:
      //printf("Lambert\n");
      input_img_reflectance
        = computeLambertianReflectanceFromNormal(input_img_params.sunPosition,
                                                 xyz,  normal);
      break;

    default:
      //printf("No reflectance model\n");
      input_img_reflectance = 1;
    }

  return input_img_reflectance;
}

bool computeReflectanceAndIntensity(double left_h, double center_h, double right_h,
                                    double bottom_h, double top_h,
                                    int col, int row,
                                    ImageView<double> const& dem,
                                    cartography::GeoReference const& geo,
                                    bool model_shadows,
                                    double max_dem_height,
                                    double grid_x, double grid_y,
                                    ModelParams const& model_params,
                                    GlobalParams const& global_params,
                                    BilinearInterpT const & image,
                                    CameraModel const* camera,
                                    PixelMask<double> &reflectance,
                                    PixelMask<double> & intensity) {

  // Set output values
  reflectance = 0; reflectance.invalidate();
  intensity = 0;   intensity.invalidate();

  if (col >= dem.cols() - 1 || row >= dem.rows() - 1) return false;

  // TODO: Investigate various ways of finding the normal.

  // The xyz position at the center grid point
  Vector2 lonlat = geo.pixel_to_lonlat(Vector2(col, row));
  double h = center_h;
  Vector3 lonlat3 = Vector3(lonlat(0), lonlat(1), h);
  Vector3 base = geo.datum().geodetic_to_cartesian(lonlat3);

  // The xyz position at the left grid point
  lonlat = geo.pixel_to_lonlat(Vector2(col-1, row));
  h = left_h;
  lonlat3 = Vector3(lonlat(0), lonlat(1), h);
  Vector3 left = geo.datum().geodetic_to_cartesian(lonlat3);

  // The xyz position at the right grid point
  lonlat = geo.pixel_to_lonlat(Vector2(col+1, row));
  h = right_h;
  lonlat3 = Vector3(lonlat(0), lonlat(1), h);
  Vector3 right = geo.datum().geodetic_to_cartesian(lonlat3);

  // The xyz position at the bottom grid point
  lonlat = geo.pixel_to_lonlat(Vector2(col, row+1));
  h = bottom_h;
  lonlat3 = Vector3(lonlat(0), lonlat(1), h);
  Vector3 bottom = geo.datum().geodetic_to_cartesian(lonlat3);

  // The xyz position at the top grid point
  lonlat = geo.pixel_to_lonlat(Vector2(col, row-1));
  h = top_h;
  lonlat3 = Vector3(lonlat(0), lonlat(1), h);
  Vector3 top = geo.datum().geodetic_to_cartesian(lonlat3);

#if 0
  // two-point normal
  Vector3 dx = right - base;
  Vector3 dy = bottom - base;
#else
  // four-point normal (centered)
  Vector3 dx = right - left;
  Vector3 dy = bottom - top;
#endif
  Vector3 normal = -normalize(cross_prod(dx, dy)); // so normal points up

  // Update the camera position for the given pixel (camera position
  // always changes for linescan cameras).
  ModelParams local_model_params = model_params;
  Vector2 pix = camera->point_to_pixel(base);
  try {
    local_model_params.cameraPosition = camera->camera_center(pix);
  } catch(...){
    reflectance = 0; reflectance.invalidate();
    intensity = 0;   intensity.invalidate();
    return false;
  }

  double phase_angle;
  reflectance = ComputeReflectance(normal, base, local_model_params,
                                   global_params, phase_angle);
  reflectance.validate();


  // Check for out of range
  if (pix[0] < 0 || pix[0] >= image.cols()-1 ||
      pix[1] < 0 || pix[1] >= image.rows()-1) {
    reflectance = 0; reflectance.invalidate();
    intensity = 0;   intensity.invalidate();
    return false;
  }

  intensity = image(pix[0], pix[1]); // this interpolates
  if (!is_valid(intensity)) {
    reflectance = 0; reflectance.invalidate();
    intensity = 0;   intensity.invalidate();
    return false;
  }


  if (model_shadows) {
    bool inShadow = isInShadow(col, row, local_model_params.sunPosition,
                               dem, max_dem_height, grid_x, grid_y,
                               geo);

    if (inShadow) {
      // The reflectance is valid, it is just zero
      reflectance = 0;
      reflectance.validate();
    }
  }

  return true;
}

void computeReflectanceAndIntensity(ImageView<double> const& dem,
                                    cartography::GeoReference const& geo,
                                    bool model_shadows,
                                    double & max_dem_height, // alias
                                    double grid_x, double grid_y,
                                    ModelParams const& model_params,
                                    GlobalParams const& global_params,
                                    BilinearInterpT const & image,
                                    CameraModel const* camera,
                                    ImageView< PixelMask<double> > & reflectance,
                                    ImageView< PixelMask<double> > & intensity) {

  // Update max_dem_height
  max_dem_height = -std::numeric_limits<double>::max();
  for (int col = 0; col < dem.cols(); col++) {
    for (int row = 0; row < dem.rows(); row++) {
      if (dem(col, row) > max_dem_height) {
        max_dem_height = dem(col, row);
      }
    }
  }

  // Init the reflectance and intensity as invalid
  reflectance.set_size(dem.cols(), dem.rows());
  intensity.set_size(dem.cols(), dem.rows());
  for (int col = 0; col < dem.cols(); col++) {
    for (int row = 0; row < dem.rows(); row++) {
      reflectance(col, row).invalidate();
      intensity(col, row).invalidate();
    }
  }

  for (int col = 1; col < dem.cols()-1; col++) {
    for (int row = 1; row < dem.rows()-1; row++) {
      computeReflectanceAndIntensity(dem(col-1, row), dem(col, row),
                                     dem(col+1, row),
                                     dem(col, row+1), dem(col, row-1),
                                     col, row, dem,  geo,
                                     model_shadows, max_dem_height,
                                     grid_x, grid_y,
                                     model_params, global_params,
                                     image, camera,
                                     reflectance(col, row), intensity(col, row));
    }
  }

  return;
}

// Form a finer resolution image with given dimensions from a coarse image.
// Use constant edge extension.
void interp_image(ImageView<double> const& coarse_image, double scale,
                  ImageView<double> & fine_image){

  ImageViewRef<double> coarse_interp = interpolate(coarse_image,
                                         BilinearInterpolation(), ConstantEdgeExtension());
  for (int col = 0; col < fine_image.cols(); col++) {
    for (int row = 0; row < fine_image.rows(); row++) {
      fine_image(col, row) = coarse_interp(col*scale, row*scale);
    }
  }
}

// A function to invoke at every iteration of ceres.
// We need a lot of global variables to do something useful.
int                                            g_iter = -1;
Options                                const * g_opt;
ImageView<double>                            * g_dem, *g_albedo;
cartography::GeoReference              const * g_geo;
GlobalParams                           const * g_global_params;
std::vector<ModelParams>               const * g_model_params;
std::vector<BilinearInterpT>           const * g_interp_images;
std::vector<boost::shared_ptr<CameraModel> > * g_cameras;
double                                       * g_nodata_val;
float                                        * g_img_nodata_val;
double                                       * g_exposures;
std::vector<double>                          * g_adjustments;
double                                       * g_max_dem_height;
double                                       * g_grid_x;
double                                       * g_grid_y;
int                                            g_level = -1;

// When floating the camera position and orientation,
// multiply the position variables by this number to give
// it a greater range in searching (it makes more sense to wiggle
// the camera position by say 1 meter than by a tiny fraction
// of one millimeter).
double g_position_scale = 1e+8;

class SfsCallback: public ceres::IterationCallback {
public:
  virtual ceres::CallbackReturnType operator()
    (const ceres::IterationSummary& summary) {

    g_iter++;

    vw_out() << "Finished iteration: " << g_iter << std::endl;

    vw_out() << "cam adj: ";
    for (int s = 0; s < int((*g_adjustments).size()); s++) {
      vw_out() << (*g_adjustments)[s] << " ";
    }
    vw_out() << std::endl;

    std::ostringstream os;
    os << "-iter" << g_iter;

    if ((*g_opt).coarse_levels > 0) {
      os << "-level" << g_level;
    }
    std::string iter_str = os.str();

    std::string out_dem_file = g_opt->out_prefix + "-DEM"
      + iter_str + ".tif";
    vw_out() << "Writing: " << out_dem_file << std::endl;
    TerminalProgressCallback tpc("asp", ": ");
    block_write_gdal_image(out_dem_file, *g_dem, *g_geo, *g_nodata_val,
                           *g_opt, tpc);

    std::string out_albedo_file = g_opt->out_prefix + "-comp-albedo"
      + iter_str + ".tif";
    vw_out() << "Writing: " << out_albedo_file << std::endl;
    block_write_gdal_image(out_albedo_file, *g_albedo, *g_geo, *g_nodata_val,
                           *g_opt, tpc);

    // If there's just one image, print reflectance and other things
    for (size_t image_iter = 0; image_iter < (*g_interp_images).size();
         image_iter++) {

      // Separate into blocks for each image
      vw_out() << "\n";

      ImageView< PixelMask<double> > reflectance, intensity, comp_intensity;

      std::ostringstream os;
      os << iter_str << "-img" << image_iter;
      std::string iter_str2 = os.str();

      // Save the camera adjustments for the current iteration
      std::string out_camera_file = g_opt->out_prefix + "-camera"
        + iter_str2 + ".adjust";
      vw_out() << "Writing: " << out_camera_file << std::endl;
      AdjustedCameraModel * icam
        = dynamic_cast<AdjustedCameraModel*>((*g_cameras)[image_iter].get());
      if (icam == NULL)
        vw_throw( ArgumentErr() << "Expecting adjusted camera.\n");
      Vector3 position_correction = icam->translation();
      Quaternion<double> pose_correction = icam->rotation();
      asp::write_adjustments(out_camera_file, position_correction, pose_correction);

      // Compute reflectance and intensity with optimized DEM
      computeReflectanceAndIntensity(*g_dem, *g_geo,
                                     g_opt->model_shadows,
                                     *g_max_dem_height,
                                     *g_grid_x, *g_grid_y,
                                     (*g_model_params)[image_iter],
                                     *g_global_params,
                                     (*g_interp_images)[image_iter],
                                     (*g_cameras)[image_iter].get(),
                                     reflectance, intensity);

      std::string out_intensity_file = g_opt->out_prefix + "-meas-intensity"
        + iter_str2 + ".tif";
      vw_out() << "Writing: " << out_intensity_file << std::endl;
      block_write_gdal_image(out_intensity_file,
                             apply_mask(intensity, *g_img_nodata_val),
                             *g_geo, *g_img_nodata_val, *g_opt, tpc);

      std::string out_reflectance_file = g_opt->out_prefix + "-comp-reflectance"
        + iter_str2 + ".tif";
      vw_out() << "Writing: " << out_reflectance_file << std::endl;
      block_write_gdal_image(out_reflectance_file,
                             apply_mask(reflectance, *g_img_nodata_val),
                             *g_geo, *g_img_nodata_val, *g_opt, tpc);

      // Find the measured normalized albedo, after correcting for
      // reflectance.
      ImageView<double> measured_albedo;
      measured_albedo.set_size(reflectance.cols(), reflectance.rows());
      for (int col = 0; col < measured_albedo.cols(); col++) {
        for (int row = 0; row < measured_albedo.rows(); row++) {
          if (!is_valid(reflectance(col, row)))
            measured_albedo(col, row) = 1;
          else
            measured_albedo(col, row)
              = intensity(col, row)/(reflectance(col, row)*g_exposures[image_iter]);
        }
      }
      std::string out_albedo_file = g_opt->out_prefix
        + "-meas-albedo" + iter_str2 + ".tif";
      vw_out() << "Writing: " << out_albedo_file << std::endl;
      block_write_gdal_image(out_albedo_file, measured_albedo,
                             *g_geo, 0, *g_opt, tpc);

      // Find the computed intensity
      comp_intensity.set_size(reflectance.cols(), reflectance.rows());
      for (int col = 0; col < comp_intensity.cols(); col++) {
        for (int row = 0; row < comp_intensity.rows(); row++) {
          comp_intensity(col, row)
            = (*g_albedo)(col, row) * g_exposures[image_iter] * reflectance(col, row);
        }
      }
      std::string out_comp_intensity_file = g_opt->out_prefix
        + "-comp-intensity" + iter_str2 + ".tif";
      vw_out() << "Writing: " << out_comp_intensity_file << std::endl;
      block_write_gdal_image(out_comp_intensity_file,
                             apply_mask(comp_intensity, *g_img_nodata_val),
                             *g_geo, *g_img_nodata_val, *g_opt, tpc);

      double imgmean, imgstdev, refmean, refstdev;
      compute_image_stats(intensity, imgmean, imgstdev);
      compute_image_stats(comp_intensity, refmean, refstdev);

      vw_out() << "meas image mean and std: " << imgmean << ' ' << imgstdev
                << std::endl;
      vw_out() << "comp image mean and std: " << refmean << ' ' << refstdev
                << std::endl;

      vw_out() << "Exposure " << " for image " << image_iter << ": "
               << g_exposures[image_iter] << std::endl;

#if 0
      // Dump the points in shadow
      ImageView<float> shadow; // don't use int, scaled weirdly by ASP on reading
      Vector3 sunPos = (*g_model_params)[image_iter].sunPosition;
      areInShadow(sunPos, *g_dem, *g_grid_x, *g_grid_y,  *g_geo, shadow);

      std::string out_shadow_file = g_opt->out_prefix
        + "-shadow" + iter_str2 + ".tif";
      vw_out() << "Writing: " << out_shadow_file << std::endl;
      block_write_gdal_image(out_shadow_file, shadow, *g_geo,
                             -std::numeric_limits<float>::max(), *g_opt, tpc);
#endif

    }

    return ceres::SOLVER_CONTINUE;
  }
};


// Discrepancy between measured and computed intensity.
// sum_i | I_i - albedo * exposures[i] * reflectance_i |^2
struct IntensityError {
  IntensityError(int col, int row,
                 ImageView<double> const& dem,
                 cartography::GeoReference const& geo,
                 bool model_shadows,
                 double const& max_dem_height, // note: this is an alias
                 double grid_x, double grid_y,
                 GlobalParams const& global_params,
                 ModelParams const& model_params,
                 BilinearInterpT const& image,
                 boost::shared_ptr<CameraModel> const& camera):
    m_col(col), m_row(row), m_dem(dem), m_geo(geo),
    m_model_shadows(model_shadows), m_max_dem_height(max_dem_height),
    m_grid_x(grid_x), m_grid_y(grid_y),
    m_global_params(global_params),
    m_model_params(model_params),
    m_image(image), m_camera(camera) {}

  // See SmoothnessError() for the definitions of bottom, top, etc.
  template <typename F>
  bool operator()(const F* const exposure,
                  const F* const left,
                  const F* const center,
                  const F* const right,
                  const F* const bottom,
                  const F* const top,
                  const F* const albedo,
                  const F* const adjustments, // camera adjustments
                  F* residuals) const {

    // Default residuals. Using here 0 rather than some big number tuned out to
    // work better than the alternative.
    residuals[0] = F(0.0);
    try{

      // Apply current adjustments to the camera
      AdjustedCameraModel * adj_cam
        = dynamic_cast<AdjustedCameraModel*>(m_camera.get());
      if (adj_cam == NULL)
        vw_throw( ArgumentErr() << "Expecting adjusted camera.\n");
      Vector3 axis_angle;
      Vector3 translation;
      for (int param_iter = 0; param_iter < 3; param_iter++) {
        translation[param_iter] = g_position_scale*adjustments[param_iter];
        axis_angle[param_iter] = adjustments[3 + param_iter];
      }
      adj_cam->set_translation(translation);
      adj_cam->set_axis_angle_rotation(axis_angle);

      PixelMask<double> reflectance, intensity;
      bool success =
        computeReflectanceAndIntensity(left[0], center[0], right[0],
                                       bottom[0], top[0],
                                       m_col, m_row,  m_dem, m_geo,
                                       m_model_shadows, m_max_dem_height,
                                       m_grid_x, m_grid_y,
                                       m_model_params,  m_global_params,
                                       m_image, adj_cam,
                                       reflectance, intensity);
      if (success && is_valid(intensity) && is_valid(reflectance))
        residuals[0] = (intensity - albedo[0]*exposure[0]*reflectance).child();

    } catch (const camera::PointToPixelErr& e) {
      // To be able to handle robustly DEMs that extend beyond the camera,
      // always return true when we fail to project, but with a somewhat
      // large residual.
      return true;
    }

    return true;
  }

  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(int col, int row,
                                     ImageView<double> const& dem,
                                     vw::cartography::GeoReference const& geo,
                                     bool model_shadows,
                                     double const& max_dem_height, // alias
                                     double grid_x, double grid_y,
                                     GlobalParams const& global_params,
                                     ModelParams const& model_params,
                                     BilinearInterpT const& image,
                                     boost::shared_ptr<CameraModel> const& camera){
    return (new ceres::NumericDiffCostFunction<IntensityError,
            ceres::CENTRAL, 1, 1, 1, 1, 1, 1, 1, 1, 6>
            (new IntensityError(col, row, dem, geo,
                                model_shadows, max_dem_height,
                                grid_x, grid_y,
                                global_params, model_params,
                                image, camera)));
  }

  int m_col, m_row;
  ImageView<double>                 const & m_dem;            // alias
  cartography::GeoReference         const & m_geo;            // alias
  bool                                      m_model_shadows;
  double                            const & m_max_dem_height; // alias
  double                                    m_grid_x, m_grid_y;
  GlobalParams                      const & m_global_params;  // alias
  ModelParams                       const & m_model_params;   // alias
  BilinearInterpT                   const & m_image;          // alias
  boost::shared_ptr<CameraModel>    const & m_camera;         // alias
};


// The smoothness error is the sum of squares of
// the 4 second order partial derivatives, with a weight:
// error = smoothness_weight * ( u_xx^2 + u_xy^2 + u_yx^2 + u_yy^2 )

// We will use finite differences to compute these.
// Consider a grid point and its neighbors, 9 points in all.
//
// bl   = u(c-1, r+1)  bottom = u(c, r+1) br    = u(c+1,r+1)
// left = u(c-1, r  )  center = u(c, r  ) right = u(c+1,r  )
// tl   = u(c-1, r-1)  top    = u(c, r-1) tr    = u(c+1,r-1)
//
// See https://en.wikipedia.org/wiki/Finite_difference
// for the obtained formulas.

struct SmoothnessError {
  SmoothnessError(double smoothness_weight, double grid_x, double grid_y):
    m_smoothness_weight(smoothness_weight),
    m_grid_x(grid_x), m_grid_y(grid_y) {}

  template <typename T>
  bool operator()(const T* const bl,   const T* const bottom,    const T* const br,
                  const T* const left, const T* const center, const T* const right,
                  const T* const tl,   const T* const top, const T* const tr,
                  T* residuals) const {
    try{

      // Normalize by grid size seems to make the functional less,
      // sensitive to the actual grid size used.
      residuals[0] = (left[0] + right[0] - 2*center[0])/m_grid_x/m_grid_x;   // u_xx
      residuals[1] = (br[0] + tl[0] - bl[0] - tr[0] )/4.0/m_grid_x/m_grid_y; // u_xy
      residuals[2] = residuals[1];                                           // u_yx
      residuals[3] = (bottom[0] + top[0] - 2*center[0])/m_grid_y/m_grid_y;   // u_yy

      for (int i = 0; i < 4; i++)
        residuals[i] *= m_smoothness_weight;

    } catch (const camera::PointToPixelErr& e) {
      // Failed to compute the residuals
      residuals[0] = T(1e+20);
      residuals[1] = T(1e+20);
      residuals[2] = T(1e+20);
      residuals[3] = T(1e+20);
      return false;
    }

    return true;
  }

  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(double smoothness_weight,
                                     double grid_x, double grid_y){
    return (new ceres::NumericDiffCostFunction<SmoothnessError,
            ceres::CENTRAL, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1>
            (new SmoothnessError(smoothness_weight, grid_x, grid_y)));
  }

  double m_smoothness_weight, m_grid_x, m_grid_y;
};

// A cost function that will penalize deviating by more than
// a given value from the original height
struct HeightChangeError {
  HeightChangeError(double orig_height, double max_height_change,
                    double height_change_weight):
    m_orig_height(orig_height), m_max_height_change(max_height_change),
    m_height_change_weight(height_change_weight){}

  template <typename T>
  bool operator()(const T* const center, T* residuals) const {

    double delta = std::abs(center[0] - m_orig_height);
    if (delta < m_max_height_change) {
      residuals[0] = T(0);
    }else{
      // Use a smooth function here, with a value of 0
      // when delta == m_max_height_change.
      double delta2 = delta - m_max_height_change;
      residuals[0] = T( delta2*delta2*m_height_change_weight );
    }
    return true;
  }

  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(double orig_height,
                                     double max_height_change,
                                     double height_change_weight){
    return (new ceres::NumericDiffCostFunction<HeightChangeError,
            ceres::CENTRAL, 1, 1>
            (new HeightChangeError(orig_height, max_height_change,
                                   height_change_weight)));
  }

  double m_orig_height, m_max_height_change, m_height_change_weight;
};

// Given a DEM, estimate the median grid size in x and in y in meters.
// Given that the DEM heights are in meters as well, having these grid sizes
// will make it possible to handle heights and grids in same units.
void compute_grid_sizes_in_meters(ImageView<double> const& dem,
                                  GeoReference const& geo,
                                  double nodata_val,
                                  double & grid_x, double & grid_y){

  // Initialize the outputs
  grid_x = 0; grid_y = 0;

  // Estimate the median height
  std::vector<double> heights;
  for (int col = 0; col < dem.cols(); col++) {
    for (int row = 0; row < dem.rows(); row++) {
      double h = dem(col, row);
      if (h == nodata_val) continue;
      heights.push_back(h);
    }
  }
  std::sort(heights.begin(), heights.end());
  double median_height = 0.0;
  if (!heights.empty())
    median_height = heights[heights.size()/2];

  // Find the grid sizes by estimating the Euclidean distances
  // between points of a DEM at constant height.
  std::vector<double> grid_x_vec, grid_y_vec;
  for (int col = 0; col < dem.cols()-1; col++) {
    for (int row = 0; row < dem.rows()-1; row++) {

      // The xyz position at the center grid point
      Vector2 lonlat = geo.pixel_to_lonlat(Vector2(col, row));
      Vector3 lonlat3 = Vector3(lonlat(0), lonlat(1), median_height);
      Vector3 base = geo.datum().geodetic_to_cartesian(lonlat3);

      // The xyz position at the right grid point
      lonlat = geo.pixel_to_lonlat(Vector2(col+1, row));
      lonlat3 = Vector3(lonlat(0), lonlat(1), median_height);
      Vector3 right = geo.datum().geodetic_to_cartesian(lonlat3);

      // The xyz position at the bottom grid point
      lonlat = geo.pixel_to_lonlat(Vector2(col, row+1));
      lonlat3 = Vector3(lonlat(0), lonlat(1), median_height);
      Vector3 bottom = geo.datum().geodetic_to_cartesian(lonlat3);

      grid_x_vec.push_back(norm_2(right-base));
      grid_y_vec.push_back(norm_2(bottom-base));
    }
  }

  // Median grid size
  if (!grid_x_vec.empty()) grid_x = grid_x_vec[grid_x_vec.size()/2];
  if (!grid_y_vec.empty()) grid_y = grid_y_vec[grid_y_vec.size()/2];
}

void handle_arguments(int argc, char *argv[], Options& opt) {
  po::options_description general_options("");
  general_options.add_options()
    ("input-dem,i",  po::value(&opt.input_dem),
     "The input DEM to refine using SfS.")
    ("output-prefix,o", po::value(&opt.out_prefix),
     "Prefix for output filenames.")
    ("max-iterations,n", po::value(&opt.max_iterations)->default_value(100),
     "Set the maximum number of iterations.")
    ("reflectance-type", po::value(&opt.reflectance_type)->default_value(1),
     "Reflectance type (0 = Lambertian, 1 = Lunar Lambertian).")
    ("smoothness-weight", po::value(&opt.smoothness_weight)->default_value(0.04),
     "A larger value will result in a smoother solution.")
    ("coarse-levels", po::value(&opt.coarse_levels)->default_value(0),
     "Solve the problem on a grid coarser than the original by a factor of 2 to this power, then refine the solution on finer grids.")
    ("max-coarse-iterations,n", po::value(&opt.max_coarse_iterations)->default_value(10),
     "How many iterations to do at levels of resolution coarser than the final result.")
    ("float-albedo",   po::bool_switch(&opt.float_albedo)->default_value(false)->implicit_value(true),
     "Float the albedo for each pixel. Will give incorrect results if only one image is present.")
    ("float-exposure",   po::bool_switch(&opt.float_exposure)->default_value(false)->implicit_value(true),
     "Float the exposure for each image. Will give incorrect results if only one image is present.")
    ("float-cameras",   po::bool_switch(&opt.float_cameras)->default_value(false)->implicit_value(true),
     "Float the camera pose for each image except the first one.")
    ("model-shadows",   po::bool_switch(&opt.model_shadows)->default_value(false)->implicit_value(true),
     "Model the fact that some points on the DEM are in the shadow.")
    ("shadow-thresholds", po::value(&opt.shadow_thresholds)->default_value(""),
     "Optional shadow thresholds for the input images (a list of real values in quotes).")
    ("init-dem-height", po::value(&opt.init_dem_height)->default_value(std::numeric_limits<double>::quiet_NaN()),
     "Use this value as for initial DEM heights. An input DEM still needs to be provided for georeference information.")
    ("float-dem-at-boundary",   po::bool_switch(&opt.float_dem_at_boundary)->default_value(false)->implicit_value(true),
     "Allow the DEM values at the boundary of the region to also float.")
    ("max-height-change", po::value(&opt.max_height_change)->default_value(0),
     "How much the DEM heights are allowed to differ from the initial guess, in meters. The default is 0, which means this constraint is not applied.")
    ("height-change-weight", po::value(&opt.height_change_weight)->default_value(0),
     "How much weight to give to the height change penalty (this penalty will only kick in when the DEM height changes by more than max-height-change).")
    ("bundle-adjust-prefix", po::value(&opt.bundle_adjust_prefix),
     "Use the camera adjustment obtained by previously running bundle_adjust with this output prefix.");

  general_options.add( asp::BaseOptionsDescription(opt) );

  po::options_description positional("");
  positional.add_options()
    ("input-images", po::value(&opt.input_images));

  po::positional_options_description positional_desc;
  positional_desc.add("input-images", -1);

  std::string usage("-i <input DEM> -n <max iterations> -o <output prefix> <images> [other options]");
  bool allow_unregistered = false;
  std::vector<std::string> unregistered;
  po::variables_map vm =
    asp::check_command_line(argc, argv, opt, general_options, general_options,
                            positional, positional_desc, usage,
                             allow_unregistered, unregistered);


  // If the images are tif files, and the cameras are cub files, separate them
  std::vector<std::string> images, cameras;
  for (size_t i = 0; i < opt.input_images.size(); i++) {
    std::string file = opt.input_images[i];
    if (asp::has_cam_extension(file))          // can be .cub
      cameras.push_back(file);
    else if (asp::has_image_extension(file))   // can be .cub
      images.push_back(file);
    else
      vw_throw( ArgumentErr() << "Invalid image or camera file: " << file << ".\n"
                << usage << general_options );
  }
  if (images.empty())
    images = cameras;
  if (images.size() != cameras.size()) {
    vw_throw( ArgumentErr() << "Expecting as many images as cameras.\n"
              << usage << general_options );
  }
  opt.input_images = images;
  opt.input_cameras = cameras;

  // Sanity checks
  if (opt.input_dem.empty())
    vw_throw( ArgumentErr() << "Missing input DEM.\n"
              << usage << general_options );
  if (opt.out_prefix.empty())
    vw_throw( ArgumentErr() << "Missing output prefix.\n"
              << usage << general_options );
  if (opt.max_iterations < 0)
    vw_throw( ArgumentErr() << "The number of iterations must be non-negative.\n"
              << usage << general_options );
  if (opt.input_images.empty())
  vw_throw( ArgumentErr() << "Missing input images.\n"
            << usage << general_options );

  // Parse shadow thresholds
  std::istringstream is(opt.shadow_thresholds);
  opt.shadow_threshold_vec.clear();
  float val;
  while (is >> val){
    opt.shadow_threshold_vec.push_back(val);
  }
  if (!opt.shadow_threshold_vec.empty() &&
      opt.shadow_threshold_vec.size() != opt.input_images.size())
    vw_throw(ArgumentErr()
             << "If specified, there must be as many shadow thresholds as images.\n");

  // Default thresholds are the smallest float
  if (opt.shadow_threshold_vec.empty()) {
    for (size_t i = 0; i < opt.input_images.size(); i++) {
      opt.shadow_threshold_vec.push_back(-std::numeric_limits<float>::max());
    }
  }

  // Need this to be able to load adjusted camera models. That will happen
  // in the stereo session.
  asp::stereo_settings().bundle_adjust_prefix = opt.bundle_adjust_prefix;

  // Create the output directory
  asp::create_out_dir(opt.out_prefix);

  // Turn on logging to file
  asp::log_to_file(argc, argv, "", opt.out_prefix);

  if (opt.input_images.size() <=1 && opt.float_albedo)
    vw_throw(ArgumentErr()
             << "Floating albedo is ill-posed for just one image.\n");

  if (opt.input_images.size() <=1 && opt.float_exposure)
    vw_throw(ArgumentErr()
             << "Floating exposure is ill-posed for just one image.\n");

  if (opt.input_images.size() <=1 && opt.float_dem_at_boundary)
    vw_throw(ArgumentErr()
             << "Floating the DEM at the boundary is ill-posed for just one image.\n");

}

// Run sfs at a given coarseness level
void run_sfs_level(// Fixed inputs
                   int num_iterations,  Options & opt,
                   GeoReference const& geo,
                   double smoothness_weight,
                   double nodata_val,
                   std::vector<BilinearInterpT> const& interp_images,
                   GlobalParams const& global_params,
                   std::vector<ModelParams> const & model_params,
                   // Quantities that will float
                   ImageView<double> & dem,
                   ImageView<double> & albedo,
                   std::vector<boost::shared_ptr<CameraModel> > & cameras,
                   std::vector<double> & exposures,
                   std::vector<double> & adjustments){

  int num_images = opt.input_images.size();

  // Keep here the unmodified copy of the DEM
  ImageView<double> orig_dem = copy(dem);

  int ncols = dem.cols(), nrows = dem.rows();
  vw_out() << "DEM cols and rows: " << ncols << " " << nrows << std::endl;

  // Find the grid sizes in meters. Note that dem heights are in
  // meters too, so we treat both horizontal and vertical
  // measurements in same units.
  double grid_x, grid_y;
  compute_grid_sizes_in_meters(dem, geo, nodata_val, grid_x, grid_y);
  vw_out() << "grid in x and y in meters: "
           << grid_x << ' ' << grid_y << std::endl;
  g_grid_x = &grid_x;
  g_grid_y = &grid_y;

  // Find the max DEM height
  double max_dem_height = -std::numeric_limits<double>::max();
  for (int col = 0; col < dem.cols(); col++) {
    for (int row = 0; row < dem.rows(); row++) {
      if (dem(col, row) > max_dem_height) {
        max_dem_height = dem(col, row);
      }
    }
  }
  g_max_dem_height = &max_dem_height;

  // Add a residual block for every grid point not at the boundary
  ceres::Problem problem;
  for (int col = 1; col < ncols-1; col++) {
    for (int row = 1; row < nrows-1; row++) {

      // Intensity error for each image
      for (int image_iter = 0; image_iter < num_images; image_iter++) {

        ceres::CostFunction* cost_function_img =
          IntensityError::Create(col, row, dem, geo,
                                 opt.model_shadows, max_dem_height,
                                 grid_x, grid_y,
                                 global_params, model_params[image_iter],
                                 interp_images[image_iter],
                                 cameras[image_iter]);
        ceres::LossFunction* loss_function_img = NULL;
        problem.AddResidualBlock(cost_function_img, loss_function_img,
                                 &exposures[image_iter],      // exposure
                                 &dem(col-1, row),            // left
                                 &dem(col, row),              // center
                                 &dem(col+1, row),            // right
                                 &dem(col, row+1),            // bottom
                                 &dem(col, row-1),            // top
                                 &albedo(col, row),           // albedo
                                 &adjustments[6*image_iter]); // camera

        // If to float the albedo
        if (!opt.float_albedo)
          problem.SetParameterBlockConstant(&albedo(col, row));
      }

      // Smoothness penalty
      ceres::LossFunction* loss_function_sm = NULL;
      ceres::CostFunction* cost_function_sm =
        SmoothnessError::Create(smoothness_weight, grid_x, grid_y);
      problem.AddResidualBlock(cost_function_sm, loss_function_sm,
                               &dem(col-1, row+1), &dem(col, row+1),
                               &dem(col+1, row+1),
                               &dem(col-1, row  ), &dem(col, row  ),
                               &dem(col+1, row  ),
                               &dem(col-1, row-1), &dem(col, row-1),
                               &dem(col+1, row-1));

      // Deviation from prescribed height constraint
      if (opt.max_height_change > 0 && opt.height_change_weight > 0) {
        ceres::LossFunction* loss_function_hc = NULL;
        ceres::CostFunction* cost_function_hc =
          HeightChangeError::Create(orig_dem(col, row),
                                    opt.max_height_change,
                                    opt.height_change_weight);
        problem.AddResidualBlock(cost_function_hc, loss_function_hc,
                                 &dem(col, row));
      }
    }
  }

  // If there's just one image, don't float the exposure,
  // as the problem is under-determined. If we float the
  // albedo, we will implicitly float the exposure, hence
  // keep the exposure itself fixed.
  if (!opt.float_exposure) {
    for (int image_iter = 0; image_iter < num_images; image_iter++)
      problem.SetParameterBlockConstant(&exposures[image_iter]);
  }

  if (!opt.float_cameras) {
    for (int image_iter = 0; image_iter < num_images; image_iter++)
      problem.SetParameterBlockConstant(&adjustments[6*image_iter]);
  }else{
    // Always fix the first camera, let the other ones conform to it.
    problem.SetParameterBlockConstant(&adjustments[0]);
  }

  // Variables at the boundary must be fixed.
  if (!opt.float_dem_at_boundary) {
    for (int col = 0; col < dem.cols(); col++) {
      for (int row = 0; row < dem.rows(); row++) {
        if (col == 0 || col == dem.cols() - 1 ||
            row == 0 || row == dem.rows() - 1 ) {
          problem.SetParameterBlockConstant(&dem(col, row));
        }
      }
    }
  }

  ceres::Solver::Options options;
  options.gradient_tolerance = 1e-16;
  options.function_tolerance = 1e-16;
  options.max_num_iterations = num_iterations;
  options.minimizer_progress_to_stdout = 1;
  options.num_threads = opt.num_threads;
  options.linear_solver_type = ceres::SPARSE_SCHUR;
  if (options.num_threads != 1)
    vw_throw(ArgumentErr()
             << "Due to ISIS, only a single thread can be used.\n");

  // Use a callback function at every iteration
  SfsCallback callback;
  options.callbacks.push_back(&callback);
  options.update_state_every_iteration = true;

  // A bunch of global variables to use in the callback
  g_opt            = &opt;
  g_dem            = &dem;
  g_albedo         = &albedo;
  g_geo            = &geo;
  g_global_params  = &global_params;
  g_model_params   = &model_params;
  g_interp_images  = &interp_images;
  g_cameras        = &cameras;
  g_iter           = -1; // reset the iterations for each level

  // Solve the problem
  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);
  vw_out() << summary.FullReport() << "\n";
}

int main(int argc, char* argv[]) {

  Options opt;
  try {
    handle_arguments( argc, argv, opt );

    ImageView<double> dem
      = copy(DiskImageView<double>(opt.input_dem) );
    double nodata_val = -std::numeric_limits<float>::max(); // note we use a float nodata
    if (vw::read_nodata_val(opt.input_dem, nodata_val)){
      vw_out() << "Found DEM nodata value: " << nodata_val << std::endl;
    }
    g_nodata_val = &nodata_val;
    // Replace no-data values with the mean of valid values
    double mean = 0, num = 0;
    for (int col = 0; col < dem.cols(); col++) {
      for (int row = 0; row < dem.rows(); row++) {
        if (dem(col, row) != nodata_val) {
          mean += dem(col, row);
          num += 1;
        }
      }
    }
    if (num > 0) mean /= num;
    for (int col = 0; col < dem.cols(); col++) {
      for (int row = 0; row < dem.rows(); row++) {
        if (dem(col, row) == nodata_val) {
          dem(col, row) = mean;
        }
      }
    }

    // See if to use a constant init value
    if (!boost::math::isnan(opt.init_dem_height)) {
      for (int col = 0; col < dem.cols(); col++) {
        for (int row = 0; row < dem.rows(); row++) {
          dem(col, row) = opt.init_dem_height;
        }
      }
    }

    // Read the georeference
    GeoReference geo;
    if (!read_georeference(geo, opt.input_dem))
      vw_throw( ArgumentErr() << "The input DEM has no georeference.\n" );

    // Read in the camera models for the input images.
    int num_images = opt.input_images.size();
    std::vector<boost::shared_ptr<CameraModel> > cameras;
    for (int image_iter = 0; image_iter < num_images; image_iter++){
      typedef boost::scoped_ptr<asp::StereoSession> SessionPtr;
      SessionPtr session(asp::StereoSessionFactory::create
                         (opt.stereo_session_string, opt,
                          opt.input_images[image_iter],
                          opt.input_images[image_iter],
                          opt.input_cameras[image_iter],
                          opt.input_cameras[image_iter],
                          opt.out_prefix));

      vw_out() << "Loading image and camera: " << opt.input_images[image_iter] << " "
               <<  opt.input_cameras[image_iter] << "\n";
      cameras.push_back(session->camera_model(opt.input_images[image_iter],
                                              opt.input_cameras[image_iter]));
    }

    // Since we may float the cameras, ensure our camera models are
    // always adjustable.
    for (int image_iter = 0; image_iter < num_images; image_iter++){
      CameraModel * icam
        = dynamic_cast<AdjustedCameraModel*>(cameras[image_iter].get());
      if (icam == NULL) {
        Vector2 pixel_offset;
        Vector3 position_correction;
        Quaternion<double> pose_correction = Quat(math::identity_matrix<3>());
        cameras[image_iter] = boost::shared_ptr<CameraModel>
          (new AdjustedCameraModel(cameras[image_iter], position_correction,
                                   pose_correction, pixel_offset));
      }
    }

    // Images with bilinear interpolation
    std::vector< ImageViewRef< PixelMask<float> > > masked_images(num_images);
    std::vector<BilinearInterpT> interp_images; // will have to use push_back
    float img_nodata_val = -std::numeric_limits<float>::max();
    for (int image_iter = 0; image_iter < num_images; image_iter++){
      std::string img_file = opt.input_images[image_iter];
      if (vw::read_nodata_val(img_file, img_nodata_val)){
        vw_out() << "Found image " << image_iter << " nodata value: "
                 << img_nodata_val << std::endl;
      }
      // Model the shadow threshold
      float shadow_thresh = opt.shadow_threshold_vec[image_iter];
      masked_images[image_iter]
        = create_mask_less_or_equal (DiskImageView<float>(img_file),
                                     std::max(img_nodata_val, shadow_thresh));
      interp_images.push_back(BilinearInterpT(masked_images[image_iter]));
    }
    g_img_nodata_val = &img_nodata_val;

    GlobalParams global_params;
    if (opt.reflectance_type == 0)
      global_params.reflectanceType = LAMBERT;
    else if (opt.reflectance_type == 1)
      global_params.reflectanceType = LUNAR_LAMBERT;
    else
      vw_throw( ArgumentErr()
                << "Expecting Lambertian or Lunar-Lambertian reflectance." );
    global_params.phaseCoeffC1 = 0; //1.383488;
    global_params.phaseCoeffC2 = 0; //0.501149;

    // Get the sun and camera positions from the ISIS cube
    std::vector<ModelParams> model_params;
    model_params.resize(num_images);
    for (int image_iter = 0; image_iter < num_images; image_iter++){
      IsisCameraModel* icam = dynamic_cast<IsisCameraModel*>
        (unadjusted_model(cameras[image_iter].get()));
      if (icam == NULL)
        vw_throw( ArgumentErr() << "ISIS camera model expected." );
      model_params[image_iter].sunPosition    = icam->sun_position();
      model_params[image_iter].cameraPosition = icam->camera_center();
      vw_out() << "sun position: "
               << model_params[image_iter].sunPosition << std::endl;
      vw_out() << "camera position: "
               << model_params[image_iter].cameraPosition << std::endl;
    }

    // Find the grid sizes in meters. Note that dem heights are in
    // meters too, so we treat both horizontal and vertical
    // measurements in same units.
    double grid_x, grid_y;
    compute_grid_sizes_in_meters(dem, geo, nodata_val, grid_x, grid_y);
    vw_out() << "grid in x and y in meters: "
             << grid_x << ' ' << grid_y << std::endl;
    g_grid_x = &grid_x;
    g_grid_y = &grid_y;

    // Find the max DEM height
    double max_dem_height = -std::numeric_limits<double>::max();
    for (int col = 0; col < dem.cols(); col++) {
      for (int row = 0; row < dem.rows(); row++) {
        if (dem(col, row) > max_dem_height) {
          max_dem_height = dem(col, row);
        }
      }
    }
    g_max_dem_height = &max_dem_height;

    // We have intensity = reflectance*exposure*albedo.
    // The albedo is 1 in the first approximation. Find
    // the exposure as mean(intensity)/mean(reflectance).
    // We will update the exposure later.
    std::vector<double> exposures(num_images, 0);
    for (int image_iter = 0; image_iter < num_images; image_iter++) {
      ImageView< PixelMask<double> > reflectance, intensity;
      computeReflectanceAndIntensity(dem, geo,
                                     opt.model_shadows, max_dem_height,
                                     grid_x, grid_y,
                                     model_params[image_iter],
                                     global_params,
                                     interp_images[image_iter],
                                     cameras[image_iter].get(),
                                     reflectance, intensity);

      double imgmean, imgstdev, refmean, refstdev;
      compute_image_stats(intensity, imgmean, imgstdev);
      compute_image_stats(reflectance, refmean, refstdev);
      exposures[image_iter] = imgmean/refmean;
      vw_out() << "img mean std: " << imgmean << ' ' << imgstdev << std::endl;
      vw_out() << "ref mean std: " << refmean << ' ' << refstdev << std::endl;
      vw_out() << "Exposure for image " << image_iter << ": "
                <<  exposures[image_iter] << std::endl;
    }
    g_exposures = &exposures[0];

    // Initial albedo. This will be updated later.
    ImageView<double> albedo(dem.cols(), dem.rows());
    for (int col = 0; col < albedo.cols(); col++) {
      for (int row = 0; row < albedo.rows(); row++) {
        albedo(col, row) = 1;
      }
    }

    // The initial camera adjustments. They will be updated later.
    std::vector<double> adjustments(6*num_images, 0);
    for (int image_iter = 0; image_iter < num_images; image_iter++) {
      AdjustedCameraModel * icam
        = dynamic_cast<AdjustedCameraModel*>(cameras[image_iter].get());
      if (icam == NULL)
        vw_throw(ArgumentErr() << "Expecting adjusted camera models.\n");
      Vector3 translation = icam->translation();
      Vector3 axis_angle = icam->rotation().axis_angle();
      Vector2 pixel_offset = icam->pixel_offset();
      if (pixel_offset != Vector2())
        vw_throw(ArgumentErr() << "Expecting zero pixel offset.\n");
      for (int param_iter = 0; param_iter < 3; param_iter++) {
        adjustments[6*image_iter + 0 + param_iter]
          = translation[param_iter]/g_position_scale;
        adjustments[6*image_iter + 3 + param_iter] = axis_angle[param_iter];
      }
    }
    g_adjustments = &adjustments;

    // Prepare data at each coarseness level
    int levels = opt.coarse_levels;
    std::vector<GeoReference> geos(levels+1);
    std::vector< ImageView<double> > dems(levels+1), albedos(levels+1);
    std::vector< std::vector< ImageViewRef< PixelMask<float> > > > masked_images_vec(levels+1);
    geos[0] = geo;
    dems[0] = dem;
    albedos[0] = albedo;
    masked_images_vec[0] = masked_images;
    int factor = 2;
    double sub_scale = 1.0/factor;
    std::vector<int> factors;
    factors.push_back(1);

    for (int level = 1; level <= levels; level++) {

      factors.push_back(factors[level-1]*factor);

      geos[level] = resample(geos[level-1], sub_scale);
      dems[level] = pixel_cast<double>(vw::resample_aa
                                          (pixel_cast< PixelMask<double> >
                                           (dems[level-1]), sub_scale));

      albedos[level] = pixel_cast<double>(vw::resample_aa
                                          (pixel_cast< PixelMask<double> >
                                           (albedos[level-1]), sub_scale));

      // We must write the subsampled images to disk, and then read
      // them back, as VW cannot access individual pixels of the
      // monstrosities created using the logic below, and even if it
      // could, it is best if resampling is done once, and offline,
      // rather than redoing it each time within the optimization
      // loop.
      masked_images_vec[level].resize(num_images);
      for (int image_iter = 0; image_iter < num_images; image_iter++) {
        fs::path image_path(opt.input_images[image_iter]);
        std::ostringstream os; os << "-level" << level;
        std::string sub_image = opt.out_prefix + "-"
          + image_path.stem().string() + os.str() + ".tif";
        vw_out() << "Writing subsampled image: " << sub_image << "\n";
        bool has_img_georef = false;
        GeoReference img_georef;
        bool has_img_nodata = true;
        int tile_size = 256;
        int sub_threads = 1;
        TerminalProgressCallback tpc("asp", ": ");
        asp::block_write_gdal_image
          (sub_image,
           apply_mask
           (block_rasterize
            (vw::cache_tile_aware_render
             (vw::resample_aa
              (masked_images_vec[level-1][image_iter], sub_scale),
              Vector2i(tile_size,tile_size) * sub_scale),
             tile_size, sub_threads), img_nodata_val),
           has_img_georef, img_georef, has_img_nodata, img_nodata_val, opt, tpc);

        // Read it right back
        masked_images_vec[level][image_iter]
          = create_mask(DiskImageView<float>(sub_image), img_nodata_val);

      }
    }

    // Start going from the coarsest to the finest level
    for (int level = levels; level >= 0; level--) {

      g_level = level;

      std::vector<BilinearInterpT> interp_images; // will have to use push_back
      for (int image_iter = 0; image_iter < num_images; image_iter++) {
        interp_images.push_back(BilinearInterpT(masked_images_vec[level][image_iter]));
      }

      int num_iterations;
      if (level == 0)
        num_iterations = opt.max_iterations;
      else
        num_iterations = opt.max_coarse_iterations;

      // Scale the cameras
      for (int image_iter = 0; image_iter < num_images; image_iter++) {
        AdjustedCameraModel * adj_cam
          = dynamic_cast<AdjustedCameraModel*>(cameras[image_iter].get());
        if (adj_cam == NULL)
          vw_throw( ArgumentErr() << "Expecting adjusted camera.\n");
        adj_cam->set_scale(factors[level]);
      }


      // Note that when we go the finer resolution level, we keep only
      // the camera corrections, but discard the albedo, dem, and exposures,
      // preferring to start from scratch.
      std::vector<double> local_exposures = exposures;
      g_exposures = &local_exposures[0];

      run_sfs_level(// Fixed inputs
                    num_iterations, opt, geos[level],
                    opt.smoothness_weight*factors[level]*factors[level],
                    nodata_val, interp_images,
                      global_params, model_params,
                    // Quantities that will float
                    dems[level], albedos[level], cameras, local_exposures, adjustments);

      // TODO: Study this. Turn it off apparently works better. Also note
      // that we also discard the exposures above.
      // Refine the coarse DEM and albedo
#if 0
       if (level > 0) {
         interp_image(dems[level],    sub_scale, dems[level-1]);
         interp_image(albedos[level], sub_scale, albedos[level-1]);
       }
#endif

    }

  } ASP_STANDARD_CATCHES;
}
