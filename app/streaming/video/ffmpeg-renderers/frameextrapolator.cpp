#include "frameextrapolator.h"

#if defined(Q_OS_LINUX) && defined(HAVE_LIBPLACEBO_VULKAN)

#include <algorithm>
#include <cmath>

#include <libplacebo/shaders/custom.h>

FrameExtrapolator::FrameExtrapolator(pl_log log, pl_gpu gpu, int streamFps) :
    m_Log(log),
    m_Gpu(gpu),
    m_StreamFps(std::max(streamFps, 1)),
    m_FrameIntervalUs(1000000ULL / std::max(streamFps, 1))
{
}

FrameExtrapolator::~FrameExtrapolator()
{
    // Destroy GPU resources before releasing the decoded surface reference.
    // pl_tex_destroy() handles any outstanding GPU use of these resources.
    destroyResources();
    pl_dispatch_destroy(&m_Dispatch);
    av_frame_free(&m_LatestRealFrame);
}

bool FrameExtrapolator::initialize()
{
    m_Dispatch = pl_dispatch_create(m_Log, m_Gpu);
    m_LatestRealFrame = av_frame_alloc();
    return m_Dispatch != nullptr && m_LatestRealFrame != nullptr;
}

bool FrameExtrapolator::createTexture(pl_tex* texture, int width, int height, int components)
{
    // Three-component storage images are not universally available, so use
    // RGBA for three-component video planes while preserving pl_plane metadata.
    const int storageComponents = components == 3 ? 4 : components;
    const enum pl_fmt_caps caps = (enum pl_fmt_caps)
            (PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR |
             PL_FMT_CAP_STORABLE | PL_FMT_CAP_RENDERABLE);
    pl_fmt format = pl_find_fmt(m_Gpu, PL_FMT_FLOAT, storageComponents, 16, 0, caps);
    if (format == nullptr) {
        return false;
    }

    struct pl_tex_params params = {};
    params.w = width;
    params.h = height;
    params.format = format;
    params.sampleable = true;
    params.renderable = true;
    params.storable = true;

    *texture = pl_tex_create(m_Gpu, &params);
    return *texture != nullptr;
}

void FrameExtrapolator::destroyResources()
{
    if (m_Gpu == nullptr) {
        return;
    }

    for (int i = 0; i < 2; ++i) {
        pl_tex_destroy(m_Gpu, &m_FineLuma[i]);
        pl_tex_destroy(m_Gpu, &m_CoarseLuma[i]);
    }

    pl_tex_destroy(m_Gpu, &m_CoarseMotion);
    pl_tex_destroy(m_Gpu, &m_FineMotion);
    pl_tex_destroy(m_Gpu, &m_SceneMetric);

    for (int i = 0; i < PL_MAX_PLANES; ++i) {
        pl_tex_destroy(m_Gpu, &m_SyntheticPlanes[i]);
    }

    m_ResourcesReady = false;
    m_HasHistory = false;
    m_HasMotion = false;
}

