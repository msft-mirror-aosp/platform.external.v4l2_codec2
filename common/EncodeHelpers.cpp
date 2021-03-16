// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "EncodeHelpers"

#include <v4l2_codec2/common/EncodeHelpers.h>

#include <linux/v4l2-controls.h>
#include <string.h>

#include <C2AllocatorGralloc.h>
#include <cutils/native_handle.h>
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>

namespace android {

media::VideoCodecProfile c2ProfileToVideoCodecProfile(C2Config::profile_t profile) {
    switch (profile) {
    case C2Config::PROFILE_AVC_BASELINE:
        return media::VideoCodecProfile::H264PROFILE_BASELINE;
    case C2Config::PROFILE_AVC_MAIN:
        return media::VideoCodecProfile::H264PROFILE_MAIN;
    case C2Config::PROFILE_AVC_EXTENDED:
        return media::VideoCodecProfile::H264PROFILE_EXTENDED;
    case C2Config::PROFILE_AVC_HIGH:
        return media::VideoCodecProfile::H264PROFILE_HIGH;
    case C2Config::PROFILE_AVC_HIGH_10:
        return media::VideoCodecProfile::H264PROFILE_HIGH10PROFILE;
    case C2Config::PROFILE_AVC_HIGH_422:
        return media::VideoCodecProfile::H264PROFILE_HIGH422PROFILE;
    case C2Config::PROFILE_AVC_HIGH_444_PREDICTIVE:
        return media::VideoCodecProfile::H264PROFILE_HIGH444PREDICTIVEPROFILE;
    case C2Config::PROFILE_AVC_SCALABLE_BASELINE:
        return media::VideoCodecProfile::H264PROFILE_SCALABLEBASELINE;
    case C2Config::PROFILE_AVC_SCALABLE_HIGH:
        return media::VideoCodecProfile::H264PROFILE_SCALABLEHIGH;
    case C2Config::PROFILE_AVC_STEREO_HIGH:
        return media::VideoCodecProfile::H264PROFILE_STEREOHIGH;
    case C2Config::PROFILE_AVC_MULTIVIEW_HIGH:
        return media::VideoCodecProfile::H264PROFILE_MULTIVIEWHIGH;
    case C2Config::PROFILE_VP9_0:
        return media::VideoCodecProfile::VP9PROFILE_PROFILE0;
    case C2Config::PROFILE_VP9_1:
        return media::VideoCodecProfile::VP9PROFILE_PROFILE1;
    case C2Config::PROFILE_VP9_2:
        return media::VideoCodecProfile::VP9PROFILE_PROFILE2;
    case C2Config::PROFILE_VP9_3:
        return media::VideoCodecProfile::VP9PROFILE_PROFILE3;
    default:
        ALOGE("Unrecognizable C2 profile (value = 0x%x)...", profile);
        return media::VideoCodecProfile::VIDEO_CODEC_PROFILE_UNKNOWN;
    }
}

uint8_t c2LevelToV4L2Level(C2Config::level_t level) {
    switch (level) {
    case C2Config::LEVEL_AVC_1:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
    case C2Config::LEVEL_AVC_1B:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1B;
    case C2Config::LEVEL_AVC_1_1:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_1;
    case C2Config::LEVEL_AVC_1_2:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_2;
    case C2Config::LEVEL_AVC_1_3:
        return V4L2_MPEG_VIDEO_H264_LEVEL_1_3;
    case C2Config::LEVEL_AVC_2:
        return V4L2_MPEG_VIDEO_H264_LEVEL_2_0;
    case C2Config::LEVEL_AVC_2_1:
        return V4L2_MPEG_VIDEO_H264_LEVEL_2_1;
    case C2Config::LEVEL_AVC_2_2:
        return V4L2_MPEG_VIDEO_H264_LEVEL_2_2;
    case C2Config::LEVEL_AVC_3:
        return V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
    case C2Config::LEVEL_AVC_3_1:
        return V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
    case C2Config::LEVEL_AVC_3_2:
        return V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
    case C2Config::LEVEL_AVC_4:
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
    case C2Config::LEVEL_AVC_4_1:
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
    case C2Config::LEVEL_AVC_4_2:
        return V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
    case C2Config::LEVEL_AVC_5:
        return V4L2_MPEG_VIDEO_H264_LEVEL_5_0;
    case C2Config::LEVEL_AVC_5_1:
        return V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
    default:
        ALOGE("Unrecognizable C2 level (value = 0x%x)...", level);
        return 0;
    }
}

android_ycbcr getGraphicBlockInfo(const C2ConstGraphicBlock& block) {
    uint32_t width, height, format, stride, igbp_slot, generation;
    uint64_t usage, igbp_id;
    android::_UnwrapNativeCodec2GrallocMetadata(block.handle(), &width, &height, &format, &usage,
                                                &stride, &generation, &igbp_id, &igbp_slot);
    native_handle_t* grallocHandle = android::UnwrapNativeCodec2GrallocHandle(block.handle());
    sp<GraphicBuffer> buf = new GraphicBuffer(grallocHandle, GraphicBuffer::CLONE_HANDLE, width,
                                              height, format, 1, usage, stride);
    native_handle_delete(grallocHandle);

    // Pass SW flag so that ARCVM returns the guest buffer dimensions instead
    // of the host buffer dimensions. This means we will have to convert the
    // return value from ptrs to buffer offsets ourselves.
    android_ycbcr ycbcr = {};
    int32_t status = buf->lockYCbCr(GRALLOC_USAGE_SW_READ_OFTEN, &ycbcr);
    if (status != OK) ALOGE("lockYCbCr is failed: %d", (int)status);
    buf->unlock();

    uintptr_t y = reinterpret_cast<uintptr_t>(ycbcr.y);
    ycbcr.y = nullptr;
    ycbcr.cb = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ycbcr.cb) - y);
    ycbcr.cr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ycbcr.cr) - y);

    return ycbcr;
}

