#include "plvk_fused_rcas.h"
#include "plvk_rcas.h"

#include <cstdio>
#include <cstdlib>

void FusedRcas::initialize()
{
    m_Hook = {};
    m_Hook.stages = PL_HOOK_OUTPUT;
#ifdef PL_HAVE_HOOK_COLOR_SAMPLER
    m_Hook.input = PL_HOOK_SIG_COLOR_SAMPLER;
#else
    m_Hook.input = PL_HOOK_SIG_COLOR;
#endif
    m_Hook.priv = this;
    m_Hook.hook = hook;
    m_Hook.signature = UINT64_C(0x4d4c465243415332); // MLFRCAS2
}

const pl_hook* FusedRcas::prepare(float strength)
{
    m_Failed = false;
    m_Strength = strength;
#ifdef PL_HAVE_HOOK_COLOR_SAMPLER
    return &m_Hook;
#else
    m_AvailabilityKnown = true;
    m_Available = false;
    m_Reason = "patched libplacebo color sampler unavailable";
    return nullptr;
#endif
}

pl_hook_res FusedRcas::hook(void* priv, const pl_hook_params* params)
{
    auto& self = *static_cast<FusedRcas*>(priv);
    pl_hook_res result = {};

#ifdef PL_HAVE_HOOK_COLOR_SAMPLER
    self.m_AvailabilityKnown = true;
    if (!params->sample_color) {
        self.m_Available = false;
        self.m_Reason = params->sample_color_reason ?
                params->sample_color_reason : "libplacebo color sampler unavailable";
        return result;
    }

    self.m_Available = true;
    self.m_Reason = "available";

    // A zero-strength invocation is used only to probe availability. Avoid
    // adding any sharpening code to the actual frame in that case.
    if (self.m_Strength <= 0.0f)
        return result;

    pl_shader_var strength = {};
    strength.var = pl_var_float("mlRcasStrength");
    strength.data = &self.m_Strength;
    strength.dynamic = true;

    char body[1024];
    std::snprintf(body, sizeof(body),
                  "vec3 b = %s(vec2( 0.0, -1.0)).rgb;\n"
                  "vec3 d = %s(vec2(-1.0,  0.0)).rgb;\n"
                  "vec3 f = %s(vec2( 1.0,  0.0)).rgb;\n"
                  "vec3 h = %s(vec2( 0.0,  1.0)).rgb;\n"
                  "color.rgb = moonlightRcas(b, d, color.rgb, f, h, "
                  "mlRcasStrength);",
                  params->sample_color, params->sample_color,
                  params->sample_color, params->sample_color);

    pl_custom_shader shader = {};
    shader.input = shader.output = PL_SHADER_SIG_COLOR;
    shader.description = "Moonlight fused RGB RCAS";
    shader.header = kRcasCore;
    shader.body = body;
    shader.variables = &strength;
    shader.num_variables = 1;
    shader.output_w = std::abs(params->dst_rect.x1 - params->dst_rect.x0);
    shader.output_h = std::abs(params->dst_rect.y1 - params->dst_rect.y0);

    if (!pl_shader_custom(params->sh, &shader)) {
        self.m_Available = false;
        self.m_Reason = "unable to append RCAS to output shader";
        self.m_Failed = result.failed = true;
        return result;
    }

    result.output = PL_HOOK_SIG_COLOR;
    result.sh = params->sh;
    result.rect = params->rect;
    result.repr = params->repr;
    result.color = params->color;
    result.components = params->components;
#endif

    return result;
}
