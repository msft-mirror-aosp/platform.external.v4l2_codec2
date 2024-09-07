// Copyright 2024 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <v4l2_codec2/common/H264.h>

#include <log/log.h>

namespace android {

uint32_t maxFramerateForLevelH264(C2Config::level_t level, const ui::Size& videoSize) {
    uint32_t maxFramerate = std::numeric_limits<uint32_t>::max();

    bool found = false;
    for (const H264LevelLimits& limit : kH264Limits) {
        if (limit.level != level) continue;

        uint64_t frameSizeMB =
                static_cast<uint64_t>((videoSize.width + 15) / 16) * ((videoSize.height + 15) / 16);
        maxFramerate = limit.maxMBPS / frameSizeMB;
        found = true;
        break;
    }

    if (!found) ALOGW("%s - failed to find matching H264 level=%d", __func__, level);

    return maxFramerate;
}

}  // namespace android
