// Copyright 2024 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_V4L2_CODEC2_COMMON_H264_H
#define ANDROID_V4L2_CODEC2_COMMON_H264_H

#include <C2Config.h>

#include <ui/Size.h>

namespace android {

// Table A-1 in spec
struct H264LevelLimits {
    C2Config::level_t level;
    float maxMBPS;   // max macroblock processing rate in macroblocks per second
    uint64_t maxFS;  // max frame size in macroblocks
    uint32_t maxBR;  // max video bitrate in bits per second
};

constexpr H264LevelLimits kH264Limits[] = {
        {C2Config::LEVEL_AVC_1, 1485, 99, 64000},
        {C2Config::LEVEL_AVC_1B, 1485, 99, 128000},
        {C2Config::LEVEL_AVC_1_1, 3000, 396, 192000},
        {C2Config::LEVEL_AVC_1_2, 6000, 396, 384000},
        {C2Config::LEVEL_AVC_1_3, 11880, 396, 768000},
        {C2Config::LEVEL_AVC_2, 11880, 396, 2000000},
        {C2Config::LEVEL_AVC_2_1, 19800, 792, 4000000},
        {C2Config::LEVEL_AVC_2_2, 20250, 1620, 4000000},
        {C2Config::LEVEL_AVC_3, 40500, 1620, 10000000},
        {C2Config::LEVEL_AVC_3_1, 108000, 3600, 14000000},
        {C2Config::LEVEL_AVC_3_2, 216000, 5120, 20000000},
        {C2Config::LEVEL_AVC_4, 245760, 8192, 20000000},
        {C2Config::LEVEL_AVC_4_1, 245760, 8192, 50000000},
        {C2Config::LEVEL_AVC_4_2, 522240, 8704, 50000000},
        {C2Config::LEVEL_AVC_5, 589824, 22080, 135000000},
        {C2Config::LEVEL_AVC_5_1, 983040, 36864, 240000000},
        {C2Config::LEVEL_AVC_5_2, 2073600, 36864, 240000000},
};

uint32_t maxFramerateForLevelH264(C2Config::level_t level, const ui::Size& videoSize);

inline bool isH264Profile(C2Config::profile_t profile) {
    return (profile >= C2Config::PROFILE_AVC_BASELINE &&
            profile <= C2Config::PROFILE_AVC_ENHANCED_MULTIVIEW_DEPTH_HIGH);
}

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_COMMON_H264_H
