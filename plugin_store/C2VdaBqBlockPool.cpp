// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "C2VdaBqBlockPool"

#include <v4l2_codec2/plugin_store/C2VdaBqBlockPool.h>

#include <errno.h>

#include <chrono>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <C2BlockInternal.h>
#include <android/hardware/graphics/bufferqueue/2.0/IGraphicBufferProducer.h>
#include <android/hardware/graphics/bufferqueue/2.0/IProducerListener.h>
#include <base/callback.h>
#include <log/log.h>
#include <system/window.h>
#include <types.h>
#include <ui/BufferQueueDefs.h>

#include <v4l2_codec2/plugin_store/V4L2AllocatorId.h>
#include <v4l2_codec2/plugin_store/V4L2GraphicAllocator.h>

namespace android {
namespace {

// The wait time for acquire fence in milliseconds.
constexpr int kFenceWaitTimeMs = 10;

}  // namespace

using namespace std::chrono_literals;

using ::android::C2AndroidMemoryUsage;
using ::android::Fence;
using ::android::GraphicBuffer;
using ::android::sp;
using ::android::status_t;
using ::android::BufferQueueDefs::BUFFER_NEEDS_REALLOCATION;
using ::android::BufferQueueDefs::NUM_BUFFER_SLOTS;
using ::android::BufferQueueDefs::RELEASE_ALL_BUFFERS;
using ::android::hardware::hidl_handle;
using ::android::hardware::Return;

using HBuffer = ::android::hardware::graphics::common::V1_2::HardwareBuffer;
using HStatus = ::android::hardware::graphics::bufferqueue::V2_0::Status;
using HGraphicBufferProducer =
        ::android::hardware::graphics::bufferqueue::V2_0::IGraphicBufferProducer;
using HProducerListener = ::android::hardware::graphics::bufferqueue::V2_0::IProducerListener;
using HConnectionType = hardware::graphics::bufferqueue::V2_0::ConnectionType;
using HQueueBufferOutput =
        ::android::hardware::graphics::bufferqueue::V2_0::IGraphicBufferProducer::QueueBufferOutput;

using ::android::hardware::graphics::bufferqueue::V2_0::utils::b2h;
using ::android::hardware::graphics::bufferqueue::V2_0::utils::h2b;
using ::android::hardware::graphics::bufferqueue::V2_0::utils::HFenceWrapper;

static c2_status_t asC2Error(int32_t err) {
    switch (err) {
    case android::NO_ERROR:
        return C2_OK;
    case android::NO_INIT:
        return C2_NO_INIT;
    case android::BAD_VALUE:
        return C2_BAD_VALUE;
    case android::TIMED_OUT:
        return C2_TIMED_OUT;
    case android::WOULD_BLOCK:
        return C2_BLOCKING;
    case android::NO_MEMORY:
        return C2_NO_MEMORY;
    }
    return C2_CORRUPTED;
}

class H2BGraphicBufferProducer {
public:
    explicit H2BGraphicBufferProducer(sp<HGraphicBufferProducer> base) : mBase(base) {}
    ~H2BGraphicBufferProducer() = default;

    status_t requestBuffer(int slot, sp<GraphicBuffer>* buf) {
        bool converted = false;
        status_t status = UNKNOWN_ERROR;
        Return<void> transResult = mBase->requestBuffer(
                slot, [&converted, &status, buf](HStatus hStatus, HBuffer const& hBuffer,
                                                 uint32_t generationNumber) {
                    converted = h2b(hStatus, &status) && h2b(hBuffer, buf);
                    if (*buf) {
                        (*buf)->setGenerationNumber(generationNumber);
                    }
                });

        if (!transResult.isOk()) {
            ALOGE("%s(): transaction failed: %s", __func__, transResult.description().c_str());
            return FAILED_TRANSACTION;
        }
        if (!converted) {
            ALOGE("%s(): corrupted transaction.", __func__);
            return FAILED_TRANSACTION;
        }
        if (status != android::NO_ERROR) {
            ALOGD("%s() failed: %d", __func__, status);
        }
        return status;
    }

    status_t setMaxDequeuedBufferCount(int maxDequeuedBuffers) {
        status_t status = UNKNOWN_ERROR;
        Return<HStatus> transResult =
                mBase->setMaxDequeuedBufferCount(static_cast<int32_t>(maxDequeuedBuffers));

        if (!transResult.isOk()) {
            ALOGE("%s(): transaction failed: %s", __func__, transResult.description().c_str());
            return FAILED_TRANSACTION;
        }
        if (!h2b(static_cast<HStatus>(transResult), &status)) {
            ALOGE("%s(): corrupted transaction.", __func__);
            return FAILED_TRANSACTION;
        }
        if (status != android::NO_ERROR) {
            ALOGD("%s() failed: %d", __func__, status);
        }
        return status;
    }

    status_t dequeueBuffer(uint32_t width, uint32_t height, uint32_t pixelFormat,
                           C2AndroidMemoryUsage androidUsage, int* slot, sp<Fence>* fence) {
        using Input = HGraphicBufferProducer::DequeueBufferInput;
        using Output = HGraphicBufferProducer::DequeueBufferOutput;
        Input input{width, height, pixelFormat, androidUsage.asGrallocUsage()};

        bool converted = false;
        status_t status = UNKNOWN_ERROR;
        Return<void> transResult = mBase->dequeueBuffer(
                input, [&converted, &status, &slot, &fence](HStatus hStatus, int32_t hSlot,
                                                            Output const& hOutput) {
                    converted = h2b(hStatus, &status);
                    if (!converted || status != android::NO_ERROR) {
                        return;
                    }

                    *slot = hSlot;
                    if (hOutput.bufferNeedsReallocation) {
                        status = BUFFER_NEEDS_REALLOCATION;
                    }
                    converted = h2b(hOutput.fence, fence);
                });

        if (!transResult.isOk()) {
            ALOGE("%s(): transaction failed: %s", __func__, transResult.description().c_str());
            return FAILED_TRANSACTION;
        }
        if (!converted) {
            ALOGE("%s(): corrupted transaction.", __func__);
            return FAILED_TRANSACTION;
        }
        if (status != android::NO_ERROR && status != BUFFER_NEEDS_REALLOCATION &&
            status != android::TIMED_OUT) {
            ALOGD("%s() failed: %d", __func__, status);
        }
        return status;
    }