bool FrameExtrapolator::ensureResources(const pl_frame& frame)
{
    if (frame.num_planes <= 0 || frame.num_planes > PL_MAX_PLANES ||
            frame.planes[0].texture == nullptr) {
        return false;
    }

    const int sourceWidth = frame.planes[0].texture->params.w;
    const int sourceHeight = frame.planes[0].texture->params.h;

    if (m_ResourcesReady) {
        if (sourceWidth != m_SourceWidth || sourceHeight != m_SourceHeight ||
                frame.num_planes != m_PlaneCount) {
            m_HasMotion = false;
            return false;
        }

        for (int i = 0; i < frame.num_planes; ++i) {
            pl_tex source = frame.planes[i].texture;
            if (source == nullptr ||
                    source->params.w != m_PlaneWidths[i] ||
                    source->params.h != m_PlaneHeights[i] ||
                    frame.planes[i].components != m_PlaneComponents[i]) {
                m_HasMotion = false;
                return false;
            }
        }

        return true;
    }

    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;
    m_FineWidth = (sourceWidth + 3) / 4;
    m_FineHeight = (sourceHeight + 3) / 4;
    m_CoarseWidth = (m_FineWidth + 1) / 2;
    m_CoarseHeight = (m_FineHeight + 1) / 2;
    m_PlaneCount = frame.num_planes;

    if (!createTexture(&m_FineLuma[0], m_FineWidth, m_FineHeight, 1) ||
            !createTexture(&m_FineLuma[1], m_FineWidth, m_FineHeight, 1) ||
            !createTexture(&m_CoarseLuma[0], m_CoarseWidth, m_CoarseHeight, 1) ||
            !createTexture(&m_CoarseLuma[1], m_CoarseWidth, m_CoarseHeight, 1) ||
            !createTexture(&m_CoarseMotion, (m_CoarseWidth + 3) / 4, (m_CoarseHeight + 3) / 4, 4) ||
            !createTexture(&m_FineMotion, (m_FineWidth + 3) / 4, (m_FineHeight + 3) / 4, 4) ||
            !createTexture(&m_SceneMetric, 1, 1, 1)) {
        destroyResources();
        return false;
    }

    for (int i = 0; i < frame.num_planes; ++i) {
        pl_tex source = frame.planes[i].texture;
        if (source == nullptr || frame.planes[i].components <= 0) {
            destroyResources();
            return false;
        }

        m_PlaneWidths[i] = source->params.w;
        m_PlaneHeights[i] = source->params.h;
        m_PlaneComponents[i] = frame.planes[i].components;

        if (!createTexture(&m_SyntheticPlanes[i],
                           m_PlaneWidths[i],
                           m_PlaneHeights[i],
                           m_PlaneComponents[i])) {
            destroyResources();
            return false;
        }
    }

    m_ResourcesReady = true;
    return true;
}

bool FrameExtrapolator::runCompute(pl_tex target,
                                   const char* description,
                                   const char* header,
                                   const char* body,
                                   const pl_shader_desc* descriptors,
                                   int descriptorCount,
                                   const pl_shader_var* variables,
                                   int variableCount,
                                   int groupSizeX,
                                   int groupSizeY)
{
    pl_shader shader = pl_dispatch_begin(m_Dispatch);
    if (shader == nullptr) {
        return false;
    }

    struct pl_custom_shader params = {};
    params.description = description;
    params.header = header;
    params.body = body;
    params.input = PL_SHADER_SIG_NONE;
    params.output = PL_SHADER_SIG_COLOR;
    params.descriptors = descriptors;
    params.num_descriptors = descriptorCount;
    params.variables = variables;
    params.num_variables = variableCount;
    params.compute = true;
    params.compute_group_size[0] = groupSizeX;
    params.compute_group_size[1] = groupSizeY;

    if (!pl_shader_custom(shader, &params)) {
        return false;
    }

    struct pl_dispatch_params dispatch = {};
    dispatch.shader = &shader;
    dispatch.target = target;
    return pl_dispatch_finish(m_Dispatch, &dispatch);
}

bool FrameExtrapolator::dispatchDownsample(pl_tex source, pl_tex target, int scale)
{
    pl_shader_desc descriptor = {};
    descriptor.desc.name = "src_tex";
    descriptor.desc.type = PL_DESC_SAMPLED_TEX;
    descriptor.binding.object = (void*)source;
    descriptor.binding.sample_mode =
            (source->params.format->caps & PL_FMT_CAP_LINEAR) ?
                PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;

    const char* body4 = R"(
        ivec2 p = ivec2(gl_GlobalInvocationID.xy);
        ivec2 dstSize = (textureSize(src_tex, 0) + ivec2(3)) / 4;
        vec2 uv = (vec2(p) + vec2(0.5)) / vec2(dstSize);
        float y = textureLod(src_tex, uv, 0.0).r;
        color = vec4(y, 0.0, 0.0, 1.0);
    )";

    const char* body2 = R"(
        ivec2 p = ivec2(gl_GlobalInvocationID.xy);
        ivec2 dstSize = (textureSize(src_tex, 0) + ivec2(1)) / 2;
        vec2 uv = (vec2(p) + vec2(0.5)) / vec2(dstSize);
        float y = textureLod(src_tex, uv, 0.0).r;
        color = vec4(y, 0.0, 0.0, 1.0);
    )";

    return runCompute(target,
                      scale == 4 ? "frame extrapolation luma quarter-res" :
                                   "frame extrapolation luma coarse-res",
                      nullptr,
                      scale == 4 ? body4 : body2,
                      &descriptor,
                      1);
}

