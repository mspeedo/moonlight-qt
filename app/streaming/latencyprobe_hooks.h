#pragma once

// This file is force-included for Linux C++ translation units by globaldefs.pri.
// It is intentionally inert unless the build selected Moonlight's libplacebo
// Vulkan renderer. Keeping the probe hooks here avoids invasive edits to
// upstream plvk.cpp and makes rebases substantially easier.

#ifdef HAVE_LIBPLACEBO_VULKAN

#include "latencyprobe.h"

#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace LatencyProbeHooks {

struct RendererContext;

struct SampleSlot {
    RendererContext* owner = nullptr;
    pl_tex texture = nullptr;
    uint8_t rgba[4] = {};
    uint64_t serial = 0;
    std::atomic<bool> pending { false };
};

struct RendererContext {
    pl_log log = nullptr;
    pl_gpu gpu = nullptr;
    pl_renderer mainRenderer = nullptr;
    pl_renderer detectorRenderer = nullptr;
    pl_fmt sampleFormat = nullptr;
    bool detectorUnavailable = false;
    uint64_t nextSerial = 1;
    SampleSlot slots[4];

    RendererContext()
    {
        for (auto& slot : slots) {
            slot.owner = this;
        }
    }
};

struct PendingFrame {
    pl_swapchain swapchain = nullptr;
    pl_renderer renderer = nullptr;
    pl_frame image = {};
    bool sampleRequested = false;
};

inline std::mutex g_ContextLock;
inline std::unordered_map<pl_renderer, std::unique_ptr<RendererContext>> g_Contexts;
inline thread_local PendingFrame g_PendingFrame;

inline RendererContext* findContext(pl_renderer renderer)
{
    std::lock_guard<std::mutex> lock(g_ContextLock);
    auto it = g_Contexts.find(renderer);
    return it != g_Contexts.end() ? it->second.get() : nullptr;
}

inline void sampleComplete(void* opaque)
{
    auto* slot = static_cast<SampleSlot*>(opaque);
    const uint64_t serial = slot->serial;

    const float r = slot->rgba[0] / 255.0f;
    const float g = slot->rgba[1] / 255.0f;
    const float b = slot->rgba[2] / 255.0f;
    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;

    slot->pending.store(false, std::memory_order_release);
    LatencyProbe::instance().onVideoSample(serial, luma);
}

inline bool ensureDetectorResources(RendererContext* ctx, SampleSlot* slot)
{
    if (ctx->detectorUnavailable) {
        return false;
    }

    if (!ctx->gpu->limits.callbacks) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Latency probe disabled: libplacebo GPU callbacks are unavailable");
        ctx->detectorUnavailable = true;
        return false;
    }

    if (ctx->detectorRenderer == nullptr) {
        ctx->detectorRenderer = pl_renderer_create(ctx->log, ctx->gpu);
        if (ctx->detectorRenderer == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Latency probe disabled: unable to create detector renderer");
            ctx->detectorUnavailable = true;
            return false;
        }
    }

    if (ctx->sampleFormat == nullptr) {
        ctx->sampleFormat = pl_find_fmt(ctx->gpu,
                                       PL_FMT_UNORM,
                                       4,
                                       8,
                                       8,
                                       (enum pl_fmt_caps)(PL_FMT_CAP_RENDERABLE |
                                                          PL_FMT_CAP_HOST_READABLE));
        if (ctx->sampleFormat == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Latency probe disabled: no renderable host-readable RGBA8 format");
            ctx->detectorUnavailable = true;
            return false;
        }
    }

    if (slot->texture == nullptr) {
        pl_tex_params params = {};
        params.w = 1;
        params.h = 1;
        params.format = ctx->sampleFormat;
        params.renderable = true;
        params.host_readable = true;
        params.debug_tag = PL_DEBUG_TAG;

        slot->texture = pl_tex_create(ctx->gpu, &params);
        if (slot->texture == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Latency probe: unable to create 1x1 readback texture");
            return false;
        }
    }

    return true;
}

inline bool scheduleSample(RendererContext* ctx, const pl_frame& image, uint64_t serial)
{
    SampleSlot* slot = nullptr;
    for (auto& candidate : ctx->slots) {
        bool expected = false;
        if (candidate.pending.compare_exchange_strong(expected, true,
                                                      std::memory_order_acq_rel)) {
            slot = &candidate;
            break;
        }
    }

    if (slot == nullptr) {
        // Never stall the Moonlight render thread for the detector. A later
        // frame will be sampled when a slot becomes available.
        return false;
    }

    if (!ensureDetectorResources(ctx, slot)) {
        slot->pending.store(false, std::memory_order_release);
        return false;
    }

    slot->serial = serial;

    pl_frame target = {};
    target.num_planes = 1;
    target.planes[0].texture = slot->texture;
    target.planes[0].components = 4;
    target.planes[0].component_mapping[0] = 0;
    target.planes[0].component_mapping[1] = 1;
    target.planes[0].component_mapping[2] = 2;
    target.planes[0].component_mapping[3] = 3;
    target.crop = { 0.0f, 0.0f, 1.0f, 1.0f };
    target.repr = pl_color_repr_rgb;
    target.color = pl_color_space_srgb;

    // Render the source into a single RGB pixel. The helper's dark and bright
    // noise distributions have a deliberately wide gap, so even a single
    // downsampled value is enough for robust classification.
    if (!pl_render_image(ctx->detectorRenderer, &image, &target, &pl_render_fast_params)) {
        slot->pending.store(false, std::memory_order_release);
        return false;
    }

    pl_tex_transfer_params transfer = {};
    transfer.tex = slot->texture;
    transfer.rc = { 0, 0, 0, 1, 1, 1 };
    transfer.ptr = slot->rgba;
    transfer.callback = sampleComplete;
    transfer.priv = slot;

    if (!pl_tex_download(ctx->gpu, &transfer)) {
        slot->pending.store(false, std::memory_order_release);
        return false;
    }

    // Queue the detector work before plvk.cpp unmaps the source frame, but do
    // not wait for it. The callback is intentionally asynchronous.
    pl_gpu_flush(ctx->gpu);
    return true;
}

