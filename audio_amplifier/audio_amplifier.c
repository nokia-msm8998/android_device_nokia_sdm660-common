/*
 * Copyright (C) 2015 The CyanogenMod Open Source Project
 * Copyright (C) 2020-2026 The LineageOS Project
 *
 * Standalone Audio Amplifier HAL for Nokia / FIH SDM660 Devices (TFA98xx variants)
 * Reverse-engineered from stock Nokia SDM660 audio HAL (audio.primary.sdm660.so)
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

#define LOG_TAG "amplifier_tfa98xx_nokia"
#define LOG_NDEBUG 0

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

#include <cutils/properties.h>
#include <log/log.h>
#include <system/audio.h>
#include <hardware/audio_amplifier.h>
#include <tinyalsa/asoundlib.h>

#include "platform.h"
#include "platform_api.h"
#include "audio_hw.h"

#define TFA98XX_MIXER_CTL "speaker Profile"

typedef enum {
    PROFILE_NONE = -1,
    PROFILE_MUSIC = 0,
    PROFILE_VOICE = 1,
    PROFILE_RINGTONE = 2,
    PROFILE_SYSTEM_SOUND = 3,
} tfa_profile_t;

static const char *const tfa_profile_names[] = {
    [PROFILE_MUSIC]        = "music",
    [PROFILE_VOICE]        = "voice",
    [PROFILE_RINGTONE]     = "ringtone",
    [PROFILE_SYSTEM_SOUND] = "system_sound",
};

typedef struct amp_device {
    amplifier_device_t amp_dev;
    struct mixer* mixer;
    struct mixer_ctl* ctl_profile;
    audio_mode_t cur_mode;
    uint32_t cur_out_devices;
    uint32_t cur_in_devices;
    tfa_profile_t active_profile;
    bool is_tfa_supported;

    const struct hw_module_t* module_ahal;
    int (*enable_snd_device)(struct audio_device*, snd_device_t);
    int (*enable_audio_route)(struct audio_device*, struct audio_usecase*);
    int (*disable_snd_device)(struct audio_device*, snd_device_t);
    int (*disable_audio_route)(struct audio_device*, struct audio_usecase*);
    struct audio_usecase* (*get_usecase_from_list)(const struct audio_device*, audio_usecase_t);
} tfa_amp_t;

static tfa_amp_t* g_tfa_dev = NULL;
static bool is_speaker_device(uint32_t devices) {
    return (devices & AUDIO_DEVICE_OUT_SPEAKER) != 0;
}

static void tfa98xx_set_profile(tfa_amp_t* dev, tfa_profile_t profile) {
    if (!dev || !dev->ctl_profile)
        return;

    if (profile < PROFILE_MUSIC || profile > PROFILE_SYSTEM_SOUND)
        return;

    if (dev->active_profile == profile)
        return;

    /* 20ms kernel stabilization delay matching stock HAL */
    usleep(20000);

    const char* prof_name = tfa_profile_names[profile];
    int ret = mixer_ctl_set_enum_by_string(dev->ctl_profile, prof_name);
    if (ret != 0) {
        ALOGE("%s: Failed to set '%s' to '%s' (err: %d)", __func__,
              TFA98XX_MIXER_CTL, prof_name, ret);
    } else {
        ALOGI("%s: TFA9891 switched to profile '%s'", __func__, prof_name);
        dev->active_profile = profile;
    }
}