void extractCSDInfo(std::unique_ptr<C2StreamInitDataInfo::output>* const csd, const uint8_t* data,
                    size_t length) {
    constexpr uint8_t kTypeSeqParamSet = 7;
    constexpr uint8_t kTypePicParamSet = 8;

    // Android frameworks needs 4 bytes start code.
    constexpr uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};
    constexpr int kStartCodeLength = 4;

    csd->reset();

    // Temporarily allocate a byte array to copy codec config data. This should be freed after
    // codec config data extraction is done.
    auto tmpConfigData = std::make_unique<uint8_t[]>(length);
    uint8_t* tmpOutput = tmpConfigData.get();
    uint8_t* tmpConfigDataEnd = tmpOutput + length;

    NalParser parser(data, length);
    while (parser.locateNextNal()) {
        if (parser.length() == 0) continue;
        uint8_t nalType = *parser.data() & 0x1f;
        ALOGV("find next NAL: type=%d, length=%zu", nalType, parser.length());
        if (nalType != kTypeSeqParamSet && nalType != kTypePicParamSet) continue;

        if (tmpOutput + kStartCodeLength + parser.length() > tmpConfigDataEnd) {
            ALOGE("Buffer overflow on extracting codec config data (length=%zu)", length);
            return;
        }
        std::memcpy(tmpOutput, kStartCode, kStartCodeLength);
        tmpOutput += kStartCodeLength;
        std::memcpy(tmpOutput, parser.data(), parser.length());
        tmpOutput += parser.length();
    }

    size_t configDataLength = tmpOutput - tmpConfigData.get();
    ALOGV("Extracted codec config data: length=%zu", configDataLength);
    *csd = C2StreamInitDataInfo::output::AllocUnique(configDataLength, 0u);
    std::memcpy((*csd)->m.value, tmpConfigData.get(), configDataLength);
}

NalParser::NalParser(const uint8_t* data, size_t length)
      : mCurrNalDataPos(data), mDataEnd(data + length) {
    mNextNalStartCodePos = findNextStartCodePos();
}

bool NalParser::locateNextNal() {
    if (mNextNalStartCodePos == mDataEnd) return false;
    mCurrNalDataPos = mNextNalStartCodePos + kNalStartCodeLength;  // skip start code.
    mNextNalStartCodePos = findNextStartCodePos();
    return true;
}

const uint8_t* NalParser::data() const {
    return mCurrNalDataPos;
}

size_t NalParser::length() const {
    if (mNextNalStartCodePos == mDataEnd) return mDataEnd - mCurrNalDataPos;
    size_t length = mNextNalStartCodePos - mCurrNalDataPos;
    // The start code could be 3 or 4 bytes, i.e., 0x000001 or 0x00000001.
    return *(mNextNalStartCodePos - 1) == 0x00 ? length - 1 : length;
}

const uint8_t* NalParser::findNextStartCodePos() const {
    return std::search(mCurrNalDataPos, mDataEnd, kNalStartCode,
                       kNalStartCode + kNalStartCodeLength);
}

}  // namespace android
