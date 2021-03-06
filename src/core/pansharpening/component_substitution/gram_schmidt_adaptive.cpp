#include "gram_schmidt_adaptive.h"

#include <cstdint>

#include <vector>

#include <Eigen/Dense>
#include <opencv2/opencv.hpp>

#include <rs-toolset/pansharpening.h>
#include <rs-toolset/utils.hpp>


namespace rs_toolset {
namespace pansharpening {

void GramSchmidtAdaptiveImpl::UpdateDownsampleInfo(
    const Data& data,
    void* s) {
  spdlog::debug("Updating the downsample information");
  auto _s(static_cast<Statistic*>(s));
#pragma omp parallel for schedule(dynamic)
  for (int row = 0; row < data.mat.rows; row ++) {
    for (int col = 0; col < data.mat.cols; col ++) {
      int idx(row * data.mat.cols + col);
      bool nodata(false);
      if (data.mat.depth() == CV_8U) {
        if (data.mat.at<uint8_t>(row, col) == 0)
          nodata = true;
        for (int b = 0; b < data.mats.size(); b++)
          _s->A(idx, b) = nodata ? 0 : data.mats[b].at<uint8_t>(row, col);
        _s->b(idx) = nodata ? 0 : data.mat.at<uint8_t>(row, col);
      } else {
        if (data.mat.at<uint16_t>(row, col) == 0)
          nodata = true;
        for (int b = 0; b < data.mats.size(); b++)
          _s->A(idx, b) = nodata ? 0 : data.mats[b].at<uint16_t>(row, col);
        _s->b(idx) = nodata ? 0 : data.mat.at<uint16_t>(row, col);
      }
    }
  }
  spdlog::info("Updating the downsample information - done");
}

std::vector<double> GramSchmidtAdaptiveImpl::CreateWeights(void* s) {
  spdlog::debug("Creating the weights");
  auto _s(static_cast<Statistic*>(s));
  Eigen::VectorXf solve(
      _s->A.bdcSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(_s->b));
  std::vector<double> weights(solve.rows());
  for (int b = 0; b < solve.rows(); b++)
    weights[b] = solve(b);
  spdlog::info("Creating the weights - done");
  return weights;
}

void GramSchmidtAdaptiveImpl::UpdateStatistic(
    const Data& data,
    const std::vector<double>& weights,
    void* s) {
  spdlog::debug("Updating the statistic struct");
  auto _s(static_cast<Statistic*>(s));
  int bands_count(static_cast<int>(data.mats.size()));
  cv::Mat synthetic_low_reso_pan_mat;
  data.mats[0].convertTo(synthetic_low_reso_pan_mat, CV_16SC1, weights[0]);
  double synthetic_low_reso_pan_mean(weights[0] * _s->upsampled_ms_means[0]);
  for (int i = 1; i < bands_count; i++) {
    synthetic_low_reso_pan_mat += weights[i] * data.mats[i];
    synthetic_low_reso_pan_mean += weights[i] * _s->upsampled_ms_means[i];
  }
  synthetic_low_reso_pan_mat.convertTo(
      synthetic_low_reso_pan_mat, data.mat.type());
  std::vector<cv::Mat> 
      cur_pan_hist_mat(utils::CalcHist(data.mat, cv::Mat())),
      cur_synthetic_low_reso_pan_hist_mat(utils::CalcHist(
          synthetic_low_reso_pan_mat, cv::Mat()));
  if (_s->pan_hist_mat.empty()) {
    _s->pan_hist_mat.push_back(cur_pan_hist_mat[0]);
    _s->synthetic_low_reso_pan_hist_mat.push_back(
        cur_synthetic_low_reso_pan_hist_mat[0]);
  } else {
    _s->pan_hist_mat[0] += cur_pan_hist_mat[0];
    _s->synthetic_low_reso_pan_hist_mat[0] +=
        cur_synthetic_low_reso_pan_hist_mat[0];
  }
  _s->upsample_pixels_count += cv::countNonZero(data.mat);
  _s->synthetic_low_reso_pan_sum_ += cv::sum(synthetic_low_reso_pan_mat)[0];
  _s->synthetic_low_reso_pan_square_sum_ +=
      synthetic_low_reso_pan_mat.dot(synthetic_low_reso_pan_mat);
#pragma omp parallel for schedule(static, bands_count)
  for (int b = 0; b < bands_count; b++) {
    cv::Mat temp1, temp2;
    synthetic_low_reso_pan_mat.convertTo(temp1, CV_32SC1);
    data.mats[b].convertTo(temp2, CV_32SC1);
    _s->upsampled_ms_sums_[b] += cv::sum(data.mats[b])[0];
    _s->product_sums_[b] += cv::sum(temp1.mul(temp2))[0];
  }
  spdlog::info("Updating the statistic struct - done");
}

std::vector<double> GramSchmidtAdaptiveImpl::CreateInjectionGains(void* s) {
  spdlog::debug("Creating the injection gains");
  auto _s(static_cast<Statistic*>(s));
  int bands_count(static_cast<int>(_s->upsampled_ms_means.size()));
  std::vector<double> injection_gains(bands_count);
  double var(
      _s->upsample_pixels_count * _s->synthetic_low_reso_pan_square_sum_ -
      _s->synthetic_low_reso_pan_sum_ * _s->synthetic_low_reso_pan_sum_);
#pragma omp parallel for schedule(static, bands_count)
  for (int b = 0; b < injection_gains.size(); b++)
    injection_gains[b] = 
        (_s->upsample_pixels_count * _s->product_sums_[b] -
        _s->synthetic_low_reso_pan_sum_ * _s->upsampled_ms_sums_[b]) / var;
  spdlog::info("Creating the injection gains - done");
  return injection_gains;
}

std::vector<cv::Mat> GramSchmidtAdaptiveImpl::CreateDeltaMats(
    const Data& data,
    const std::vector<double>& weights,
    void* s) {
  spdlog::debug("Creating the delta mats");
  auto _s(static_cast<Statistic*>(s));
  int bands_count(static_cast<int>(data.mats.size()));
  cv::Mat synthetic_low_reso_pan_mat, delta_mat;
  data.mats[0].convertTo(delta_mat, CV_16SC1, -weights[0]);
  for (int i = 1; i < bands_count; i++)
    delta_mat -= weights[i] * data.mats[i];
  if (weights.size() == bands_count + 1) {
    cv::Mat constant_mat;
    cv::threshold(data.mat, constant_mat, 0, weights.back(), cv::THRESH_BINARY);
    delta_mat -= constant_mat;
  }
  cv::Mat(-delta_mat).convertTo(synthetic_low_reso_pan_mat, CV_16UC1);
  delta_mat += utils::TransformMat(
      data.mat, utils::CreateHistLUT(
          _s->pan_hist_mat, _s->synthetic_low_reso_pan_hist_mat));
  spdlog::info("Creating the delta mats - done");
  return std::vector<cv::Mat>(bands_count, delta_mat);
}

std::shared_ptr<GramSchmidtAdaptive> GramSchmidtAdaptive::Create(
    int block_size) {
  return std::make_shared<GramSchmidtAdaptiveImpl>(block_size);
}

} // pansharpening
} // rs_toolset