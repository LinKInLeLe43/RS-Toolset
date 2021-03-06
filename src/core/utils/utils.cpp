#include <rs-toolset/utils.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gdal_alg.h>
#include <gdal_priv.h>
#include <gdalwarper.h>
#include <nlohmann/json.hpp>
#include <ogrsf_frmts.h>
#include <opencv2/opencv.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>


namespace rs_toolset {
namespace utils {

OGRGeometryUniquePtr ApplyGeoTransToPolygon(
    OGRGeometry* geometry,
    double* geotrans) {
  try {
    auto geometry_json(nlohmann::json::parse(geometry->exportToJson()));
    auto type(geometry_json.at("type").get<std::string>());
    double new_point[2];
    if (!type.compare("LineString")) {
      for (auto& point : geometry_json.at("coordinates")) {
        GDALApplyGeoTransform(
            geotrans, point[0], point[1], new_point, new_point + 1);
        point = new_point;
      }
    } else if (!type.compare("Polygon") || !type.compare("MultiLineString")) {
      for (auto& line_string : geometry_json.at("coordinates"))
        for (auto& point : line_string) {
          GDALApplyGeoTransform(
              geotrans, point[0], point[1], new_point, new_point + 1);
          point = new_point;
        }
    } else if (!type.compare("MultiPolygon")) {
      for (auto& polygon : geometry_json.at("coordinates"))
        for (auto& line_string : polygon)
          for (auto& point : line_string) {
            GDALApplyGeoTransform(
                geotrans, point[0], point[1], new_point, new_point + 1);
            point = new_point;
          }
    } else {
      return nullptr;
    }
    return OGRGeometryUniquePtr(OGRGeometryFactory::createFromGeoJson(
        geometry_json.dump().c_str()));
  } catch(...) {
    return nullptr;
  }
}

std::vector<cv::Mat> CalcHist(
    const cv::Mat& mat,
    const cv::Mat& mask_mat) {
  assert(mat.depth() == CV_8U || mat.depth() == CV_16U);
  assert(
      !mask_mat.empty() || !mask_mat.type() == CV_8UC1 || 
      mask_mat.size() == mat.size());

  int bands_count(mat.channels()), 
      hist_size[1]{ static_cast<int>(std::pow(256, mat.elemSize1())) };
  float _ranges[2]{ 0.0, static_cast<float>(hist_size[0]) };
  const float* ranges[1]{ _ranges };
  std::vector<cv::Mat> hist_mats(bands_count);
#pragma omp parallel for schedule(static, bands_count)
  for (int b = 0; b < bands_count; b++) {
    const int bands[1]{ b };
    cv::calcHist(
        &mat, 1, bands, mask_mat, hist_mats[b], 1, hist_size, ranges);
  }
  return hist_mats;
}

OGRGeometryUniquePtr CreateBorderGeometry(
    GDALDataset* source_raster_dataset,
    int overview_idx) {
  GDALDriver* memory_driver(GetGDALDriverManager()->GetDriverByName("Memory"));
  GDALDatasetUniquePtr 
      mask_raster_dataset(CreateMaskRasterDataset(
          source_raster_dataset->GetRasterBand(1), overview_idx)),
      border_vector_dataset(memory_driver->Create(
          "", 0, 0, 0, GDT_Unknown, nullptr));
  OGRFieldDefn value_field("value", OFTInteger);
  OGRLayer* border_layer(border_vector_dataset->CreateLayer(
      "", const_cast<OGRSpatialReference*>(
          source_raster_dataset->GetSpatialRef()), wkbPolygon));
  border_layer->CreateField(&value_field);
  GDALPolygonize(
      mask_raster_dataset->GetRasterBand(1), nullptr, border_layer, 0, nullptr,
      nullptr, nullptr);
  double area;
  std::pair<GIntBig, double> max_geometry(-1, -1.);
  for (const auto& border_feature : border_layer)
    if (border_feature->GetFieldAsInteger(0) == 255) {
      area = border_feature->GetGeometryRef()->toPolygon()->get_Area();
      if (area > max_geometry.second)
        max_geometry = { border_feature->GetFID(), area };
    }
  return OGRGeometryUniquePtr(
      border_layer->GetFeature(max_geometry.first)->GetGeometryRef()->clone());
}

GDALDatasetUniquePtr CreateDatasetFromMat(
    const cv::Mat& mat,
    const std::string& path,
    double* geotrans,
    OGRSpatialReference* spatial_ref,
    int bands_count,
    int* bands_map, 
    const std::string& driver_name) {
  GDALDataType dataset_type;
  switch(mat.depth()) {
    case CV_8U: {
      dataset_type = GDT_Byte;
      break;
    }
    case CV_16U: {
      dataset_type = GDT_UInt16;
      break;
    }
    case CV_16S: {
      dataset_type = GDT_Int16;
      break;
    }
    case CV_32S: {
      dataset_type = GDT_Int32;
      break;
    }
    case CV_32F: {
      dataset_type = GDT_Float32;
      break;
    }
    case CV_64F: {
      dataset_type = GDT_Float64;
      break;
    }
    default: {
      return nullptr;
    }
  }
  int x_size(mat.cols), y_size(mat.rows);
  if (!bands_count)
    bands_count = mat.channels();
  int bytes_count(static_cast<int>(mat.elemSize1()));
  GDALDriver* driver(GetGDALDriverManager()->GetDriverByName(
      driver_name.c_str()));
  GDALDatasetUniquePtr dataset(driver->Create(
      path.c_str(), x_size, y_size, bands_count, dataset_type, nullptr));
  if (geotrans)
    dataset->SetGeoTransform(geotrans);
  if (spatial_ref)
    dataset->SetSpatialRef(spatial_ref);
  dataset->RasterIO(
      GF_Write, 0, 0, x_size, y_size, mat.data, x_size, y_size, dataset_type,
      bands_count, bands_map, bytes_count * mat.channels(),
      bytes_count * mat.channels() * x_size, bytes_count);
  return dataset;
}

cv::Mat CreateHistLUT(
    const std::vector<cv::Mat>& source_hist_mats,
    const std::vector<cv::Mat>& target_hist_mats) {
  int bands_count(static_cast<int>(source_hist_mats.size())), 
      type(target_hist_mats[0].rows <= 256 ? CV_8UC(bands_count) 
          : CV_16UC(bands_count));
  cv::Mat 
      _source_hist_mat, 
      _target_hist_mat,
      lut_mat(1, source_hist_mats[0].rows , type);
  for (int b = 0; b < bands_count; b++) {
    _source_hist_mat = source_hist_mats[b].clone();
    _source_hist_mat.at<float>(0) = 0;
    cv::normalize(_source_hist_mat, _source_hist_mat, 1.0, 0.0, cv::NORM_L1);
    _target_hist_mat = target_hist_mats[b].clone();
    _target_hist_mat.at<float>(0) = 0;
    cv::normalize(_target_hist_mat, _target_hist_mat, 1.0, 0.0, cv::NORM_L1);
    auto source_hist_cdf(std::make_unique<float[]>(_source_hist_mat.rows)),
        target_hist_cdf(std::make_unique<float[]>(_target_hist_mat.rows));
    source_hist_cdf[0] = _source_hist_mat.at<float>(0);
    target_hist_cdf[0] = _target_hist_mat.at<float>(0);
    for (int i = 1; i < _source_hist_mat.rows; i++) {
      source_hist_cdf[i] = 
          source_hist_cdf[i - 1] + _source_hist_mat.at<float>(i);
      target_hist_cdf[i] =
          target_hist_cdf[i - 1] + _target_hist_mat.at<float>(i);
    }
#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < _source_hist_mat.rows; i++) {
      std::pair<double, int> min_idx{ 
          fabs(source_hist_cdf[i] - target_hist_cdf[0]) , 0};
      for (int j = 1; j < _source_hist_mat.rows; j++) {
        double temp(fabs(source_hist_cdf[i] - target_hist_cdf[j]));
        if (min_idx.first > temp) {
          min_idx.first = temp;
          min_idx.second = j;
        }
      }
      if (type == CV_8U) {
        *(lut_mat.ptr<uint8_t>(0) + bands_count * i + b) =
            static_cast<uint8_t>(min_idx.second);
      } else {
        *(lut_mat.ptr<uint16_t>(0) + bands_count * i + b) =
            static_cast<uint16_t>(min_idx.second);
      }
    }
  }
  return lut_mat;
}

GDALDatasetUniquePtr CreateMaskRasterDataset(
    GDALRasterBand* source_raster_band,
    int overview_idx) {
  auto dataset_type(source_raster_band->GetRasterDataType());
  std::unique_ptr<uint8_t[]> mask_data(nullptr);
  switch(dataset_type) {
    case GDT_Byte: {
      mask_data = CreateMaskRasterDatasetImpl<uint8_t>(source_raster_band);
      break;
    }
    case GDT_UInt16: {
      mask_data = CreateMaskRasterDatasetImpl<uint16_t>(source_raster_band);
      break;
    }
    case GDT_Int16: {
      mask_data = CreateMaskRasterDatasetImpl<int16_t>(source_raster_band);
      break;
    }
    case GDT_UInt32: {
      mask_data = CreateMaskRasterDatasetImpl<uint32_t>(source_raster_band);
      break;
    }
    case GDT_Int32: {
      mask_data = CreateMaskRasterDatasetImpl<int32_t>(source_raster_band);
      break;
    }
    case GDT_Float32: {
      mask_data = CreateMaskRasterDatasetImpl<float>(source_raster_band);
      break;
    }
    case GDT_Float64: {
      mask_data = CreateMaskRasterDatasetImpl<double>(source_raster_band);
      break;
    }
    default: {
      return nullptr;
    }
  }
  int x_size(source_raster_band->GetXSize()),
      y_size(source_raster_band->GetYSize());
  GDALDriver* mem_driver(GetGDALDriverManager()->GetDriverByName("MEM"));
  GDALDatasetUniquePtr mask_raster_dataset(mem_driver->Create(
      "", x_size, y_size, 1, GDT_Byte, nullptr));
  double geotrans[6];
  source_raster_band->GetDataset()->GetGeoTransform(geotrans);
  mask_raster_dataset->SetGeoTransform(geotrans);
  mask_raster_dataset->SetSpatialRef(
      source_raster_band->GetDataset()->GetSpatialRef());
  mask_raster_dataset->GetRasterBand(1)->RasterIO(
      GF_Write, 0, 0, x_size, y_size, mask_data.get(), x_size, y_size, GDT_Byte,
      1, x_size);
  return mask_raster_dataset;
}

cv::Mat CreateMatFromDataset(
    GDALDataset* dataset,
    int* range,
    int bands_count,
    int* bands_map) {
  auto dataset_type(dataset->GetRasterBand(1)->GetRasterDataType());
  int bytes_count, mat_type;
  if (!bands_count)
    bands_count = dataset->GetRasterCount();
  switch(dataset_type) {
    case GDT_Byte: {
      bytes_count = GDALGetDataTypeSizeBytes(GDT_Byte);
      mat_type = CV_8UC(bands_count);
      break;
    }
    case GDT_UInt16: {
      bytes_count = GDALGetDataTypeSizeBytes(GDT_UInt16);
      mat_type = CV_16UC(bands_count);
      break;
    }
    case GDT_Int16: {
      bytes_count = GDALGetDataTypeSizeBytes(GDT_Int16);
      mat_type = CV_16SC(bands_count);
      break;
    }
    case GDT_Int32: {
      bytes_count = GDALGetDataTypeSizeBytes(GDT_Int32);
      mat_type = CV_32SC(bands_count);
      break;
    }
    case GDT_Float32: {
      bytes_count = GDALGetDataTypeSizeBytes(GDT_Float32);
      mat_type = CV_32FC(bands_count);
      break;
    }
    case GDT_Float64: {
      bytes_count = GDALGetDataTypeSizeBytes(GDT_Float64);
      mat_type = CV_64FC(bands_count);
      break;
    }
    default: {
      return cv::Mat();
    }
  }
  int _range[4]{ 0, 0, dataset->GetRasterXSize() , dataset->GetRasterYSize() };
  if (!range)
    range = _range;
  cv::Mat output_mat(range[3], range[2], mat_type);
  dataset->RasterIO(
      GF_Read, range[0], range[1], range[2], range[3], output_mat.data,
      range[2], range[3], dataset_type, bands_count, bands_map,
      bytes_count * dataset->GetRasterCount(), 
      bytes_count * dataset->GetRasterCount() * range[2], bytes_count);
  return output_mat;
}

void CreateRasterPyra(
    GDALDataset* dataset,
    const std::string& compress_method,
    bool clean) {
  CPLSetConfigOption("COMPRESS_OVERVIEW", compress_method.c_str());
  CPLSetConfigOption("GDAL_OVR_CHUNKYSIZE", "4096");
  if (clean)
    dataset->BuildOverviews("", 0, nullptr, 0, nullptr, nullptr, nullptr);
  if (dataset->GetRasterBand(1)->GetOverviewCount() != 0) return;
  int x_size(dataset->GetRasterXSize()),
      y_size(dataset->GetRasterYSize()),
      downsample_factor(1),
      min_pixels_count(256),
      overviews_count(0),
      overviews[16]{ 0 };
  while (DIV_ROUND_UP(x_size, downsample_factor) > min_pixels_count ||
      DIV_ROUND_UP(y_size, downsample_factor) > min_pixels_count) {
    downsample_factor *= 2;
    overviews[overviews_count++] = downsample_factor;
  }
  dataset->BuildOverviews(
      "NEAREST", overviews_count, overviews, 0, nullptr, nullptr, nullptr);
}

void CreateRange(
    int block_idx,
    int block_cols_count,
    int block_rows_count,
    int block_x_size,
    int block_y_size,
    int last_block_x_size,
    int last_block_y_size,
    int(&range)[4]) {
  int block_row(block_idx / block_cols_count), 
      block_col(block_idx % block_cols_count);
  range[0] = block_col * block_x_size;
  range[1] = block_row * block_y_size;
  range[2] = 
      block_col != (block_cols_count - 1) ? block_x_size : last_block_x_size;
  range[3] = 
      block_row != (block_rows_count - 1) ? block_y_size : last_block_y_size;
}

RPCTransPtr CreateRPCTrans(
    const std::string& path,
    bool reserved,
    RPCSource rpc_source) {
  GDALDatasetUniquePtr dataset(GDALDataset::Open(
      path.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
  char** rpc_info_text;
  GDALRPCInfoV2 rpc_info;
  switch (rpc_source) {
    case RPCSource::kInternal: {
      rpc_info_text = dataset->GetMetadata("RPC");
      break;
    }
    case RPCSource::kRPBFile: {
      auto pos(static_cast<int>(path.rfind('.')));
      rpc_info_text = LoadRPBFile(path.substr(0, pos) + ".rpb");
      break;
    }
    case RPCSource::kRPCFile: {
      break;
    }
  }
  GDALExtractRPCInfoV2(rpc_info_text, &rpc_info);
  return RPCTransPtr(
      GDALCreateRPCTransformerV2(&rpc_info, reserved, 0.0, nullptr),
      [](void* p) { GDALDestroyRPCTransformer(p); });
}

int DateMatching(const std::string& string) {
  std::regex r(
      "20[0-9]{2}((0[1-9]|1[0-2])"
      "(0[1-9]|1[0-9]|2[0-8])|(0[13-9]|1[0-2])(29|30)|(0[13578]|1[02])31)");
  std::smatch result;
  if (std::regex_search(string, result, r)) {
    return std::stoi(result.str());
  } else {
    return 0;
  }
}

OGRGeometryUniquePtr FindBiggestPolygon(OGRGeometry* geometry) {
  assert(geometry->getGeometryType() == wkbPolygon || 
      geometry->getGeometryType() == wkbMultiPolygon);

  if (geometry->getGeometryType() == wkbPolygon)
    return OGRGeometryUniquePtr(geometry->clone());
  double area;
  std::pair<int, double> geometry_max(-1, -1.);
  for (int i = 0; i < geometry->toMultiPolygon()->getNumGeometries(); i++) {
    area = geometry->toMultiPolygon()->getGeometryRef(i)->get_Area();
    if (area > geometry_max.second)
      geometry_max = { i, area };
  }
  return OGRGeometryUniquePtr(
      geometry->toMultiPolygon()->getGeometryRef(geometry_max.first)->clone());
}

std::string GetDate() {
  std::time_t now(
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
  tm _tm;
  localtime_s(&_tm, &now);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%A_%d_%B_%Y_%Hh_%Mm_%Ss", &_tm);
  return buf;
}

void InitGdal(const std::string& app_path) {
  _putenv_s(
      "PROJ_LIB",
      std::filesystem::path(app_path).parent_path().string().c_str());
  GDALAllRegister();
  CPLSetConfigOption("GDAL_NUM_THREADS", "ALL_CPUS");
  CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");
  GDALSetCacheMax64(static_cast<GIntBig>(8.0) * 1024 * 1024 * 1024);
}

void InitSpdlog(
    const std::string& logger_name,
    spdlog::level::level_enum log_level) {
  auto console_sink(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  console_sink->set_level(spdlog::level::info);
  auto file_sink(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      "logs/" + logger_name + "-" + GetDate() + ".txt", true));
  file_sink->set_level(spdlog::level::debug);
  spdlog::set_default_logger(std::make_shared<spdlog::logger>(
      logger_name, spdlog::sinks_init_list({ console_sink, file_sink })));
  spdlog::default_logger()->flush_on(spdlog::level::debug);
  spdlog::set_level(log_level);
}

std::map<std::string, std::string> rpb_names{
    {"errBias", "ERR_BIAS"}, {"errRand", "ERR_RAND"},
    { "lineOffset", "LINE_OFF" }, { "sampOffset", "SAMP_OFF" },
    { "latOffset", "LAT_OFF" }, { "longOffset", "LONG_OFF" },
    { "heightOffset", "HEIGHT_OFF" }, { "lineScale", "LINE_SCALE" },
    { "sampScale", "SAMP_SCALE" }, { "latScale", "LAT_SCALE" }, 
    { "longScale", "LONG_SCALE" }, { "heightScale", "HEIGHT_SCALE" },
    { "lineNumCoef", "LINE_NUM_COEFF" }, { "lineDenCoef", "LINE_DEN_COEFF" },
    { "sampNumCoef", "SAMP_NUM_COEFF" }, { "sampDenCoef", "SAMP_DEN_COEFF" } };

char** LoadRPBFile(const std::string& rpb_path) {
  std::ifstream rpb(rpb_path);
  std::string line, word, temp, name, value;
  char** rpc_info(nullptr);
  int idx(0);
  while (std::getline(rpb, line)) {
    std::istringstream sstream1(line);
    sstream1 >> word;
    if (rpb_names.find(word) != rpb_names.end()) {
      name = rpb_names.at(word);
      if (name.find("COEFF") == std::string::npos) {
        sstream1 >> value;
        sstream1 >> value;
      } else {
        value.clear();
        for (int i = 0; i < 20; i++) {
          std::getline(rpb, line);
          std::istringstream sstream2(line);
          sstream2 >> temp;
          value.append(temp);
          value.back() = ' ';
        }
        value.pop_back();
      }
      value.pop_back();
      rpc_info = CSLSetNameValue(rpc_info, name.c_str(), value.c_str());
    }
  }
  return rpc_info;
}

cv::Mat TransformMat(
    const cv::Mat& source_mat,
    const cv::Mat& lut_mat) {
  assert(lut_mat.rows == std::pow(256, source_mat.elemSize1()));

  cv::Mat output_mat(source_mat.size(), source_mat.type());
  if (lut_mat.depth() == CV_8U) {
    cv::LUT(source_mat, lut_mat, output_mat);
  } else {
    auto lut_ptr(lut_mat.ptr<uint16_t>(0));
#pragma omp parallel for schedule(dynamic)
    for (int row = 0; row < source_mat.rows; row++) {
      auto source_ptr(source_mat.ptr<uint16_t>(row));
      auto output_ptr(output_mat.ptr<uint16_t>(row));
      for (int col = 0; col < source_mat.cols; col++) {
        for (int b = 0; b < source_mat.channels(); b++) {
          uint16_t idx(*(source_ptr + source_mat.channels() * col + b));
          *(output_ptr + source_mat.channels() * col + b) =
            *(lut_ptr + source_mat.channels() * idx + b);
        }
      }
    }
  }
  return output_mat;
}

void WarpByGeometry(
    const std::vector<GDALDataset*>& source_rasters_dataset,
    GDALDataset* result_raster_dataset,
    OGRGeometry* geometry,
    GDALResampleAlg resample_arg,
    double nodata_value) {
  int bands_count(result_raster_dataset->GetRasterCount());
  auto bands_map(std::make_unique<int[]>(bands_count));
  auto bands_nodata(std::make_unique<double[]>(bands_count));
  for (int b = 0; b < bands_count; b++) {
    bands_map[b] = b + 1;
    bands_nodata[b] = nodata_value;
  }
  std::unique_ptr<void, void(*)(void*)> trans_arg(
      nullptr, [](void* p) { GDALDestroyGenImgProjTransformer(p); });
  std::unique_ptr<GDALWarpOptions> warp_options(GDALCreateWarpOptions());
  warp_options->eResampleAlg = resample_arg;
  warp_options->eWorkingDataType = GDT_Byte;
  warp_options->hDstDS = result_raster_dataset;
  warp_options->nBandCount = bands_count;
  warp_options->panSrcBands = bands_map.get();
  warp_options->panDstBands = bands_map.get();
  warp_options->padfSrcNoDataReal = bands_nodata.get();
  warp_options->padfDstNoDataReal = bands_nodata.get();
  warp_options->pfnTransformer = GDALGenImgProjTransform;
  warp_options->dfWarpMemoryLimit = 8.0 * 1024 * 1024 * 1024;
  warp_options->papszWarpOptions = CSLSetNameValue(
      warp_options->papszWarpOptions, "SKIP_NOSOURCE", "YES");
  warp_options->papszWarpOptions = CSLSetNameValue(
      warp_options->papszWarpOptions, "CUTLINE_ALL_TOUCHED", "TRUE");
  GDALWarpOperation warp_operation;
  for (const auto& source_raster_dataset : source_rasters_dataset) {
    trans_arg.reset(GDALCreateGenImgProjTransformer2(
        source_raster_dataset, result_raster_dataset, nullptr));
    OGRGeometryUniquePtr cutline_geometry(nullptr);
    if (geometry) {
      double geotrans[6], inv_geotrans[6];
      source_raster_dataset->GetGeoTransform(geotrans);
      GDALInvGeoTransform(geotrans, inv_geotrans);
      cutline_geometry = ApplyGeoTransToPolygon(geometry, inv_geotrans);
      warp_options->hCutline = cutline_geometry.get();
    }
    warp_options->hSrcDS = source_raster_dataset;
    warp_options->pTransformerArg = trans_arg.get();
    warp_operation.Initialize(warp_options.get());
    warp_operation.ChunkAndWarpMulti(
        0, 0, result_raster_dataset->GetRasterXSize(),
        result_raster_dataset->GetRasterYSize());
  }
}

} // utils
} // rs_toolset