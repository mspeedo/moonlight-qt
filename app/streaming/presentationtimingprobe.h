#pragma once

#ifdef HAVE_LIBPLACEBO_VULKAN

#include "SDL_compat.h"
#include "displaypresentlatency.h"

#include <libplacebo/swapchain.h>
#include <libplacebo/vulkan.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// Additive presentation-confirmation bridge for the latency benchmark.
//
// Benchmark-sampled vkQueuePresentKHR calls are tagged with VK_KHR_present_id.
// After pl_swapchain_submit_frame() returns and the pre-existing benchmark and
// pipeline timestamps have already been taken, the tagged present is queued to a
// dedicated worker that blocks in vkWaitForPresentKHR(). The render thread never
// waits for presentation. The SDL performance-counter timestamp taken immediately
// after a successful wait is paired with the same 1x1 marker sample as the
// original input -> submit-return metric.
namespace PresentationTimingProbe {

#if defined(VK_KHR_present_id) && defined(VK_KHR_present_wait)

static constexpr size_t kHistorySize = 256;
static constexpr size_t kWaitQueueCapacity = 8;
static constexpr uint64_t kWaitTimeoutNs = 250000000ULL;

struct PresentFeedback {
    uint64_t presentId = 0;
    uint64_t sdlTicks = 0;
};

struct PendingSample {
    uint64_t presentId = 0;
    float luma = 0.0f;
};

struct WaitRequest {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint64_t presentId = 0;
};

inline std::array<PresentFeedback, kHistorySize> g_Feedback = {};
inline std::array<PendingSample, kHistorySize> g_PendingSamples = {};
inline std::mutex g_FeedbackLock;

inline std::mutex g_WaitLock;
inline std::condition_variable g_WaitCv;
inline std::deque<WaitRequest> g_WaitQueue;
inline std::thread g_WaitThread;
inline bool g_StopWaiter = false;

inline PFN_vkGetInstanceProcAddr g_RealGetInstanceProcAddr = nullptr;
inline PFN_vkGetDeviceProcAddr g_RealGetDeviceProcAddr = nullptr;
inline PFN_vkQueuePresentKHR g_RealQueuePresentKHR = nullptr;
inline PFN_vkWaitForPresentKHR g_WaitForPresentKHR = nullptr;
inline VkDevice g_Device = VK_NULL_HANDLE;

inline std::atomic<bool> g_TimingSupported { false };
inline uint64_t g_NextPresentId = 1;
inline thread_local bool g_CaptureNextPresent = false;
inline thread_local uint64_t g_LastPresentId = 0;
inline thread_local VkSwapchainKHR g_LastSwapchain = VK_NULL_HANDLE;

inline bool extensionEnabled(pl_vulkan vk, const char* name)
{
    if (vk == nullptr || name == nullptr) {
        return false;
    }

    for (int i = 0; i < vk->num_extensions; ++i) {
        if (std::strcmp(vk->extensions[i], name) == 0) {
            return true;
        }
    }

    return false;
}

inline bool physicalDeviceExtensionSupported(const struct pl_vulkan_params* params,
                                             const char* name)
{
    if (params == nullptr || params->get_proc_addr == nullptr ||
            params->instance == VK_NULL_HANDLE || params->device == VK_NULL_HANDLE ||
            name == nullptr) {
        return false;
    }

    auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            params->get_proc_addr(params->instance,
                                  "vkEnumerateDeviceExtensionProperties"));
    if (enumerate == nullptr) {
        return false;
    }

    uint32_t count = 0;
    if (enumerate(params->device, nullptr, &count, nullptr) != VK_SUCCESS || count == 0) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(count);
    if (enumerate(params->device, nullptr, &count, extensions.data()) != VK_SUCCESS) {
        return false;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(extensions[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

inline bool presentIdFeatureEnabled(pl_vulkan vk)
{
    if (vk == nullptr || vk->features == nullptr) {
        return false;
    }

    const VkBaseInStructure* node =
            reinterpret_cast<const VkBaseInStructure*>(vk->features->pNext);
    while (node != nullptr) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR) {
            const auto* features =
                    reinterpret_cast<const VkPhysicalDevicePresentIdFeaturesKHR*>(node);
            return features->presentId == VK_TRUE;
        }
        node = node->pNext;
    }
    return false;
}

inline bool presentWaitFeatureEnabled(pl_vulkan vk)
{
    if (vk == nullptr || vk->features == nullptr) {
        return false;
    }

    const VkBaseInStructure* node =
            reinterpret_cast<const VkBaseInStructure*>(vk->features->pNext);
    while (node != nullptr) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR) {
            const auto* features =
                    reinterpret_cast<const VkPhysicalDevicePresentWaitFeaturesKHR*>(node);
            return features->presentWait == VK_TRUE;
        }
        node = node->pNext;
    }
    return false;
}

