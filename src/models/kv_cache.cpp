#include "../generators.h"
#include "model.h"
#include "kv_cache.h"

namespace Generators {

namespace {

std::string ComposeKeyValueName(const std::string& template_string, int index) {
  constexpr int32_t KeyValueNameLength = 64;
  char key_value_name[KeyValueNameLength];
  if (auto length = snprintf(key_value_name, std::size(key_value_name), template_string.c_str(), index);
      length < 0 || length >= KeyValueNameLength) {
    throw std::runtime_error("Unable to compose key value name from the provided template " + template_string +
                             ". This could be either due to an encoding error or the name being too long.");
  }
  return std::string(key_value_name);
}

int64_t ElementCountFromShape(const std::array<int64_t, 4>& shape) {
  return std::accumulate(shape.begin(), shape.end(), int64_t{1}, std::multiplies<int64_t>());
}

}  // namespace

KV_Cache_Combined::KV_Cache_Combined(State& state)
    : state_{state},
      layer_count_{model_.config_->model.decoder.num_hidden_layers},
      shape_{2, state_.params_->BatchBeamSize(), model_.config_->model.decoder.num_key_value_heads, 0, model_.config_->model.decoder.head_size} {
  pasts_.resize(layer_count_);
  presents_.reserve(layer_count_);

  for (int i = 0; i < layer_count_; ++i) {
    input_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.inputs.past_names, i));
    output_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.outputs.present_names, i));
  }

  // Derive the KV data type from the KV input 0
  type_ = model_.session_info_->GetInputDataType(input_name_strings_[0]);

  empty_past_ = OrtValue::CreateTensor(*model_.allocator_kvcache_, shape_, type_);
  shape_[3] = state_.params_->sequence_length;

  for (int i = 0; i < layer_count_; ++i) {
    presents_.push_back(OrtValue::CreateTensor(*model_.allocator_kvcache_, shape_, type_));
  }
}

void KV_Cache_Combined::Add() {
  input_index_ = state_.inputs_.size();
  output_index_ = state_.outputs_.size();

  for (int i = 0; i < layer_count_; i++) {
    state_.inputs_.push_back(empty_past_.get());
    state_.input_names_.push_back(input_name_strings_[i].c_str());
    state_.outputs_.push_back(presents_[i].get());
    state_.output_names_.push_back(output_name_strings_[i].c_str());
  }
}

void KV_Cache_Combined::Update(DeviceSpan<int32_t> beam_indices, int current_length) {
  assert(state_.params_->search.num_beams == 1 || !beam_indices.empty());  // We require beam_indices if we're a beam search

  for (int i = 0; i < layer_count_; i++) {
    if (beam_indices.empty()) {
      pasts_[i] = std::move(presents_[i]);
    } else {
      PickPastState(beam_indices, i);
    }
  }

  shape_[3] = current_length;
  for (int i = 0; i < layer_count_; i++) {
    presents_[i] = OrtValue::CreateTensor(*model_.allocator_kvcache_, shape_, type_);
    state_.inputs_[input_index_ + i] = pasts_[i].get();
    state_.outputs_[output_index_ + i] = presents_[i].get();
  }
}

