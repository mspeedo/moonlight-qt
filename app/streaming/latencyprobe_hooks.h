#pragma once

// This file is force-included for Linux C++ translation units by globaldefs.pri.
// It is intentionally inert unless the build selected Moonlight's libplacebo
// Vulkan renderer. Keeping the probe hooks here avoids invasive edits to
// upstream plvk.cpp and makes rebases substantially easier.

#ifdef HAVE_LIBPLACEBO_VULKAN

#include "latencyprobe.h"
#include "streampipelinetelemetry.h"
#include "streamhealthtelemetry.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

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
    uint64_t submitTimestamp = 0;
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
    const uint64_t submitTimestamp = slot->submitTimestamp;

    const float r = slot->rgba[0] / 255.0f;
    const float g = slot->rgba[1] / 255.0f;
    const float b = slot->rgba[2] / 255.0f;
    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;

    slot->pending.store(false, std::memory_order_release);
    LatencyProbe::instance().onVideoSample(serial, submitTimestamp, luma);
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

inline bool scheduleSample(RendererContext* ctx,
                           const pl_frame& image,
                           uint64_t serial,
                           uint64_t submitTimestamp)
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
    slot->submitTimestamp = submitTimestamp;

    const float sourceLeft = image.crop.x0 < image.crop.x1 ? image.crop.x0 : image.crop.x1;
    const float sourceRight = image.crop.x0 < image.crop.x1 ? image.crop.x1 : image.crop.x0;
    const float sourceTop = image.crop.y0 < image.crop.y1 ? image.crop.y0 : image.crop.y1;
    const float sourceBottom = image.crop.y0 < image.crop.y1 ? image.crop.y1 : image.crop.y0;

    if ((sourceRight - sourceLeft) < 1.0f || (sourceBottom - sourceTop) < 1.0f) {
        slot->pending.store(false, std::memory_order_release);
        return false;
    }

    // Sample exactly one logical source pixel at the center of the current
    // decoded stream crop. Mapping a 1x1 source crop to a 1x1 target avoids
    // downscaling/averaging the frame, so unrelated motion or noise elsewhere
    // on screen cannot change the detector value.
    const int centerX = static_cast<int>((sourceLeft + sourceRight) * 0.5f);
    const int centerY = static_cast<int>((sourceTop + sourceBottom) * 0.5f);

    pl_frame sampleImage = image;
    sampleImage.crop = {
        static_cast<float>(centerX),
        static_cast<float>(centerY),
        static_cast<float>(centerX + 1),
        static_cast<float>(centerY + 1),
    };

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

    if (!pl_render_image(ctx->detectorRenderer, &sampleImage, &target, &pl_render_fast_params)) {
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

    // swapchainStartFrame() performs the sole steady-state atomic gate. If it
    // did not opt this frame into sampling, the thread-local swapchain is null
    // and this wrapper becomes a couple of predictable local branches only.
    if (!result || image == nullptr ||
            g_PendingFrame.swapchain == nullptr ||
            g_PendingFrame.sampleRequested) {
        return result;
    }

    g_PendingFrame.renderer = renderer;
    g_PendingFrame.image = *image;
    g_PendingFrame.sampleRequested = true;
    return result;
}