bool FrameExtrapolator::dispatchSceneMetric(pl_tex current, pl_tex previous)
{
    pl_shader_desc descriptors[2] = {};
    descriptors[0].desc.name = "current_luma";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)current;
    descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
    descriptors[1].desc.name = "previous_luma";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)previous;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;

    const char* body = R"(
        float sumDiff = 0.0;
        const int gridX = 32;
        const int gridY = 18;
        for (int y = 0; y < gridY; ++y) {
            for (int x = 0; x < gridX; ++x) {
                vec2 uv = (vec2(x, y) + vec2(0.5)) / vec2(gridX, gridY);
                float a = textureLod(current_luma, uv, 0.0).r;
                float b = textureLod(previous_luma, uv, 0.0).r;
                sumDiff += abs(a - b);
            }
        }
        color = vec4(sumDiff / float(gridX * gridY), 0.0, 0.0, 1.0);
    )";

    return runCompute(m_SceneMetric,
                      "frame extrapolation scene metric",
                      nullptr,
                      body,
                      descriptors,
                      2,
                      nullptr,
                      0,
                      1,
                      1);
}

bool FrameExtrapolator::dispatchCoarseMotion(pl_tex current, pl_tex previous)
{
    pl_shader_desc descriptors[2] = {};
    descriptors[0].desc.name = "current_luma";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)current;
    descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[1].desc.name = "previous_luma";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)previous;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    const char* body = R"(
        ivec2 block = ivec2(gl_GlobalInvocationID.xy);
        ivec2 size = textureSize(current_luma, 0);
        ivec2 base = block * 4;
        float bestCost = 1e20;
        ivec2 bestVector = ivec2(0);

        // Backward vector convention: for a current-frame block at p, v points
        // to the matching location p+v in the previous frame.
        for (int dy = -8; dy <= 8; ++dy) {
            for (int dx = -8; dx <= 8; ++dx) {
                ivec2 candidate = ivec2(dx, dy);
                float sad = 0.0;
                for (int by = 0; by < 4; ++by) {
                    for (int bx = 0; bx < 4; ++bx) {
                        ivec2 cp = clamp(base + ivec2(bx, by), ivec2(0), size - ivec2(1));
                        ivec2 pp = clamp(cp + candidate, ivec2(0), size - ivec2(1));
                        sad += abs(texelFetch(current_luma, cp, 0).r -
                                   texelFetch(previous_luma, pp, 0).r);
                    }
                }
                if (sad < bestCost) {
                    bestCost = sad;
                    bestVector = candidate;
                }
            }
        }

        color = vec4(vec2(bestVector), bestCost / 16.0, 1.0);
    )";

    return runCompute(m_CoarseMotion,
                      "frame extrapolation coarse block motion",
                      nullptr,
                      body,
                      descriptors,
                      2);
}