inline void completeWait(uint64_t presentId, uint64_t sdlTimestamp)
{
    PendingSample sample = {};
    bool haveSample = false;

    {
        std::lock_guard<std::mutex> lock(g_FeedbackLock);
        PendingSample& pending = g_PendingSamples[presentId % kHistorySize];
        if (pending.presentId == presentId) {
            sample = pending;
            pending = {};
            haveSample = true;
        }
        else {
            PresentFeedback& feedback = g_Feedback[presentId % kHistorySize];
            feedback.presentId = presentId;
            // Zero is an intentional completion marker for timeout/error/drop. It
            // prevents a later detector callback from leaving a stale sample.
            feedback.sdlTicks = sdlTimestamp;
        }
    }

    if (haveSample && sdlTimestamp != 0) {
        DisplayPresentLatency::onVideoSample(sdlTimestamp, sample.luma);
    }
}

inline void waiterMain()
{
    for (;;) {
        WaitRequest request = {};
        {
            std::unique_lock<std::mutex> lock(g_WaitLock);
            g_WaitCv.wait(lock, []() {
                return g_StopWaiter || !g_WaitQueue.empty();
            });

            if (g_StopWaiter) {
                return;
            }

            request = g_WaitQueue.front();
            g_WaitQueue.pop_front();
        }

        PFN_vkWaitForPresentKHR waitForPresent = g_WaitForPresentKHR;
        const VkDevice device = g_Device;
        if (waitForPresent == nullptr || device == VK_NULL_HANDLE ||
                request.swapchain == VK_NULL_HANDLE || request.presentId == 0) {
            completeWait(request.presentId, 0);
            continue;
        }

        const VkResult result = waitForPresent(device,
                                               request.swapchain,
                                               request.presentId,
                                               kWaitTimeoutNs);
        const uint64_t timestamp = result == VK_SUCCESS ?
                SDL_GetPerformanceCounter() : 0;

        completeWait(request.presentId, timestamp);
    }
}

inline void startWaiterLocked()
{
    if (g_WaitThread.joinable()) {
        return;
    }

    g_StopWaiter = false;
    g_WaitThread = std::thread(waiterMain);
}

inline void stopWaiter()
{
    {
        std::lock_guard<std::mutex> lock(g_WaitLock);
        if (!g_WaitThread.joinable()) {
            g_WaitQueue.clear();
            g_StopWaiter = false;
            return;
        }

        g_StopWaiter = true;
        g_WaitQueue.clear();
        g_WaitCv.notify_all();
    }

    g_WaitThread.join();

    {
        std::lock_guard<std::mutex> lock(g_WaitLock);
        g_StopWaiter = false;
        g_WaitQueue.clear();
    }
}

inline void clearPresentationPairs()
{
    std::lock_guard<std::mutex> lock(g_FeedbackLock);
    g_Feedback.fill({});
    g_PendingSamples.fill({});
}

inline void resetState()
{
    stopWaiter();
    clearPresentationPairs();

    g_TimingSupported.store(false, std::memory_order_release);
    g_NextPresentId = 1;
    g_Device = VK_NULL_HANDLE;
    g_WaitForPresentKHR = nullptr;
    g_RealQueuePresentKHR = nullptr;
    g_RealGetDeviceProcAddr = nullptr;
    g_RealGetInstanceProcAddr = nullptr;
    g_CaptureNextPresent = false;
    g_LastPresentId = 0;
    g_LastSwapchain = VK_NULL_HANDLE;
}

inline VKAPI_ATTR VkResult VKAPI_CALL queuePresentKHR(VkQueue queue,
                                                       const VkPresentInfoKHR* presentInfo)
{
    if (g_RealQueuePresentKHR == nullptr || presentInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // The wrapper exists for every present, but ordinary streaming frames take
    // this branch immediately without touching any other probe state.
    if (!g_CaptureNextPresent) {
        return g_RealQueuePresentKHR(queue, presentInfo);
    }

    g_LastPresentId = 0;
    g_LastSwapchain = VK_NULL_HANDLE;

    if (!g_TimingSupported.load(std::memory_order_acquire) ||
            presentInfo->swapchainCount != 1 ||
            presentInfo->pSwapchains == nullptr ||
            presentInfo->pSwapchains[0] == VK_NULL_HANDLE) {
        return g_RealQueuePresentKHR(queue, presentInfo);
    }

    g_LastSwapchain = presentInfo->pSwapchains[0];

    uint64_t presentId = g_NextPresentId++;
    if (presentId == 0) {
        presentId = g_NextPresentId++;
    }

    VkPresentIdKHR idInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .pNext = presentInfo->pNext,
        .swapchainCount = 1,
        .pPresentIds = &presentId,
    };
    VkPresentInfoKHR taggedPresent = *presentInfo;
    taggedPresent.pNext = &idInfo;

    const VkResult result = g_RealQueuePresentKHR(queue, &taggedPresent);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        g_LastPresentId = presentId;
    }

    return result;
}

