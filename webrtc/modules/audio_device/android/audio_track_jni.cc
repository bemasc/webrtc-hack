/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/audio_device/android/audio_manager.h"
#include "webrtc/modules/audio_device/android/audio_track_jni.h"

#include <android/log.h>

#include "webrtc/base/arraysize.h"
#include "webrtc/base/checks.h"

#define TAG "AudioTrackJni"
#define ALOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

namespace webrtc {

// AudioTrackJni::JavaAudioTrack implementation.
AudioTrackJni::JavaAudioTrack::JavaAudioTrack(
    NativeRegistration* native_reg, rtc::scoped_ptr<GlobalRef> audio_track)
    : audio_track_(audio_track.Pass()),
      init_playout_(native_reg->GetMethodId("InitPlayout", "(II)V")),
      start_playout_(native_reg->GetMethodId("StartPlayout", "()Z")),
      stop_playout_(native_reg->GetMethodId("StopPlayout", "()Z")),
      set_stream_volume_(native_reg->GetMethodId("SetStreamVolume", "(I)Z")),
      get_stream_max_volume_(native_reg->GetMethodId(
          "GetStreamMaxVolume", "()I")),
      get_stream_volume_(native_reg->GetMethodId("GetStreamVolume", "()I")) {
}

AudioTrackJni::JavaAudioTrack::~JavaAudioTrack() {}

void AudioTrackJni::JavaAudioTrack::InitPlayout(int sample_rate, int channels) {
  audio_track_->CallVoidMethod(init_playout_, sample_rate, channels);
}

bool AudioTrackJni::JavaAudioTrack::StartPlayout() {
  return audio_track_->CallBooleanMethod(start_playout_);
}

bool AudioTrackJni::JavaAudioTrack::StopPlayout() {
  return audio_track_->CallBooleanMethod(stop_playout_);
}

bool AudioTrackJni::JavaAudioTrack::SetStreamVolume(int volume) {
  return audio_track_->CallBooleanMethod(set_stream_volume_, volume);
}

int AudioTrackJni::JavaAudioTrack::GetStreamMaxVolume() {
  return audio_track_->CallIntMethod(get_stream_max_volume_);
}

int AudioTrackJni::JavaAudioTrack::GetStreamVolume() {
  return audio_track_->CallIntMethod(get_stream_volume_);
}

// TODO(henrika): possible extend usage of AudioManager and add it as member.
AudioTrackJni::AudioTrackJni(AudioManager* audio_manager)
    : j_environment_(JVM::GetInstance()->environment()),
      audio_parameters_(audio_manager->GetPlayoutAudioParameters()),
      direct_buffer_address_(nullptr),
      direct_buffer_capacity_in_bytes_(0),
      frames_per_buffer_(0),
      initialized_(false),
      playing_(false),
      audio_device_buffer_(nullptr) {
  ALOGD("ctor%s", GetThreadInfo().c_str());
  DCHECK(audio_parameters_.is_valid());
  CHECK(j_environment_);
  JNINativeMethod native_methods[] = {
      {"nativeCacheDirectBufferAddress", "(Ljava/nio/ByteBuffer;J)V",
      reinterpret_cast<void*>(
          &webrtc::AudioTrackJni::CacheDirectBufferAddress)},
      {"nativeGetPlayoutData", "(IJ)V",
      reinterpret_cast<void*>(&webrtc::AudioTrackJni::GetPlayoutData)}};
  j_native_registration_ = j_environment_->RegisterNatives(
      "org/webrtc/voiceengine/WebRtcAudioTrack",
      native_methods, arraysize(native_methods));
  j_audio_track_.reset(new JavaAudioTrack(
      j_native_registration_.get(),
      j_native_registration_->NewObject(
          "<init>", "(Landroid/content/Context;J)V",
          JVM::GetInstance()->context(), PointerTojlong(this))));
  // Detach from this thread since we want to use the checker to verify calls
  // from the Java based audio thread.
  thread_checker_java_.DetachFromThread();
}

AudioTrackJni::~AudioTrackJni() {
  ALOGD("~dtor%s", GetThreadInfo().c_str());
  DCHECK(thread_checker_.CalledOnValidThread());
  Terminate();
}

int32_t AudioTrackJni::Init() {
  ALOGD("Init%s", GetThreadInfo().c_str());
  DCHECK(thread_checker_.CalledOnValidThread());
  return 0;
}

int32_t AudioTrackJni::Terminate() {
  ALOGD("Terminate%s", GetThreadInfo().c_str());
  DCHECK(thread_checker_.CalledOnValidThread());
  StopPlayout();
  return 0;
}

int32_t AudioTrackJni::InitPlayout() {
  ALOGD("InitPlayout%s", GetThreadInfo().c_str());
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK(!initialized_);
  DCHECK(!playing_);
  j_audio_track_->InitPlayout(
      audio_parameters_.sample_rate(), audio_parameters_.channels());
  initialized_ = true;
  return 0;
}

int32_t AudioTrackJni::StartPlayout() {
  ALOGD("StartPlayout%s", GetThreadInfo().c_str());
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK(initialized_);
  DCHECK(!playing_);
  if (!j_audio_track_->StartPlayout()) {
    ALOGE("StartPlayout failed!");
    return -1;
  }
  playing_ = true;
  return 0;
}

int32_t AudioTrackJni::StopPlayout() {
  ALOGD("StopPlayout%s", GetThreadInfo().c_str());
  DCHECK(thread_checker_.CalledOnValidThread());
  if (!initialized_ || !playing_) {
    return 0;
  }
  if (!j_audio_track_->StopPlayout()) {
    ALOGE("StopPlayout failed!");
    return -1;
  }
  // If we don't detach here, we will hit a DCHECK in OnDataIsRecorded() next
  // time StartRecording() is called since it will create a new Java thread.
  thread_checker_java_.DetachFromThread();
  initialized_ = false;
  playing_ = false;
  return 0;
}

int AudioTrackJni::SpeakerVolumeIsAvailable(bool& available) {
  available = true;
  return 0;
}

int AudioTrackJni::SetSpeakerVolume(uint32_t volume) {
  ALOGD("SetSpeakerVolume(%d)%s", volume, GetThreadInfo().c_str());
  DCHECK(thread_checker_.CalledOnValidThread());
  return j_audio_track_->SetStreamVolume(volume) ? 0 : -1;
}

int AudioTrackJni::MaxSpeakerVolume(uint32_t& max_volume) const {
  ALOGD("MaxSpeakerVolume%s", GetThreadInfo().c_str());
  DCHECK(thread_checker_.CalledOnValidThread());
  max_volume = j_audio_track_->GetStreamMaxVolume();
  return 0;
}

int AudioTrackJni::MinSpeakerVolume(uint32_t& min_volume) const {
  ALOGD("MaxSpeakerVolume%s", GetThreadInfo().c_str());
  DCHECK(thread_checker_.CalledOnValidThread());
  min_volume = 0;
  return 0;
}

int AudioTrackJni::SpeakerVolume(uint32_t& volume) const {
  ALOGD("SpeakerVolume%s", GetThreadInfo().c_str());
  DCHECK(thread_checker_.CalledOnValidThread());
  volume = j_audio_track_->GetStreamVolume();
  return 0;
}

// TODO(henrika): possibly add stereo support.
void AudioTrackJni::AttachAudioBuffer(AudioDeviceBuffer* audioBuffer) {
  ALOGD("AttachAudioBuffer%s", GetThreadInfo().c_str());
  DCHECK(thread_checker_.CalledOnValidThread());
  audio_device_buffer_ = audioBuffer;
  const int sample_rate_hz = audio_parameters_.sample_rate();
  ALOGD("SetPlayoutSampleRate(%d)", sample_rate_hz);
  audio_device_buffer_->SetPlayoutSampleRate(sample_rate_hz);
  const int channels = audio_parameters_.channels();
  ALOGD("SetPlayoutChannels(%d)", channels);
  audio_device_buffer_->SetPlayoutChannels(channels);
}

void JNICALL AudioTrackJni::CacheDirectBufferAddress(
    JNIEnv* env, jobject obj, jobject byte_buffer, jlong nativeAudioTrack) {
  webrtc::AudioTrackJni* this_object =
      reinterpret_cast<webrtc::AudioTrackJni*> (nativeAudioTrack);
  this_object->OnCacheDirectBufferAddress(env, byte_buffer);
}

void AudioTrackJni::OnCacheDirectBufferAddress(
    JNIEnv* env, jobject byte_buffer) {
  ALOGD("OnCacheDirectBufferAddress");
  DCHECK(thread_checker_.CalledOnValidThread());
  direct_buffer_address_ =
      env->GetDirectBufferAddress(byte_buffer);
  jlong capacity = env->GetDirectBufferCapacity(byte_buffer);
  ALOGD("direct buffer capacity: %lld", capacity);
  direct_buffer_capacity_in_bytes_ = static_cast<int> (capacity);
  frames_per_buffer_ = direct_buffer_capacity_in_bytes_ / kBytesPerFrame;
  ALOGD("frames_per_buffer: %d", frames_per_buffer_);
}

void JNICALL AudioTrackJni::GetPlayoutData(
  JNIEnv* env, jobject obj, jint length, jlong nativeAudioTrack) {
  webrtc::AudioTrackJni* this_object =
      reinterpret_cast<webrtc::AudioTrackJni*> (nativeAudioTrack);
  this_object->OnGetPlayoutData(length);
}

// This method is called on a high-priority thread from Java. The name of
// the thread is 'AudioRecordTrack'.
void AudioTrackJni::OnGetPlayoutData(int length) {
  DCHECK(thread_checker_java_.CalledOnValidThread());
  DCHECK_EQ(frames_per_buffer_, length / kBytesPerFrame);
  if (!audio_device_buffer_) {
    ALOGE("AttachAudioBuffer has not been called!");
    return;
  }
  // Pull decoded data (in 16-bit PCM format) from jitter buffer.
  int samples = audio_device_buffer_->RequestPlayoutData(frames_per_buffer_);
  if (samples <= 0) {
    ALOGE("AudioDeviceBuffer::RequestPlayoutData failed!");
    return;
  }
  DCHECK_EQ(samples, frames_per_buffer_);
  // Copy decoded data into common byte buffer to ensure that it can be
  // written to the Java based audio track.
  samples = audio_device_buffer_->GetPlayoutData(direct_buffer_address_);
  DCHECK_EQ(length, kBytesPerFrame * samples);
}

}  // namespace webrtc