    status_t detachBuffer(int slot) {
        status_t status = UNKNOWN_ERROR;
        Return<HStatus> transResult = mBase->detachBuffer(static_cast<int32_t>(slot));

        if (!transResult.isOk()) {
            ALOGE("%s(): transaction failed: %s", __func__, transResult.description().c_str());
            return FAILED_TRANSACTION;
        }
        if (!h2b(static_cast<HStatus>(transResult), &status)) {
            ALOGE("%s(): corrupted transaction.", __func__);
            return FAILED_TRANSACTION;
        }
        if (status != android::NO_ERROR) {
            ALOGD("%s() failed: %d", __func__, status);
        }
        return status;
    }

    status_t attachBuffer(const sp<GraphicBuffer>& buffer, int* outSlot) {
        HBuffer hBuffer;
        uint32_t hGenerationNumber;
        if (!b2h(buffer, &hBuffer, &hGenerationNumber)) {
            ALOGE("%s: invalid input buffer.", __func__);
            return BAD_VALUE;
        }

        bool converted = false;
        status_t status = UNKNOWN_ERROR;
        Return<void> transResult = mBase->attachBuffer(
                hBuffer, hGenerationNumber,
                [&converted, &status, outSlot](HStatus hStatus, int32_t hSlot,
                                               bool releaseAllBuffers) {
                    converted = h2b(hStatus, &status);
                    *outSlot = static_cast<int>(hSlot);
                    if (converted && releaseAllBuffers && status == android::NO_ERROR) {
                        status = RELEASE_ALL_BUFFERS;
                    }
                });

        if (!transResult.isOk()) {
            ALOGE("%s(): transaction failed: %s", __func__, transResult.description().c_str());
            return FAILED_TRANSACTION;
        }
        if (!converted) {
            ALOGE("%s(): corrupted transaction.", __func__);
            return FAILED_TRANSACTION;
        }
        if (status != android::NO_ERROR) {
            ALOGD("%s() failed: %d", __func__, status);
        }
        return status;
    }

    status_t cancelBuffer(int slot, const sp<Fence>& fence) {
        HFenceWrapper hFenceWrapper;
        if (!b2h(fence, &hFenceWrapper)) {
            ALOGE("%s(): corrupted input fence.", __func__);
            return UNKNOWN_ERROR;
        }

        status_t status = UNKNOWN_ERROR;
        Return<HStatus> transResult =
                mBase->cancelBuffer(static_cast<int32_t>(slot), hFenceWrapper.getHandle());

        if (!transResult.isOk()) {
            ALOGE("%s(): transaction failed: %s", __func__, transResult.description().c_str());
            return FAILED_TRANSACTION;
        }
        if (!h2b(static_cast<HStatus>(transResult), &status)) {
            ALOGE("%s(): corrupted transaction.", __func__);
            return FAILED_TRANSACTION;
        }
        if (status != android::NO_ERROR) {
            ALOGD("%s() failed: %d", __func__, status);
        }
        return status;
    }

    int query(int what, int* value) {
        int result = 0;
        Return<void> transResult =
                mBase->query(static_cast<int32_t>(what), [&result, value](int32_t r, int32_t v) {
                    result = static_cast<int>(r);
                    *value = static_cast<int>(v);
                });

        if (!transResult.isOk()) {
            ALOGE("%s(): transaction failed: %s", __func__, transResult.description().c_str());
            return FAILED_TRANSACTION;
        }
        return result;
    }

    status_t allowAllocation(bool allow) {
        status_t status = UNKNOWN_ERROR;
        Return<HStatus> transResult = mBase->allowAllocation(allow);

        if (!transResult.isOk()) {
            ALOGE("%s(): transaction failed: %s", __func__, transResult.description().c_str());
            return FAILED_TRANSACTION;
        }
        if (!h2b(static_cast<HStatus>(transResult), &status)) {
            ALOGE("%s(): corrupted transaction.", __func__);
            return FAILED_TRANSACTION;
        }
        if (status != android::NO_ERROR) {
            ALOGD("%s() failed: %d", __func__, status);
        }
        return status;
    }

    status_t getUniqueId(uint64_t* outId) const {
        Return<uint64_t> transResult = mBase->getUniqueId();

        if (!transResult.isOk()) {
            ALOGE("%s(): transaction failed: %s", __func__, transResult.description().c_str());
            return FAILED_TRANSACTION;
        }

        *outId = static_cast<uint64_t>(transResult);
        return android::NO_ERROR;
    }

    // android::IProducerListener cannot be depended by vendor library, so we use HProducerListener
    // directly.
    status_t connect(sp<HProducerListener> const& hListener, int32_t api,
                     bool producerControlledByApp) {
        bool converted = false;
        status_t status = UNKNOWN_ERROR;
        // hack(b/146409777): We pass self-defined api, so we don't use b2h() here.
        Return<void> transResult = mBase->connect(
                hListener, static_cast<HConnectionType>(api), producerControlledByApp,
                [&converted, &status](HStatus hStatus, HQueueBufferOutput const& /* hOutput */) {
                    converted = h2b(hStatus, &status);
                });

        if (!transResult.isOk()) {
            ALOGE("%s(): transaction failed: %s", __func__, transResult.description().c_str());
            return FAILED_TRANSACTION;
        }
        if (!converted) {
            ALOGE("%s(): corrupted transaction.", __func__);
            return FAILED_TRANSACTION;
        }
        return status;
    }

    status_t setDequeueTimeout(nsecs_t timeout) {
        status_t status = UNKNOWN_ERROR;
        Return<HStatus> transResult = mBase->setDequeueTimeout(static_cast<int64_t>(timeout));

        if (!transResult.isOk()) {
            ALOGE("%s(): transaction failed: %s", __func__, transResult.description().c_str());
            return FAILED_TRANSACTION;
        }
        if (!h2b(static_cast<HStatus>(transResult), &status)) {
            ALOGE("%s(): corrupted transaction.", __func__);
            return FAILED_TRANSACTION;
        }
        return status;
    }

private:
    const sp<HGraphicBufferProducer> mBase;
};

// This class is used to notify the listener when a certain event happens.
class EventNotifier : public virtual android::RefBase {
public:
    class Listener {
    public:
        virtual ~Listener() = default;

        // Called by EventNotifier when a certain event happens.
        virtual void onEventNotified() = 0;
    };

    explicit EventNotifier(const std::shared_ptr<Listener>& listener) : mListener(listener) {}
    virtual ~EventNotifier() = default;

protected:
    void notify() {
        ALOGV("%s()", __func__);
        std::shared_ptr<Listener> listener = mListener.lock();
        if (listener) {
            listener->onEventNotified();
        }
    }

    std::weak_ptr<Listener> mListener;
};

// Notifies the listener when the connected IGBP releases buffers.
class BufferReleasedNotifier : public EventNotifier, public HProducerListener {
public:
    using EventNotifier::EventNotifier;
    ~BufferReleasedNotifier() override = default;

