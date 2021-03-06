#ifndef RS_TOOLSET_SRC_CORE_STRETCH_STANDARD_DEVIATION_STRETCH_H_
#define RS_TOOLSET_SRC_CORE_STRETCH_STANDARD_DEVIATION_STRETCH_H_

#pragma warning(disable:4250)

#include <vector>

#include <opencv2/opencv.hpp>

#include <rs-toolset/stretch.h>
#include "stretch_base.h"


namespace rs_toolset {
namespace stretch {

class StandardDeviationImpl final 
    : public StretchBase, public StandardDeviation {
 public:
  StandardDeviationImpl(double scale) : scale_(scale) {}
  StandardDeviationImpl(const StandardDeviationImpl&) = delete;
  StandardDeviationImpl& operator=(const StandardDeviationImpl&) = delete;
  ~StandardDeviationImpl() = default;

  void AddBlock(
      const cv::Mat& mat,
      int idx) override;

  void SetScale(double scale) override {
    scale_ = scale;
  }
  void GetScale(double& scale) override {
    scale = scale_;
  }

 protected:
  void CreateThreshold(
      std::vector<int>& low_thres,
      std::vector<int>& high_thres) override;

 private:
  std::vector<int> pixels_counts_;
  std::vector<double> sums_;
  std::vector<double> square_sums_;

  double scale_;
};

} // stretch 
} // rs_toolset

#endif // RS_TOOLSET_SRC_CORE_STRETCH_STANDARD_DEVIATION_STRETCH_H_