static void tfa98xx_update_route(tfa_amp_t* dev) {
    char smartamp_prop[PROPERTY_VALUE_MAX] = {0};
    char debug_prop[PROPERTY_VALUE_MAX] = {0};

    if (!dev || !dev->is_tfa_supported)
        return;

    /*
     * DEBUG PROPERTY: Force amplifier profile
     * Usage: setprop persist.vendor.audio.debug.tfa.profile <music|voice|ringtone|system_sound|auto>
     */
    property_get("persist.vendor.audio.debug.tfa.profile", debug_prop, "auto");
    if (strcmp(debug_prop, "auto") != 0 && debug_prop[0] != '\0') {
        ALOGW("%s: [DEBUG OVERRIDE ACTIVE] Forcing profile: '%s'", __func__, debug_prop);
        for (int i = 0; i <= PROFILE_SYSTEM_SOUND; i++) {
            if (strcmp(debug_prop, tfa_profile_names[i]) == 0) {
                tfa98xx_set_profile(dev, (tfa_profile_t)i);
                return;
            }
        }
    }

    /* Check global enable toggle */
    property_get("persist.sys.smartamp", smartamp_prop, "1");
    if (smartamp_prop[0] != '1') {
        ALOGD("%s: persist.sys.smartamp != 1, skipping profile update", __func__);
        return;
    }

    if (!is_speaker_device(dev->cur_out_devices)) {
        dev->active_profile = PROFILE_NONE;
        return;
    }

    /* Mode priorities matching stock HAL */
    switch (dev->cur_mode) {
        case AUDIO_MODE_NORMAL:
            ALOGD("%s: Route active on speaker: mode=MUSIC", __func__);
            tfa98xx_set_profile(dev, PROFILE_MUSIC);
            break;
        case AUDIO_MODE_IN_CALL:
        case AUDIO_MODE_IN_COMMUNICATION:
            ALOGD("%s: Route active on speaker: mode=VOICE", __func__);
            tfa98xx_set_profile(dev, PROFILE_VOICE);
            break;
        case AUDIO_MODE_RINGTONE:
            ALOGD("%s: Route active on speaker: mode=RINGTONE", __func__);
            tfa98xx_set_profile(dev, PROFILE_RINGTONE);
            break;
        default:
            ALOGW("%s: Unknown mode: %d", __func__, dev->cur_mode);
            break;
    }
}

static int amp_set_mode(amplifier_device_t* device, audio_mode_t mode) {
    tfa_amp_t* dev = (tfa_amp_t*)device;
    if (!dev) return -EINVAL;

    dev->cur_mode = mode;
    tfa98xx_update_route(dev);
    return 0;
}

static int amp_set_output_devices(amplifier_device_t* device, uint32_t devices) {
    tfa_amp_t* dev = (tfa_amp_t*)device;
    if (!dev) return -EINVAL;

    dev->cur_out_devices = devices;
    tfa98xx_update_route(dev);
    return 0;
}

static int amp_set_feedback(amplifier_device_t* device, void* adev,
                            uint32_t snd_device, bool enable) {
    (void)device;
    (void)adev;
    (void)snd_device;
    (void)enable;
    return 0;
}

static int amp_dev_close(hw_device_t* device) {
    tfa_amp_t* dev = (tfa_amp_t*)device;
    if (dev) {
        if (dev->mixer) {
            mixer_close(dev->mixer);
        }
        free(dev);
        g_tfa_dev = NULL;
    }
    return 0;
}

