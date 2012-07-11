/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ALOG_TAG "AudioPolicyManager"
//#define ALOG_NDEBUG 0
#include <utils/log.h>
#include "AudioPolicyManager.h"
#include <media/mediarecorder.h>
#include <fcntl.h>
#include <cutils/properties.h>
#include <math.h>


namespace android_audio_legacy {

// ----------------------------------------------------------------------------
// AudioPolicyManager for msm7k platform
// Common audio policy manager code is implemented in AudioPolicyManagerBase class
// ----------------------------------------------------------------------------

// ---  class factory


extern "C" AudioPolicyInterface* createAudioPolicyManager(AudioPolicyClientInterface *clientInterface)
{
    return new AudioPolicyManager(clientInterface);
}

extern "C" void destroyAudioPolicyManager(AudioPolicyInterface *interface)
{
    delete interface;
}

uint32_t AudioPolicyManager::getDeviceForStrategy(routing_strategy strategy, bool fromCache)
{
    uint32_t device = 0;

    if (fromCache) {
        ALOGV("getDeviceForStrategy() from cache strategy %d, device %x", strategy, mDeviceForStrategy[strategy]);
        return mDeviceForStrategy[strategy];
    }

    switch (strategy) {
        case STRATEGY_DTMF:
            if (mPhoneState != AudioSystem::MODE_IN_CALL) {
                // when off call, DTMF strategy follows the same rules as MEDIA strategy
                device = getDeviceForStrategy(STRATEGY_MEDIA, false);
                break;
            }
            // when in call, DTMF and PHONE strategies follow the same rules
            // FALL THROUGH

        case STRATEGY_PHONE:
            // for phone strategy, we first consider the forced use and then the available devices by order
            // of priority
            switch (mForceUse[AudioSystem::FOR_COMMUNICATION]) {
                case AudioSystem::FORCE_BT_SCO:
                    if (mPhoneState != AudioSystem::MODE_IN_CALL || strategy != STRATEGY_DTMF) {
                        device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT;
                        if (device) break;
                    }
                    device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET;
                    if (device) break;
                    device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO;
                    if (device) break;
                    // if SCO device is requested but no SCO device is available, fall back to default case
                    // FALL THROUGH

                default:    // FORCE_NONE
                    device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE;
                    if (device) break;
                    device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET;
                    if (device) break;
#ifdef WITH_A2DP
                    // when not in a phone call, phone strategy should route STREAM_VOICE_CALL to A2DP
                    if (mPhoneState != AudioSystem::MODE_IN_CALL) {
                        device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP;
                        if (device) break;
                        device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES;
                        if (device) break;
                    }
#endif
                    if (mPhoneState == AudioSystem::MODE_RINGTONE)
                        device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_SPEAKER;
                    if (device) break;

                    device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_EARPIECE;
                    if (device == 0) {
                        ALOGE("getDeviceForStrategy() earpiece device not found");
                    }
                    break;

                case AudioSystem::FORCE_SPEAKER:
                    if (mPhoneState != AudioSystem::MODE_IN_CALL || strategy != STRATEGY_DTMF) {
                        device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT;
                        if (device) break;
                    }
#ifdef WITH_A2DP
                    // when not in a phone call, phone strategy should route STREAM_VOICE_CALL to
                    // A2DP speaker when forcing to speaker output
                    if (mPhoneState != AudioSystem::MODE_IN_CALL) {
                        device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER;
                        if (device) break;
                    }
#endif
                    device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_SPEAKER;
                    if (device == 0) {
                        ALOGE("getDeviceForStrategy() speaker device not found");
                    }
                    break;
            }
            break;

        case STRATEGY_SONIFICATION:
            device = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_SPEAKER;
            if (device == 0) {
                ALOGE("getDeviceForStrategy() speaker device not found");
            }
            // FALL THROUGH

        case STRATEGY_ENFORCED_AUDIBLE:
            // If incall, just select the STRATEGY_PHONE device: The rest of the behavior is handled by
            // handleIncallSonification().
            if (mPhoneState == AudioSystem::MODE_IN_CALL) {
                device = getDeviceForStrategy(STRATEGY_PHONE, false);
                break;
            }
            // FALL THROUGH

        case STRATEGY_MEDIA:
            // If we come from case STRATEGY_SONIFICATION, this code is skipped
            if (strategy != STRATEGY_SONIFICATION)
            {
#ifdef HAVE_FM_RADIO
                uint32_t device2 = 0;
                if (mForceUse[AudioSystem::FOR_MEDIA] == AudioSystem::FORCE_SPEAKER) {
                    device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_SPEAKER;
                }
                if (device2 == 0) {
                    device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_AUX_DIGITAL;
                }
#else
                uint32_t device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_AUX_DIGITAL;
#endif
#ifdef WITH_A2DP
                if (mA2dpOutput != 0) {
                    if (strategy == STRATEGY_SONIFICATION && !a2dpUsedForSonification()) {
                        break;
                    }
                    if (device2 == 0) {
                        device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP;
                    }
                    if (device2 == 0) {
                        device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES;
                    }
                    if (device2 == 0) {
                        device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER;
                    }
                }
#endif

                if (device2 == 0) {
                    device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADPHONE;
                }
                if (device2 == 0) {
                    device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_WIRED_HEADSET;
                }
                if (device2 == 0) {
                    device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_SPEAKER;
                }
                if (device2 == 0) {
                    device2 = mAvailableOutputDevices & AudioSystem::DEVICE_OUT_EARPIECE;
                }

                device |= device2;

#ifdef HAVE_FM_RADIO
                if (mAvailableOutputDevices & AudioSystem::DEVICE_OUT_FM_ALL) {
                        device |= AudioSystem::DEVICE_OUT_FM_ALL;
                }
#endif

                // Do not play media stream if in call and the requested device would change the hardware
                // output routing
                if (mPhoneState == AudioSystem::MODE_IN_CALL &&
                    !AudioSystem::isA2dpDevice((AudioSystem::audio_devices)device) &&
                    device != getDeviceForStrategy(STRATEGY_PHONE)) {
                    device = 0;
                    ALOGV("getDeviceForStrategy() incompatible media and phone devices");
                }
            } 
            break;

        default:
            ALOGW("getDeviceForStrategy() unknown strategy: %d", strategy);
            break;
    }

    ALOGV("getDeviceForStrategy() strategy %d, device %x", strategy, device);
    return device;
}

status_t AudioPolicyManager::checkAndSetVolume(int stream, int index, audio_io_handle_t output, uint32_t device, int delayMs, bool force)
{
    // do not change actual stream volume if the stream is muted
    if (mOutputs.valueFor(output)->mMuteCount[stream] != 0) {
        ALOGV("checkAndSetVolume() stream %d muted count %d", stream, mOutputs.valueFor(output)->mMuteCount[stream]);
        return NO_ERROR;
    }

    // do not change in call volume if bluetooth is connected and vice versa
    if ((stream == AudioSystem::VOICE_CALL && mForceUse[AudioSystem::FOR_COMMUNICATION] == AudioSystem::FORCE_BT_SCO) ||
        (stream == AudioSystem::BLUETOOTH_SCO && mForceUse[AudioSystem::FOR_COMMUNICATION] != AudioSystem::FORCE_BT_SCO)) {
        ALOGV("checkAndSetVolume() cannot set stream %d volume with force use = %d for comm",
             stream, mForceUse[AudioSystem::FOR_COMMUNICATION]);
        return INVALID_OPERATION;
    }

    // get volume level
    float volume = computeVolume(stream, index, output, device);

    // when the output device is the speaker it's necessary to apply an extra volume attenuation (default 6dB) to prevent audio distortion
    if (device == AudioSystem::DEVICE_OUT_SPEAKER) {
        char speakerBuf[PROPERTY_VALUE_MAX];
        property_get("persist.sys.speaker-attn", speakerBuf, "6");
        ALOGI("setStreamVolume() attenuation [%s]", speakerBuf);
        float volumeFactor = pow(10.0, -atof(speakerBuf)/20.0);
        ALOGV("setStreamVolume() applied volume factor %f to device %d", volumeFactor, device);
        volume *= volumeFactor;
    }

    // apply optional volume attenuation (default 0dB) to headset/headphone
    if (device == AudioSystem::DEVICE_OUT_WIRED_HEADSET ||
        device == AudioSystem::DEVICE_OUT_WIRED_HEADPHONE) {
        char headsetBuf[PROPERTY_VALUE_MAX];
        property_get("persist.sys.headset-attn", headsetBuf, "0");
        ALOGI("setStreamVolume() attenuation [%s]", headsetBuf);
        float volumeFactor = pow(10.0, -atof(headsetBuf)/20.0);
        ALOGV("setStreamVolume() applied volume factor %f to device %d", volumeFactor, device);
        volume *= volumeFactor;
    }

#ifdef HAVE_FM_RADIO
    // apply optional volume attenuation (default 0dB) to FM audio
    if (stream == AudioSystem::FM) {
        char fmBuf[PROPERTY_VALUE_MAX];
        property_get("persist.sys.fm-attn", fmBuf, "0");
        ALOGI("setStreamVolume() attenuation [%s]", fmBuf);
        float volumeFactor = pow(10.0, -atof(fmBuf)/20.0);
        ALOGV("setStreamVolume() applied volume factor %f to device %d", volumeFactor, device);
        volume *= volumeFactor;
    }
#endif

    // We actually change the volume if:
    // - the float value returned by computeVolume() changed
    // - the force flag is set
    if (volume != mOutputs.valueFor(output)->mCurVolume[stream] ||
        (stream == AudioSystem::VOICE_CALL) ||
#ifdef HAVE_FM_RADIO
            (stream == AudioSystem::FM) ||
#endif
            force) {
        mOutputs.valueFor(output)->mCurVolume[stream] = volume;
        ALOGV("setStreamVolume() for output %d stream %d, volume %f, delay %d", output, stream, volume, delayMs);
        if (stream == AudioSystem::VOICE_CALL ||
            stream == AudioSystem::DTMF ||
            stream == AudioSystem::BLUETOOTH_SCO) {
            // offset value to reflect actual hardware volume that never reaches 0
            // 1% corresponds roughly to first step in VOICE_CALL stream volume setting (see AudioService.java)
            volume = 0.01 + 0.99 * volume;
#ifdef HAVE_FM_RADIO
        } else if (stream == AudioSystem::FM) {
            float fmVolume = -1.0;
            fmVolume = volume;
            if (fmVolume >= 0 && output == mHardwareOutput) {
                mpClientInterface->setFmVolume(fmVolume, delayMs);
            }
            return NO_ERROR;
#endif
        }

        mpClientInterface->setStreamVolume((AudioSystem::stream_type)stream, volume, output, delayMs);
    }

    if (stream == AudioSystem::VOICE_CALL ||
        stream == AudioSystem::BLUETOOTH_SCO) {
        float voiceVolume;
        // Force voice volume to max for bluetooth SCO as volume is managed by the headset
        if (stream == AudioSystem::VOICE_CALL) {
            voiceVolume = (float)index/(float)mStreams[stream].mIndexMax;
        } else {
            voiceVolume = 1.0;
        }
        if (voiceVolume >= 0 && output == mHardwareOutput) {
            mpClientInterface->setVoiceVolume(voiceVolume, delayMs);
            mLastVoiceVolume = voiceVolume;
        }
    }

    return NO_ERROR;
}
}; // namespace android_audio_legacy