// Copy present state to past state reordered by the beam_indices
template <typename ScoreType>
void KV_Cache_Combined::PickPastState(DeviceSpan<int32_t> beam_indices_device, int index) {
  std::span<const int32_t> beam_indices = beam_indices_device.CopyDeviceToCpu();
  auto block_size_per_beam = shape_[2] * shape_[3] * shape_[4];
  auto past_key_size = shape_[1] * block_size_per_beam;
  auto element_count = shape_[0] * past_key_size;

  const OrtValue& present = *presents_[index];
  std::unique_ptr<OrtValue> past = OrtValue::CreateTensor<ScoreType>(*model_.allocator_kvcache_, shape_);
  auto past_span = std::span<ScoreType>(past->GetTensorMutableData<ScoreType>(), element_count);
  auto present_span = std::span<const ScoreType>(present.GetTensorData<ScoreType>(), element_count);

#if USE_CUDA
  if (model_.device_type_ == DeviceType::CUDA) {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t beam_index = beam_indices[j];
      auto present_key = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto present_value = present_span.subspan(past_key_size + beam_index * block_size_per_beam, block_size_per_beam);

      auto past_key = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      auto past_value = past_span.subspan(past_key_size + j * block_size_per_beam, block_size_per_beam);
      cudaMemcpyAsync(past_key.data(), present_key.data(), present_key.size_bytes(), cudaMemcpyDeviceToDevice, model_.cuda_stream_);
      cudaMemcpyAsync(past_value.data(), present_value.data(), present_value.size_bytes(), cudaMemcpyDeviceToDevice, model_.cuda_stream_);
    }
  } else
#endif
  {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t const beam_index = beam_indices[j];
      auto present_key = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto present_value = present_span.subspan(past_key_size + beam_index * block_size_per_beam, block_size_per_beam);

      auto past_key = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      auto past_value = past_span.subspan(past_key_size + j * block_size_per_beam, block_size_per_beam);
      copy(present_key, past_key);
      copy(present_value, past_value);
    }
  }

  pasts_[index] = std::move(past);
}

void KV_Cache_Combined::PickPastState(DeviceSpan<int32_t> beam_indices, int index) {
  if (type_ == Ort::TypeToTensorType<float>) {
    PickPastState<float>(beam_indices, index);
  } else {
    PickPastState<Ort::Float16_t>(beam_indices, index);
  }
}

bool KV_Cache::IsCacheNeeded(const Model& model) {
  return model.session_info_->HasInput(ComposeKeyValueName(model.config_->model.decoder.inputs.past_key_names, 0));
}

KV_Cache::KV_Cache(State& state)
    : state_{state},
      layer_count_{model_.config_->model.decoder.num_hidden_layers},
      past_present_share_buffer_{state_.params_->search.past_present_share_buffer && (state_.params_->search.num_beams == 1 || model_.config_->model.type == "whisper")},
      shape_{state_.params_->BatchBeamSize(), model_.config_->model.decoder.num_key_value_heads, 0, model_.config_->model.decoder.head_size} {
  if (g_log.enabled && g_log.warning && past_present_share_buffer_ != state_.params_->search.past_present_share_buffer)
    Log("warning", "past_present_share_buffer search option set to true, but has been disabled due to the current configuration. See https://aka.ms/generate_config for details");

  pasts_.resize(layer_count_ * 2);
  presents_.reserve(layer_count_ * 2);

  for (int i = 0; i < layer_count_; ++i) {
    input_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.inputs.past_key_names, i));
    input_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.inputs.past_value_names, i));

    output_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.outputs.present_key_names, i));
    output_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.outputs.present_value_names, i));
  }

  // Derive the KV data type from the KV input 0
  type_ = model_.session_info_->GetInputDataType(input_name_strings_[0]);

  empty_past_ = OrtValue::CreateTensor(*model_.allocator_kvcache_, shape_, type_);

  // Set the size after empty_past_ has been created with 0 for this field
  if (past_present_share_buffer_)
    shape_[2] = state_.params_->search.max_length;
  else
    shape_[2] = state_.params_->sequence_length;

  if (state_.GetCapturedGraphInfo()) {
    assert(past_present_share_buffer_);
    sb_kv_caches_.reserve(layer_count_ * 2);
    for (int i = 0; i < layer_count_ * 2; ++i) {
      sb_kv_caches_.push_back(state_.GetCapturedGraphInfo()->sb_kv_caches_[i].get());
    }
  }

  auto kv_cache_size_bytes = SizeOf(type_) * shape_[0] * shape_[1] * shape_[2] * shape_[3];
  for (int i = 0; i < layer_count_ * 2; ++i) {
    presents_.push_back(
        sb_kv_caches_.empty() ? OrtValue::CreateTensor(*model_.allocator_kvcache_, shape_, type_)
                              : sb_kv_caches_[i]->CreateTensorOnStaticBuffer(shape_, type_));
#if USE_CUDA
    if (model_.device_type_ == DeviceType::CUDA) {
      cudaMemsetAsync(presents_.back()->GetTensorMutableRawData(), 0, kv_cache_size_bytes, model_.cuda_stream_);
    } else
#endif
    {
      memset(presents_.back()->GetTensorMutableRawData(), 0, kv_cache_size_bytes);
    }
  }
}