bool FrameExtrapolator::dispatchFineMotion(pl_tex current, pl_tex previous)
{
    pl_shader_desc descriptors[3] = {};
    descriptors[0].desc.name = "current_luma";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)current;
    descriptors[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[1].desc.name = "previous_luma";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)previous;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    descriptors[2].desc.name = "coarse_motion";
    descriptors[2].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[2].binding.object = (void*)m_CoarseMotion;
    descriptors[2].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    const char* body = R"(
        ivec2 block = ivec2(gl_GlobalInvocationID.xy);
        ivec2 size = textureSize(current_luma, 0);
        ivec2 base = block * 4;

        ivec2 coarseSize = textureSize(coarse_motion, 0);
        ivec2 coarseBlock = clamp(block / 2, ivec2(0), coarseSize - ivec2(1));
        ivec2 seed = ivec2(round(texelFetch(coarse_motion, coarseBlock, 0).rg * 2.0));

        float bestCost = 1e20;
        float secondCost = 1e20;
        ivec2 bestVector = seed;

        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                ivec2 candidate = seed + ivec2(dx, dy);
                float sad = 0.0;
                for (int by = 0; by < 4; ++by) {
                    for (int bx = 0; bx < 4; ++bx) {
                        ivec2 cp = clamp(base + ivec2(bx, by), ivec2(0), size - ivec2(1));
                        ivec2 pp = clamp(cp + candidate, ivec2(0), size - ivec2(1));
                        sad += abs(texelFetch(current_luma, cp, 0).r -
                                   texelFetch(previous_luma, pp, 0).r);
                    }
                }

                if (sad < bestCost) {
                    secondCost = bestCost;
                    bestCost = sad;
                    bestVector = candidate;
                }
                else if (sad < secondCost) {
                    secondCost = sad;
                }
            }
        }

        float meanCost = bestCost / 16.0;
        float separation = max(secondCost - bestCost, 0.0) / max(secondCost, 1e-5);
        float uniqueness = clamp(separation * 5.0, 0.0, 1.0);
        float quality = 1.0 - smoothstep(0.08, 0.22, meanCost);
        float confidence = uniqueness * quality;

        color = vec4(vec2(bestVector), confidence, meanCost);
    )";

    return runCompute(m_FineMotion,
                      "frame extrapolation fine block motion",
                      nullptr,
                      body,
                      descriptors,
                      3);
}

bool FrameExtrapolator::dispatchWarp(pl_tex source, pl_tex target, float alpha)
{
    pl_shader_desc descriptors[3] = {};
    descriptors[0].desc.name = "src_plane";
    descriptors[0].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[0].binding.object = (void*)source;
    descriptors[0].binding.sample_mode =
            (source->params.format->caps & PL_FMT_CAP_LINEAR) ?
                PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
    descriptors[1].desc.name = "motion_field";
    descriptors[1].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[1].binding.object = (void*)m_FineMotion;
    descriptors[1].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
    descriptors[2].desc.name = "scene_metric";
    descriptors[2].desc.type = PL_DESC_SAMPLED_TEX;
    descriptors[2].binding.object = (void*)m_SceneMetric;
    descriptors[2].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;

    pl_shader_var variable = {};
    variable.var = pl_var_float("extrapolation_alpha");
    variable.data = &alpha;
    variable.dynamic = true;

    const char* body = R"(
        ivec2 p = ivec2(gl_GlobalInvocationID.xy);
        ivec2 planeSize = textureSize(src_plane, 0);
        vec2 uv = (vec2(p) + vec2(0.5)) / vec2(planeSize);

        vec4 motion = textureLod(motion_field, uv, 0.0);
        float sceneDiff = textureLod(scene_metric, vec2(0.5), 0.0).r;

        // Scene cuts and ambiguous blocks smoothly collapse to the current real
        // image instead of being warped. No CPU readback is required.
        float sceneConfidence = 1.0 - smoothstep(0.12, 0.18, sceneDiff);
        float confidence = clamp(motion.b * sceneConfidence, 0.0, 1.0);

        // motion.rg is a backward current->previous displacement measured in
        // quarter-resolution luma pixels. A future output location therefore
        // samples current at x + alpha*v. Normalized motion applies correctly
        // to luma and chroma planes regardless of chroma subsampling.
        vec2 fineSize = vec2(textureSize(motion_field, 0)) * 4.0;
        vec2 motionUv = motion.rg / max(fineSize, vec2(1.0));
        vec2 warpedUv = clamp(uv + extrapolation_alpha * motionUv,
                              vec2(0.0), vec2(1.0));

        vec4 original = textureLod(src_plane, uv, 0.0);
        vec4 warped = textureLod(src_plane, warpedUv, 0.0);
        color = mix(original, warped, confidence);
    )";

    return runCompute(target,
                      "frame extrapolation full-resolution warp",
                      nullptr,
                      body,
                      descriptors,
                      3,
                      &variable,
                      1);
}