    // HProducerListener implementation
    Return<void> onBuffersReleased(uint32_t count) override {
        ALOGV("%s(%u)", __func__, count);
        if (count > 0) {
            notify();
        }
        return {};
    }
};

/**
 * BlockPoolData implementation for C2VdaBqBlockPool. The life cycle of this object should be as
 * long as its accompanied C2GraphicBlock.
 *
 * When C2VdaBqBlockPoolData is created, |mShared| is false, and the owner of the accompanied
 * C2GraphicBlock is the component that called fetchGraphicBlock(). If this is released before
 * sharing, the destructor will call detachBuffer() to BufferQueue to free the slot.
 *
 * When the accompanied C2GraphicBlock is going to share to client from component, component should
 * call MarkBlockPoolDataAsShared() to set |mShared| to true, and then this will be released after
 * the transition of C2GraphicBlock across HIDL interface. At this time, the destructor will not
 * call detachBuffer().
 */
struct C2VdaBqBlockPoolData : public _C2BlockPoolData {
    // This type should be a different value than what _C2BlockPoolData::type_t has defined.
    static constexpr int kTypeVdaBufferQueue = TYPE_BUFFERQUEUE + 256;

    C2VdaBqBlockPoolData(uint64_t producerId, int32_t slotId, uint32_t uniqueId,
                         const std::shared_ptr<C2VdaBqBlockPool::Impl>& pool);
    C2VdaBqBlockPoolData() = delete;

    // If |mShared| is false, call detach buffer to BufferQueue via |mPool|
    virtual ~C2VdaBqBlockPoolData() override;

    type_t getType() const override { return static_cast<type_t>(kTypeVdaBufferQueue); }

    bool mShared = false;  // whether is shared from component to client.
    const uint64_t mProducerId;
    const int32_t mSlotId;
    const uint32_t mUniqueId;
    const std::shared_ptr<C2VdaBqBlockPool::Impl> mPool;
};

c2_status_t MarkBlockPoolDataAsShared(const C2ConstGraphicBlock& sharedBlock) {
    std::shared_ptr<_C2BlockPoolData> data = _C2BlockFactory::GetGraphicBlockPoolData(sharedBlock);
    if (!data || data->getType() != C2VdaBqBlockPoolData::kTypeVdaBufferQueue) {
        // Skip this functtion if |sharedBlock| is not fetched from C2VdaBqBlockPool.
        return C2_OMITTED;
    }
    const std::shared_ptr<C2VdaBqBlockPoolData> poolData =
            std::static_pointer_cast<C2VdaBqBlockPoolData>(data);
    if (poolData->mShared) {
        ALOGE("C2VdaBqBlockPoolData(id=%" PRIx64 ", slot=%d) is already marked as shared...",
              poolData->mProducerId, poolData->mSlotId);
        return C2_BAD_STATE;
    }
    poolData->mShared = true;
    return C2_OK;
}

// Used to store the tracked graphic buffers requestsed from IGBP. This class keeps the
// bidirectional mapping between unique ID of the buffer and IGBP slot, and the
// mapping from IGBP slot to C2Allocation.
class TrackedGraphicBuffers {
public:
    using value_type = std::tuple<int32_t /* slotId */, uint32_t /* uniqueId */,
                                  std::shared_ptr<C2GraphicAllocation> /* allocation */>;

    TrackedGraphicBuffers() = default;
    ~TrackedGraphicBuffers() = default;

    bool insert(int32_t slotId, uint32_t uniqueId,
                std::shared_ptr<C2GraphicAllocation> allocation) {
        ALOGV("%s(slotId=%d, uniqueId=%u)", __func__, slotId, uniqueId);
        ALOG_ASSERT(allocation != nullptr);

        if (!mUniqueId2SlotId.emplace(uniqueId, slotId).second) {
            ALOGE("%s(): duplicate uniqueId %u", __func__, uniqueId);
            return false;
        }
        if (!mSlotId2UniqueId.emplace(slotId, uniqueId).second) {
            ALOGE("%s(): duplicate slotId %u", __func__, uniqueId);
            mUniqueId2SlotId.erase(uniqueId);
            return false;
        }
        mSlotId2Allocation.emplace(slotId, std::move(allocation));
        return true;
    }

    value_type pop() {
        ALOG_ASSERT(size() > 0);

        auto idIter = mSlotId2UniqueId.begin();
        ALOG_ASSERT(idIter != mSlotId2UniqueId.end());
        int32_t slotId = idIter->first;
        uint32_t uniqueId = idIter->second;
        mSlotId2UniqueId.erase(idIter);
        mUniqueId2SlotId.erase(uniqueId);

        auto allocIter = mSlotId2Allocation.find(slotId);
        ALOG_ASSERT(allocIter != mSlotId2Allocation.end());
        std::shared_ptr<C2GraphicAllocation> allocation = allocIter->second;
        mSlotId2Allocation.erase(allocIter);

        return std::make_tuple(slotId, uniqueId, std::move(allocation));
    }

    void clear() {
        ALOGV("%s(): current size = %zu", __func__, size());

        mUniqueId2SlotId.clear();
        mSlotId2UniqueId.clear();
        mSlotId2Allocation.clear();
    }

    size_t size() const { return mUniqueId2SlotId.size(); }

    bool hasUniqueId(uint32_t uniqueId) const {
        return mUniqueId2SlotId.find(uniqueId) != mUniqueId2SlotId.end();
    }

    bool hasSlotId(int32_t slotId) const {
        return mSlotId2UniqueId.find(slotId) != mSlotId2UniqueId.end();
    }

    uint32_t getUniqueId(int32_t slotId) const {
        auto iter = mSlotId2UniqueId.find(slotId);
        ALOG_ASSERT(iter != mSlotId2UniqueId.end());
        return iter->second;
    }

    int32_t getSlotId(uint32_t uniqueId) const {
        auto iter = mUniqueId2SlotId.find(uniqueId);
        ALOG_ASSERT(iter != mUniqueId2SlotId.end());
        return iter->second;
    }

    std::shared_ptr<C2GraphicAllocation> getAllocation(int32_t slotId) const {
        auto iter = mSlotId2Allocation.find(slotId);
        ALOG_ASSERT(iter != mSlotId2Allocation.end());
        return iter->second;
    }