void KV_Cache::AddEncoder() {
  // We don't set the input_index_ & output_index_ because the encoder step only runs once, there's no update

  for (int i = 0; i < layer_count_ * 2; ++i) {
    state_.outputs_.push_back(presents_[i].get());
    state_.output_names_.push_back(output_name_strings_[i].c_str());
  }
}

void KV_Cache::Add() {
  input_index_ = state_.inputs_.size();
  output_index_ = state_.outputs_.size();

  for (int i = 0; i < layer_count_ * 2; ++i) {
    state_.inputs_.push_back(empty_past_.get());  // Set empty past here, AddEncoder() & Update() take care of the rest
    state_.input_names_.push_back(input_name_strings_[i].c_str());
    state_.outputs_.push_back(presents_[i].get());
    state_.output_names_.push_back(output_name_strings_[i].c_str());
  }

  // For shared_past_present, the past & presents never change, so set the inputs to the present values (outputs are already set above)
  if (past_present_share_buffer_) {
    for (int i = 0; i < layer_count_ * 2; ++i) {
      state_.inputs_[input_index_ + i] = presents_[i].get();
    }
  }
}

void KV_Cache::Update(DeviceSpan<int32_t> beam_indices, int current_length) {
  // If we're sharing past & present buffers there is nothing to do here, so early exit
  if (past_present_share_buffer_)
    return;

  for (int i = 0; i < layer_count_ * 2; i++) {
    if (beam_indices.empty()) {
      pasts_[i] = std::move(presents_[i]);
    } else {
      PickPastState(beam_indices, i);
    }
    state_.inputs_[input_index_ + i] = pasts_[i].get();
  }

  shape_[2] = current_length;
  for (int i = 0; i < layer_count_ * 2; i++) {
    presents_[i] = OrtValue::CreateTensor(*model_.allocator_kvcache_, shape_, type_);
    state_.outputs_[output_index_ + i] = presents_[i].get();
  }
}

// Copy present state to past state reordered by the beam_indices
template <typename ScoreType>
void KV_Cache::PickPastState(DeviceSpan<int32_t> beam_indices_device, int index) {
  std::span<int32_t> beam_indices = beam_indices_device.Span();
  auto block_size_per_beam = shape_[1] * shape_[2] * shape_[3];
  auto element_count = shape_[0] * block_size_per_beam;

  const OrtValue& present_value = *presents_[index];
  std::unique_ptr<OrtValue> past_value = OrtValue::CreateTensor<ScoreType>(*model_.allocator_kvcache_, shape_);
  auto past_span = std::span<ScoreType>(past_value->GetTensorMutableData<ScoreType>(), element_count);
  auto present_span = std::span<const ScoreType>(present_value.GetTensorData<ScoreType>(), element_count);

#if USE_CUDA
  if (model_.device_type_ == DeviceType::CUDA) {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t beam_index = beam_indices[j];
      auto present = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto past = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      cudaMemcpyAsync(past.data(), present.data(), present.size_bytes(), cudaMemcpyDeviceToDevice, model_.cuda_stream_);
    }
  } else
#endif
  {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t const beam_index = beam_indices[j];
      auto present = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto past = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      copy(present, past);
    }
  }

  pasts_[index] = std::move(past_value);
}