inline pl_renderer rendererCreate(pl_log log, pl_gpu gpu)
{
    pl_renderer renderer = pl_renderer_create(log, gpu);
    if (renderer == nullptr) {
        return nullptr;
    }

    auto context = std::make_unique<RendererContext>();
    context->log = log;
    context->gpu = gpu;
    context->mainRenderer = renderer;

    std::lock_guard<std::mutex> lock(g_ContextLock);
    g_Contexts.emplace(renderer, std::move(context));
    return renderer;
}

inline void rendererDestroy(pl_renderer* renderer)
{
    std::unique_ptr<RendererContext> context;

    if (renderer != nullptr && *renderer != nullptr) {
        std::lock_guard<std::mutex> lock(g_ContextLock);
        auto it = g_Contexts.find(*renderer);
        if (it != g_Contexts.end()) {
            context = std::move(it->second);
            g_Contexts.erase(it);
        }
    }

    if (context) {
        // Teardown is allowed to block. Ensure all asynchronous callbacks have
        // completed before their slots/context are released.
        pl_gpu_finish(context->gpu);

        for (auto& slot : context->slots) {
            pl_tex_destroy(context->gpu, &slot.texture);
        }

        if (context->detectorRenderer != nullptr) {
            pl_renderer_destroy(&context->detectorRenderer);
        }
    }

    pl_renderer_destroy(renderer);
}

inline bool renderImage(pl_renderer renderer,
                        const pl_frame* image,
                        const pl_frame* target,
                        const pl_render_params* params)
{
    const bool result = pl_render_image(renderer, image, target, params);

    // One relaxed atomic branch is the only steady-state cost while the OSD is
    // disabled. No locks, lookups, sampling, or readback occur in that state.
    if (!result || image == nullptr || !LatencyProbe::instance().isEnabled()) {
        return result;
    }

    if (g_PendingFrame.swapchain != nullptr &&
            !g_PendingFrame.sampleRequested &&
            LatencyProbe::instance().needsVideoSample()) {
        g_PendingFrame.renderer = renderer;
        g_PendingFrame.image = *image;
        g_PendingFrame.sampleRequested = true;
    }

    return result;
}

inline bool swapchainStartFrame(pl_swapchain swapchain, pl_swapchain_frame* outFrame)
{
    g_PendingFrame = {};

    const bool result = pl_swapchain_start_frame(swapchain, outFrame);
    if (result) {
        g_PendingFrame.swapchain = swapchain;
    }

    return result;
}

inline bool swapchainSubmitFrame(pl_swapchain swapchain)
{
    const bool result = pl_swapchain_submit_frame(swapchain);
    const uint64_t submitTimestamp = result ? SDL_GetPerformanceCounter() : 0;

    PendingFrame pending = {};
    if (g_PendingFrame.swapchain == swapchain) {
        pending = g_PendingFrame;
        g_PendingFrame = {};
    }

    if (!result || !pending.sampleRequested || pending.renderer == nullptr ||
            !LatencyProbe::instance().isEnabled()) {
        return result;
    }

    RendererContext* context = findContext(pending.renderer);
    if (context == nullptr) {
        return result;
    }

    const uint64_t serial = context->nextSerial++;

    // The 1x1 detector render is deliberately queued only after the actual
    // swapchain frame has been submitted. This keeps detector GPU work out of
    // the measured frame's presentation path.
    if (scheduleSample(context, pending.image, serial)) {
        LatencyProbe::instance().onFrameSubmitted(serial, submitTimestamp);
    }

    return result;
}

} // namespace LatencyProbeHooks

// These wrappers are defined after all real libplacebo declarations and after
// the wrapper function bodies above. Calls in Moonlight's C++ sources are
// therefore redirected without changing upstream plvk.cpp itself.
#define pl_renderer_create(log, gpu) \
    LatencyProbeHooks::rendererCreate((log), (gpu))
#define pl_renderer_destroy(renderer) \
    LatencyProbeHooks::rendererDestroy((renderer))
#define pl_render_image(renderer, image, target, params) \
    LatencyProbeHooks::renderImage((renderer), (image), (target), (params))
#define pl_swapchain_start_frame(swapchain, frame) \
    LatencyProbeHooks::swapchainStartFrame((swapchain), (frame))
#define pl_swapchain_submit_frame(swapchain) \
    LatencyProbeHooks::swapchainSubmitFrame((swapchain))

#endif // HAVE_LIBPLACEBO_VULKAN
