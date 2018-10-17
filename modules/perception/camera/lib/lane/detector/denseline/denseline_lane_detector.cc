/******************************************************************************
* Copyright 2018 The Apollo Authors. All Rights Reserved.
*
* Licensed under the Apache License, Version 2.0 (the License);
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an AS IS BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*****************************************************************************/
#include "modules/perception/camera/lib/lane/detector/denseline/denseline_lane_detector.h"

#include <map>
#include <algorithm>

#include "modules/common/util/file.h"
#include "modules/perception/camera/common/util.h"
#include "modules/perception/lib/io/file_util.h"
#include "modules/perception/inference/utils/resize.h"
#include "modules/perception/inference/inference_factory.h"

namespace apollo {
namespace perception {
namespace camera {

bool DenselineLaneDetector::Init(const LaneDetectorInitOptions &options) {
  std::string proto_path =
      lib::FileUtil::GetAbsolutePath(options.root_dir, options.conf_file);
  if (!apollo::common::util::GetProtoFromFile(proto_path, &denseline_param_)) {
    AINFO << "load proto param failed, root dir: " << options.root_dir;
    return false;
  }
  std::string param_str;
  google::protobuf::TextFormat::PrintToString(denseline_param_, &param_str);
  AINFO << "denseline param: " << param_str;

  const auto model_param = denseline_param_.model_param();
  std::string model_root =
      lib::FileUtil::GetAbsolutePath(options.root_dir,
                                     model_param.model_name());
  std::string proto_file =
      lib::FileUtil::GetAbsolutePath(model_root,
                                     model_param.proto_file());
  std::string weight_file =
      lib::FileUtil::GetAbsolutePath(model_root,
                                     model_param.weight_file());
  base_camera_model_ = options.base_camera_model;
  if (base_camera_model_ == nullptr) {
    AERROR << "options.intrinsic is nullptr!";
    input_height_ = 1080;
    input_width_ = 1920;
  } else {
    input_height_ = base_camera_model_->get_height();
    input_width_ = base_camera_model_->get_width();
  }
  CHECK(input_width_ > 0) << "input width should be more than 0";
  CHECK(input_height_ > 0) << "input height should be more than 0";

  AINFO << "input_height: " << input_height_;
  AINFO << "input_width: " << input_width_;

  image_scale_ = model_param.resize_scale();
  input_offset_y_ = model_param.input_offset_y();
  input_offset_x_ = model_param.input_offset_x();
  crop_height_ = model_param.crop_height();
  crop_width_ = model_param.crop_width();

  CHECK_LE(crop_height_, input_height_)
    << "crop height larger than input height";
  CHECK_LE(crop_width_, input_width_)
    << "crop width larger than input width";

  if (model_param.is_bgr()) {
    data_provider_image_option_.target_color = base::Color::BGR;
    image_mean_[0] = model_param.mean_b();
    image_mean_[1] = model_param.mean_g();
    image_mean_[2] = model_param.mean_r();
  } else {
    data_provider_image_option_.target_color = base::Color::RGB;
    image_mean_[0] = model_param.mean_r();
    image_mean_[1] = model_param.mean_g();
    image_mean_[2] = model_param.mean_b();
  }
  data_provider_image_option_.do_crop = true;
  data_provider_image_option_.crop_roi.x = input_offset_x_;
  data_provider_image_option_.crop_roi.y = input_offset_y_;
  data_provider_image_option_.crop_roi.height = crop_height_;
  data_provider_image_option_.crop_roi.width = crop_width_;

  cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, options.gpu_id);
  AINFO << "GPU: " << prop.name;

  const auto net_param = denseline_param_.net_param();
  net_inputs_.push_back(net_param.in_blob());
  net_outputs_.push_back(net_param.out_blob());
  std::copy(net_param.internal_blob_int8().begin(),
            net_param.internal_blob_int8().end(),
            std::back_inserter(net_outputs_));

  for (auto name : net_inputs_) {
    AINFO << "net input blobs: " << name;
  }
  for (auto name : net_outputs_) {
    AINFO << "net output blobs: " << name;
  }