inline bool swapchainStartFrame(pl_swapchain swapchain, pl_swapchain_frame* outFrame)
{
    // This relaxed atomic load is the only benchmark-specific steady-state work
    // in the presentation path while no video sample is needed. With OSD off or
    // with the benchmark idle, there are no pending-frame writes or GPU actions.
    if (!LatencyProbe::instance().needsVideoSampleFast()) {
        return pl_swapchain_start_frame(swapchain, outFrame);
    }

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

    const bool matchingFrame = g_PendingFrame.swapchain == swapchain;
    const bool sampleRequested = matchingFrame &&
            g_PendingFrame.sampleRequested &&
            g_PendingFrame.renderer != nullptr;

    // Preserve the frozen input benchmark's t1 ordering: its SDL timestamp stays
    // the first benchmark clock sampled after a successful real swapchain submit.
    const uint64_t submitTimestamp = result && sampleRequested ?
                SDL_GetPerformanceCounter() : 0;

    // Phase 1 uses LiGetMicroseconds() so it shares the Common C clock domain.
    // Keep this immediately after the pre-existing input t1 capture. On the vast
    // majority of frames no input sample is pending, so only the TLS predicate
    // sits between the real submit return and this timestamp.
    if (result && StreamPipelineTelemetry::presentPendingFast()) {
        StreamPipelineTelemetry::presentSuccess(LiGetMicroseconds());
    }

    PendingFrame pending = {};
    if (matchingFrame) {
        pending = g_PendingFrame;
        g_PendingFrame = {};
    }

    if (!result || !sampleRequested) {
        return result;
    }

    RendererContext* context = findContext(pending.renderer);
    if (context == nullptr) {
        return result;
    }

    const uint64_t serial = context->nextSerial++;

    // The 1x1 detector render is deliberately queued only after the actual
    // swapchain frame has been submitted. This keeps detector GPU work out of
    // the measured frame's presentation path. Carry the exact submission
    // timestamp with this sample so the asynchronous callback needs no history
    // lookup or later frame association.
    scheduleSample(context, pending.image, serial, submitTimestamp);

    return result;
}

inline bool waitForNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit)
{
    const bool result = LiWaitForNextVideoFrame(frameHandle, decodeUnit);
    if (result && decodeUnit != nullptr) {
        StreamHealthTelemetry::noteDecodeUnit((*decodeUnit)->frameNumber);
        if (StreamPipelineTelemetry::isActiveFast()) {
            StreamPipelineTelemetry::noteDecodeUnit(*decodeUnit);
        }
    }
    return result;
}

inline bool pollNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit)
{
    const bool result = LiPollNextVideoFrame(frameHandle, decodeUnit);
    if (result && decodeUnit != nullptr) {
        StreamHealthTelemetry::noteDecodeUnit((*decodeUnit)->frameNumber);
        if (StreamPipelineTelemetry::isActiveFast()) {
            StreamPipelineTelemetry::noteDecodeUnit(*decodeUnit);
        }
    }
    return result;
}

inline int avcodecSendPacket(AVCodecContext* context, const AVPacket* packet)
{
    if (!StreamPipelineTelemetry::isActiveFast()) {
        return avcodec_send_packet(context, packet);
    }

    // No telemetry bookkeeping may occur between this timestamp and the real
    // decoder submission. Association/ring updates happen after the call returns.
    const std::uint64_t decodeStartUs = LiGetMicroseconds();
    const int result = avcodec_send_packet(context, packet);
    StreamPipelineTelemetry::decodeSubmitted(decodeStartUs, result >= 0);
    return result;
}

inline int avcodecReceiveFrame(AVCodecContext* context, AVFrame* frame)
{
    if (!StreamPipelineTelemetry::isActiveFast()) {
        return avcodec_receive_frame(context, frame);
    }

    const int result = avcodec_receive_frame(context, frame);
    if (result == 0) {
        // The active gate ran before the decoder call, leaving only the result
        // branch between a successful return and this exact output timestamp.
        StreamPipelineTelemetry::decodedFrame(frame, LiGetMicroseconds());
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
#define LiWaitForNextVideoFrame(frameHandle, decodeUnit) \
    LatencyProbeHooks::waitForNextVideoFrame((frameHandle), (decodeUnit))
#define LiPollNextVideoFrame(frameHandle, decodeUnit) \
    LatencyProbeHooks::pollNextVideoFrame((frameHandle), (decodeUnit))
#define avcodec_send_packet(context, packet) \
    LatencyProbeHooks::avcodecSendPacket((context), (packet))
#define avcodec_receive_frame(context, frame) \
    LatencyProbeHooks::avcodecReceiveFrame((context), (frame))

#endif // HAVE_LIBPLACEBO_VULKAN
