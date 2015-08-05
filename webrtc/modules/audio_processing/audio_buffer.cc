/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/audio_processing/audio_buffer.h"

#include "webrtc/common_audio/include/audio_util.h"
#include "webrtc/common_audio/resampler/push_sinc_resampler.h"
#include "webrtc/common_audio/signal_processing/include/signal_processing_library.h"
#include "webrtc/common_audio/channel_buffer.h"
#include "webrtc/modules/audio_processing/common.h"

namespace webrtc {
namespace {

const int kSamplesPer16kHzChannel = 160;
const int kSamplesPer32kHzChannel = 320;
const int kSamplesPer48kHzChannel = 480;

int KeyboardChannelIndex(const StreamConfig& stream_config) {
  if (!stream_config.has_keyboard()) {
    assert(false);
    return -1;
  }

  return stream_config.num_channels();
}

int NumBandsFromSamplesPerChannel(int num_frames) {
  int num_bands = 1;
  if (num_frames == kSamplesPer32kHzChannel ||
      num_frames == kSamplesPer48kHzChannel) {
    num_bands = rtc::CheckedDivExact(num_frames,
                                     static_cast<int>(kSamplesPer16kHzChannel));
  }
  return num_bands;
}

}  // namespace

AudioBuffer::AudioBuffer(int input_num_frames,
                         int num_input_channels,
                         int process_num_frames,
                         int num_process_channels,
                         int output_num_frames)
  : input_num_frames_(input_num_frames),
    num_input_channels_(num_input_channels),
    proc_num_frames_(process_num_frames),
    num_proc_channels_(num_process_channels),
    output_num_frames_(output_num_frames),
    num_channels_(num_process_channels),
    num_bands_(NumBandsFromSamplesPerChannel(proc_num_frames_)),
    num_split_frames_(rtc::CheckedDivExact(proc_num_frames_, num_bands_)),
    mixed_low_pass_valid_(false),
    reference_copied_(false),
    activity_(AudioFrame::kVadUnknown),
    keyboard_data_(NULL),
    data_(new IFChannelBuffer(proc_num_frames_, num_proc_channels_)) {
  assert(input_num_frames_ > 0);
  assert(proc_num_frames_ > 0);
  assert(output_num_frames_ > 0);
  assert(num_input_channels_ > 0);
  assert(num_proc_channels_ > 0 && num_proc_channels_ <= num_input_channels_);

  if (input_num_frames_ != proc_num_frames_ ||
      output_num_frames_ != proc_num_frames_) {
    // Create an intermediate buffer for resampling.
    process_buffer_.reset(new ChannelBuffer<float>(proc_num_frames_,
                                                   num_proc_channels_));

    if (input_num_frames_ != proc_num_frames_) {
      for (int i = 0; i < num_proc_channels_; ++i) {
        input_resamplers_.push_back(
            new PushSincResampler(input_num_frames_,
                                  proc_num_frames_));
      }
    }

    if (output_num_frames_ != proc_num_frames_) {
      for (int i = 0; i < num_proc_channels_; ++i) {
        output_resamplers_.push_back(
            new PushSincResampler(proc_num_frames_,
                                  output_num_frames_));
      }
    }
  }

  if (num_bands_ > 1) {
    split_data_.reset(new IFChannelBuffer(proc_num_frames_,
                                          num_proc_channels_,
                                          num_bands_));
    splitting_filter_.reset(new SplittingFilter(num_proc_channels_,
                                                num_bands_,
                                                proc_num_frames_));
  }
}

AudioBuffer::~AudioBuffer() {}

void AudioBuffer::CopyFrom(const float* const* data,
                           const StreamConfig& stream_config) {
  assert(stream_config.num_frames() == input_num_frames_);
  assert(stream_config.num_channels() == num_input_channels_);
  InitForNewData();
  // Initialized lazily because there's a different condition in
  // DeinterleaveFrom.
  const bool need_to_downmix =
      num_input_channels_ > 1 && num_proc_channels_ == 1;
  if (need_to_downmix && !input_buffer_) {
    input_buffer_.reset(
        new IFChannelBuffer(input_num_frames_, num_proc_channels_));
  }

  if (stream_config.has_keyboard()) {
    keyboard_data_ = data[KeyboardChannelIndex(stream_config)];
  }

  // Downmix.
  const float* const* data_ptr = data;
  if (need_to_downmix) {
    DownmixToMono<float, float>(data, input_num_frames_, num_input_channels_,
                                input_buffer_->fbuf()->channels()[0]);
    data_ptr = input_buffer_->fbuf_const()->channels();
  }

  // Resample.
  if (input_num_frames_ != proc_num_frames_) {
    for (int i = 0; i < num_proc_channels_; ++i) {
      input_resamplers_[i]->Resample(data_ptr[i],
                                     input_num_frames_,
                                     process_buffer_->channels()[i],
                                     proc_num_frames_);
    }
    data_ptr = process_buffer_->channels();
  }

  // Convert to the S16 range.
  for (int i = 0; i < num_proc_channels_; ++i) {
    FloatToFloatS16(data_ptr[i],
                    proc_num_frames_,
                    data_->fbuf()->channels()[i]);
  }
}

void AudioBuffer::CopyTo(const StreamConfig& stream_config,
                         float* const* data) {
  assert(stream_config.num_frames() == output_num_frames_);
  assert(stream_config.num_channels() == num_channels_);

  // Convert to the float range.
  float* const* data_ptr = data;
  if (output_num_frames_ != proc_num_frames_) {
    // Convert to an intermediate buffer for subsequent resampling.
    data_ptr = process_buffer_->channels();
  }
  for (int i = 0; i < num_channels_; ++i) {
    FloatS16ToFloat(data_->fbuf()->channels()[i],
                    proc_num_frames_,
                    data_ptr[i]);
  }

  // Resample.
  if (output_num_frames_ != proc_num_frames_) {
    for (int i = 0; i < num_channels_; ++i) {
      output_resamplers_[i]->Resample(data_ptr[i],
                                      proc_num_frames_,
                                      data[i],
                                      output_num_frames_);
    }
  }
}

void AudioBuffer::InitForNewData() {
  keyboard_data_ = NULL;
  mixed_low_pass_valid_ = false;
  reference_copied_ = false;
  activity_ = AudioFrame::kVadUnknown;
  num_channels_ = num_proc_channels_;
}

const int16_t* const* AudioBuffer::channels_const() const {
  return data_->ibuf_const()->channels();
}

int16_t* const* AudioBuffer::channels() {
  mixed_low_pass_valid_ = false;
  return data_->ibuf()->channels();
}

const int16_t* const* AudioBuffer::split_bands_const(int channel) const {
  return split_data_.get() ?
         split_data_->ibuf_const()->bands(channel) :
         data_->ibuf_const()->bands(channel);
}

int16_t* const* AudioBuffer::split_bands(int channel) {
  mixed_low_pass_valid_ = false;
  return split_data_.get() ?
         split_data_->ibuf()->bands(channel) :
         data_->ibuf()->bands(channel);
}

const int16_t* const* AudioBuffer::split_channels_const(Band band) const {
  if (split_data_.get()) {
    return split_data_->ibuf_const()->channels(band);
  } else {
    return band == kBand0To8kHz ? data_->ibuf_const()->channels() : nullptr;
  }
}

int16_t* const* AudioBuffer::split_channels(Band band) {
  mixed_low_pass_valid_ = false;
  if (split_data_.get()) {
    return split_data_->ibuf()->channels(band);
  } else {
    return band == kBand0To8kHz ? data_->ibuf()->channels() : nullptr;
  }
}

ChannelBuffer<int16_t>* AudioBuffer::data() {
  mixed_low_pass_valid_ = false;
  return data_->ibuf();
}

const ChannelBuffer<int16_t>* AudioBuffer::data() const {
  return data_->ibuf_const();
}

ChannelBuffer<int16_t>* AudioBuffer::split_data() {
  mixed_low_pass_valid_ = false;
  return split_data_.get() ? split_data_->ibuf() : data_->ibuf();
}

const ChannelBuffer<int16_t>* AudioBuffer::split_data() const {
  return split_data_.get() ? split_data_->ibuf_const() : data_->ibuf_const();
}

const float* const* AudioBuffer::channels_const_f() const {
  return data_->fbuf_const()->channels();
}

float* const* AudioBuffer::channels_f() {
  mixed_low_pass_valid_ = false;
  return data_->fbuf()->channels();
}

const float* const* AudioBuffer::split_bands_const_f(int channel) const {
  return split_data_.get() ?
         split_data_->fbuf_const()->bands(channel) :
         data_->fbuf_const()->bands(channel);
}

float* const* AudioBuffer::split_bands_f(int channel) {
  mixed_low_pass_valid_ = false;
  return split_data_.get() ?
         split_data_->fbuf()->bands(channel) :
         data_->fbuf()->bands(channel);
}

const float* const* AudioBuffer::split_channels_const_f(Band band) const {
  if (split_data_.get()) {
    return split_data_->fbuf_const()->channels(band);
  } else {
    return band == kBand0To8kHz ? data_->fbuf_const()->channels() : nullptr;
  }
}

float* const* AudioBuffer::split_channels_f(Band band) {
  mixed_low_pass_valid_ = false;
  if (split_data_.get()) {
    return split_data_->fbuf()->channels(band);
  } else {
    return band == kBand0To8kHz ? data_->fbuf()->channels() : nullptr;
  }
}

ChannelBuffer<float>* AudioBuffer::data_f() {
  mixed_low_pass_valid_ = false;
  return data_->fbuf();
}

const ChannelBuffer<float>* AudioBuffer::data_f() const {
  return data_->fbuf_const();
}

ChannelBuffer<float>* AudioBuffer::split_data_f() {
  mixed_low_pass_valid_ = false;
  return split_data_.get() ? split_data_->fbuf() : data_->fbuf();
}

const ChannelBuffer<float>* AudioBuffer::split_data_f() const {
  return split_data_.get() ? split_data_->fbuf_const() : data_->fbuf_const();
}

const int16_t* AudioBuffer::mixed_low_pass_data() {
  if (num_proc_channels_ == 1) {
    return split_bands_const(0)[kBand0To8kHz];
  }

  if (!mixed_low_pass_valid_) {
    if (!mixed_low_pass_channels_.get()) {
      mixed_low_pass_channels_.reset(
          new ChannelBuffer<int16_t>(num_split_frames_, 1));
    }

    DownmixToMono<int16_t, int32_t>(split_channels_const(kBand0To8kHz),
                                    num_split_frames_, num_channels_,
                                    mixed_low_pass_channels_->channels()[0]);
    mixed_low_pass_valid_ = true;
  }
  return mixed_low_pass_channels_->channels()[0];
}

const int16_t* AudioBuffer::low_pass_reference(int channel) const {
  if (!reference_copied_) {
    return NULL;
  }

  return low_pass_reference_channels_->channels()[channel];
}

const float* AudioBuffer::keyboard_data() const {
  return keyboard_data_;
}

void AudioBuffer::set_activity(AudioFrame::VADActivity activity) {
  activity_ = activity;
}

AudioFrame::VADActivity AudioBuffer::activity() const {
  return activity_;
}

int AudioBuffer::num_channels() const {
  return num_channels_;
}

void AudioBuffer::set_num_channels(int num_channels) {
  num_channels_ = num_channels;
}

int AudioBuffer::num_frames() const {
  return proc_num_frames_;
}

int AudioBuffer::num_frames_per_band() const {
  return num_split_frames_;
}

int AudioBuffer::num_keyboard_frames() const {
  // We don't resample the keyboard channel.
  return input_num_frames_;
}

int AudioBuffer::num_bands() const {
  return num_bands_;
}

// The resampler is only for supporting 48kHz to 16kHz in the reverse stream.
void AudioBuffer::DeinterleaveFrom(AudioFrame* frame) {
  assert(frame->num_channels_ == num_input_channels_);
  assert(frame->samples_per_channel_ == input_num_frames_);
  InitForNewData();
  // Initialized lazily because there's a different condition in CopyFrom.
  if ((input_num_frames_ != proc_num_frames_) && !input_buffer_) {
    input_buffer_.reset(
        new IFChannelBuffer(input_num_frames_, num_proc_channels_));
  }
  activity_ = frame->vad_activity_;

  int16_t* const* deinterleaved;
  if (input_num_frames_ == proc_num_frames_) {
    deinterleaved = data_->ibuf()->channels();
  } else {
    deinterleaved = input_buffer_->ibuf()->channels();
  }
  if (num_proc_channels_ == 1) {
    // Downmix and deinterleave simultaneously.
    DownmixInterleavedToMono(frame->data_, input_num_frames_,
                             num_input_channels_, deinterleaved[0]);
  } else {
    assert(num_proc_channels_ == num_input_channels_);
    Deinterleave(frame->data_,
                 input_num_frames_,
                 num_proc_channels_,
                 deinterleaved);
  }

  // Resample.
  if (input_num_frames_ != proc_num_frames_) {
    for (int i = 0; i < num_proc_channels_; ++i) {
      input_resamplers_[i]->Resample(input_buffer_->fbuf_const()->channels()[i],
                                     input_num_frames_,
                                     data_->fbuf()->channels()[i],
                                     proc_num_frames_);
    }
  }
}

void AudioBuffer::InterleaveTo(AudioFrame* frame, bool data_changed) const {
  assert(proc_num_frames_ == output_num_frames_);
  assert(num_channels_ == num_input_channels_);
  assert(frame->num_channels_ == num_channels_);
  assert(frame->samples_per_channel_ == proc_num_frames_);
  frame->vad_activity_ = activity_;

  if (!data_changed) {
    return;
  }

  Interleave(data_->ibuf()->channels(),
             proc_num_frames_,
             num_channels_,
             frame->data_);
}

void AudioBuffer::CopyLowPassToReference() {
  reference_copied_ = true;
  if (!low_pass_reference_channels_.get() ||
      low_pass_reference_channels_->num_channels() != num_channels_) {
    low_pass_reference_channels_.reset(
        new ChannelBuffer<int16_t>(num_split_frames_,
                                   num_proc_channels_));
  }
  for (int i = 0; i < num_proc_channels_; i++) {
    memcpy(low_pass_reference_channels_->channels()[i],
           split_bands_const(i)[kBand0To8kHz],
           low_pass_reference_channels_->num_frames_per_band() *
               sizeof(split_bands_const(i)[kBand0To8kHz][0]));
  }
}

void AudioBuffer::SplitIntoFrequencyBands() {
  splitting_filter_->Analysis(data_.get(), split_data_.get());
}

void AudioBuffer::MergeFrequencyBands() {
  splitting_filter_->Synthesis(split_data_.get(), data_.get());
}

}  // namespace webrtc