    std::string debugString() const {
        std::stringstream ss;
        for (const auto& slot : mSlotId2UniqueId) ss << slot.first << ", ";
        return ss.str();
    }

private:
    std::map<int32_t, uint32_t> mSlotId2UniqueId;
    std::map<uint32_t, int32_t> mUniqueId2SlotId;
    std::map<int32_t, std::shared_ptr<C2GraphicAllocation>> mSlotId2Allocation;
};

class C2VdaBqBlockPool::Impl : public std::enable_shared_from_this<C2VdaBqBlockPool::Impl>,
                               public EventNotifier::Listener {
public:
    using HGraphicBufferProducer = C2VdaBqBlockPool::HGraphicBufferProducer;

    explicit Impl(const std::shared_ptr<C2Allocator>& allocator);
    // TODO: should we detach buffers on producer if any on destructor?
    ~Impl() = default;

    // EventNotifier::Listener implementation.
    void onEventNotified() override;

    c2_status_t fetchGraphicBlock(uint32_t width, uint32_t height, uint32_t format,
                                  C2MemoryUsage usage,
                                  std::shared_ptr<C2GraphicBlock>* block /* nonnull */);
    void setRenderCallback(const C2BufferQueueBlockPool::OnRenderCallback& renderCallback);
    void configureProducer(const sp<HGraphicBufferProducer>& producer);
    c2_status_t requestNewBufferSet(int32_t bufferCount, uint32_t width, uint32_t height,
                                    uint32_t format, C2MemoryUsage usage);
    bool setNotifyBlockAvailableCb(::base::OnceClosure cb);

private:
    friend struct C2VdaBqBlockPoolData;

    // Requested buffer formats.
    struct BufferFormat {
        BufferFormat(uint32_t width, uint32_t height, uint32_t pixelFormat,
                     C2AndroidMemoryUsage androidUsage)
              : mWidth(width), mHeight(height), mPixelFormat(pixelFormat), mUsage(androidUsage) {}
        BufferFormat() = default;

        uint32_t mWidth = 0;
        uint32_t mHeight = 0;
        uint32_t mPixelFormat = 0;
        C2AndroidMemoryUsage mUsage = C2MemoryUsage(0);
    };

    status_t getFreeSlotLocked(uint32_t width, uint32_t height, uint32_t format,
                               C2MemoryUsage usage, int32_t* slot, sp<Fence>* fence);

    // Called when C2GraphicBlock and its C2VdaBqBlockPoolData are released.
    void onC2GraphicBlockReleased(uint64_t producerId, int32_t slotId, uint32_t uniqueId,
                                  bool shared);

    // Queries the generation and usage flags from the given producer by dequeuing and requesting a
    // buffer (the buffer is then detached and freed).
    c2_status_t queryGenerationAndUsage(H2BGraphicBufferProducer* const producer, uint32_t width,
                                        uint32_t height, uint32_t pixelFormat,
                                        C2AndroidMemoryUsage androidUsage, uint32_t* generation,
                                        uint64_t* usage);

    // Detaches all the tracked buffers from |mProducer|, and returns all the buffers.
    std::vector<std::shared_ptr<C2GraphicAllocation>> detachAndMoveTrackedBuffers();
    // Switches producer and transfers allocated buffers from old producer to the new one.
    bool prepareMigrateBuffers();
    bool pumpMigrateBuffers();

    const std::shared_ptr<C2Allocator> mAllocator;

    std::unique_ptr<H2BGraphicBufferProducer> mProducer;
    uint64_t mProducerId = 0;
    C2BufferQueueBlockPool::OnRenderCallback mRenderCallback;

    // Function mutex to lock at the start of each API function call for protecting the
    // synchronization of all member variables.
    std::mutex mMutex;

    TrackedGraphicBuffers mTrackedGraphicBuffers;

    // Number of buffers requested on requestNewBufferSet() call.
    size_t mBuffersRequested = 0u;
    // Set to true when we need to call IGBP::setMaxDequeuedBufferCount() at next fetching buffer.
    bool mPendingBuffersRequested = false;
    // Currently requested buffer formats.
    BufferFormat mBufferFormat;

    // The unique ids of the buffers owned by V4L2DecodeComponent.
    std::set<uint32_t> mComponentOwnedUniquedIds;

    // Listener for buffer release events.
    sp<EventNotifier> mFetchBufferNotifier;

    std::mutex mBufferReleaseMutex;
    // Set to true when the buffer release event is triggered after dequeueing buffer from IGBP
    // times out. Reset when fetching new slot times out, or |mNotifyBlockAvailableCb| is executed.
    bool mBufferReleasedAfterTimedOut GUARDED_BY(mBufferReleaseMutex) = false;
    // The callback to notify the caller the buffer is available.
    ::base::OnceClosure mNotifyBlockAvailableCb GUARDED_BY(mBufferReleaseMutex);

    // Fields for surface switching.
    // The dequeued slots that comes from attaching buffers to the new surface.
    // All the slots |mDequeuedSlots| should be also in |mTrackedGraphicBuffers|.
    std::vector<int32_t> mDequeuedSlots;
    // The allocations needed to be migrated to the new surface.
    std::vector<std::shared_ptr<C2GraphicAllocation>> mAllocationsToBeMigrated;
    // The generation and usage of the new surface.
    uint32_t mGenerationToBeMigrated = 0;
    uint64_t mUsageToBeMigrated = 0;
    // Set to true if any error occurs at previous configureProducer().
    bool mConfigureProducerError = false;
};

C2VdaBqBlockPool::Impl::Impl(const std::shared_ptr<C2Allocator>& allocator)
      : mAllocator(allocator) {}

c2_status_t C2VdaBqBlockPool::Impl::fetchGraphicBlock(
        uint32_t width, uint32_t height, uint32_t format, C2MemoryUsage usage,
        std::shared_ptr<C2GraphicBlock>* block /* nonnull */) {
    ALOGV("%s(%ux%u)", __func__, width, height);
    std::lock_guard<std::mutex> lock(mMutex);

    if (width != mBufferFormat.mWidth || height != mBufferFormat.mHeight ||
        format != mBufferFormat.mPixelFormat || usage.expected != mBufferFormat.mUsage.expected) {
        ALOGE("%s(): buffer format (%ux%u, format=%u, usage=%" PRIx64
              ") is different from requested format (%ux%u, format=%u, usage=%" PRIx64 ")",
              __func__, width, height, format, usage.expected, mBufferFormat.mWidth,
              mBufferFormat.mHeight, mBufferFormat.mPixelFormat, mBufferFormat.mUsage.expected);
        return C2_BAD_VALUE;
    }

    if (mConfigureProducerError) {
        ALOGE("%s(): error occurred at previous configureProducer()", __func__);
        return C2_CORRUPTED;
    }

    if (!mProducer) {
        // Producer will not be configured in byte-buffer mode. Allocate buffers from allocator
        // directly as a basic graphic block pool.
        std::shared_ptr<C2GraphicAllocation> alloc;
        c2_status_t err = mAllocator->newGraphicAllocation(width, height, format, usage, &alloc);
        if (err != C2_OK) {
            return err;
        }
        *block = _C2BlockFactory::CreateGraphicBlock(alloc);
        if (*block == nullptr) {
            ALOGE("failed to create GraphicBlock: no memory");
            return C2_NO_MEMORY;
        }
        return C2_OK;
    }

    if (mPendingBuffersRequested) {
        status_t status = mProducer->setMaxDequeuedBufferCount(mBuffersRequested);
        if (status == android::BAD_VALUE) {
            // Note: We might be stuck here forever if the consumer never release enough buffers or
            // we hit other restriction of IGBP::setMaxDequeuedBufferCount() unexpectedly.
            ALOGI("Free buffers are not enough, waiting for consumer release buffers.");
            return C2_TIMED_OUT;
        } else if (status != android::NO_ERROR) {
            return asC2Error(status);
        }

        mPendingBuffersRequested = false;
    }

    // prepareMigrateBuffers() set maximum dequeued buffer count to the size of tracked buffers.
    // To migrate all the tracked buffer by ourselves, we need to wait for the client releasing all
    // the buffers that are migrated by the codec2 framework. Because the component calls
    // fetchGraphicBlock() when a buffer is released to IGBP, we defer the buffer migration here.
    if (!mAllocationsToBeMigrated.empty()) {
        if (!pumpMigrateBuffers()) {
            ALOGE("%s(): failed to migrate all buffers to the new surface.", __func__);
            return C2_CORRUPTED;
        }
        if (!mAllocationsToBeMigrated.empty()) {
            ALOGV("%s(): surface migration is not finished.", __func__);
            return C2_TIMED_OUT;
        }
    }

    int32_t slot;
    sp<Fence> fence = new Fence();
    status_t status = getFreeSlotLocked(width, height, format, usage, &slot, &fence);
    if (status != android::NO_ERROR) {
        return asC2Error(status);
    }

    if (!mTrackedGraphicBuffers.hasSlotId(slot)) {
        if (mTrackedGraphicBuffers.size() >= mBuffersRequested) {
            // The dequeued slot has a pre-allocated buffer whose size and format is as same as
            // currently requested (but was not dequeued during allocation cycle). Just detach it to
            // free this slot. And try dequeueBuffer again.
            ALOGD("dequeued a new slot %d but already allocated enough buffers. Detach it.", slot);

            if (mProducer->detachBuffer(slot) != android::NO_ERROR) {
                return C2_CORRUPTED;
            }
            return C2_TIMED_OUT;
        }

        // Call requestBuffer to allocate buffer for the slot and obtain the reference.
        sp<GraphicBuffer> slotBuffer = new GraphicBuffer();
        status = mProducer->requestBuffer(slot, &slotBuffer);
        if (status != android::NO_ERROR) {
            if (mProducer->cancelBuffer(slot, fence) != android::NO_ERROR) {
                return C2_CORRUPTED;
            }
            return asC2Error(status);
        }

        // Convert GraphicBuffer to C2GraphicAllocation and wrap producer id and slot index
        ALOGV("buffer wraps { producer id: %" PRIx64 ", slot: %d }", mProducerId, slot);
        C2Handle* handleWithId = V4L2GraphicAllocator::WrapNativeHandleToC2HandleWithId(
                slotBuffer->handle, slotBuffer->width, slotBuffer->height, slotBuffer->format,
                slotBuffer->usage, slotBuffer->stride, slotBuffer->getGenerationNumber(),
                mProducerId, slot);
        if (!handleWithId) {
            ALOGE("WrapNativeHandleToC2HandleWithId failed");
            return C2_NO_MEMORY;
        }

        std::optional<uint32_t> uniqueId =
                V4L2GraphicAllocator::getIdFromC2HandleWithId(handleWithId);
        ALOG_ASSERT(uniqueId, "Failed to get uniqueId from handleWithId");
        ALOGV("%s(): buffer %u wraps { producerId: %" PRIx64 ", slot: %d }", __func__, *uniqueId,
              mProducerId, slot);

        std::shared_ptr<C2GraphicAllocation> alloc;
        c2_status_t err = mAllocator->priorGraphicAllocation(handleWithId, &alloc);
        if (err != C2_OK) {
            ALOGE("priorGraphicAllocation failed: %d", err);
            return err;
        }

        mTrackedGraphicBuffers.insert(slot, *uniqueId, std::move(alloc));
        ALOGV("%s(): mTrackedGraphicBuffers.size=%zu", __func__, mTrackedGraphicBuffers.size());
        if (mTrackedGraphicBuffers.size() == mBuffersRequested) {
            ALOGV("Tracked IGBP slots: %s", mTrackedGraphicBuffers.debugString().c_str());
            // Already allocated enough buffers, set allowAllocation to false to restrict the
            // eligible slots to allocated ones for future dequeue.
            status = mProducer->allowAllocation(false);
            if (status != android::NO_ERROR) {
                return asC2Error(status);
            }
        }
    }

    ALOG_ASSERT(mTrackedGraphicBuffers.hasSlotId(slot));
    std::shared_ptr<C2GraphicAllocation> allocation = mTrackedGraphicBuffers.getAllocation(slot);
    const uint32_t uniqueId = mTrackedGraphicBuffers.getUniqueId(slot);
    auto poolData =
            std::make_shared<C2VdaBqBlockPoolData>(mProducerId, slot, uniqueId, shared_from_this());
    *block = _C2BlockFactory::CreateGraphicBlock(std::move(allocation), std::move(poolData));
    if (*block == nullptr) {
        ALOGE("failed to create GraphicBlock: no memory");
        return C2_NO_MEMORY;
    }
    mComponentOwnedUniquedIds.insert(uniqueId);
    return C2_OK;
}

status_t C2VdaBqBlockPool::Impl::getFreeSlotLocked(uint32_t width, uint32_t height, uint32_t format,
                                                   C2MemoryUsage usage, int32_t* slot,
                                                   sp<Fence>* fence) {
    // If there is an dequeued slot that is not owned by the component, then return it directly.
    if (!mDequeuedSlots.empty()) {
        ALOGV("%s(): mDequeuedSlots.size()=%zu", __func__, mDequeuedSlots.size());
        // Erasing the last feasible element is faster, so we use reverse iterator here.
        for (auto rIter = mDequeuedSlots.rbegin(); rIter != mDequeuedSlots.rend(); rIter++) {
            const uint32_t uniqueId = mTrackedGraphicBuffers.getUniqueId(*rIter);
            if (mComponentOwnedUniquedIds.find(uniqueId) == mComponentOwnedUniquedIds.end()) {
                ALOGV("%s(): got slot %d from mDequeuedSlots, mDequeuedSlots.size()=%zu", __func__,
                      *rIter, mDequeuedSlots.size());
                *slot = *rIter;
                mDequeuedSlots.erase(std::next(rIter).base());
                return android::NO_ERROR;
            }
        }
    }

    // Dequeue a free slot from IGBP.
    ALOGV("%s(): try to dequeue free slot from IGBP.", __func__);
    status_t status = mProducer->dequeueBuffer(width, height, format, usage, slot, fence);
    // The C2VdaBqBlockPool does not fully own the bufferqueue. After buffers are dequeued here,
    // they are passed into the codec2 framework, processed, and eventually queued into the
    // bufferqueue. The C2VdaBqBlockPool cannot determine exactly when a buffer gets queued.
    // However, if every buffer is being processed by the codec2 framework, then dequeueBuffer()
    // will return INVALID_OPERATION because of an attempt to dequeue too many buffers.
    // The C2VdaBqBlockPool cannot prevent this from happening, so just map it to TIMED_OUT
    // and let the C2VdaBqBlockPool's caller's timeout retry logic handle the failure.
    if (status == android::INVALID_OPERATION) {
        status = android::TIMED_OUT;
    }
    if (status == android::TIMED_OUT) {
        std::lock_guard<std::mutex> lock(mBufferReleaseMutex);
        mBufferReleasedAfterTimedOut = false;
    }
    if (status != android::NO_ERROR && status != BUFFER_NEEDS_REALLOCATION) {
        return status;
    }

    // Wait for acquire fence if we get one.
    if (*fence) {
        // The underlying sync-file kernel API guarantees that fences will
        // be signaled in a relative short, finite time.
        status_t fenceStatus = (*fence)->waitForever(LOG_TAG);
        if (fenceStatus != android::NO_ERROR) {
            status_t cancelStatus = mProducer->cancelBuffer(*slot, *fence);
            if (cancelStatus != android::NO_ERROR) {
                return cancelStatus;
            }
            ALOGE("buffer fence wait error: %d", fenceStatus);
            return fenceStatus;
        }

        if (mRenderCallback) {
            nsecs_t signalTime = (*fence)->getSignalTime();
            if (signalTime >= 0 && signalTime < INT64_MAX) {
                mRenderCallback(mProducerId, *slot, signalTime);
            } else {
                ALOGV("got fence signal time of %" PRId64 " nsec", signalTime);
            }
        }
    }

    ALOGV("%s(%ux%u): dequeued slot=%d", __func__, mBufferFormat.mWidth, mBufferFormat.mHeight,
          *slot);
    return android::NO_ERROR;
}

void C2VdaBqBlockPool::Impl::onEventNotified() {
    ALOGV("%s()", __func__);
    ::base::OnceClosure outputCb;
    {
        std::lock_guard<std::mutex> lock(mBufferReleaseMutex);

        mBufferReleasedAfterTimedOut = true;
        if (mNotifyBlockAvailableCb) {
            mBufferReleasedAfterTimedOut = false;
            outputCb = std::move(mNotifyBlockAvailableCb);
        }
    }

    // Calling the callback outside the lock to avoid the deadlock.
    if (outputCb) {
        std::move(outputCb).Run();
    }
}

c2_status_t C2VdaBqBlockPool::Impl::queryGenerationAndUsage(
        H2BGraphicBufferProducer* const producer, uint32_t width, uint32_t height,
        uint32_t pixelFormat, C2AndroidMemoryUsage androidUsage, uint32_t* generation,
        uint64_t* usage) {
    ALOGV("queryGenerationAndUsage");
    sp<Fence> fence = new Fence();
    int32_t status;
    int32_t slot;

    status = producer->dequeueBuffer(width, height, pixelFormat, androidUsage, &slot, &fence);
    if (status != android::NO_ERROR && status != BUFFER_NEEDS_REALLOCATION) {
        return asC2Error(status);
    }

    // Wait for acquire fence if we get one.
    if (fence) {
        status_t fenceStatus = fence->wait(kFenceWaitTimeMs);
        if (fenceStatus != android::NO_ERROR) {
            if (producer->cancelBuffer(slot, fence) != android::NO_ERROR) {
                return C2_CORRUPTED;
            }
            if (fenceStatus == -ETIME) {  // fence wait timed out
                ALOGV("%s(): buffer (slot=%d) fence wait timed out", __func__, slot);
                return C2_TIMED_OUT;
            }
            ALOGE("buffer fence wait error: %d", fenceStatus);
            return asC2Error(fenceStatus);
        }
    }

    // Call requestBuffer to allocate buffer for the slot and obtain the reference.
    // Get generation number here.
    sp<GraphicBuffer> slotBuffer = new GraphicBuffer();
    status = producer->requestBuffer(slot, &slotBuffer);

    // Detach and delete the temporary buffer.
    if (producer->detachBuffer(slot) != android::NO_ERROR) {
        return C2_CORRUPTED;
    }

    // Check requestBuffer return flag.
    if (status != android::NO_ERROR) {
        return asC2Error(status);
    }

    // Get generation number and usage from the slot buffer.
    *usage = slotBuffer->getUsage();
    *generation = slotBuffer->getGenerationNumber();
    ALOGV("Obtained from temp buffer: generation = %u, usage = %" PRIu64 "", *generation, *usage);
    return C2_OK;
}

void C2VdaBqBlockPool::Impl::setRenderCallback(
        const C2BufferQueueBlockPool::OnRenderCallback& renderCallback) {
    ALOGV("setRenderCallback");
    std::lock_guard<std::mutex> lock(mMutex);
    mRenderCallback = renderCallback;
}

c2_status_t C2VdaBqBlockPool::Impl::requestNewBufferSet(int32_t bufferCount, uint32_t width,
                                                        uint32_t height, uint32_t format,
                                                        C2MemoryUsage usage) {
    ALOGV("%s(bufferCount=%d, size=%ux%u, format=0x%x, usage=%" PRIu64 ")", __func__, bufferCount,
          width, height, format, usage.expected);

    if (bufferCount <= 0) {
        ALOGE("Invalid requested buffer count = %d", bufferCount);
        return C2_BAD_VALUE;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    if (!mProducer) {
        ALOGD("No HGraphicBufferProducer is configured...");
        return C2_NO_INIT;
    }

    status_t status = mProducer->allowAllocation(true);
    if (status != android::NO_ERROR) {
        return asC2Error(status);
    }

    // Release all remained slot buffer references here. CCodec should either cancel or queue its
    // owned buffers from this set before the next resolution change.
    detachAndMoveTrackedBuffers();

    mBuffersRequested = static_cast<size_t>(bufferCount);
    mPendingBuffersRequested = true;

    // Store buffer formats for future usage.
    mBufferFormat = BufferFormat(width, height, format, C2AndroidMemoryUsage(usage));

    return C2_OK;
}

std::vector<std::shared_ptr<C2GraphicAllocation>>
C2VdaBqBlockPool::Impl::detachAndMoveTrackedBuffers() {
    // Detach all dequeued slots.
    for (const int32_t slotId : mDequeuedSlots) {
        status_t status = mProducer->detachBuffer(slotId);
        if (status != android::NO_ERROR) {
            ALOGW("detachBuffer slot=%d from old producer failed: %d", slotId, status);
        }
    }
    mDequeuedSlots.clear();

    // Clear all the tracked graphic buffers.
    std::vector<std::shared_ptr<C2GraphicAllocation>> allocations;
    while (mTrackedGraphicBuffers.size() > 0) {
        int32_t slotId;
        uint32_t uniqueId;
        std::shared_ptr<C2GraphicAllocation> allocation;
        std::tie(slotId, uniqueId, allocation) = mTrackedGraphicBuffers.pop();

        allocations.push_back(std::move(allocation));
    }
    return allocations;
}

void C2VdaBqBlockPool::Impl::configureProducer(const sp<HGraphicBufferProducer>& producer) {
    ALOGV("%s(producer=%p)", __func__, producer.get());

    std::lock_guard<std::mutex> lock(mMutex);
    if (producer == nullptr) {
        ALOGI("input producer is nullptr...");

        mProducer = nullptr;
        mProducerId = 0;
        detachAndMoveTrackedBuffers();
        return;
    }

    auto newProducer = std::make_unique<H2BGraphicBufferProducer>(producer);
    if (newProducer->setDequeueTimeout(0) != android::NO_ERROR) {
        ALOGE("%s(): failed to setDequeueTimeout(0)", __func__);
        mConfigureProducerError = true;
        return;
    }
    // hack(b/146409777): Try to connect ARC-specific listener first.
    sp<BufferReleasedNotifier> listener = new BufferReleasedNotifier(shared_from_this());
    if (newProducer->connect(listener, 'ARC\0', false) == android::NO_ERROR) {
        ALOGI("connected to ARC-specific IGBP listener.");
        mFetchBufferNotifier = listener;
    }

    uint64_t newProducerId;
    if (newProducer->getUniqueId(&newProducerId) != android::NO_ERROR) {
        ALOGE("%s(): failed to get IGBP ID", __func__);
        mConfigureProducerError = true;
        return;
    }
    if (newProducerId == mProducerId) {
        ALOGI("%s(): configure the same producer, ignore", __func__);
        return;
    }

    ALOGI("Producer (Surface) is going to switch... ( 0x%" PRIx64 " -> 0x%" PRIx64 " )",
          mProducerId, newProducerId);
    mAllocationsToBeMigrated = detachAndMoveTrackedBuffers();

    mProducer = std::move(newProducer);
    mProducerId = newProducerId;
    if (!prepareMigrateBuffers()) {
        ALOGE("%s(): prepareMigrateBuffers() failed", __func__);
        mConfigureProducerError = true;
    }
}

bool C2VdaBqBlockPool::Impl::prepareMigrateBuffers() {
    ALOGV("%s()", __func__);

    if (mAllocationsToBeMigrated.empty()) {
        ALOGI("No buffers need to be migrated.");
        return true;
    }

    if (mAllocator->getId() == android::V4L2AllocatorId::SECURE_GRAPHIC) {
        // TODO(johnylin): support this when we meet the use case in the future.
        ALOGE("Switch producer for secure buffer is not supported...");
        return false;
    }

    // Set allowAllocation to new producer.
    if (mProducer->allowAllocation(true) != android::NO_ERROR) {
        return false;
    }

    // Before calling configureProducer(), the codec2 framework already attached some of the buffers
    // to the new surface. However, we have to migrate one more time to keep track of the uniqueId
    // of each buffer.
    // To guarantee each buffer is only attached at one slot, we set maximum dequeued buffer count
    // to the size of tracked buffers before we attach all tracked buffers.
    // Also, while attaching buffers, generation number and usage must be aligned to the producer.
    // The generation number and usage could be queried by getting a buffer from the new producer.
    // So we temporarily need one extra slot for querying generation and usage.
    if (mProducer->setMaxDequeuedBufferCount(mAllocationsToBeMigrated.size() + 1) !=
        android::NO_ERROR) {
        return false;
    }
    c2_status_t err =
            queryGenerationAndUsage(mProducer.get(), mBufferFormat.mWidth, mBufferFormat.mHeight,
                                    mBufferFormat.mPixelFormat, mBufferFormat.mUsage,
                                    &mGenerationToBeMigrated, &mUsageToBeMigrated);
    if (err != C2_OK) {
        ALOGE("queryGenerationAndUsage failed: %d", err);
        return false;
    }
    if (mProducer->setMaxDequeuedBufferCount(mAllocationsToBeMigrated.size()) !=
        android::NO_ERROR) {
        return false;
    }

    return pumpMigrateBuffers();
}

bool C2VdaBqBlockPool::Impl::pumpMigrateBuffers() {
    ALOGV("%s(): mAllocationsToBeMigrated.size()=%zu", __func__, mAllocationsToBeMigrated.size());

    while (!mAllocationsToBeMigrated.empty()) {
        const C2Handle* oldHandleWithId = mAllocationsToBeMigrated.back()->handle();

        // Convert C2GraphicAllocation to GraphicBuffer, and update generation number and usage.
        uint32_t uniqueId, width, height, format, stride, igbpSlot, generation;
        uint64_t usage, igbpId;
        native_handle_t* nativeHandle =
                V4L2GraphicAllocator::UnwrapAndMoveC2HandleWithId2NativeHandle(
                        oldHandleWithId, &uniqueId, &width, &height, &format, &usage, &stride,
                        &generation, &igbpId, &igbpSlot);
        sp<GraphicBuffer> graphicBuffer =
                new GraphicBuffer(nativeHandle, GraphicBuffer::CLONE_HANDLE, width, height, format,
                                  1, mUsageToBeMigrated, stride);
        native_handle_delete(nativeHandle);
        if (graphicBuffer->initCheck() != android::NO_ERROR) {
            ALOGE("Failed to create GraphicBuffer: %d", graphicBuffer->initCheck());
            return false;
        }
        graphicBuffer->setGenerationNumber(mGenerationToBeMigrated);

        int32_t newSlot;
        status_t attachStatus = mProducer->attachBuffer(graphicBuffer, &newSlot);
        if (attachStatus == android::TIMED_OUT || attachStatus == android::INVALID_OPERATION) {
            ALOGV("%s(): No free slot yet.", __func__);
            std::lock_guard<std::mutex> lock(mBufferReleaseMutex);
            mBufferReleasedAfterTimedOut = false;
            return true;
        }
        if (attachStatus != android::NO_ERROR) {
            ALOGE("%s(): Failed to attach buffer to new producer: %d", __func__, attachStatus);
            return false;
        }

        // Migrate C2GraphicAllocation wrapping new usage, generation number, producer id, and
        // slot index, and store it to |newSlotAllocations|.
        C2Handle* migratedHandle = V4L2GraphicAllocator::MigrateC2HandleWithId(
                oldHandleWithId, mUsageToBeMigrated, mGenerationToBeMigrated, mProducerId, newSlot);
        if (!migratedHandle) {
            ALOGE("MigrateC2HandleWithId() failed");
            return false;
        }

        std::shared_ptr<C2GraphicAllocation> migratedAllocation;
        c2_status_t err = mAllocator->priorGraphicAllocation(migratedHandle, &migratedAllocation);
        if (err != C2_OK) {
            ALOGE("priorGraphicAllocation failed: %d", err);
            return false;
        }

        mTrackedGraphicBuffers.insert(newSlot, uniqueId, std::move(migratedAllocation));
        ALOGV("%s(): Migrated buffer %u to slot %d, mTrackedGraphicBuffers.size=%zu", __func__,
              uniqueId, newSlot, mTrackedGraphicBuffers.size());

        mDequeuedSlots.push_back(newSlot);
        mAllocationsToBeMigrated.pop_back();
    }

    if (mProducer->setMaxDequeuedBufferCount(mBuffersRequested) != android::NO_ERROR) {
        return false;
    }

    // Set allowAllocation to false if we track enough buffers, so that the producer does not
    // allocate new buffers. Otherwise allocation will be disabled in fetchGraphicBlock after enough
    // buffers have been allocated.
    if (mTrackedGraphicBuffers.size() == mBuffersRequested) {
        if (mProducer->allowAllocation(false) != android::NO_ERROR) {
            ALOGE("allowAllocation(false) failed");
            return false;
        }
    }
    return true;
}

void C2VdaBqBlockPool::Impl::onC2GraphicBlockReleased(uint64_t producerId, int32_t slotId,
                                                      uint32_t uniqueId, bool shared) {
    ALOGV("%s(producerId=%" PRIx64 ", slotId=%d, uniqueId=%u shared=%d)", __func__, producerId,
          slotId, uniqueId, shared);
    std::lock_guard<std::mutex> lock(mMutex);

    mComponentOwnedUniquedIds.erase(uniqueId);

    if (!shared && mProducer && producerId == mProducerId) {
        sp<Fence> fence = new Fence();
        if (mProducer->cancelBuffer(slotId, fence) != android::NO_ERROR) {
            ALOGW("%s(): Failed to cancelBuffer()", __func__);
        }
    }
}

bool C2VdaBqBlockPool::Impl::setNotifyBlockAvailableCb(::base::OnceClosure cb) {
    ALOGV("%s()", __func__);
    if (mFetchBufferNotifier == nullptr) {
        return false;
    }

    ::base::OnceClosure outputCb;
    {
        std::lock_guard<std::mutex> lock(mBufferReleaseMutex);

        // If there is any buffer released after dequeueBuffer() timed out, then we could notify the
        // caller directly.
        if (mBufferReleasedAfterTimedOut) {
            mBufferReleasedAfterTimedOut = false;
            outputCb = std::move(cb);
        } else {
            mNotifyBlockAvailableCb = std::move(cb);
        }
    }

    // Calling the callback outside the lock to avoid the deadlock.
    if (outputCb) {
        std::move(outputCb).Run();
    }
    return true;
}

C2VdaBqBlockPool::C2VdaBqBlockPool(const std::shared_ptr<C2Allocator>& allocator,
                                   const local_id_t localId)
      : C2BufferQueueBlockPool(allocator, localId), mLocalId(localId), mImpl(new Impl(allocator)) {}

c2_status_t C2VdaBqBlockPool::fetchGraphicBlock(
        uint32_t width, uint32_t height, uint32_t format, C2MemoryUsage usage,
        std::shared_ptr<C2GraphicBlock>* block /* nonnull */) {
    if (mImpl) {
        return mImpl->fetchGraphicBlock(width, height, format, usage, block);
    }
    return C2_NO_INIT;
}

void C2VdaBqBlockPool::setRenderCallback(
        const C2BufferQueueBlockPool::OnRenderCallback& renderCallback) {
    if (mImpl) {
        mImpl->setRenderCallback(renderCallback);
    }
}

c2_status_t C2VdaBqBlockPool::requestNewBufferSet(int32_t bufferCount, uint32_t width,
                                                  uint32_t height, uint32_t format,
                                                  C2MemoryUsage usage) {
    if (mImpl) {
        return mImpl->requestNewBufferSet(bufferCount, width, height, format, usage);
    }
    return C2_NO_INIT;
}

void C2VdaBqBlockPool::configureProducer(const sp<HGraphicBufferProducer>& producer) {
    if (mImpl) {
        mImpl->configureProducer(producer);
    }
}

bool C2VdaBqBlockPool::setNotifyBlockAvailableCb(::base::OnceClosure cb) {
    if (mImpl) {
        return mImpl->setNotifyBlockAvailableCb(std::move(cb));
    }
    return false;
}

C2VdaBqBlockPoolData::C2VdaBqBlockPoolData(uint64_t producerId, int32_t slotId, uint32_t uniqueId,
                                           const std::shared_ptr<C2VdaBqBlockPool::Impl>& pool)
      : mProducerId(producerId), mSlotId(slotId), mUniqueId(uniqueId), mPool(pool) {}

C2VdaBqBlockPoolData::~C2VdaBqBlockPoolData() {
    if (!mPool) {
        return;
    }

    mPool->onC2GraphicBlockReleased(mProducerId, mSlotId, mUniqueId, mShared);
}

}  // namespace android