void KV_Cache::PickPastState(DeviceSpan<int32_t> beam_indices, int index) {
  if (type_ == Ort::TypeToTensorType<float>) {
    PickPastState<float>(beam_indices, index);
  } else {
    PickPastState<Ort::Float16_t>(beam_indices, index);
  }
}

Cross_Cache::Cross_Cache(State& state)
    : state_{state},
      layer_count_{model_.config_->model.decoder.num_hidden_layers},
      shape_{state_.params_->BatchBeamSize(), model_.config_->model.decoder.num_key_value_heads, 1500, model_.config_->model.decoder.head_size} {
  values_.reserve(layer_count_ * 2);

  for (int i = 0; i < layer_count_; ++i) {
    input_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.inputs.cross_past_key_names, i));
    input_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.inputs.cross_past_value_names, i));

    output_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.outputs.cross_present_key_names, i));
    output_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.outputs.cross_present_value_names, i));
  }

  // Derive the KV data type from the KV input 0
  type_ = model_.session_info_->GetInputDataType(input_name_strings_[0]);

  for (int i = 0; i < layer_count_; ++i) {
    values_.push_back(OrtValue::CreateTensor(*model_.allocator_kvcache_, shape_, type_));
    values_.push_back(OrtValue::CreateTensor(*model_.allocator_kvcache_, shape_, type_));
  }
}

void Cross_Cache::AddOutputs() {
  for (int i = 0; i < layer_count_ * 2; ++i) {
    state_.outputs_.push_back(values_[i].get());
    state_.output_names_.push_back(output_name_strings_[i].c_str());
  }
}

void Cross_Cache::AddInputs() {
  for (int i = 0; i < layer_count_ * 2; ++i) {
    state_.inputs_.push_back(values_[i].get());
    state_.input_names_.push_back(input_name_strings_[i].c_str());
  }
}

SlidingWindowKeyValueCache::SlidingWindowKeyValueCache(State& state)
    : state_{state},
      layer_count_{model_.config_->model.decoder.num_hidden_layers},
      window_size_{model_.config_->model.decoder.sliding_window_key_value_cache->window_size},
      key_cache_shape_in_{model_.config_->model.decoder.num_key_value_heads, 1,
                          model_.config_->model.decoder.head_size, model_.config_->model.context_length - window_size_},
      key_cache_shape_out_{model_.config_->model.decoder.num_key_value_heads, 1,
                           model_.config_->model.decoder.head_size, window_size_},
      value_cache_shape_in_{model_.config_->model.decoder.num_key_value_heads, 1,
                            model_.config_->model.context_length - window_size_, model_.config_->model.decoder.head_size},
      value_cache_shape_out_{model_.config_->model.decoder.num_key_value_heads, 1,
                             window_size_, model_.config_->model.decoder.head_size} {
  for (int i = 0; i < layer_count_; ++i) {
    input_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.inputs.past_key_names, i));
    input_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.inputs.past_value_names, i));

    output_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.outputs.present_key_names, i));
    output_name_strings_.emplace_back(ComposeKeyValueName(model_.config_->model.decoder.outputs.present_value_names, i));
  }

  type_ = model_.session_info_->GetInputDataType(input_name_strings_[0]);
  if (type_ != ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8) {
    throw std::runtime_error("Expected input data type to be uint8_t for SlidingWindowKeyValueCache. Actual: " +
                             std::to_string(type_));
  }

  for (int i = 0; i < layer_count_; ++i) {
    key_caches_in_.push_back(
        OrtValue::CreateTensor(*model_.allocator_device_, key_cache_shape_in_, type_));
    std::fill_n(key_caches_in_[i]->GetTensorMutableData<uint8_t>(),
                ElementCountFromShape(key_cache_shape_in_),
                static_cast<uint8_t>(model_.config_->model.decoder.sliding_window_key_value_cache->pad_value));

    value_caches_in_.push_back(
        OrtValue::CreateTensor(*model_.allocator_device_, value_cache_shape_in_, type_));
    std::fill_n(value_caches_in_[i]->GetTensorMutableData<uint8_t>(),
                ElementCountFromShape(value_cache_shape_in_),
                static_cast<uint8_t>(model_.config_->model.decoder.sliding_window_key_value_cache->pad_value));

    key_caches_out_.push_back(
        OrtValue::CreateTensor(*model_.allocator_device_, key_cache_shape_out_, type_));
    value_caches_out_.push_back(
        OrtValue::CreateTensor(*model_.allocator_device_, value_cache_shape_out_, type_));
  }
}

