// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "driver/nvidia_tensorrt/program.h"
#include <unistd.h>
#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>
#include "driver/nvidia_tensorrt/converter/converter.h"
#include "driver/nvidia_tensorrt/optimizer/remove_reshape_before_fully_connected.h"
#include "driver/nvidia_tensorrt/optimizer/unpack_op_fusion.h"
#include "optimizer/fuse_matmul_add_into_fully_connected.h"
#include "optimizer/partition_model_into_submodels.h"
#include "utility/debug.h"
#include "utility/logging.h"
#include "utility/modeling.h"
#include "utility/string.h"
#include "utility/utility.h"

namespace nnadapter {
namespace nvidia_tensorrt {

// Malloc gpu memory according to max dims
static void SetMaxDims(const NNAdapterOperandType& type, Tensor* tensor) {
  // Get max dims
  std::vector<int> shape;
  auto& dims = type.dimensions;
  if (dims.dynamic_count == 0) {
    shape = std::vector<int>(dims.data, dims.data + dims.count);
  } else {
    NNADAPTER_CHECK_EQ(dims.dynamic_count, 3U);
    shape = std::vector<int>(dims.dynamic_data[2],
                             dims.dynamic_data[2] + dims.count);
  }
  // Set tensor
  tensor->SetDateType(ConvertToNVDataType(type.precision));
  tensor->Resize(shape);
}

Context::Context(void* device, const char* properties) : device_(device) {
  // Extract the runtime parameters from the context properties
  NNADAPTER_VLOG(1) << "properties: " << std::string(properties);
  auto key_values = GetKeyValues(properties);
  // Device type
  std::string device_type = key_values.count(NVIDIA_TENSORRT_DEVICE_TYPE)
                                ? key_values[NVIDIA_TENSORRT_DEVICE_TYPE]
                                : GetStringFromEnv(NVIDIA_TENSORRT_DEVICE_TYPE);
  if (!device_type.empty()) {
    if (device_type == "GPU") {
      device_type_ = nvinfer1::DeviceType::kGPU;
    } else if (device_type == "DLA") {
      device_type_ = nvinfer1::DeviceType::kDLA;
    } else {
      NNADAPTER_LOG(FATAL) << "Not support NVIDIA_TENSORRT_DEVICE_TYPE: "
                           << device_type;
    }
  }
  // Device id
  device_id_ = key_values.count(NVIDIA_TENSORRT_DEVICE_ID)
                   ? atoi(key_values[NVIDIA_TENSORRT_DEVICE_ID].c_str())
                   : GetIntFromEnv(NVIDIA_TENSORRT_DEVICE_ID);
  device_id_ = device_id_ > 0 ? device_id_ : 0;
  // Precision
  std::string precision = key_values.count(NVIDIA_TENSORRT_PRECISION)
                              ? key_values[NVIDIA_TENSORRT_PRECISION]
                              : GetStringFromEnv(NVIDIA_TENSORRT_PRECISION);
  if (!precision.empty()) {
    if (precision == "float32") {
      precision_ = kFloat32;
    } else if (precision == "float16") {
      precision_ = kFloat16;
    } else if (precision == "int8") {
      precision_ = kInt8;
    } else {
      NNADAPTER_LOG(FATAL) << "Not support NVIDIA_TENSORRT_PRECISION: "
                           << precision;
    }
  }
  // Fallback
  gpu_fallback_ = key_values.count(NVIDIA_TENSORRT_GPU_FALLBACK)
                      ? key_values[NVIDIA_TENSORRT_GPU_FALLBACK] == "1"
                      : GetBoolFromEnv(NVIDIA_TENSORRT_GPU_FALLBACK, true);
  // Calibration dataset
  calibration_dataset_path_ =
      key_values.count(NVIDIA_TENSORRT_CALIBRATION_DATASET_PATH)
          ? key_values[NVIDIA_TENSORRT_CALIBRATION_DATASET_PATH]
          : GetStringFromEnv(NVIDIA_TENSORRT_CALIBRATION_DATASET_PATH);
  // Calibration table
  calibration_table_path_ =
      key_values.count(NVIDIA_TENSORRT_CALIBRATION_TABLE_PATH)
          ? key_values[NVIDIA_TENSORRT_CALIBRATION_TABLE_PATH]
          : GetStringFromEnv(NVIDIA_TENSORRT_CALIBRATION_TABLE_PATH);
  if (precision_ == kInt8) {
    NNADAPTER_CHECK(!calibration_dataset_path_.empty() ||
                    !calibration_table_path_.empty())
        << "Either NVIDIA_TENSORRT_CALIBRATION_DATASET_PATH or "
           "NVIDIA_TENSORRT_CALIBRATION_TABLE_PATH should be set if precision "
           "is int8.";
  }
  // Cuda operations list
  auto cuda_operations_str =
      key_values.count(NVIDIA_TENSORRT_CUDA_OPERATIONS_LIST)
          ? key_values[NVIDIA_TENSORRT_CUDA_OPERATIONS_LIST]
          : GetStringFromEnv(NVIDIA_TENSORRT_CUDA_OPERATIONS_LIST);
  if (!cuda_operations_str.empty()) {
    auto operations = string_split(cuda_operations_str, ",");
    for (auto operation : operations) {
      if (operation == "NNADAPTER_SOFTMAX") {
        cuda_operations_.push_back(NNADAPTER_SOFTMAX);
      } else {
        NNADAPTER_LOG(FATAL) << "Not support.";
      }
    }
  }
  // Host operations list
  auto host_operations_str =
      key_values.count(NVIDIA_TENSORRT_HOST_OPERATIONS_LIST)
          ? key_values[NVIDIA_TENSORRT_HOST_OPERATIONS_LIST]
          : GetStringFromEnv(NVIDIA_TENSORRT_HOST_OPERATIONS_LIST);
  if (!host_operations_str.empty()) {
    auto operations = string_split(host_operations_str, ",");
    for (auto operation : operations) {
      if (operation == "NNADAPTER_SOFTMAX") {
        host_operations_.push_back(NNADAPTER_SOFTMAX);
      } else {
        NNADAPTER_LOG(FATAL) << "Not support.";
      }
    }
  }
}

void TensorrtProgram::Clear() {
  tensors_.clear();
  input_indices_.clear();
  output_indices_.clear();
  input_types_.clear();
  output_types_.clear();
}

int TensorrtProgram::Build() {
  Clear();
  // 1. Build model_ to engine_
  if (cache_->empty()) {
    NNADAPTER_CHECK_EQ(BuildFromModel(), NNADAPTER_NO_ERROR);
    cache_->resize(plan_->size());
    memcpy(cache_->data(), plan_->data(), sizeof(int8_t) * plan_->size());
  } else {
    NNADAPTER_CHECK_EQ(BuildFromCache(), NNADAPTER_NO_ERROR);
  }
  // 2. Identify the inputs and outputs
  size_t input_count = model_->input_operands.size();
  NNADAPTER_VLOG(3) << "Model input count: " << input_count;
  input_types_.resize(input_count);
  size_t output_count = model_->output_operands.size();
  NNADAPTER_VLOG(3) << "Model output count: " << output_count;
  output_types_.resize(output_count);
  for (size_t i = 0; i < input_count; i++) {
    auto operand = model_->input_operands.at(i);
    input_types_.at(i) = operand->type;
    ConvertDynamicDimensions(&input_types_.at(i));
  }
  for (size_t i = 0; i < output_count; i++) {
    auto operand = model_->output_operands.at(i);
    output_types_.at(i) = operand->type;
    ConvertDynamicDimensions(&output_types_.at(i));
  }
  // 3. Create execution_context_
  execution_context_.reset(engine_->createExecutionContext());
  NNADAPTER_CHECK(execution_context_);
  // 4. Prepare input/output indexes
  NNADAPTER_CHECK_EQ(static_cast<size_t>(engine_->getNbBindings()),
                     input_count + output_count);
  for (size_t i = 0; i < input_count; i++) {
    std::string name = "input" + std::to_string(i);
    input_indices_.push_back(engine_->getBindingIndex(name.c_str()));
  }
  for (size_t i = 0; i < output_count; i++) {
    std::string name = "output" + std::to_string(i);
    output_indices_.push_back(engine_->getBindingIndex(name.c_str()));
  }
  return NNADAPTER_NO_ERROR;
}

void TensorrtProgram::CompleteConfig() {
  config_.reset(builder_->createBuilderConfig());
  NNADAPTER_CHECK(config_);
  // Set dynamic shapes
  if (with_dynamic_shape_) {
    for (auto operand : model_->input_operands) {
      auto type = operand->type;
      auto& dimensions = type.dimensions;
      if (dimensions.dynamic_count == 0) continue;
      ConvertDynamicDimensions(&type);
      NNADAPTER_CHECK_EQ(dimensions.dynamic_count, 3);
      // need not to delete by user
      auto profile = builder_->createOptimizationProfile();
      auto name = tensors_.at(operand).back()->getName();
      nvinfer1::Dims dims;
      dims.nbDims = dimensions.count;
      memcpy(dims.d, dimensions.dynamic_data[0], sizeof(int32_t) * dims.nbDims);
      profile->setDimensions(name, nvinfer1::OptProfileSelector::kOPT, dims);
      memcpy(dims.d, dimensions.dynamic_data[1], sizeof(int32_t) * dims.nbDims);
      profile->setDimensions(name, nvinfer1::OptProfileSelector::kMIN, dims);
      memcpy(dims.d, dimensions.dynamic_data[2], sizeof(int32_t) * dims.nbDims);
      profile->setDimensions(name, nvinfer1::OptProfileSelector::kMAX, dims);
      config_->addOptimizationProfile(profile);
    }
  }
  // Set device_type
  auto device_type = context_->DeviceType();
  config_->setDefaultDeviceType(device_type);
  // Set device_id
  if (device_type == nvinfer1::DeviceType::kDLA) {
    int device_id = context_->DeviceId();
    if (builder_->getNbDLACores() > device_id) {
      config_->setDLACore(device_id);
      NNADAPTER_VLOG(1) << "Tring to use DLA core " << device_id;
    } else {
      NNADAPTER_LOG(WARNING) << "Trying to use DLA core " << device_id
                             << " failed. The platform only has "
                             << builder_->getNbDLACores() << " DLA cores.";
    }
  }
  // Set precision
  PrecisionMode precision = context_->Precision();
  switch (precision) {
    case kFloat32:
      if (device_type == nvinfer1::DeviceType::kDLA) {
        NNADAPTER_LOG(WARNING) << "Only support float16 or int8 if device type "
                                  "is DLA. Float16 is selected by default.";
        config_->setFlag(nvinfer1::BuilderFlag::kFP16);
      }
      break;
    case kFloat16:
      config_->setFlag(nvinfer1::BuilderFlag::kFP16);
      break;
    case kInt8:
      config_->setFlag(nvinfer1::BuilderFlag::kINT8);
      break;
    default:
      NNADAPTER_LOG(FATAL) << "Not support precision mode: "
                           << static_cast<int>(precision);
      break;
  }
  // Set fallback
  if (context_->GpuFallback()) {
    config_->setFlag(nvinfer1::BuilderFlag::kGPU_FALLBACK);
  }
  // Calibration
  if (precision == kInt8) {
    NNADAPTER_CHECK(!with_dynamic_shape_)
        << "Int8 and dynamic shape is incompatible.";
    calibrator_.reset(new Int8EntropyCalibrator(
        model_->input_operands.at(0)->type.dimensions.data[0],
        context_->CalibrationDatasetPath(),
        context_->CalibrationTablePath()));
    config_->setInt8Calibrator(calibrator_.get());
  }
}

int TensorrtProgram::BuildFromModel() {
  for (auto operand : model_->input_operands) {
    if (IsOperandWithDynamicShape(operand)) {
      with_dynamic_shape_ = true;
      break;
    }
  }
  // 1. Optimize the model_
  NNADAPTER_VLOG(5) << "Origin model:" << std::endl << Visualize(model_);
  UnpackOpFusion(model_);
  FuseMatMulAddIntoFullyConnected(model_);
  RemoveReshapeBeforeFullyConnected(model_);
  NNADAPTER_VLOG(5) << "Optimized model:" << std::endl << Visualize(model_);
  // 2. Build model_, serialize to plan_, create engnie_
  builder_.reset(nvinfer1::createInferBuilder(*TrtLogger::Global()));
  NNADAPTER_CHECK(builder_);
  if (context_->Precision() == kInt8) {
    network_.reset(builder_->createNetworkV2(0U));
  } else {
    network_.reset(builder_->createNetworkV2(
        1U << static_cast<int>(
            nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH)));
  }
  NNADAPTER_CHECK(network_);
  // Convert a NNAdapter model_ to a tensorrt network
  Converter converter(network_.get(), &tensors_);
  NNADAPTER_CHECK_EQ(converter.Apply(model_), NNADAPTER_NO_ERROR);
  // Create config_ and set options
  CompleteConfig();
// Serialize to plan_
#if TENSORRT_MAJOR_VERSION >= 8
  plan_.reset(builder_->buildSerializedNetwork(*network_, *config_));
  NNADAPTER_CHECK(plan_);
  runtime_.reset(nvinfer1::createInferRuntime(*TrtLogger::Global()));
  NNADAPTER_CHECK(runtime_);
  engine_.reset(runtime_->deserializeCudaEngine(plan_->data(), plan_->size()));
  NNADAPTER_CHECK(engine_);
#else
  engine_.reset(builder_->buildEngineWithConfig(*network_, *config_));
  NNADAPTER_CHECK(engine_);
  plan_.reset(engine_->serialize());
  NNADAPTER_CHECK(plan_);
#endif
  return NNADAPTER_NO_ERROR;
}

int TensorrtProgram::BuildFromCache() {
  runtime_.reset(nvinfer1::createInferRuntime(*TrtLogger::Global()));
  NNADAPTER_CHECK(runtime_);
  engine_.reset(runtime_->deserializeCudaEngine(
      reinterpret_cast<void*>(cache_->data()), cache_->size()));
  NNADAPTER_CHECK(engine_);
  return NNADAPTER_NO_ERROR;
}

int TensorrtProgram::Execute(
    std::vector<std::shared_ptr<Tensor>>* input_tensors,
    std::vector<std::shared_ptr<Tensor>>* output_tensors) {
  // Malloc max output memory
  int input_size = input_types_.size();
  int output_size = output_types_.size();
  for (int i = 0; i < output_size; i++) {
    SetMaxDims(output_types_.at(i), output_tensors->at(i).get());
  }
  // Prepare input/output buffers
  std::vector<void*> device_ptrs(input_size + output_size, nullptr);
  for (int i = 0; i < input_size; i++) {
    device_ptrs[input_indices_[i]] = input_tensors->at(i)->Data();
  }
  for (int i = 0; i < output_size; i++) {
    device_ptrs[output_indices_[i]] = output_tensors->at(i)->Data();
  }
  for (auto ptr : device_ptrs) {
    NNADAPTER_CHECK(ptr);
  }
  // Set input dims
  for (int i = 0; i < input_size; i++) {
    auto shape = input_tensors->at(i)->Dims();
    nvinfer1::Dims dims;
    dims.nbDims = shape.size();
    memcpy(dims.d, shape.data(), dims.nbDims * sizeof(int32_t));
    execution_context_->setBindingDimensions(input_indices_.at(i), dims);
  }
  NNADAPTER_CHECK(execution_context_->allInputDimensionsSpecified());
  // Execute model
  execution_context_->execute(1, device_ptrs.data());
  // Get output dims
  for (int i = 0; i < output_size; i++) {
    auto dims = execution_context_->getBindingDimensions(output_indices_.at(i));
    std::vector<int> shape(dims.d, dims.d + dims.nbDims);
    output_tensors->at(i)->Resize(shape);
  }
  return NNADAPTER_NO_ERROR;
}

void CudaProgram::Clear() {
  operations_.clear();
  kernels_.clear();
  operand_map_.clear();
}

int CudaProgram::Build() {
  operations_ = SortOperationsInTopologicalOrder(model_);
  for (auto operation : operations_) {
    switch (operation->type) {
#define REGISTER_KERNEL(__op_type__, __kernel_name__)                          \
  case NNADAPTER_##__op_type__:                                                \
    kernels_.emplace_back(std::shared_ptr<KernelBase>(new __kernel_name__())); \
    break;
#include "driver/nvidia_tensorrt/kernels/cuda/all.h"  // NOLINT
#undef __NNADAPTER_DRIVER_NVIDIA_TENSORRT_KERNELS_CUDA_ALL_H__
#undef REGISTER_KERNEL
      default:
        NNADAPTER_LOG(FATAL) << "Unsupported operation("
                             << OperationTypeToString(operation->type)
                             << ") is found.";
        break;
    }
  }
  return NNADAPTER_NO_ERROR;
}

int CudaProgram::Execute(std::vector<std::shared_ptr<Tensor>>* input_tensors,
                         std::vector<std::shared_ptr<Tensor>>* output_tensors) {
  for (auto& operand : model_->operands) {
    if (!operand_map_.count(&operand)) {
      operand_map_[&operand] = std::shared_ptr<Tensor>(new Tensor());
    }
  }
  auto& input_operands = model_->input_operands;
  for (size_t i = 0; i < input_operands.size(); i++) {
    operand_map_[input_operands[i]] = input_tensors->at(i);
  }
  auto& output_operands = model_->output_operands;
  for (size_t i = 0; i < output_operands.size(); i++) {
    operand_map_[output_operands[i]] = output_tensors->at(i);
  }
  for (size_t i = 0; i < kernels_.size(); i++) {
    NNADAPTER_CHECK_EQ(kernels_[i]->Run(operations_[i], &operand_map_),
                       NNADAPTER_NO_ERROR);
    cudaDeviceSynchronize();
  }
  return NNADAPTER_NO_ERROR;
}

void HostProgram::Clear() {
  operations_.clear();
  kernels_.clear();
  operand_map_.clear();
}

int HostProgram::Build() {
  operations_ = SortOperationsInTopologicalOrder(model_);
  for (auto operation : operations_) {
    switch (operation->type) {
#define REGISTER_KERNEL(__op_type__, __kernel_name__)                          \
  case NNADAPTER_##__op_type__:                                                \
    kernels_.emplace_back(std::shared_ptr<KernelBase>(new __kernel_name__())); \
    break;
#include "driver/nvidia_tensorrt/kernels/host/all.h"  // NOLINT
#undef __NNADAPTER_DRIVER_NVIDIA_TENSORRT_KERNELS_HOST_ALL_H__
#undef REGISTER_KERNEL
      default:
        NNADAPTER_LOG(FATAL) << "Unsupported operation("
                             << OperationTypeToString(operation->type)
                             << ") is found.";
        break;
    }
  }
  return NNADAPTER_NO_ERROR;
}

int HostProgram::Execute(std::vector<std::shared_ptr<Tensor>>* input_tensors,
                         std::vector<std::shared_ptr<Tensor>>* output_tensors) {
  for (auto& operand : model_->operands) {
    if (!operand_map_.count(&operand)) {
      operand_map_[&operand] = std::shared_ptr<Tensor>(new Tensor());
    }
  }
  auto& input_operands = model_->input_operands;
  for (size_t i = 0; i < input_operands.size(); i++) {
    operand_map_[input_operands[i]] = input_tensors->at(i);
  }
  auto& output_operands = model_->output_operands;
  for (size_t i = 0; i < output_operands.size(); i++) {
    operand_map_[output_operands[i]] = output_tensors->at(i);
  }
  for (size_t i = 0; i < kernels_.size(); i++) {
    NNADAPTER_CHECK_EQ(kernels_[i]->Run(operations_[i], &operand_map_),
                       NNADAPTER_NO_ERROR);
  }
  return NNADAPTER_NO_ERROR;
}

}  // namespace nvidia_tensorrt
}  // namespace nnadapter
