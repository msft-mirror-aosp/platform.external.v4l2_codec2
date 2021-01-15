// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "V4L2ComponentStore"

#include <v4l2_codec2/store/V4L2ComponentStore.h>

#include <dlfcn.h>
#include <stdint.h>

#include <memory>
#include <mutex>

#include <C2.h>
#include <C2Config.h>
#include <log/log.h>
#include <media/stagefright/foundation/MediaDefs.h>

#include <v4l2_codec2/common/V4L2ComponentCommon.h>

namespace android {
namespace {
const char* kLibPath = "libv4l2_codec2_components.so";
const char* kCreateFactoryFuncName = "CreateCodec2Factory";
const char* kDestroyFactoryFuncName = "DestroyCodec2Factory";

const uint32_t kComponentRank = 0x80;

std::string getMediaTypeFromComponentName(const std::string& name) {
    if (name == V4L2ComponentName::kH264Decoder || name == V4L2ComponentName::kH264SecureDecoder ||
        name == V4L2ComponentName::kH264Encoder) {
        return MEDIA_MIMETYPE_VIDEO_AVC;
    }
    if (name == V4L2ComponentName::kVP8Decoder || name == V4L2ComponentName::kVP8SecureDecoder) {
        return MEDIA_MIMETYPE_VIDEO_VP8;
    }
    if (name == V4L2ComponentName::kVP9Decoder || name == V4L2ComponentName::kVP9SecureDecoder) {
        return MEDIA_MIMETYPE_VIDEO_VP9;
    }
    return "";
}

}  // namespace

// static
std::shared_ptr<C2ComponentStore> V4L2ComponentStore::Create() {
    ALOGV("%s()", __func__);

    static std::mutex mutex;
    static std::weak_ptr<C2ComponentStore> platformStore;

    std::lock_guard<std::mutex> lock(mutex);
    std::shared_ptr<C2ComponentStore> store = platformStore.lock();
    if (store != nullptr) return store;

    void* libHandle = dlopen(kLibPath, RTLD_NOW | RTLD_NODELETE);
    if (!libHandle) {
        ALOGE("Failed to load library: %s", kLibPath);
        return nullptr;
    }

    auto createFactoryFunc = (CreateV4L2FactoryFunc)dlsym(libHandle, kCreateFactoryFuncName);
    auto destroyFactoryFunc = (DestroyV4L2FactoryFunc)dlsym(libHandle, kDestroyFactoryFuncName);
    if (!createFactoryFunc || !destroyFactoryFunc) {
        ALOGE("Failed to load functions: %s, %s", kCreateFactoryFuncName, kDestroyFactoryFuncName);
        dlclose(libHandle);
        return nullptr;
    }

    store = std::shared_ptr<C2ComponentStore>(
            new V4L2ComponentStore(libHandle, createFactoryFunc, destroyFactoryFunc));
    platformStore = store;
    return store;
}

V4L2ComponentStore::V4L2ComponentStore(void* libHandle, CreateV4L2FactoryFunc createFactoryFunc,
                                       DestroyV4L2FactoryFunc destroyFactoryFunc)
      : mLibHandle(libHandle),
        mCreateFactoryFunc(createFactoryFunc),
        mDestroyFactoryFunc(destroyFactoryFunc),
        mReflector(std::make_shared<C2ReflectorHelper>()) {
    ALOGV("%s()", __func__);
}

V4L2ComponentStore::~V4L2ComponentStore() {
    ALOGV("%s()", __func__);

    std::lock_guard<std::mutex> lock(mCachedFactoriesLock);
    for (const auto& kv : mCachedFactories) mDestroyFactoryFunc(kv.second);
    mCachedFactories.clear();

    dlclose(mLibHandle);
}

C2String V4L2ComponentStore::getName() const {
    return "android.componentStore.v4l2";
}

c2_status_t V4L2ComponentStore::createComponent(C2String name,
                                                std::shared_ptr<C2Component>* const component) {
    ALOGV("%s(%s)", __func__, name.c_str());

    if (!V4L2ComponentName::isValid(name.c_str())) {
        ALOGI("%s(): Invalid component name: %s", __func__, name.c_str());
        return C2_NOT_FOUND;
    }

    auto factory = GetFactory(name);
    if (factory == nullptr) return C2_CORRUPTED;

    component->reset();
    return factory->createComponent(0, component);
}

c2_status_t V4L2ComponentStore::createInterface(
        C2String name, std::shared_ptr<C2ComponentInterface>* const interface) {
    ALOGV("%s(%s)", __func__, name.c_str());

    if (!V4L2ComponentName::isValid(name.c_str())) {
        ALOGI("%s(): Invalid component name: %s", __func__, name.c_str());
        return C2_NOT_FOUND;
    }

    auto factory = GetFactory(name);
    if (factory == nullptr) return C2_CORRUPTED;

    interface->reset();
    return factory->createInterface(0, interface);
}

std::vector<std::shared_ptr<const C2Component::Traits>> V4L2ComponentStore::listComponents() {
    ALOGV("%s()", __func__);

    std::vector<std::shared_ptr<const C2Component::Traits>> ret;
    ret.push_back(GetTraits(V4L2ComponentName::kH264Encoder));
    ret.push_back(GetTraits(V4L2ComponentName::kH264Decoder));
    ret.push_back(GetTraits(V4L2ComponentName::kH264SecureDecoder));
    ret.push_back(GetTraits(V4L2ComponentName::kVP8Decoder));
    ret.push_back(GetTraits(V4L2ComponentName::kVP8SecureDecoder));
    ret.push_back(GetTraits(V4L2ComponentName::kVP9Decoder));
    ret.push_back(GetTraits(V4L2ComponentName::kVP9SecureDecoder));
    return ret;
}

std::shared_ptr<C2ParamReflector> V4L2ComponentStore::getParamReflector() const {
    return mReflector;
}

c2_status_t V4L2ComponentStore::copyBuffer(std::shared_ptr<C2GraphicBuffer> /* src */,
                                           std::shared_ptr<C2GraphicBuffer> /* dst */) {
    return C2_OMITTED;
}

c2_status_t V4L2ComponentStore::querySupportedParams_nb(
        std::vector<std::shared_ptr<C2ParamDescriptor>>* const /* params */) const {
    return C2_OK;
}

c2_status_t V4L2ComponentStore::query_sm(
        const std::vector<C2Param*>& stackParams,
        const std::vector<C2Param::Index>& heapParamIndices,
        std::vector<std::unique_ptr<C2Param>>* const /* heapParams */) const {
    // There are no supported config params.
    return stackParams.empty() && heapParamIndices.empty() ? C2_OK : C2_BAD_INDEX;
}

c2_status_t V4L2ComponentStore::config_sm(
        const std::vector<C2Param*>& params,
        std::vector<std::unique_ptr<C2SettingResult>>* const /* failures */) {
    // There are no supported config params.
    return params.empty() ? C2_OK : C2_BAD_INDEX;
}

c2_status_t V4L2ComponentStore::querySupportedValues_sm(
        std::vector<C2FieldSupportedValuesQuery>& fields) const {
    // There are no supported config params.
    return fields.empty() ? C2_OK : C2_BAD_INDEX;
}

::C2ComponentFactory* V4L2ComponentStore::GetFactory(const C2String& name) {
    ALOGV("%s(%s)", __func__, name.c_str());
    ALOG_ASSERT(V4L2ComponentName::isValid(name.c_str()));

    std::lock_guard<std::mutex> lock(mCachedFactoriesLock);
    const auto it = mCachedFactories.find(name);
    if (it != mCachedFactories.end()) return it->second;

    ::C2ComponentFactory* factory = mCreateFactoryFunc(name.c_str());
    if (factory == nullptr) {
        ALOGE("Failed to create factory for %s", name.c_str());
        return nullptr;
    }

    mCachedFactories.emplace(name, factory);
    return factory;
}

std::shared_ptr<const C2Component::Traits> V4L2ComponentStore::GetTraits(const C2String& name) {
    ALOGV("%s(%s)", __func__, name.c_str());

    if (!V4L2ComponentName::isValid(name.c_str())) {
        ALOGE("Invalid component name: %s", name.c_str());
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mCachedTraitsLock);
    auto it = mCachedTraits.find(name);
    if (it != mCachedTraits.end()) return it->second;

    auto traits = std::make_shared<C2Component::Traits>();
    traits->name = name;
    traits->domain = C2Component::DOMAIN_VIDEO;
    traits->rank = kComponentRank;
    traits->mediaType = getMediaTypeFromComponentName(name);
    traits->kind = V4L2ComponentName::isEncoder(name.c_str()) ? C2Component::KIND_ENCODER
                                                              : C2Component::KIND_DECODER;

    mCachedTraits.emplace(name, traits);
    return traits;
}

}  // namespace android
