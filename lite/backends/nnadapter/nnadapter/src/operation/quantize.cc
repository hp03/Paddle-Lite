// Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
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

#include "operation/quantize.h"
#include "core/types.h"
#include "utility/debug.h"
#include "utility/logging.h"
#include "utility/modeling.h"
#include "utility/utility.h"

namespace nnadapter {
namespace operation {

bool ValidateQuantize(const core::Operation* operation) { return false; }

int PrepareQuantize(core::Operation* operation) {
  QUANTIZE_OPERATION_EXTRACT_INPUTS_OUTPUTS

  // Infer the shape and type of output operands
  CopyOperandTypeExceptQuantParams(&output_operand->type, input_operand->type);
  if (is_per_layer_quant && is_symm_quant) {
    output_operand->type.precision = NNADAPTER_QUANT_INT8_SYMM_PER_LAYER;
  } else if (!is_per_layer_quant && is_symm_quant) {
    output_operand->type.precision = NNADAPTER_QUANT_INT8_SYMM_PER_CHANNEL;
  } else if (is_per_layer_quant && !is_symm_quant) {
    output_operand->type.precision = NNADAPTER_QUANT_UINT8_ASYMM_PER_LAYER;
  } else {
    NNADAPTER_LOG(FATAL) << "Unsupported quant mode.";
  }
  NNADAPTER_VLOG(5) << "output: " << OperandToString(output_operand);
  return NNADAPTER_NO_ERROR;
}

int ExecuteQuantize(core::Operation* operation) {
  return NNADAPTER_FEATURE_NOT_SUPPORTED;
}

}  // namespace operation
}  // namespace nnadapter