bool FrameExtrapolator::submitRealFrame(const AVFrame* frame,
                                        const pl_frame& mappedFrame,
                                        uint64_t renderTimeUs)
{
    if (!ensureResources(mappedFrame)) {
        return false;
    }

    pl_dispatch_reset_frame(m_Dispatch);

    const int currentIndex = m_HasHistory ? 1 - m_HistoryIndex : 0;
    pl_tex luma = mappedFrame.planes[0].texture;

    if (!dispatchDownsample(luma, m_FineLuma[currentIndex], 4) ||
            !dispatchDownsample(m_FineLuma[currentIndex], m_CoarseLuma[currentIndex], 2)) {
        m_HasMotion = false;
        return false;
    }

    if (m_HasHistory) {
        if (!dispatchSceneMetric(m_CoarseLuma[currentIndex], m_CoarseLuma[m_HistoryIndex]) ||
                !dispatchCoarseMotion(m_CoarseLuma[currentIndex], m_CoarseLuma[m_HistoryIndex]) ||
                !dispatchFineMotion(m_FineLuma[currentIndex], m_FineLuma[m_HistoryIndex])) {
            m_HasMotion = false;
            return false;
        }

        // Encoder keyframes are a cheap CPU-visible scene-cut hint. The reduced
        // GPU scene metric still gates confidence for cuts that aren't flagged.
        m_HasMotion = !(frame->flags & AV_FRAME_FLAG_KEY);
    }
    else {
        m_HasMotion = false;
    }

    m_HistoryIndex = currentIndex;
    m_HasHistory = true;
    m_LastRealRenderTimeUs = renderTimeUs;
    m_SyntheticSinceLastReal = false;

    av_frame_unref(m_LatestRealFrame);
    if (av_frame_ref(m_LatestRealFrame, frame) < 0) {
        m_HasMotion = false;
        return false;
    }

    return true;
}

bool FrameExtrapolator::canExtrapolate(uint64_t targetTimeUs)
{
    if (!m_ResourcesReady || !m_HasMotion || m_SyntheticSinceLastReal ||
            m_LatestRealFrame == nullptr || m_LatestRealFrame->width <= 0 ||
            m_LastRealRenderTimeUs == 0 || targetTimeUs <= m_LastRealRenderTimeUs) {
        return false;
    }

    const uint64_t elapsedUs = targetTimeUs - m_LastRealRenderTimeUs;
    const double alpha = (double)elapsedUs / (double)m_FrameIntervalUs;
    if (alpha < 0.75 || alpha > 1.25) {
        return false;
    }

    // A zero-time poll must never stall presentation. Skip this opportunity if
    // analysis is still in flight rather than synchronizing the CPU with GPU.
    if (pl_tex_poll(m_Gpu, m_FineMotion, 0) ||
            pl_tex_poll(m_Gpu, m_SceneMetric, 0)) {
        return false;
    }

    return true;
}

bool FrameExtrapolator::buildSyntheticFrame(const pl_frame& currentFrame,
                                             uint64_t targetTimeUs,
                                             pl_frame* syntheticFrame)
{
    if (!canExtrapolate(targetTimeUs) || currentFrame.num_planes != m_PlaneCount) {
        return false;
    }

    const double rawAlpha = (double)(targetTimeUs - m_LastRealRenderTimeUs) /
            (double)m_FrameIntervalUs;
    const float alpha = (float)std::clamp(rawAlpha, 0.75, 1.25);

    pl_dispatch_reset_frame(m_Dispatch);

    for (int i = 0; i < currentFrame.num_planes; ++i) {
        pl_tex source = currentFrame.planes[i].texture;
        if (source == nullptr ||
                source->params.w != m_PlaneWidths[i] ||
                source->params.h != m_PlaneHeights[i] ||
                currentFrame.planes[i].components != m_PlaneComponents[i]) {
            return false;
        }

        if (!dispatchWarp(source, m_SyntheticPlanes[i], alpha)) {
            return false;
        }
    }

    *syntheticFrame = currentFrame;
    for (int i = 0; i < syntheticFrame->num_planes; ++i) {
        syntheticFrame->planes[i].texture = m_SyntheticPlanes[i];
    }

    return true;
}

#endif