  const auto &model_type = model_param.model_type();
  AINFO << "model_type: " << model_type;
  rt_net_.reset(
      inference::CreateInferenceByName(model_type,
                                                        proto_file,
                                                        weight_file,
                                                        net_outputs_,
                                                        net_inputs_,
                                                        model_root));
  CHECK(rt_net_ != nullptr);
  rt_net_->set_gpu_id(options.gpu_id);

  resize_height_ = crop_height_ * image_scale_;
  resize_width_ = crop_width_ * image_scale_;
  CHECK(resize_width_ > 0) << "resize width should be more than 0";
  CHECK(resize_height_ > 0) << "resize height should be more than 0";

  std::vector<int> shape = {1, 3, resize_height_, resize_width_};

  std::map<std::string, std::vector<int>>
      input_reshape{{net_inputs_[0], shape}};
  AINFO << "input_reshape: "
           << input_reshape[net_inputs_[0]][0] << ", "
           << input_reshape[net_inputs_[0]][1] << ", "
           << input_reshape[net_inputs_[0]][2] << ", "
           << input_reshape[net_inputs_[0]][3];
  if (!rt_net_->Init(input_reshape)) {
    AINFO << "net init fail.";
    return false;
  }

  for (auto &input_blob_name : net_inputs_) {
    auto input_blob = rt_net_->get_blob(input_blob_name);
    AINFO << input_blob_name << ": " << input_blob->channels() << " "
             << input_blob->height() << " " << input_blob->width();
  }

  for (auto &output_blob_name : net_outputs_) {
    auto output_blob = rt_net_->get_blob(output_blob_name);
    AINFO << output_blob_name << " : " << output_blob->channels() << " "
             << output_blob->height() << " " << output_blob->width();
  }

  return true;
}

bool DenselineLaneDetector::Detect(
    const LaneDetectorOptions &options,
    CameraFrame *frame) {
  if (frame == nullptr) {
    AINFO << "camera frame is empty.";
    return false;
  }

  auto data_provider = frame->data_provider;
  CHECK_EQ(input_width_, data_provider->src_width())
    << "Input size is not correct: "
    << input_width_ << " vs " << data_provider->src_width();
  CHECK_EQ(input_height_, data_provider->src_height())
    << "Input size is not correct: "
    << input_height_ << " vs " << data_provider->src_height();

  CHECK(data_provider->GetImage(data_provider_image_option_, &image_src_));

  //  bottom 0 is data
  auto input_blob = rt_net_->get_blob(net_inputs_[0]);
  auto blob_channel = input_blob->channels();
  auto blob_height = input_blob->height();
  auto blob_width = input_blob->width();
  AINFO << "input_blob: " << blob_channel << " "
           << blob_height << " " << blob_width << std::endl;

  CHECK_EQ(blob_height, resize_height_) << "height is not equal" << blob_height
                                        << " vs " << resize_height_;
  CHECK_EQ(blob_width, resize_width_) << "width is not equal" << blob_width
                                      << " vs " << resize_width_;
  ADEBUG << "image_blob: " << image_src_.blob()->shape_string();
  ADEBUG << "input_blob: " << input_blob->shape_string();

  inference::ResizeGPU(image_src_,
            input_blob,
            static_cast<int>(crop_width_),
            0,
            image_mean_[0],
            image_mean_[1],
            image_mean_[2],
            false,
            static_cast<float>(1.0));
  AINFO << "resize gpu finish.";
  cudaDeviceSynchronize();
  rt_net_->Infer();
  AINFO << "infer finish.";

  frame->lane_detected_blob = rt_net_->get_blob(net_outputs_[0]);
  ADEBUG << frame->lane_detected_blob->shape_string();
  return true;
}

std::string DenselineLaneDetector::Name() const {
  return "DenselineLaneDetector";
}

REGISTER_LANE_DETECTOR(DenselineLaneDetector);

}  // namespace camera
}  // namespace perception
}  // namespace apollo