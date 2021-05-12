// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Note: ported from Chromium commit head: 85fdf90

#include "base/logging.h"

#include "video_decode_accelerator.h"

namespace media {

VideoDecodeAccelerator::SupportedProfile::SupportedProfile()
    : profile(VIDEO_CODEC_PROFILE_UNKNOWN), encrypted_only(false) {}

VideoDecodeAccelerator::SupportedProfile::~SupportedProfile() = default;

} // namespace media