inline VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getDeviceProcAddr(VkDevice device,
                                                                  const char* name)
{
    if (g_RealGetDeviceProcAddr == nullptr) {
        return nullptr;
    }

    PFN_vkVoidFunction real = g_RealGetDeviceProcAddr(device, name);
    if (real != nullptr && name != nullptr && std::strcmp(name, "vkQueuePresentKHR") == 0) {
        g_RealQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&queuePresentKHR);
    }

    return real;
}

inline VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getInstanceProcAddr(VkInstance instance,
                                                                    const char* name)
{
    if (g_RealGetInstanceProcAddr == nullptr) {
        return nullptr;
    }

    PFN_vkVoidFunction real = g_RealGetInstanceProcAddr(instance, name);
    if (real != nullptr && name != nullptr && std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        g_RealGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(real);
        return reinterpret_cast<PFN_vkVoidFunction>(&getDeviceProcAddr);
    }

    return real;
}

inline pl_vulkan vulkanCreate(pl_log log, const struct pl_vulkan_params* params)
{
    if (params == nullptr || params->get_proc_addr == nullptr) {
        return pl_vulkan_create(log, params);
    }

    resetState();
    g_RealGetInstanceProcAddr = params->get_proc_addr;

    struct pl_vulkan_params wrapped = *params;
    std::vector<const char*> optionalExtensions;
    optionalExtensions.reserve(static_cast<size_t>(params->num_opt_extensions) + 2);

    bool havePresentIdRequest = false;
    bool havePresentWaitRequest = false;
    for (int i = 0; i < params->num_opt_extensions; ++i) {
        const char* extension = params->opt_extensions[i];
        optionalExtensions.push_back(extension);
        if (extension != nullptr) {
            havePresentIdRequest |=
                    std::strcmp(extension, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
            havePresentWaitRequest |=
                    std::strcmp(extension, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0;
        }
    }

    if (!havePresentIdRequest) {
        optionalExtensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
    }
    if (!havePresentWaitRequest) {
        optionalExtensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    }

    wrapped.opt_extensions = optionalExtensions.data();
    wrapped.num_opt_extensions = static_cast<int>(optionalExtensions.size());
    wrapped.get_proc_addr = &getInstanceProcAddr;

    const bool canRequestFeatures =
            physicalDeviceExtensionSupported(params, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
            physicalDeviceExtensionSupported(params, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);

    VkPhysicalDeviceFeatures2 requestedFeatures = {};
    VkPhysicalDevicePresentIdFeaturesKHR presentIdFeatures = {};
    VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeatures = {};
    if (canRequestFeatures) {
        if (params->features != nullptr) {
            requestedFeatures = *params->features;
        }
        else {
            requestedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        }

        void* originalNext = requestedFeatures.pNext;
        presentWaitFeatures.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
        presentWaitFeatures.pNext = originalNext;
        presentWaitFeatures.presentWait = VK_TRUE;

        presentIdFeatures.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
        presentIdFeatures.pNext = &presentWaitFeatures;
        presentIdFeatures.presentId = VK_TRUE;

        requestedFeatures.pNext = &presentIdFeatures;
        wrapped.features = &requestedFeatures;
    }

    pl_vulkan vk = pl_vulkan_create(log, &wrapped);
    if (vk == nullptr) {
        resetState();
        return nullptr;
    }

    g_Device = vk->device;
    if (g_RealGetDeviceProcAddr == nullptr) {
        PFN_vkVoidFunction proc = g_RealGetInstanceProcAddr(vk->instance,
                                                            "vkGetDeviceProcAddr");
        g_RealGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(proc);
    }

    if (g_RealGetDeviceProcAddr != nullptr) {
        g_WaitForPresentKHR = reinterpret_cast<PFN_vkWaitForPresentKHR>(
                g_RealGetDeviceProcAddr(vk->device, "vkWaitForPresentKHR"));
    }

    const bool supported =
            extensionEnabled(vk, VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
            extensionEnabled(vk, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) &&
            presentIdFeatureEnabled(vk) &&
            presentWaitFeatureEnabled(vk) &&
            g_WaitForPresentKHR != nullptr;
    g_TimingSupported.store(supported, std::memory_order_release);

    // The present-wait worker is created lazily on the first queued benchmark
    // sample. Merely starting a normal Vulkan stream adds no extra thread.
    return vk;
}

inline void swapchainDestroy(pl_swapchain* swapchain)
{
    // vkWaitForPresentKHR references the private VkSwapchainKHR. Join the waiter
    // before libplacebo can destroy that object. A later sampled present lazily
    // restarts the worker if the swapchain was recreated while the device lives.
    stopWaiter();
    clearPresentationPairs();
    pl_swapchain_destroy(swapchain);
}

inline void vulkanDestroy(pl_vulkan* vk)
{
    stopWaiter();
    g_TimingSupported.store(false, std::memory_order_release);
    clearPresentationPairs();
    pl_vulkan_destroy(vk);

    g_Device = VK_NULL_HANDLE;
    g_WaitForPresentKHR = nullptr;
    g_RealQueuePresentKHR = nullptr;
}

inline void setCaptureNextPresent(bool capture)
{
    // Avoid TLS writes on ordinary frames. The render hook calls this with false
    // before and after every unsampled submit, so the common path stays read-only.
    if (g_CaptureNextPresent == capture) {
        return;
    }

    g_CaptureNextPresent = capture;
    if (capture) {
        g_LastPresentId = 0;
        g_LastSwapchain = VK_NULL_HANDLE;
    }
}

inline uint64_t lastPresentId()
{
    return g_LastPresentId;
}

inline void queuePresentWait(uint64_t presentId)
{
    if (presentId == 0 ||
            !g_TimingSupported.load(std::memory_order_acquire) ||
            g_WaitForPresentKHR == nullptr || g_Device == VK_NULL_HANDLE ||
            g_LastPresentId != presentId || g_LastSwapchain == VK_NULL_HANDLE) {
        return;
    }

    const WaitRequest request = {g_LastSwapchain, presentId};
    bool dropped = false;

    {
        std::lock_guard<std::mutex> lock(g_WaitLock);
        startWaiterLocked();
        if (g_WaitQueue.size() >= kWaitQueueCapacity) {
            // Benchmark samples are sparse, so reaching this means presentation
            // confirmation is not keeping up. Drop only the sidecar sample rather
            // than allowing diagnostic work to build pressure on streaming.
            dropped = true;
        }
        else {
            g_WaitQueue.push_back(request);
            g_WaitCv.notify_one();
        }
    }

    if (dropped) {
        // Publish an explicit zero completion so a later 1x1 detector callback
        // consumes and clears this present ID instead of leaving stale pair state.
        completeWait(presentId, 0);
    }
}

inline void deliverVideoSample(uint64_t, uint64_t presentId, float luma)
{
    if (presentId == 0 || !g_TimingSupported.load(std::memory_order_acquire)) {
        return;
    }

    PresentFeedback feedback = {};
    bool haveFeedback = false;

    {
        std::lock_guard<std::mutex> lock(g_FeedbackLock);
        PresentFeedback& existing = g_Feedback[presentId % kHistorySize];
        if (existing.presentId == presentId) {
            feedback = existing;
            existing = {};
            haveFeedback = true;
        }
        else {
            PendingSample& pending = g_PendingSamples[presentId % kHistorySize];
            pending.presentId = presentId;
            pending.luma = luma;
        }
    }

    if (haveFeedback && feedback.sdlTicks != 0) {
        DisplayPresentLatency::onVideoSample(feedback.sdlTicks, luma);
    }
}

#else

inline pl_vulkan vulkanCreate(pl_log log, const struct pl_vulkan_params* params)
{
    return pl_vulkan_create(log, params);
}

inline void swapchainDestroy(pl_swapchain* swapchain)
{
    pl_swapchain_destroy(swapchain);
}

inline void vulkanDestroy(pl_vulkan* vk)
{
    pl_vulkan_destroy(vk);
}

inline void setCaptureNextPresent(bool) {}
inline uint64_t lastPresentId() { return 0; }
inline void queuePresentWait(uint64_t) {}
inline void deliverVideoSample(uint64_t, uint64_t, float) {}

#endif

} // namespace PresentationTimingProbe

#endif // HAVE_LIBPLACEBO_VULKAN