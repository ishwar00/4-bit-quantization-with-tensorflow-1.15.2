/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/lite/tools/optimize/quantization_utils.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>

#include "absl/memory/memory.h"
#include "third_party/eigen3/Eigen/Core"
#include "tensorflow/lite/c/c_api_internal.h"
#include "tensorflow/lite/kernels/internal/quantization_util.h"
#include "tensorflow/lite/kernels/internal/round.h"
#include "tensorflow/lite/kernels/internal/tensor_utils.h"
#include "tensorflow/lite/kernels/internal/types.h"

namespace tflite {
namespace optimize {
namespace utils {

namespace {
const int8_t kMinQuantizedValue = -127;
const int8_t kMaxQuantizedValue = 127;
}  // namespace

TfLiteStatus NumElements(const TensorT& tensor, uint64_t* num_elements) {
  *num_elements = 1;
  for (const uint64_t dim : tensor.shape) {
    *num_elements *= dim;
  }
  return kTfLiteOk;
}

// Nudge min and max so that floating point 0 falls exactly on a quantized
// value, returning the nudges scale and zero_point.
//
// Although this code originates from FakeQuantization in quantized training,
// we may deviate from that implementation as we please since we do not fine
// tune the weights with quantized training.
void GetAsymmetricQuantizationParams(
    float min, float max, const int quant_min, const int quant_max,
    QuantizationParametersT* quantization_params) {
  const float quant_min_float = static_cast<float>(quant_min);
  const float quant_max_float = static_cast<float>(quant_max);
  // Adjust the boundaries to guarantee 0 is included.
  min = std::min(static_cast<float>(min), 0.0f);
  max = std::max(static_cast<float>(max), 0.0f);
  const float scale = (max - min) / (quant_max_float - quant_min_float);
  // Scale can be zero if min and max are exactly 0.0f.
  float zero_point_from_min = quant_min_float;
  if (scale != 0) {
    zero_point_from_min = quant_min_float - min / scale;
  }
  int64_t zero_point;
  if (zero_point_from_min < quant_min_float) {
    zero_point = static_cast<int64_t>(quant_min);
  } else if (zero_point_from_min > quant_max_float) {
    zero_point = static_cast<int64_t>(quant_max);
  } else {
    zero_point = static_cast<int64_t>(std::round(zero_point_from_min));
  }
  quantization_params->min = std::vector<float>(1, min);
  quantization_params->max = std::vector<float>(1, max);
  quantization_params->scale = std::vector<float>(1, scale);
  quantization_params->zero_point = std::vector<int64_t>(1, zero_point);
}

// Per-channel quantize a tensor at the given index and returns both scales and
// quantized values.
void SymmetricPerChannelQuantization(const float* const input,
                                     const std::vector<int>& dimension,
                                     int32_t channel_dim_index,
                                     std::vector<float>* output_scales,
                                     std::vector<int8_t>* output_value,
                                     std::vector<int64_t>* output_zerop) {
  const int32_t channel_dim_size = dimension[channel_dim_index];
  std::vector<float> min_vals(channel_dim_size);
  std::vector<float> max_vals(channel_dim_size);
  std::vector<float> alpha(channel_dim_size); //my code
  std::vector<float> sums(channel_dim_size);  //my code
  std::vector<std::vector<float> >  channel_data(channel_dim_size); //my code
  std::vector<float> means(channel_dim_size);  //my code

  std::vector<bool> has_min_max_value(channel_dim_size, false);
  int indices[4];
  RuntimeShape tensor_dims{dimension[0], dimension[1], dimension[2],
                           dimension[3]};
  
  std::cout<<"[";
  for(int k=0;k < dimension.size();k++)
  {
    std::cout<<dimension[k]<<", ";
  }
  std::cout<<"]\n";
  // Compute min max ranges per channel
  for (indices[0] = 0; indices[0] < dimension[0]; indices[0]++) {
    for (indices[1] = 0; indices[1] < dimension[1]; indices[1]++) {
      for (indices[2] = 0; indices[2] < dimension[2]; indices[2]++) {
        for (indices[3] = 0; indices[3] < dimension[3]; indices[3]++) {
          int channel_idx = indices[channel_dim_index];
          const float val = input[Offset(tensor_dims, indices)];
          channel_data[channel_idx].push_back(val); //my code
          sums[channel_idx]+=val;                   //my code
          if (has_min_max_value[channel_idx]) {
            if (min_vals[channel_idx] > val) {
              min_vals[channel_idx] = val;
            } else if (max_vals[channel_idx] < val) {
              max_vals[channel_idx] = val;
            }
          } else {
            min_vals[channel_idx] = val;
            max_vals[channel_idx] = val;
            has_min_max_value[channel_idx] = true;
          }
        }
      }
    }
  }
//   my code start
  for (int i=0;i < channel_dim_size;i++)
  {
    means[i] = sums[i] / channel_data[i].size();
  }
  for (int i=0;i < channel_dim_size; i++)
  {
    double differencesum = 0;
    for (int j=0; j < channel_data[i].size();j++)
    {
      double diff = channel_data[i][j] - means[i];
      differencesum = differencesum + (diff*diff);
    }
    alpha[i] = differencesum / channel_data[i].size();
    alpha[i] = std::sqrt(alpha[i]);
    alpha[i] = std::pow(alpha[i],0.66666666); 
  }
  double alsum = std::accumulate(alpha.begin(),alpha.end(),0.00000); //my code end
  std::vector<int> bitalloc(channel_dim_size);
  bitalloc.clear();

  float eps = 0.01;
  float delta = 1.0;
  double targetbit = 4.0;
  int itr = 0,max_itr = 10;
  while(  (std::abs(2*delta) > eps) && (itr < max_itr) )
  {
    itr+=1;
    for(int i=0; i<channel_dim_size; i++)
    {
      int64_t B = channel_dim_size * std::pow(2,targetbit);
      float med = B*alpha[i] / alsum;
      bitalloc[i] = std::ceil(std::log2(med));
      if(bitalloc[i] < 0)
      {
        bitalloc[i] = 0;
      }
      else if(bitalloc[i] > 8)
      {
        bitalloc[i] = 8;
      }
    }
    float mn = std::accumulate(bitalloc.begin(),bitalloc.end(),0.0) / bitalloc.size();
    delta = (4 - mn)/2;
    targetbit+=delta;
  }

  for(int i=0; i < channel_dim_size; i++ )
  {
    if(bitalloc[i]<=0)
    {
      bitalloc[i]=4;
    }
    //bitalloc[i]=8;
    
  }

  // Calculate scales per channel
  std::vector<float> scale_invs(channel_dim_size);
  //const float half_scale = kMaxQuantizedValue;
  for (int channel_idx = 0; channel_idx < channel_dim_size; channel_idx++) {
    const float half_scale = std::pow(2,bitalloc[channel_idx]-1) - 1;  //My code ends for weight only
    const float half_range = std::max(std::abs(min_vals[channel_idx]),
                                      std::abs(max_vals[channel_idx]));
    output_scales->at(channel_idx) = half_range / half_scale;
    if (half_range == 0) {
      scale_invs[channel_idx] = 0;
    } else {
      scale_invs[channel_idx] = half_scale / half_range;
    }
    std::cout<<"CI-"<<channel_idx<<"::"<<min_vals[channel_idx]<<" : "<<max_vals[channel_idx]<<"::"<<"Scale-"<<(half_range/half_scale)<<"::"<<"bit-"<<bitalloc[channel_idx];
    std::cout<<"\n------------------------------------------------------------------------------------------------------------------------\n";
  }
  std::cout<<"\n+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n";

  // Quantize the values.
  SymmetricPerChannelQuantizeValues(input, scale_invs, dimension,
                                    channel_dim_index, output_value, bitalloc, means, output_zerop);
}

void SymmetricPerChannelQuantizeValues(const float* const input,
                                       const std::vector<float>& scales_inv,
                                       const std::vector<int>& dimension,
                                       int32_t channel_dim_index,
                                       std::vector<int8_t>* output_value,
                                       const std::vector<int>& bitalloc,
                                       const std::vector<float>& means,
                                       std::vector<int64_t>* output_zerop) {
  // Quantize the values.
  int indices[4];
  const int32_t channel_dim_size = dimension[channel_dim_index];
  std::vector<float> qdsums(channel_dim_size);
  std::vector<std::vector<int8_t> > qchannel_data(channel_dim_size);
  RuntimeShape tensor_dims{dimension[0], dimension[1], dimension[2],
                           dimension[3]};
  for (indices[0] = 0; indices[0] < dimension[0]; indices[0]++) {
    for (indices[1] = 0; indices[1] < dimension[1]; indices[1]++) {
      for (indices[2] = 0; indices[2] < dimension[2]; indices[2]++) {
        for (indices[3] = 0; indices[3] < dimension[3]; indices[3]++) {
          int channel_idx = indices[channel_dim_index];
          int index = Offset(tensor_dims, indices);
          const float val = input[index];
          const int32_t quantized_value = static_cast<int32_t>(TfLiteRound((val * scales_inv[channel_idx])+output_zerop->at(channel_idx)));
          int mqw = std::pow(2,bitalloc[channel_idx]-1)-1; // my code
          int minqw = (-mqw); // my code
          output_value->at(index) = std::min<int8_t>(
              mqw,
              std::max<int8_t>(minqw, quantized_value));
          int8_t v = std::min<int8_t>(mqw,std::max<int8_t>(minqw, quantized_value));
          qchannel_data[channel_idx].push_back(v);
          qdsums[channel_idx]+= v/scales_inv[channel_idx];
        }
      }
    }
  }
  std::vector<double> qmeans(channel_dim_size);
  for (int i=0;i < channel_dim_size;i++)
  {
    qmeans[i] = qdsums[i] / qchannel_data[i].size();
    output_zerop->at(i) = (means[i]-qmeans[i]);
  }

}

TfLiteStatus SymmetricQuantizeTensor(ModelT* model, TensorT* tensor) {
  if (model == nullptr || tensor == nullptr) {
    return kTfLiteError;
  }

  BufferT* buffer = model->buffers[tensor->buffer].get();
  if (buffer == nullptr) {
    return kTfLiteError;
  }
  float* float_data = reinterpret_cast<float*>(buffer->data.data());
  uint64_t num_elements;
  TF_LITE_ENSURE_STATUS(NumElements(*tensor, &num_elements));

  std::vector<int8_t> quantized_buffer;
  quantized_buffer.resize(num_elements);

  float min_value, max_value, scaling_factor;
  tensor_utils::SymmetricQuantizeFloats(float_data, num_elements,
                                        quantized_buffer.data(), &min_value,
                                        &max_value, &scaling_factor);

  if (tensor->quantization == nullptr) {
    tensor->quantization = absl::make_unique<QuantizationParametersT>();
  }
  tensor->quantization->scale = std::vector<float>(1, scaling_factor);
  tensor->quantization->zero_point = std::vector<int64_t>(1, 0);

  uint8_t* uint8_buffer = reinterpret_cast<uint8_t*>(quantized_buffer.data());
  model->buffers[tensor->buffer]->data.assign(uint8_buffer,
                                              uint8_buffer + num_elements);

  // Update the tensor type.
  tensor->type = TensorType_INT8;

  return kTfLiteOk;
}

TfLiteStatus QuantizeTensorFloat16(ModelT* model, TensorT* tensor) {
  if (model == nullptr || tensor == nullptr) {
    return kTfLiteError;
  }

  BufferT* buffer = model->buffers[tensor->buffer].get();
  if (buffer == nullptr) {
    return kTfLiteError;
  }

  uint64_t num_elements;
  TF_LITE_ENSURE_STATUS(NumElements(*tensor, &num_elements));

  // Copy single byte buffer data to float vector to guard against misalignment.
  std::vector<float> float_vector(num_elements);
  uint8_t* first = buffer->data.data();
  std::copy(first, first + buffer->data.size(),
            reinterpret_cast<uint8_t*>(float_vector.data()));

  // Transform float data to float16.
  std::vector<Eigen::half> quantized_buffer;
  quantized_buffer.resize(num_elements);
  std::transform(
      float_vector.begin(), float_vector.end(), quantized_buffer.begin(),
      [](float a) { return Eigen::half_impl::float_to_half_rtne(a); });

  char* half_buffer = reinterpret_cast<char*>(quantized_buffer.data());
  model->buffers[tensor->buffer]->data.assign(
      half_buffer, half_buffer + sizeof(Eigen::half) * num_elements);

  // Update the tensor type.
  tensor->type = TensorType_FLOAT16;

  return kTfLiteOk;
}

TfLiteStatus AddQuantizationParams(const std::vector<float>& scales,
                                   const std::vector<int64_t>& zero_point,
                                   int quantized_dimension,
                                   const uint8_t* buffer_data,
                                   size_t buffer_size, TensorType output_type,
                                   ModelT* model, TensorT* tensor) {
  tensor->quantization = absl::make_unique<QuantizationParametersT>();
  tensor->quantization->scale.assign(scales.begin(), scales.end());
  if (zero_point.size() != scales.size()) {
    return kTfLiteError;
  }
  tensor->quantization->zero_point.assign(zero_point.begin(), zero_point.end());
  tensor->quantization->quantized_dimension = quantized_dimension;
  model->buffers[tensor->buffer]->data.assign(buffer_data,
                                              buffer_data + buffer_size);

  // Update the tensor type.
  tensor->type = output_type;
  return kTfLiteOk;
}

TfLiteStatus SymmetricQuantizeTensorPerChannel(ModelT* model, TensorT* tensor,
                                               int32_t channel_dim_index) {
  if (tensor->shape.size() != 4) {
    return kTfLiteError;
  }

  // Get dimensions.
  uint64_t num_elements;
  TF_LITE_ENSURE_STATUS(NumElements(*tensor, &num_elements));
  const int32_t channel_dim_size = tensor->shape[channel_dim_index];

  // Get input float data.
  BufferT* buffer = model->buffers[tensor->buffer].get();
  float* float_input_data = reinterpret_cast<float*>(buffer->data.data());

  // Create container for output scale and output data.
  std::vector<float> scales(channel_dim_size);
  std::vector<int64_t> zero_point(channel_dim_size);
  std::vector<int8_t> final_buffer(num_elements);

  // Quantize the input data with respect to channel_dim_index.
  const std::vector<int> tensor_dims = {tensor->shape[0], tensor->shape[1],
                                        tensor->shape[2], tensor->shape[3]};
  SymmetricPerChannelQuantization(float_input_data, tensor_dims,
                                  channel_dim_index, &scales, &final_buffer, &zero_point);

  // Set the buffers and output type.
  uint8_t* uint8_buffer = reinterpret_cast<uint8_t*>(final_buffer.data());
  const size_t buffer_size = num_elements * sizeof(int8_t);
  //std::vector<int64_t> zero_point(scales.size(), 0);
  return AddQuantizationParams(scales, zero_point, channel_dim_index,
                               uint8_buffer, buffer_size, TensorType_INT8,
                               model, tensor);
}

TfLiteStatus SymmetricPerLayerBiasQuantize(ModelT* model, TensorT* tensor,
                                           float input_scale,
                                           float weight_scale) {
  // Compute scales.
  float scaling_factor = input_scale * weight_scale;

  BufferT* buffer = model->buffers[tensor->buffer].get();
  float* float_data = reinterpret_cast<float*>(buffer->data.data());
  int32_t float_data_size = buffer->data.size() / sizeof(float);
  uint64_t num_elements;
  TF_LITE_ENSURE_STATUS(NumElements(*tensor, &num_elements));

  std::vector<int32_t> final_buffer(num_elements);
  const int32_t kScale = std::numeric_limits<int32_t>::max();

  for (int32_t i = 0; i < float_data_size; i++) {
    float scaling_factor_inv = (scaling_factor == 0) ? 0 : 1.0 / scaling_factor;
    const int32_t quantized_value = tflite::SafeCast<int32_t>(
        TfLiteRound(float_data[i] * scaling_factor_inv));
    final_buffer[i] = std::min(kScale, std::max(-kScale, quantized_value));
  }

  // Set the buffers and output type.
  uint8_t* uint8_buffer = reinterpret_cast<uint8_t*>(final_buffer.data());
  size_t buffer_size = num_elements * sizeof(int32_t);
  std::vector<float> scales(1, scaling_factor);
  std::vector<int64_t> zero_points(1, 0);
  return AddQuantizationParams(scales, zero_points, 0, uint8_buffer,
                               buffer_size, TensorType_INT32, model, tensor);
}

TfLiteStatus SymmetricPerChannelBiasQuantize(ModelT* model, TensorT* tensor,
                                             float input_scale,
                                             const float* weight_scales,
                                             int number_of_dimension) {
  // Compute scales.
  std::vector<float> scales(number_of_dimension);
  for (size_t i = 0; i < number_of_dimension; i++) {
    scales[i] = input_scale * weight_scales[i];
  }

  BufferT* buffer = model->buffers[tensor->buffer].get();
  float* float_data = reinterpret_cast<float*>(buffer->data.data());
  uint64_t num_elements;
  TF_LITE_ENSURE_STATUS(NumElements(*tensor, &num_elements));

  std::vector<int32_t> final_buffer(num_elements);
  const int32_t kScale = std::numeric_limits<int32_t>::max();

  for (int32_t channel_idx = 0; channel_idx < number_of_dimension;
       channel_idx++) {
    float scaling_factor = scales[channel_idx];
    float scaling_factor_inv = (scaling_factor == 0) ? 0 : 1.0 / scaling_factor;
    const int32_t quantized_value = tflite::SafeCast<int32_t>(
        TfLiteRound(float_data[channel_idx] * scaling_factor_inv));
    final_buffer[channel_idx] =
        std::min(kScale, std::max(-kScale, quantized_value));
  }

  // Set the buffers and output type.
  uint8_t* uint8_buffer = reinterpret_cast<uint8_t*>(final_buffer.data());
  size_t buffer_size = num_elements * sizeof(int32_t);
  std::vector<int64_t> zero_point(scales.size(), 0);
  return AddQuantizationParams(scales, zero_point, 0, uint8_buffer, buffer_size,
                               TensorType_INT32, model, tensor);
}

TfLiteStatus QuantizeWeight(ModelT* model, TensorT* tensor, bool per_channel,
                            int per_axis_index) {
  // TODO(suharshs): Currently we conflate quantizing weights and constants. Its
  // possible that the right thing to do is asymmetric quantize the weight. Add
  // support for this.
  if (per_channel) {
    return SymmetricQuantizeTensorPerChannel(model, tensor, per_axis_index);
  } else {
    return SymmetricQuantizeTensor(model, tensor);
  }
}

void QuantizeActivation(TensorT* tensor) {
  GetAsymmetricQuantizationParams(
      tensor->quantization->min[0], tensor->quantization->max[0],
      std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max(),
      tensor->quantization.get());
  tensor->type = TensorType_INT8;
}

}  // namespace utils
}  // namespace optimize
}  // namespace tflite