void SlidingWindowKeyValueCache::Add() {
  input_index_ = state_.inputs_.size();
  output_index_ = state_.outputs_.size();

  for (size_t layer_idx = 0; layer_idx < layer_count_; ++layer_idx) {
    state_.inputs_.push_back(key_caches_in_[layer_idx].get());
    state_.input_names_.push_back(input_name_strings_[2 * layer_idx].c_str());

    state_.inputs_.push_back(value_caches_in_[layer_idx].get());
    state_.input_names_.push_back(input_name_strings_[2 * layer_idx + 1].c_str());

    state_.outputs_.push_back(key_caches_out_[layer_idx].get());
    state_.output_names_.push_back(output_name_strings_[2 * layer_idx].c_str());

    state_.outputs_.push_back(value_caches_out_[layer_idx].get());
    state_.output_names_.push_back(output_name_strings_[2 * layer_idx + 1].c_str());
  }
}

void SlidingWindowKeyValueCache::Slide() {
  for (size_t layer_idx = 0; layer_idx < layer_count_; ++layer_idx) {
    uint8_t* key_cache_in_data = key_caches_in_[layer_idx]->GetTensorMutableData<uint8_t>();
    uint8_t* key_cache_out_data = key_caches_out_[layer_idx]->GetTensorMutableData<uint8_t>();

    int64_t num_key_cache_chunks = key_cache_shape_in_[0] * key_cache_shape_in_[2];
    for (int64_t j = 0; j < num_key_cache_chunks; ++j) {
      {
        cpu_span<uint8_t> key_cache_dst(key_cache_in_data + j * key_cache_shape_in_[3],
                                        key_cache_shape_in_[3] - window_size_);
        cpu_span<uint8_t> key_cache_src(key_cache_in_data + j * key_cache_shape_in_[3] + window_size_,
                                        key_cache_shape_in_[3] - window_size_);
        std::copy(key_cache_src.begin(), key_cache_src.end(), key_cache_dst.begin());
      }
      {
        cpu_span<uint8_t> key_cache_dst(key_cache_in_data + j * key_cache_shape_in_[3] + key_cache_shape_in_[3] - window_size_,
                                        window_size_);
        cpu_span<uint8_t> key_cache_src(key_cache_out_data + j * key_cache_shape_out_[3],
                                        window_size_);
        std::copy(key_cache_src.begin(), key_cache_src.end(), key_cache_dst.begin());
      }
    }

    uint8_t* value_cache_in_data = value_caches_in_[layer_idx]->GetTensorMutableData<uint8_t>();
    uint8_t* value_cache_out_data = value_caches_out_[layer_idx]->GetTensorMutableData<uint8_t>();

    for (int64_t j = 0; j < value_cache_shape_in_[0]; ++j) {
      {
        cpu_span<uint8_t> value_cache_dst(value_cache_in_data + (j * value_cache_shape_in_[2] * value_cache_shape_in_[3]),
                                          (value_cache_shape_in_[2] - window_size_) * value_cache_shape_in_[3]);
        cpu_span<uint8_t> value_cache_src(value_cache_in_data + (j * value_cache_shape_in_[2] * value_cache_shape_in_[3]) +
                                              (window_size_ * value_cache_shape_in_[3]),
                                          (value_cache_shape_in_[2] - window_size_) * value_cache_shape_in_[3]);
        std::copy(value_cache_src.begin(), value_cache_src.end(), value_cache_dst.begin());
      }
      {
        cpu_span<uint8_t> value_cache_dst(value_cache_in_data + (j * value_cache_shape_in_[2] * value_cache_shape_in_[3]) +
                                              ((value_cache_shape_in_[2] - window_size_) * value_cache_shape_in_[3]),
                                          window_size_ * value_cache_shape_in_[3]);
        cpu_span<uint8_t> value_cache_src(value_cache_out_data + (j * value_cache_shape_out_[2] * value_cache_shape_out_[3]),
                                          window_size_ * value_cache_shape_out_[3]);
        std::copy(value_cache_src.begin(), value_cache_src.end(), value_cache_dst.begin());
      }
    }
  }
}