static int amp_module_open(const hw_module_t* module, const char* name,
                           hw_device_t** device) {
    char boot_dev[PROPERTY_VALUE_MAX] = {0};

    if (strcmp(name, AMPLIFIER_HARDWARE_INTERFACE) != 0) {
        ALOGE("%s: %s does not match expected interface name", __func__, name);
        return -ENODEV;
    }

    g_tfa_dev = calloc(1, sizeof(tfa_amp_t));
    if (!g_tfa_dev) {
        ALOGE("%s: Unable to allocate memory for amplifier device", __func__);
        return -ENOMEM;
    }

    g_tfa_dev->amp_dev.common.tag = HARDWARE_DEVICE_TAG;
    g_tfa_dev->amp_dev.common.module = (hw_module_t*)module;
    g_tfa_dev->amp_dev.common.version = HARDWARE_DEVICE_API_VERSION(1, 0);
    g_tfa_dev->amp_dev.common.close = amp_dev_close;

    g_tfa_dev->amp_dev.set_mode = amp_set_mode;
    g_tfa_dev->amp_dev.set_output_devices = amp_set_output_devices;
    g_tfa_dev->amp_dev.set_feedback = amp_set_feedback;

    g_tfa_dev->cur_mode = AUDIO_MODE_NORMAL;
    g_tfa_dev->cur_out_devices = 0;
    g_tfa_dev->cur_in_devices = 0;
    g_tfa_dev->active_profile = PROFILE_NONE;

    property_get("ro.boot.device", boot_dev, "");
    if (strcmp(boot_dev, "PL2") == 0 ||
        strcmp(boot_dev, "DRG") == 0 ||
        strcmp(boot_dev, "B2N") == 0 ||
        strcmp(boot_dev, "CTL") == 0 ||
        strcmp(boot_dev, "SS2") == 0 ||
        strcmp(boot_dev, "SD1") == 0 ||
        strcmp(boot_dev, "HH1") == 0 ||
        strcmp(boot_dev, "HH6") == 0 ||
        strcmp(boot_dev, "HG1") == 0) {
            g_tfa_dev->is_tfa_supported = true;
            ALOGI("%s: Verified supported board ro.boot.device='%s'", __func__, boot_dev);
    } else {
            g_tfa_dev->is_tfa_supported = true;
            ALOGW("%s: Device '%s' not in primary list, enabling fallback", __func__, boot_dev);
    }

    /* Open default ALSA sound card 0 */
    g_tfa_dev->mixer = mixer_open(0);
    if (!g_tfa_dev->mixer) {
        ALOGE("%s: Failed to open ALSA mixer for sound card 0", __func__);
        free(g_tfa_dev);
        g_tfa_dev = NULL;
        return -ENODEV;
    }

    g_tfa_dev->ctl_profile = mixer_get_ctl_by_name(g_tfa_dev->mixer, TFA98XX_MIXER_CTL);
    if (!g_tfa_dev->ctl_profile) {
        ALOGW("%s: Mixer control '%s' not present on card 0 (Assuming TAS variant)", __func__, TFA98XX_MIXER_CTL);
        g_tfa_dev->is_tfa_supported = false;
    } else {
        ALOGI("%s: Identified '%s' control, initializing TFA9891 (default profile: music)",
              __func__, TFA98XX_MIXER_CTL);
        tfa98xx_set_profile(g_tfa_dev, PROFILE_MUSIC);
    }

    if (hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, AUDIO_HARDWARE_MODULE_ID_PRIMARY,
                               &g_tfa_dev->module_ahal)) {
        ALOGW("%s: Failed to load audio.primary", __func__);
        return -ENODEV;
    }

#define LOAD_AHAL_SYMBOL(symbol)                                      \
        do {                                                          \
            g_tfa_dev->symbol = dlsym(g_tfa_dev->module_ahal->dso, #symbol);\
            if (g_tfa_dev->symbol == NULL) {                          \
                ALOGW("%s: %s not found (%s)", __func__, #symbol, dlerror());\
                free(g_tfa_dev);                                             \
                return -ENODEV;                                             \
            }                                                         \
        } while (0)

        LOAD_AHAL_SYMBOL(enable_snd_device);
        LOAD_AHAL_SYMBOL(enable_audio_route);
        LOAD_AHAL_SYMBOL(disable_snd_device);
        LOAD_AHAL_SYMBOL(disable_audio_route);
        LOAD_AHAL_SYMBOL(get_usecase_from_list);
#undef LOAD_AHAL_SYMBOL

    *device = (hw_device_t*)g_tfa_dev;
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = amp_module_open,
};

amplifier_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AMPLIFIER_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AMPLIFIER_HARDWARE_MODULE_ID,
        .name = "Nokia SDM660 TFA98XX audio amplifier HAL",
        .author = "The LineageOS Open Source Project",
        .methods = &hal_module_methods,
    },
};