void SlidingWindowKeyValueCache::Update(DeviceSpan<int32_t> beam_indices, int current_length) {
  if (window_size_ == 1) {
    Slide();
    return;
  }

  // No sliding needed. But we need to concatenate the last window_size_ elements to the end of the cache

  // key_caches_in_ = Concat(key_caches_in_[:, :, :, 1:], key_caches_out_)
  // [num_key_value_heads, 1, head_size, context_length-1] = [num_key_value_heads, 1, head_size, context_length - window_size_ - 1] +
  //                                                         [num_key_value_heads, 1, head_size, window_size_]
  // value_cache = Concat(value_caches_in_[:, :, 1:, :], value_caches_out_)
  // [num_key_value_heads, 1, context_length - 1, head_size] = [num_key_value_heads, 1, context_length - window_size_ - 1, head_size] +
  //                                                           [num_key_value_heads, 1, window_size_, head_size]

  int updated_window_size = 1;
  auto updated_key_cache_shape_in = std::array<int64_t, 4>{model_.config_->model.decoder.num_key_value_heads, 1,
                                                           model_.config_->model.decoder.head_size,
                                                           model_.config_->model.context_length - updated_window_size};

  auto updated_value_cache_shape_in = std::array<int64_t, 4>{model_.config_->model.decoder.num_key_value_heads, 1,
                                                             model_.config_->model.context_length - updated_window_size,
                                                             model_.config_->model.decoder.head_size};

  auto updated_key_cache_shape_out = std::array<int64_t, 4>{model_.config_->model.decoder.num_key_value_heads, 1,
                                                            model_.config_->model.decoder.head_size,
                                                            updated_window_size};

  auto updated_value_cache_shape_out = std::array<int64_t, 4>{model_.config_->model.decoder.num_key_value_heads, 1,
                                                              updated_window_size,
                                                              model_.config_->model.decoder.head_size};

  for (size_t layer_idx = 0; layer_idx < layer_count_; ++layer_idx) {
    std::unique_ptr<OrtValue> key_cache = OrtValue::CreateTensor(*model_.allocator_device_, updated_key_cache_shape_in, type_);

    uint8_t* key_cache_data = key_cache->GetTensorMutableData<uint8_t>();
    uint8_t* key_cache_in_data = key_caches_in_[layer_idx]->GetTensorMutableData<uint8_t>();
    uint8_t* key_cache_out_data = key_caches_out_[layer_idx]->GetTensorMutableData<uint8_t>();

    int64_t num_key_cache_chunks = updated_key_cache_shape_in[0] * updated_key_cache_shape_in[2];
    for (int64_t j = 0; j < num_key_cache_chunks; ++j) {
      {
        cpu_span<uint8_t> key_cache_dst(key_cache_data + j * updated_key_cache_shape_in[3],
                                        updated_key_cache_shape_in[3] - updated_window_size);
        cpu_span<uint8_t> key_cache_src(key_cache_in_data + j * key_cache_shape_in_[3] + updated_window_size,
                                        key_cache_shape_in_[3] - updated_window_size);
        std::copy(key_cache_src.begin(), key_cache_src.end(), key_cache_dst.begin());
      }
      {
        cpu_span<uint8_t> key_cache_dst(key_cache_data + j * updated_key_cache_shape_in[3] +
                                            key_cache_shape_in_[3] - updated_window_size,
                                        window_size_);
        cpu_span<uint8_t> key_cache_src(key_cache_out_data + j * key_cache_shape_out_[3],
                                        window_size_);
        std::copy(key_cache_src.begin(), key_cache_src.end(), key_cache_dst.begin());
      }
    }

    key_caches_in_[layer_idx] = std::move(key_cache);
    key_caches_out_[layer_idx] = OrtValue::CreateTensor(*model_.allocator_device_, updated_key_cache_shape_out, type_);

    std::unique_ptr<OrtValue> value_cache = OrtValue::CreateTensor(*model_.allocator_device_, updated_value_cache_shape_in, type_);

    uint8_t* value_cache_data = value_cache->GetTensorMutableData<uint8_t>();
    uint8_t* value_cache_in_data = value_caches_in_[layer_idx]->GetTensorMutableData<uint8_t>();
    uint8_t* value_cache_out_data = value_caches_out_[layer_idx]->GetTensorMutableData<uint8_t>();

    for (int64_t j = 0; j < updated_value_cache_shape_in[0]; ++j) {
      {
        cpu_span<uint8_t> value_cache_dst(value_cache_data + (j * updated_value_cache_shape_in[2] * updated_value_cache_shape_in[3]),
                                          (value_cache_shape_in_[2] - updated_window_size) * updated_value_cache_shape_in[3]);
        cpu_span<uint8_t> value_cache_src(value_cache_in_data + (j * value_cache_shape_out_[2] * value_cache_shape_out_[3]) +
                                              (updated_window_size * value_cache_shape_out_[3]),
                                          (value_cache_shape_in_[2] - updated_window_size) * value_cache_shape_in_[3]);
        std::copy(value_cache_src.begin(), value_cache_src.end(), value_cache_dst.begin());
      }
      {
        cpu_span<uint8_t> value_cache_dst(value_cache_data + (j * updated_value_cache_shape_in[2] * updated_value_cache_shape_in[3]) +
                                              ((value_cache_shape_in_[2] - updated_window_size) * updated_value_cache_shape_in[3]),
                                          window_size_ * value_cache_shape_out_[3]);
        cpu_span<uint8_t> value_cache_src(value_cache_out_data + (j * value_cache_shape_out_[2] * value_cache_shape_out_[3]),
                                          window_size_ * value_cache_shape_out_[3]);
        std::copy(value_cache_src.begin(), value_cache_src.end(), value_cache_dst.begin());
      }
    }

    value_caches_in_[layer_idx] = std::move(value_cache);
    value_caches_out_[layer_idx] = OrtValue::CreateTensor(*model_.allocator_device_, updated_value_cache_shape_out, type_);
  }

  window_size_ = 1;
  key_cache_shape_in_ = updated_key_cache_shape_in;
  value_cache_shape_in_ = updated_value_cache_shape_in;
  key_cache_shape_out_ = updated_key_cache_shape_out;
  value_cache_shape_out_ = updated_value_cache_shape_out;

  for (size_t layer_idx = 0; layer_idx < layer_count_; ++layer_idx) {
    state_.inputs_[input_index_ + 2 * layer_idx] = key_caches_in_[layer_idx].get();
    state_.inputs_[input_index_ + 2 * layer_idx + 1] = value_caches_in_[layer_idx].get();
    state_.outputs_[output_index_ + 2 * layer_idx] = key_caches_out_[layer_idx].get();
    state_.outputs_[output_index_ + 2 * layer_idx + 1] = value_caches_out_[layer_idx].get();
  }
}

}  // namespace Generators
